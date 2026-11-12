# NumLockIndicator

[![Build](https://github.com/bthalheim/numlock-monitor/actions/workflows/build.yml/badge.svg)](https://github.com/bthalheim/numlock-monitor/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A tiny always-on-top Windows indicator that shows the live NumLock state, and logs
**what changed it**. Useful for tracking down software that toggles NumLock behind your back.

- Green square = NumLock ON, red square = NumLock OFF (the taskbar icon and title follow too).
- Every NumLock keypress and every state change is written to a log file next to the exe.
- Each change is attributed: physical keyboard, software-injected (`SendInput`/`keybd_event`),
  or set programmatically with no keystroke at all.
- Single static executable, no installer, no runtime dependencies.

## Usage

Download `NumLockIndicator.exe` from the [Releases](../../releases) page and run it.

| Action | Result |
| --- | --- |
| Drag the square | Move it |
| Right-click | Open the log in Notepad, or Exit |

### Watching a specific process

```
NumLockIndicator.exe --pid 1234 [--pid 5678 ...]
```

When a NumLock keypress happens while a watched PID owns the foreground window, an extra
line is logged with that process's path, command line, start time, session and parent.

> **Limitation:** a low-level keyboard hook cannot tell which process *sent* a keystroke.
> The only PID available is the foreground window's. A background process injecting NumLock
> while another app has focus will not match `--pid`.

## Log format

The log is `NumLockIndicator.log` beside the exe, UTF-8 with BOM:

```
2026-10-27 09:13:02.114  KEY     NumLock keypress, software-injected (SendInput/keybd_event); foreground app: foo.exe (pid 1234)
2026-10-27 09:13:02.148  WATCHED pid 1234: path=C:\...\foo.exe; cmdline=...; started=...; session=1; parent=bar.exe (pid 99)
2026-10-27 09:13:02.148  CHANGE  NumLock -> ON  caused by NumLock keypress, software-injected ...
```

Keystrokes into elevated windows are invisible to the hook; such changes are logged as
"no NumLock keystroke seen".

## Building

Requires Windows, CMake 3.15+ and MSVC (Visual Studio or Build Tools).

```
cmake -S . -B build
cmake --build build --config Release
```

The result is `build/Release/NumLockIndicator.exe` (or `build/NumLockIndicator.exe` with
single-config generators like Ninja).

## Releases

Pushing a tag like `v1.0.0` builds the project on GitHub Actions and publishes a release
with a zip containing the exe, README and license:

```
git tag v1.0.0
git push origin v1.0.0
```

## License

[MIT](LICENSE)
