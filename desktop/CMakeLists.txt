cmake_minimum_required(VERSION 3.26)
project(aicli LANGUAGES C CXX RC)

option(AICLI_SKIP_PACKAGED_ACTIVATION "Run the desktop executable directly for local UI smoke tests" OFF)
option(AICLI_USE_QT_UI "Build the Qt desktop UI instead of the legacy Win32 UI" ON)
set(AICLI_DATA_DIRECTORY_OVERRIDE "" CACHE STRING
    "Optional fixed data directory for isolated desktop integration tests")
set(AICLI_APPLICATION_USER_MODEL_ID "AiCli.Desktop.Dev_jw3zr00rmb0ym!App"
    CACHE STRING "Application user model ID used for packaged activation")
set(AICLI_PACKAGE_IDENTITY_NAME "AiCli.Desktop.Dev"
    CACHE STRING "Identity Name written to the staged AppxManifest.xml")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ${AICLI_USE_QT_UI})
if (NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
endif()
if (NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
endif()
if (NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
endif()

if (AICLI_USE_QT_UI)
    if (EXISTS "F:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6")
        list(PREPEND CMAKE_PREFIX_PATH "F:/Qt/6.8.3/msvc2022_64")
    endif()
    find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Test)
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/build_identity.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/aicli_build_identity.h"
    @ONLY
)

set(AICLI_COMMON_SOURCES
    src/agent/conversation_bridge.cpp
    src/agent/remote_agent.cpp
    src/agent/terminal_snapshot_publisher.cpp
    src/agent/terminal_stream_publisher.cpp
    src/agent/terminal_wire_codec.cpp
    src/app/application_activation.cpp
    src/app/project_launcher.cpp
    src/app/runtime_components.cpp
    src/app/single_instance.cpp
    src/core/process_metrics.cpp
    src/core/text_utils.cpp
    src/data/data_store.cpp
    src/features/git_repository.cpp
    "F:/deps/aicli/civetweb-1.16/src/civetweb.c"
    "F:/deps/aicli/qrcodegen/qrcodegen.cpp"
    src/terminal/conpty_connection_bridge.cpp
    src/terminal/terminal_working_detector.cpp
    src/terminal/xaml_island_adapter.cpp
    src/ui/theme.cpp
    src/ui/taskbar_working_indicator.cpp
    src/app/aicli.rc
)

if (AICLI_USE_QT_UI)
    list(APPEND AICLI_COMMON_SOURCES
        src/app/desktop_controller.cpp
        src/app/qt_main.cpp
        src/ui/qt_binding_dialog.cpp
        src/ui/qt_main_window.cpp
        src/ui/qt_project_dialog.cpp
        src/ui/qt_settings_dialog.cpp
        src/ui/qt_widgets.cpp
    )
    set(AICLI_APPLICATION_LOGO
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/aicli-logo.png")
else()
    list(APPEND AICLI_COMMON_SOURCES
        src/app/main.cpp
        src/ui/drawing_utils.cpp
        src/ui/footer_status.cpp
        src/ui/main_window_renderer.cpp
        src/ui/styled_input.cpp
        src/ui/management_dialog.cpp
        src/ui/window_chrome.cpp
    )
    set(AICLI_APPLICATION_LOGO
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/aicli-logo.png")
endif()

add_executable(aicli WIN32 ${AICLI_COMMON_SOURCES})

add_executable(aicli_data_store_tests
    tests/data_store_tests.cpp
    src/data/data_store.cpp
)

add_executable(aicli_terminal_stream_tests
    tests/terminal_stream_publisher_tests.cpp
    src/agent/terminal_stream_publisher.cpp
    src/agent/terminal_wire_codec.cpp
    src/core/text_utils.cpp
)

if (AICLI_USE_QT_UI)
    add_executable(aicli_qt_project_list_tests
        tests/qt_project_list_tests.cpp
        src/ui/qt_widgets.cpp
    )
endif()

target_compile_definitions(aicli PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN USE_WEBSOCKET NO_SSL_DL OPENSSL_API_3_0
    $<$<CONFIG:Debug>:AICLI_DEBUG_BUILD=1>
)
if (AICLI_USE_QT_UI)
    target_compile_definitions(aicli PRIVATE AICLI_QT_UI_BUILD=1)
endif()
if (AICLI_DATA_DIRECTORY_OVERRIDE)
    file(TO_CMAKE_PATH "${AICLI_DATA_DIRECTORY_OVERRIDE}" AICLI_DATA_DIRECTORY_OVERRIDE_NORMALIZED)
    target_compile_definitions(aicli PRIVATE
        "AICLI_DATA_DIRECTORY_OVERRIDE=L\"${AICLI_DATA_DIRECTORY_OVERRIDE_NORMALIZED}\"")
endif()
if (AICLI_SKIP_PACKAGED_ACTIVATION)
    target_compile_definitions(aicli PRIVATE AICLI_SKIP_PACKAGED_ACTIVATION)
endif()

target_compile_definitions(aicli_data_store_tests PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)

target_compile_definitions(aicli_terminal_stream_tests PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)

if (AICLI_USE_QT_UI)
    target_compile_definitions(aicli_qt_project_list_tests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    )
endif()

set_source_files_properties("F:/deps/aicli/civetweb-1.16/src/civetweb.c" PROPERTIES LANGUAGE C)

target_include_directories(aicli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
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

target_include_directories(aicli_terminal_stream_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "F:/deps/aicli"
)

if (AICLI_USE_QT_UI)
    target_include_directories(aicli_qt_project_list_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
endif()

target_link_libraries(aicli PRIVATE
    comctl32
    user32
    gdi32
    gdiplus
    kernel32
    imm32
    ole32
    runtimeobject
    dwmapi
    shell32
    winsqlite3
    ws2_32
    advapi32
    userenv
    crypt32
    psapi
    "F:/deps/aicli/Microsoft.Windows.Console.ConPTY.1.24.260710001/package/runtimes/win-x64/lib/uap10.0/conpty.lib"
    "$<$<CONFIG:Debug>:F:/deps/aicli/openssl/lib/VC/x64/MDd/libssl.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:F:/deps/aicli/openssl/lib/VC/x64/MD/libssl.lib>"
    "$<$<CONFIG:Debug>:F:/deps/aicli/openssl/lib/VC/x64/MDd/libcrypto.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:F:/deps/aicli/openssl/lib/VC/x64/MD/libcrypto.lib>"
)

if (AICLI_USE_QT_UI)
    target_link_libraries(aicli PRIVATE Qt6::Widgets)
    target_link_libraries(aicli_qt_project_list_tests PRIVATE
        Qt6::Widgets
        Qt6::Test
    )
endif()

if (AICLI_USE_QT_UI)
    add_custom_command(TARGET aicli POST_BUILD
        COMMAND "F:/Qt/6.8.3/msvc2022_64/bin/windeployqt.exe"
            "$<$<CONFIG:Debug>:--debug>"
            "$<$<NOT:$<CONFIG:Debug>>:--release>"
            --no-translations
            --no-system-d3d-compiler
            --no-opengl-sw
            --no-compiler-runtime
            "$<TARGET_FILE:aicli>"
    )
endif()

add_custom_command(TARGET aicli POST_BUILD
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/stage-runtime.ps1"
        -Output "$<TARGET_FILE_DIR:aicli>"
        -Configuration "$<CONFIG>"
        -Platform x64
        -PackageIdentityName "${AICLI_PACKAGE_IDENTITY_NAME}"
        -ApplicationLogo "${AICLI_APPLICATION_LOGO}"
)

target_link_libraries(aicli_data_store_tests PRIVATE
    winsqlite3
    ole32
    shell32
)

target_link_libraries(aicli_terminal_stream_tests PRIVATE
    ole32
)

if (MSVC)
    # Merge the Windows 10/11 compatibility declaration into the executable's
    # real RT_MANIFEST resource. A manifest embedded from the .rc file is
    # overwritten by link.exe's generated UAC manifest, which leaves loose Qt
    # builds without MaxVersionTested and makes WindowsXamlManager fail with
    # 0x8000FFFF before the first window is created.
    target_link_options(aicli PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app/aicli.manifest"
    )
    target_compile_options(aicli PRIVATE /W4 /EHsc /utf-8 /FS)
    target_compile_options(aicli_data_store_tests PRIVATE /W4 /EHsc /utf-8 /FS)
    target_compile_options(aicli_terminal_stream_tests PRIVATE /W4 /EHsc /utf-8 /FS)
    if (AICLI_USE_QT_UI)
        target_compile_options(aicli_qt_project_list_tests PRIVATE /W4 /EHsc /utf-8 /FS)
    endif()
endif()
