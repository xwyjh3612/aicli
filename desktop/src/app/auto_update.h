#pragma once
#include <windows.h>
#include <string>

namespace aicli::update {
// All entry points run on the UI thread; network and hashing run on workers.
void Start(HWND main_window);
void Stop();
enum class CheckState { Idle, Checking, UpToDate, Available, Failed };
struct CheckStatus {
    CheckState state{CheckState::Idle};
    std::wstring text{L"检查是否有可用的新版本。"};
    bool operator==(const CheckStatus&) const = default;
};

// Checking never opens a window. The settings page reads the cached result;
// closing that page does not cancel the worker or leave an HWND callback behind.
void CheckForUpdates();
CheckStatus GetCheckStatus();
void DownloadAvailableUpdate(HWND owner);
}
