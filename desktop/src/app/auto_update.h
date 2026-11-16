#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <string_view>

namespace aicli::update {
// All entry points run on the UI thread; network and hashing run on workers.
struct InstallCallbacks {
    std::function<bool()> can_install;
    std::function<bool(std::wstring_view)> prepare_restart;
    std::function<void()> cancel_restart;
};
void Start(HWND main_window, InstallCallbacks callbacks = {});
void Stop();
enum class CheckState {
    Idle, Checking, UpToDate, Available, Failed,
    Downloading, WaitingToInstall, Installing, DownloadFailed, InstallFailed
};
struct CheckStatus {
    CheckState state{CheckState::Idle};
    std::wstring text{L"检查是否有可用的新版本。"};
    bool update_available{};
    int progress{};
    bool operator==(const CheckStatus&) const = default;
};

// Checking never opens a window. The settings page reads the cached result;
// closing that page does not cancel the worker or leave an HWND callback behind.
void CheckForUpdates();
CheckStatus GetCheckStatus();
bool HasAvailableUpdate();
bool IsInstalling();
void DownloadAvailableUpdate(HWND owner);
void CancelDownloadOrInstall();
inline constexpr UINT kStatusChangedMessage = WM_APP + 70;
}
