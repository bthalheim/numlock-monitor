// NumLock indicator: a small always-on-top square that shows the live NumLock state
// and logs what appears to have caused each change to numlock.log (next to the exe).
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr UINT_PTR kPollTimer = 1;
constexpr UINT kPollMs = 50;
constexpr UINT kKeyMsg = WM_APP + 1;   // posted by the keyboard hook
constexpr int kSize = 100;             // client size at 96 DPI
constexpr DWORD kCorrelateMs = 1000;   // a key event this recent explains a state change
constexpr UINT kIdOpenLog = 1, kIdExit = 2;

bool g_numLockOn = false;
HICON g_iconOn = nullptr, g_iconOff = nullptr;
HHOOK g_hook = nullptr;
HWND g_hwnd = nullptr;
std::wstring g_logPath;
std::set<DWORD> g_watchPids;   // from "--pid N" arguments; extra details logged for these

// Last NumLock keystroke seen by the hook.
DWORD g_lastKeyTick = 0;
bool g_haveKey = false;
std::wstring g_lastKeyDesc;

bool ReadNumLock() { return (GetKeyState(VK_NUMLOCK) & 1) != 0; }

// ---- logging -------------------------------------------------------------------------

void Log(const std::wstring& text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[40];
    wsprintfW(stamp, L"%04d-%02d-%02d %02d:%02d:%02d.%03d  ", t.wYear, t.wMonth, t.wDay,
              t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::wstring line = stamp + text + L"\r\n";

    const int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return;
    std::string utf8(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8.data(), n, nullptr, nullptr);

    HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    // The text is mostly ASCII, so without a BOM Notepad (and other IsTextUnicode-based viewers)
    // can mistake it for UTF-16 and show CJK glyphs. A UTF-8 BOM on a new file prevents that.
    LARGE_INTEGER size;
    if (GetFileSizeEx(f, &size) && size.QuadPart == 0) {
        static const BYTE bom[] = {0xEF, 0xBB, 0xBF};
        WriteFile(f, bom, sizeof(bom), &written, nullptr);
    }
    WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(f);
}

std::wstring ProcessName(DWORD pid) {
    if (!pid) return L"?";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    std::wstring name = L"(inaccessible)";
    if (h) {
        wchar_t buf[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
            name = buf;
            const size_t slash = name.find_last_of(L'\\');
            if (slash != std::wstring::npos) name = name.substr(slash + 1);
        }
        CloseHandle(h);
    }
    return name + L" (pid " + std::to_wstring(pid) + L")";
}

// Full path, command line, parent, start time and session of a process, for watched PIDs.
std::wstring ProcessDetails(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"details unavailable (OpenProcess error " + std::to_wstring(GetLastError()) + L")";

    std::wstring out;
    std::wstring path(32768, L'\0');
    DWORD len = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(h, 0, path.data(), &len)) {
        path.resize(len);
        out += L"path=" + path;
    }

    // ProcessCommandLineInformation (60) works with limited access on Windows 8.1+.
    struct UnicodeString { USHORT length, maxLength; PWSTR buffer; };
    using NtQueryFn = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto ntQuery = reinterpret_cast<NtQueryFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (ntQuery) {
        std::vector<BYTE> buf(65536);
        ULONG got = 0;
        if (ntQuery(h, 60, buf.data(), static_cast<ULONG>(buf.size()), &got) >= 0) {
            const auto* us = reinterpret_cast<const UnicodeString*>(buf.data());
            if (us->buffer) out += L"; cmdline=" + std::wstring(us->buffer, us->length / sizeof(wchar_t));
        }
    }

    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(h, &created, &exited, &kernel, &user)) {
        FILETIME local;
        SYSTEMTIME st;
        if (FileTimeToLocalFileTime(&created, &local) && FileTimeToSystemTime(&local, &st)) {
            wchar_t s[40];
            wsprintfW(s, L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond);
            out += std::wstring(L"; started=") + s;
        }
    }
    CloseHandle(h);

    DWORD session = 0;
    if (ProcessIdToSessionId(pid, &session)) out += L"; session=" + std::to_wstring(session);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (pe.th32ProcessID == pid) {
                out += L"; parent=" + ProcessName(pe.th32ParentProcessID);
                break;
            }
        }
        CloseHandle(snap);
    }
    return out;
}

// ---- keyboard hook ---------------------------------------------------------------------

// Runs on the UI thread inside the message loop; keep it tiny and hand off to the window.
LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (k->vkCode == VK_NUMLOCK) {
            DWORD pid = 0;
            if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
            PostMessageW(g_hwnd, kKeyMsg, static_cast<WPARAM>(pid), static_cast<LPARAM>(k->flags));
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void OnNumLockKey(DWORD fgPid, DWORD flags) {
    std::wstring src;
    if (flags & LLKHF_INJECTED) {
        src = L"software-injected (SendInput/keybd_event";
        if (flags & LLKHF_LOWER_IL_INJECTED) src += L", from lower integrity level";
        src += L")";
    } else {
        src = L"physical keyboard (or driver)";
    }
    g_lastKeyDesc = L"NumLock keypress, " + src + L"; foreground app: " + ProcessName(fgPid);
    g_lastKeyTick = GetTickCount();
    g_haveKey = true;
    Log(L"KEY     " + g_lastKeyDesc);
    if (g_watchPids.count(fgPid)) Log(L"WATCHED pid " + std::to_wstring(fgPid) + L": " + ProcessDetails(fgPid));
}

// ---- window ----------------------------------------------------------------------------

// Draws a 32x32 icon at runtime: green with a ring when ON, red with a bar when OFF.
HICON MakeIcon(bool on) {
    constexpr int sz = 32;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, sz, sz);
    HGDIOBJ oldBmp = SelectObject(mem, color);

    RECT r = {0, 0, sz, sz};
    HBRUSH bg = CreateSolidBrush(on ? RGB(46, 160, 67) : RGB(200, 50, 50));
    FillRect(mem, &r, bg);
    DeleteObject(bg);

    HPEN pen = CreatePen(PS_SOLID, 4, RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(mem, pen);
    HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
    if (on) Ellipse(mem, 7, 7, 25, 25);
    else { MoveToEx(mem, 7, 16, nullptr); LineTo(mem, 25, 16); }
    SelectObject(mem, oldBrush);
    SelectObject(mem, oldPen);
    DeleteObject(pen);
    SelectObject(mem, oldBmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    BYTE maskBits[sz * sz / 8] = {};  // all zero = fully opaque
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, maskBits);
    ICONINFO ii = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void UpdateTitle(HWND hwnd) {
    SetWindowTextW(hwnd, g_numLockOn ? L"NumLock: ON" : L"NumLock: OFF");
    HICON icon = g_numLockOn ? g_iconOn : g_iconOff;
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
}

void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int h = rc.bottom - rc.top;

    HBRUSH bg = CreateSolidBrush(g_numLockOn ? RGB(46, 160, 67) : RGB(200, 50, 50));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    auto drawText = [&](const wchar_t* s, int fontH, RECT r) {
        HFONT font = CreateFontW(-fontH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, font);
        DrawTextW(dc, s, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(font);
    };
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    RECT top = rc;
    top.bottom = rc.top + h * 40 / 100;
    RECT mid = rc;
    mid.top = rc.top + h * 25 / 100;
    drawText(L"NUM", h * 20 / 100, top);
    drawText(g_numLockOn ? L"ON" : L"OFF", h * 36 / 100, mid);

    EndPaint(hwnd, &ps);
}

void ShowMenu(HWND hwnd, POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kIdOpenLog, L"Open log in Notepad");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kIdExit, L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_numLockOn = ReadNumLock();
        g_iconOn = MakeIcon(true);
        g_iconOff = MakeIcon(false);
        UpdateTitle(hwnd);
        Log(std::wstring(L"START   NumLock is ") + (g_numLockOn ? L"ON" : L"OFF"));
        g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
        if (!g_hook) Log(L"WARN    keyboard hook failed; changes can't be attributed to keystrokes");
        SetTimer(hwnd, kPollTimer, kPollMs, nullptr);
        return 0;
    case kKeyMsg:
        OnNumLockKey(static_cast<DWORD>(wp), static_cast<DWORD>(lp));
        return 0;
    case WM_TIMER:
        if (wp == kPollTimer) {
            const bool now = ReadNumLock();
            if (now != g_numLockOn) {
                g_numLockOn = now;
                UpdateTitle(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                std::wstring cause;
                if (g_haveKey && GetTickCount() - g_lastKeyTick <= kCorrelateMs) {
                    cause = L"caused by " + g_lastKeyDesc;
                } else {
                    cause = L"no NumLock keystroke seen: set programmatically (e.g. "
                            L"SetKeyboardState / keyboard LED API), by another session, or by "
                            L"a keystroke into an elevated window the hook can't see";
                }
                Log(std::wstring(L"CHANGE  NumLock -> ") + (now ? L"ON" : L"OFF") + L"  " + cause);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
        // Borderless: drag anywhere on the square. Kept out of WM_NCHITTEST so that
        // right-click stays a client click and produces WM_CONTEXTMENU.
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    case WM_CONTEXTMENU:
        ShowMenu(hwnd, POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == kIdOpenLog) {
            const std::wstring arg = L"\"" + g_logPath + L"\"";
            ShellExecuteW(hwnd, L"open", L"notepad.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        } else if (LOWORD(wp) == kIdExit) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_hook) UnhookWindowsHookEx(g_hook);
        KillTimer(hwnd, kPollTimer);
        DestroyIcon(g_iconOn);
        DestroyIcon(g_iconOff);
        Log(L"EXIT");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_logPath = exe;
    const size_t dot = g_logPath.find_last_of(L'.');
    g_logPath = (dot == std::wstring::npos ? g_logPath : g_logPath.substr(0, dot)) + L".log";

    // Usage: NumLockIndicator.exe [--pid N]...   (log full details when N is the foreground app)
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i) {
            if (wcscmp(argv[i], L"--pid") == 0) g_watchPids.insert(wcstoul(argv[++i], nullptr, 10));
        }
        LocalFree(argv);
    }

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    wc.lpszClassName = L"NumLockIndicatorWnd";
    if (!RegisterClassExW(&wc)) return 1;

    const int px = MulDiv(kSize, static_cast<int>(GetDpiForSystem()), 96);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, wc.lpszClassName, L"NumLock",
                                WS_POPUP | WS_BORDER | WS_SYSMENU, 100, 100, px, px,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return static_cast<int>(m.wParam);
}
