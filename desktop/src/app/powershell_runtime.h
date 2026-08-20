#pragma once

#include <filesystem>
#include <string>

namespace aicli::app {

enum class PowerShellKind {
    PowerShell7,
    WindowsPowerShell5_1,
};

struct PowerShellRuntime {
    std::filesystem::path executable;
    PowerShellKind kind{PowerShellKind::WindowsPowerShell5_1};

    bool IsPowerShell7() const noexcept {
        return kind == PowerShellKind::PowerShell7;
    }

    std::wstring DisplayName() const {
        return IsPowerShell7() ? L"PowerShell 7" :
                                 L"Windows PowerShell 5.1";
    }

    std::wstring CommandLine() const {
        return L"\"" + executable.wstring() + L"\" -NoLogo -NoExit";
    }
};

// Resolves the interactive shell once for the lifetime of the process. The
// resolver prefers pwsh.exe (including MSI and Microsoft Store installs) and
// falls back to the inbox Windows PowerShell 5.1 executable when unavailable.
PowerShellRuntime PowerShellRuntimeInfo();

// Switches the process-wide interactive shell to Windows PowerShell 5.1 after
// a discovered pwsh.exe fails to launch. Future terminals and About use the
// fallback result as well.
bool ActivateWindowsPowerShellFallback();

} // namespace aicli::app
