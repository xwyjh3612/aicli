#include "app/auto_update.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
unsigned unexpected_windows{};
LRESULT CALLBACK RejectDialogs(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HCBT_CREATEWND) { ++unexpected_windows; return 1; }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

// Live integration check against the public update endpoint. No visible UI or
// downloads/installations: even an accidental modal window is rejected by CBT.
int main() {
    HWND owner = CreateWindowW(L"STATIC", L"Update check test", WS_OVERLAPPED,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) return 1;
    aicli::update::Start(owner);
    EnableWindow(owner, FALSE); // The main owner is disabled while Settings is open.
    HHOOK hook = SetWindowsHookExW(WH_CBT, RejectDialogs, nullptr, GetCurrentThreadId());
    int result = 0;
    try {
        Require(hook != nullptr, "Could not guard against unexpected dialogs");
        const auto start = std::chrono::steady_clock::now();
        aicli::update::CheckForUpdates();
        aicli::update::CheckForUpdates(); // A repeated click must share the pending check.
        Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
            "Manual check blocked the UI thread");
        Require(aicli::update::GetCheckStatus().state == aicli::update::CheckState::Checking,
            "Checking state must be available immediately");
        const auto deadline = start + std::chrono::seconds(60);
        while (aicli::update::GetCheckStatus().state == aicli::update::CheckState::Checking &&
               std::chrono::steady_clock::now() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        }
        const auto status = aicli::update::GetCheckStatus();
        Require(status.state == aicli::update::CheckState::UpToDate ||
                status.state == aicli::update::CheckState::Available,
            "Public release check did not complete successfully");
        Require(!status.text.empty(), "Settings must have an inline result to display");
        Require(unexpected_windows == 0, "Checking opened an unexpected window");
        std::cout << "Manual update check completed without blocking or dialogs\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (hook) UnhookWindowsHookEx(hook);
    aicli::update::Stop();
    DestroyWindow(owner);
    return result;
}
