cmake_minimum_required(VERSION 3.15)
project(NumLockIndicator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Statically link the MSVC runtime (/MT, /MTd) so the exe has no VC++ redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(NumLockIndicator WIN32 main.cpp app.manifest)
target_compile_definitions(NumLockIndicator PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(NumLockIndicator PRIVATE /W4 /permissive-)
target_link_libraries(NumLockIndicator PRIVATE user32 gdi32 shell32)
