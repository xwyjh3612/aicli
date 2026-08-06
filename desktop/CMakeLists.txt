cmake_minimum_required(VERSION 3.26)
project(aicli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

add_executable(aicli WIN32
    src/app/main.cpp
    src/terminal/ansi_style_connection.cpp
    src/terminal/conpty_connection_bridge.cpp
)

target_compile_definitions(aicli PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)

target_include_directories(aicli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "F:/deps/cppwinrt-2.0.230207.1/sdkplus"
    "F:/deps/cppwinrt-2.0.230207.1/sdk"
    "F:/deps/cppwinrt-2.0.230207.1"
    "D:/Windows Kits/10/Include/10.0.22621.0/cppwinrt"
    "D:/Windows Kits/10/Include/10.0.22621.0/um"
    "D:/Windows Kits/10/Include/10.0.22621.0/shared"
    "F:/codex_tool-target/desktop/winrt-1.24.11911.0-complete"
)

target_link_libraries(aicli PRIVATE
    user32
    gdi32
    kernel32
    ole32
    runtimeobject
    dwmapi
)

if (MSVC)
    target_compile_options(aicli PRIVATE /W4 /EHsc /utf-8)
endif()
