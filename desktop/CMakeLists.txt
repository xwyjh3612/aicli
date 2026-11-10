cmake_minimum_required(VERSION 3.26)
project(aicli LANGUAGES C CXX RC)

if (CMAKE_BINARY_DIR MATCHES "^[Ff]:")
    message(FATAL_ERROR "F: is reserved for shared dependencies. Configure the build directory on C:.")
endif()

set(AICLI_DEPENDENCY_MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../dependencies.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AICLI_DEPENDENCY_MANIFEST_PATH}")
file(READ "${AICLI_DEPENDENCY_MANIFEST_PATH}" AICLI_DEPENDENCY_MANIFEST)
function(aicli_shared_dependency name output)
    string(JSON root_name GET "${AICLI_DEPENDENCY_MANIFEST}" packages "${name}" root)
    string(JSON relative_path GET "${AICLI_DEPENDENCY_MANIFEST}" packages "${name}" path)
    string(TOUPPER "${root_name}" root_variable)
    if (DEFINED ENV{AICLI_${root_variable}_ROOT} AND NOT "$ENV{AICLI_${root_variable}_ROOT}" STREQUAL "")
        set(root_path "$ENV{AICLI_${root_variable}_ROOT}")
    else()
        string(JSON root_path GET "${AICLI_DEPENDENCY_MANIFEST}" roots "${root_name}")
    endif()
    file(TO_CMAKE_PATH "${root_path}/${relative_path}" dependency_path)
    set(${output} "${dependency_path}" PARENT_SCOPE)
endfunction()

aicli_shared_dependency(civetweb AICLI_CIVETWEB_ROOT)
aicli_shared_dependency(qrcodegen AICLI_QRCODEGEN_ROOT)
aicli_shared_dependency(nlohmann AICLI_NLOHMANN_ROOT)
aicli_shared_dependency(openssl AICLI_OPENSSL_ROOT)
aicli_shared_dependency(conpty AICLI_CONPTY_ROOT)
aicli_shared_dependency(cppwinrt AICLI_CPPWINRT_ROOT)
aicli_shared_dependency(terminalWinrt AICLI_TERMINAL_WINRT_ROOT)
aicli_shared_dependency(qt AICLI_QT_ROOT)

option(AICLI_SKIP_PACKAGED_ACTIVATION "Run the desktop executable directly for local UI smoke tests" OFF)
option(AICLI_USE_QT_UI "Build the Qt desktop UI instead of the legacy Win32 UI" ON)
set(AICLI_DATA_DIRECTORY_OVERRIDE "" CACHE STRING
    "Optional fixed data directory for isolated desktop integration tests")
set(AICLI_APPLICATION_USER_MODEL_ID "AiCli.Desktop.Dev_jw3zr00rmb0ym!App"
    CACHE STRING "Application user model ID used for packaged activation")
set(AICLI_PACKAGE_IDENTITY_NAME "AiCli.Desktop.Dev"
    CACHE STRING "Identity Name written to the staged AppxManifest.xml")

# AppxManifest.xml is the release-version source of truth. Generate the UI
# constant from it so About and packaged/installer builds cannot drift.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/package/AppxManifest.xml"
    AICLI_PACKAGE_MANIFEST_TEXT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/package/AppxManifest.xml")
string(REGEX MATCH
    "<Identity[^>]*Version=\"([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)\""
    AICLI_VERSION_MATCH "${AICLI_PACKAGE_MANIFEST_TEXT}")
if (NOT AICLI_VERSION_MATCH)
    message(FATAL_ERROR "AppxManifest.xml does not contain a valid package version")
endif()
set(AICLI_VERSION "${CMAKE_MATCH_1}")

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
    if (EXISTS "${AICLI_QT_ROOT}/lib/cmake/Qt6")
        list(PREPEND CMAKE_PREFIX_PATH "${AICLI_QT_ROOT}")
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
    src/app/powershell_runtime.cpp
    src/app/runtime_components.cpp
    src/app/single_instance.cpp
    src/core/process_metrics.cpp
    src/core/text_utils.cpp
    src/data/data_store.cpp
    src/features/git_repository.cpp
    src/features/mobile_project_creation.cpp
    "${AICLI_CIVETWEB_ROOT}/src/civetweb.c"
    "${AICLI_QRCODEGEN_ROOT}/qrcodegen.cpp"
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
        src/app/auto_update.cpp
        src/app/update_manifest.cpp
        src/app/main.cpp
        src/ui/drawing_utils.cpp
        src/ui/footer_status.cpp
        src/ui/main_window_renderer.cpp
        src/ui/overlay_scrollbar.cpp
        src/ui/project_list_view.cpp
        src/ui/rounded_context_menu.cpp
        src/ui/rounded_select_popup.cpp
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

add_executable(aicli_terminal_working_detector_tests
    tests/terminal_working_detector_tests.cpp
    src/terminal/terminal_working_detector.cpp
)

add_executable(aicli_powershell_runtime_tests
    tests/powershell_runtime_tests.cpp
    src/app/powershell_runtime.cpp
)

add_executable(aicli_update_manifest_tests
    tests/update_manifest_tests.cpp
    src/app/update_manifest.cpp
)
target_include_directories(aicli_update_manifest_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${AICLI_NLOHMANN_ROOT}")
if (MSVC)
    target_compile_options(aicli_update_manifest_tests PRIVATE /W4 /EHsc /utf-8)
endif()

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

target_compile_definitions(aicli_terminal_working_detector_tests PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)

target_compile_definitions(aicli_powershell_runtime_tests PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)

if (AICLI_USE_QT_UI)
    target_compile_definitions(aicli_qt_project_list_tests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    )
endif()

set_source_files_properties("${AICLI_CIVETWEB_ROOT}/src/civetweb.c" PROPERTIES LANGUAGE C)

target_include_directories(aicli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${AICLI_CONPTY_ROOT}/inc"
    "${AICLI_CPPWINRT_ROOT}/sdkplus"
    "${AICLI_CPPWINRT_ROOT}/sdk"
    "${AICLI_CPPWINRT_ROOT}"
    "D:/Windows Kits/10/Include/10.0.22621.0/cppwinrt"
    "D:/Windows Kits/10/Include/10.0.22621.0/um"
    "D:/Windows Kits/10/Include/10.0.22621.0/shared"
    "${AICLI_TERMINAL_WINRT_ROOT}"
    "${AICLI_CIVETWEB_ROOT}/include"
    "${AICLI_NLOHMANN_ROOT}"
    "${AICLI_QRCODEGEN_ROOT}"
    "${AICLI_OPENSSL_ROOT}/include"
)

target_include_directories(aicli_data_store_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_include_directories(aicli_terminal_stream_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${AICLI_NLOHMANN_ROOT}"
)

target_include_directories(aicli_terminal_working_detector_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_include_directories(aicli_powershell_runtime_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
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
    "${AICLI_CONPTY_ROOT}/runtimes/win-x64/lib/uap10.0/conpty.lib"
    "$<$<CONFIG:Debug>:${AICLI_OPENSSL_ROOT}/lib/VC/x64/MDd/libssl.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:${AICLI_OPENSSL_ROOT}/lib/VC/x64/MD/libssl.lib>"
    "$<$<CONFIG:Debug>:${AICLI_OPENSSL_ROOT}/lib/VC/x64/MDd/libcrypto.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:${AICLI_OPENSSL_ROOT}/lib/VC/x64/MD/libcrypto.lib>"
)

if (NOT AICLI_USE_QT_UI)
    target_link_libraries(aicli PRIVATE winhttp bcrypt)
endif()

if (AICLI_USE_QT_UI)
    target_link_libraries(aicli PRIVATE Qt6::Widgets)
    target_link_libraries(aicli_qt_project_list_tests PRIVATE
        Qt6::Widgets
        Qt6::Test
    )
endif()

if (AICLI_USE_QT_UI)
    add_custom_command(TARGET aicli POST_BUILD
        COMMAND "${AICLI_QT_ROOT}/bin/windeployqt.exe"
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

target_link_libraries(aicli_powershell_runtime_tests PRIVATE
    advapi32
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
    target_compile_options(aicli_terminal_working_detector_tests PRIVATE
        /W4 /EHsc /utf-8 /FS)
    target_compile_options(aicli_powershell_runtime_tests PRIVATE
        /W4 /EHsc /utf-8 /FS)
    if (AICLI_USE_QT_UI)
        target_compile_options(aicli_qt_project_list_tests PRIVATE /W4 /EHsc /utf-8 /FS)
    endif()
endif()
