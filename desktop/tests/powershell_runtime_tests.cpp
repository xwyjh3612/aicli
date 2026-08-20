#include "app/powershell_runtime.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::filesystem::path RegisteredPowerShell7() {
    constexpr wchar_t kAppPath[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\pwsh.exe";
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        DWORD bytes = 0;
        if (RegGetValueW(hive, kAppPath, nullptr,
                         RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                         nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
            bytes < sizeof(wchar_t)) {
            continue;
        }
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(hive, kAppPath, nullptr,
                         RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                         nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
            continue;
        }
        while (!value.empty() && value.back() == L'\0') value.pop_back();
        std::error_code error;
        if (std::filesystem::is_regular_file(value, error)) return value;
    }
    return {};
}

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

} // namespace

int wmain() {
    const auto runtime = aicli::app::PowerShellRuntimeInfo();
    bool ok = Check(!runtime.executable.empty(),
                    "PowerShell runtime path is not empty");
    std::error_code error;
    ok &= Check(std::filesystem::is_regular_file(runtime.executable, error),
                "selected PowerShell runtime exists");

    const auto registered = RegisteredPowerShell7();
    if (!registered.empty()) {
        ok &= Check(runtime.IsPowerShell7(),
                    "App Paths PowerShell 7 installation is selected");
    }

    std::wcout << L"Selected runtime: " << runtime.DisplayName() << L"\n"
               << L"Executable: " << runtime.executable.wstring() << L"\n";
    if (ok) std::cout << "AI CLI PowerShell runtime tests passed.\n";
    return ok ? 0 : 1;
}
