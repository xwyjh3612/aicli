cmake_minimum_required(VERSION 3.26)
project(aicli LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

add_executable(aicli WIN32
    src/agent/conversation_bridge.cpp
    src/agent/remote_agent.cpp
    src/agent/terminal_snapshot_publisher.cpp
    src/app/application_activation.cpp
    src/app/main.cpp
    src/app/runtime_components.cpp
    src/core/process_metrics.cpp
    src/core/text_utils.cpp
    src/data/data_store.cpp
    src/features/git_repository.cpp
    "F:/deps/aicli/civetweb-1.16/src/civetweb.c"
    "F:/deps/aicli/qrcodegen/qrcodegen.cpp"
    src/terminal/conpty_connection_bridge.cpp
    src/terminal/xaml_island_adapter.cpp
    src/ui/drawing_utils.cpp
    src/ui/footer_status.cpp
    src/ui/main_window_renderer.cpp
    src/ui/theme.cpp
    src/ui/styled_input.cpp
    src/ui/management_dialog.cpp
    src/ui/window_chrome.cpp
)

add_executable(aicli_data_store_tests
    tests/data_store_tests.cpp
    src/data/data_store.cpp
)

target_compile_definitions(aicli PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN USE_WEBSOCKET NO_SSL_DL OPENSSL_API_3_0
)

target_compile_definitions(aicli_data_store_tests PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)

set_source_files_properties("F:/deps/aicli/civetweb-1.16/src/civetweb.c" PROPERTIES LANGUAGE C)

target_include_directories(aicli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "F:/deps/aicli/Microsoft.Windows.Console.ConPTY.1.24.260710001/package/inc"
    "F:/deps/cppwinrt-2.0.230207.1/sdkplus"
    "F:/deps/cppwinrt-2.0.230207.1/sdk"
    "F:/deps/cppwinrt-2.0.230207.1"
    "D:/Windows Kits/10/Include/10.0.22621.0/cppwinrt"
    "D:/Windows Kits/10/Include/10.0.22621.0/um"
    "D:/Windows Kits/10/Include/10.0.22621.0/shared"
    "F:/codex_tool-target/desktop/winrt-1.24.11911.0-complete"
    "F:/deps/aicli/civetweb-1.16/include"
    "F:/deps/aicli"
    "F:/deps/aicli/qrcodegen"
    "F:/deps/aicli/openssl/include"
)

target_include_directories(aicli_data_store_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(aicli PRIVATE
    user32
    gdi32
    gdiplus
    kernel32
    ole32
    runtimeobject
    dwmapi
    winsqlite3
    ws2_32
    advapi32
    crypt32
    psapi
    "F:/deps/aicli/Microsoft.Windows.Console.ConPTY.1.24.260710001/package/runtimes/win-x64/lib/uap10.0/conpty.lib"
    "$<$<CONFIG:Debug>:F:/deps/aicli/openssl/lib/VC/x64/MDd/libssl.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:F:/deps/aicli/openssl/lib/VC/x64/MD/libssl.lib>"
    "$<$<CONFIG:Debug>:F:/deps/aicli/openssl/lib/VC/x64/MDd/libcrypto.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:F:/deps/aicli/openssl/lib/VC/x64/MD/libcrypto.lib>"
)

# Use the same side-by-side ConPTY/OpenConsole build as Windows Terminal
# 1.24. The x64 host is deliberately placed in the architecture subfolder
# expected by Microsoft's conpty.dll package.
add_custom_command(TARGET aicli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "F:/deps/aicli/Microsoft.Windows.Console.ConPTY.1.24.260710001/package/runtimes/win-x64/native/conpty.dll"
        "$<TARGET_FILE_DIR:aicli>/conpty.dll"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:aicli>/x64"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "F:/deps/aicli/Microsoft.Windows.Console.ConPTY.1.24.260710001/package/build/native/runtimes/x64/OpenConsole.exe"
        "$<TARGET_FILE_DIR:aicli>/x64/OpenConsole.exe"
)

target_link_libraries(aicli_data_store_tests PRIVATE
    winsqlite3
    ole32
    shell32
)

if (MSVC)
    target_compile_options(aicli PRIVATE /W4 /EHsc /utf-8)
    target_compile_options(aicli_data_store_tests PRIVATE /W4 /EHsc /utf-8)
endif()
