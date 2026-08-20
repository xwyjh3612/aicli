#include "app/powershell_runtime.h"

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace aicli::app {
namespace {

std::filesystem::path ExistingFile(const std::filesystem::path& value) {
    std::error_code error;
    if (!value.empty() && std::filesystem::is_regular_file(value, error)) {
        return value;
    }
    return {};
}

std::filesystem::path SearchPathFor(const wchar_t* name) {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = SearchPathW(
            nullptr, name, nullptr, static_cast<DWORD>(buffer.size()),
            buffer.data(), nullptr);
        if (!length) return {};
        if (length < buffer.size()) {
            return ExistingFile(std::filesystem::path(
                std::wstring(buffer.data(), static_cast<size_t>(length))));
        }
        buffer.resize(static_cast<size_t>(length) + 1);
    }
}

std::wstring EnvironmentValue(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (!required) return {};
    std::wstring value(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        name, value.data(), required);
    if (!length || length >= required) return {};
    value.resize(length);
    return value;
}

std::filesystem::path FindPowerShell7InRegistry(HKEY hive) {
    HKEY versions = nullptr;
    if (RegOpenKeyExW(
            hive,
            L"SOFTWARE\\Microsoft\\PowerShellCore\\InstalledVersions",
            0, KEY_READ | KEY_WOW64_64KEY, &versions) != ERROR_SUCCESS) {
        return {};
    }

    std::filesystem::path result;
    for (DWORD index = 0; result.empty(); ++index) {
        wchar_t subkey_name[256]{};
        DWORD subkey_length = static_cast<DWORD>(std::size(subkey_name));
        const LSTATUS enumerated = RegEnumKeyExW(
            versions, index, subkey_name, &subkey_length, nullptr, nullptr,
            nullptr, nullptr);
        if (enumerated == ERROR_NO_MORE_ITEMS) break;
        if (enumerated != ERROR_SUCCESS) continue;

        HKEY installation = nullptr;
        if (RegOpenKeyExW(versions, subkey_name, 0, KEY_READ,
                          &installation) != ERROR_SUCCESS) {
            continue;
        }
        DWORD bytes = 0;
        if (RegGetValueW(installation, nullptr, L"InstallLocation",
                         RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                         nullptr, nullptr, &bytes) == ERROR_SUCCESS &&
            bytes >= sizeof(wchar_t)) {
            std::wstring location(bytes / sizeof(wchar_t), L'\0');
            if (RegGetValueW(installation, nullptr, L"InstallLocation",
                             RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                             nullptr, location.data(), &bytes) == ERROR_SUCCESS) {
                while (!location.empty() && location.back() == L'\0') {
                    location.pop_back();
                }
                result = ExistingFile(
                    std::filesystem::path(location) / L"pwsh.exe");
            }
        }
        RegCloseKey(installation);
    }
    RegCloseKey(versions);
    return result;
}

std::filesystem::path FindPowerShell7InAppPaths(HKEY hive) {
    constexpr wchar_t kAppPath[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\pwsh.exe";
    DWORD bytes = 0;
    if (RegGetValueW(hive, kAppPath, nullptr,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) {
        return {};
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(hive, kAppPath, nullptr,
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return ExistingFile(value);
}

std::filesystem::path FindPowerShell7() {
    if (const auto path = SearchPathFor(L"pwsh.exe"); !path.empty()) {
        return path;
    }
    // Microsoft Store installs publish pwsh.exe through the per-user
    // App Paths key even when WindowsApps is absent from the process PATH.
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        if (const auto path = FindPowerShell7InAppPaths(hive); !path.empty()) {
            return path;
        }
    }
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        if (const auto path = FindPowerShell7InRegistry(hive); !path.empty()) {
            return path;
        }
    }

    std::vector<std::filesystem::path> roots;
    for (const wchar_t* variable : {L"ProgramW6432", L"ProgramFiles",
                                    L"LOCALAPPDATA"}) {
        const auto value = EnvironmentValue(variable);
        if (!value.empty()) roots.emplace_back(value);
    }

    // Keep the Store app-execution alias as a fallback for machines where
    // the App Paths registration is unavailable or stale.
    if (const auto local_app_data = EnvironmentValue(L"LOCALAPPDATA");
        !local_app_data.empty()) {
        const auto alias = ExistingFile(
            std::filesystem::path(local_app_data) / L"Microsoft" /
            L"WindowsApps" / L"pwsh.exe");
        if (!alias.empty()) return alias;
    }

    // Stable releases are preferred over preview installations. The user
    // profile location covers per-user PowerShell 7 installs.
    for (const auto& root : roots) {
        for (const auto& relative : {
                 std::filesystem::path(L"PowerShell") / L"7" / L"pwsh.exe",
                 std::filesystem::path(L"Microsoft") / L"PowerShell" / L"7" /
                     L"pwsh.exe",
                 std::filesystem::path(L"PowerShell") / L"7-preview" /
                     L"pwsh.exe"}) {
            if (const auto path = ExistingFile(root / relative); !path.empty()) {
                return path;
            }
        }
    }
    return {};
}

std::filesystem::path WindowsPowerShell5_1() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const UINT length = GetSystemDirectoryW(
            buffer.data(), static_cast<UINT>(buffer.size()));
        if (!length) return ExistingFile(L"powershell.exe");
        if (length < buffer.size()) {
            return ExistingFile(std::filesystem::path(
                std::wstring(buffer.data(), static_cast<size_t>(length))) /
                L"WindowsPowerShell" / L"v1.0" / L"powershell.exe");
        }
        buffer.resize(static_cast<size_t>(length) + 1);
    }
}

PowerShellRuntime Resolve() {
    if (const auto powershell7 = FindPowerShell7(); !powershell7.empty()) {
        return {powershell7, PowerShellKind::PowerShell7};
    }
    auto fallback = WindowsPowerShell5_1();
    if (fallback.empty()) fallback = L"powershell.exe";
    return {fallback, PowerShellKind::WindowsPowerShell5_1};
}

struct RuntimeState {
    RuntimeState() : active(Resolve()) {
        auto executable = WindowsPowerShell5_1();
        if (executable.empty()) executable = L"powershell.exe";
        fallback = {std::move(executable),
                    PowerShellKind::WindowsPowerShell5_1};
    }

    std::mutex mutex;
    PowerShellRuntime active;
    PowerShellRuntime fallback;
};

RuntimeState& State() {
    static RuntimeState state;
    return state;
}

} // namespace

PowerShellRuntime PowerShellRuntimeInfo() {
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    return state.active;
}

bool ActivateWindowsPowerShellFallback() {
    auto& state = State();
    std::scoped_lock lock(state.mutex);
    if (!state.active.IsPowerShell7()) return false;
    state.active = state.fallback;
    return true;
}

} // namespace aicli::app
