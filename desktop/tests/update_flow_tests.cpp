#include "app/auto_update_testing.h"
#include "app/update_restart.h"
#include "ui/update_badge.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace aicli::update;
unsigned unexpected_windows{};
LRESULT CALLBACK RejectDialogs(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_CREATEWND) { ++unexpected_windows; return 1; }
    return CallNextHookEx(nullptr, code, w, l);
}
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Pump() {
    testing::Poll();
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
}
template<class Condition> void Until(Condition condition) {
    const auto deadline = GetTickCount64() + 10000;
    while (!condition() && GetTickCount64() < deadline) {
        Pump();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    Require(condition(), "Update state did not complete");
}

void TestRestart(const std::filesystem::path& root) {
    aicli::DataStore store;
    Require(store.Initialize((root / L"restart.db").wstring()), "create isolated database");
    const auto types = store.LoadProjectTypes();
    const auto groups = store.LoadGroups();
    std::vector<aicli::Project> projects(4);
    for (size_t index = 0; index < projects.size(); ++index) {
        auto& project = projects[index];
        project.name = L"项目 " + std::to_wstring(index);
        project.type = types.front().name;
        project.group = groups.front().name;
        project.folder = root.wstring();
        project.command = L"codex --no-alt-screen resume --last";
        Require(store.SaveProject(project), "save isolated project");
    }
    RestartPlan saved{{projects[0].id, projects[2].id, projects[0].id, L"deleted-id"},
                      projects[2].id, L"独立分组"};
    Require(SaveRestartPlan(store, L"1.0.0.9", saved), "checkpoint running project IDs");
    Require(!TakeRestartPlan(store, L"1.0.0.8"), "old version must not consume update checkpoint");
    const auto restored = TakeRestartPlan(store, L"1.0.0.9");
    Require(restored && restored->projects == std::vector<std::wstring>{projects[0].id, projects[2].id}
            && restored->selected == projects[2].id && restored->group == L"独立分组",
            "restore all started projects once, with selection and group; exclude missing and duplicate IDs");
    Require(!TakeRestartPlan(store, L"1.0.0.9"), "checkpoint is consumed exactly once");
    Require(SaveRestartPlan(store, L"1.0.0.9", saved), "save cancellation fixture");
    CancelRestartPlan(store);
    Require(!TakeRestartPlan(store, L"1.0.0.9"), "cancelled installation does not restore projects");
    Require(SaveRestartPlan(store, L"1.0.0.9", saved), "save upgrade fixture");
    Require(store.DeleteProject(projects[0].id), "delete a checkpointed project");
    Require(TakeRestartPlan(store, L"1.0.0.10")->projects == std::vector<std::wstring>{projects[2].id},
            "newer version can consume checkpoint without recreating deleted projects");
    store.SetSetting(L"update_restart_plan", L"invalid-json");
    Require(!TakeRestartPlan(store, L"1.0.0.9"), "invalid checkpoint is ignored safely");
    Require(store.LoadProjects().size() == 3, "restart never edits unrelated project data");
}

void TestDots() {
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 200;
    info.bmiHeader.biHeight = -80;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels{};
    const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto old = SelectObject(dc, bitmap);
    for (UINT dpi : {96u, 144u, 192u}) {
        RECT rect{0, 0, 200, 80};
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        aicli::ui::DrawUpdateBadge(dc, rect, dpi);
        int segments{}, previous{};
        for (int x = 0; x < 200; ++x) {
            int red{};
            for (int y = 0; y < 80; ++y) {
                const auto color = GetPixel(dc, x, y);
                if (GetRValue(color) > 180 && GetGValue(color) < 100) red = 1;
            }
            if (red && !previous) ++segments;
            previous = red;
        }
        Require(segments == 1, "update badge shows exactly one red dot at each DPI");
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
}
}

int main() {
    HWND owner{};
    HHOOK hook{};
    std::filesystem::path root;
    try {
        aicli::ui::GdiplusRuntime drawing;
        auto temp = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
        if (temp.filename().empty()) temp = temp.parent_path();
        root = temp / (L"aicli-update-flow-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64()));
        Require(root.parent_path() == temp, "test data remains in temporary workspace");
        std::filesystem::create_directories(root);
        TestRestart(root);
        TestDots();
        owner = CreateWindowW(L"STATIC", L"Update flow test", WS_OVERLAPPED,
            -30000, -30000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        bool working = true, launch_ok = false, prepare_ok = true;
        unsigned prepares{}, cancellations{}, launches{};
        std::wstring parameters;
        Start(owner, {[&]{return !working;}, [&](std::wstring_view){++prepares; return prepare_ok;},
                      [&]{++cancellations;}});
        EnableWindow(owner, FALSE); // Settings keeps the main window disabled.
        hook = SetWindowsHookExW(WH_CBT, RejectDialogs, nullptr, GetCurrentThreadId());
        Require(hook != nullptr, "guard against modal windows");
        testing::Services services;
        services.check = []{return Release{"65535.1.1", "", "", 1};};
        services.download = [&](std::atomic<bool>&, std::atomic<int>& progress){progress = 73; return root / L"verified.exe";};
        services.launch = [&](const auto&, const std::wstring& value){++launches; parameters = value; return launch_ok;};
        testing::SetServices(services);
        testing::BeginAutomaticCheck();
        Until([]{return HasAvailableUpdate();});
        Require(GetCheckStatus().state == CheckState::Available && GetCheckStatus().update_available,
                "automatic check caches available release even with settings open");
        CheckForUpdates();
        Require(HasAvailableUpdate(), "manual recheck does not clear unread update badge");
        Until([]{return GetCheckStatus().state != CheckState::Checking;});
        services.check = []()->Release{throw std::runtime_error("network unavailable");};
        testing::SetServices(services);
        CheckForUpdates();
        Until([]{return GetCheckStatus().state == CheckState::Failed;});
        Require(HasAvailableUpdate() && GetCheckStatus().update_available,
                "failed recheck preserves previously discovered update and red dots");
        const auto start = GetTickCount64();
        DownloadAvailableUpdate(owner);
        Require(GetTickCount64() - start < 500, "download starts without blocking or dialogs");
        Until([]{return GetCheckStatus().state == CheckState::WaitingToInstall;});
        Pump();
        Require(launches == 0 && prepares == 0, "working tasks defer installation and checkpoint");
        CancelDownloadOrInstall();
        Require(GetCheckStatus().state == CheckState::Available && HasAvailableUpdate(),
                "cancel waiting install preserves red dots and running tasks");

        services.download = [&](std::atomic<bool>& cancel, std::atomic<int>& progress){
            progress = 42;
            while (!cancel) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return std::filesystem::path{};
        };
        testing::SetServices(services);
        DownloadAvailableUpdate(owner);
        Until([]{return GetCheckStatus().progress == 42;});
        CancelDownloadOrInstall();
        Until([]{return GetCheckStatus().state == CheckState::Available;});
        Require(launches == 0, "cancelled download never launches installer");

        services.download = [](auto&, auto&)->std::filesystem::path{throw std::runtime_error("hash mismatch");};
        testing::SetServices(services);
        DownloadAvailableUpdate(owner);
        Until([]{return GetCheckStatus().state == CheckState::DownloadFailed;});
        Require(launches == 0 && HasAvailableUpdate(), "failed verification never launches installer or loses badge");

        services.download = [&](auto&, auto&){return root / L"verified.exe";};
        testing::SetServices(services);
        working = false;
        prepare_ok = false;
        DownloadAvailableUpdate(owner);
        Until([]{return GetCheckStatus().state == CheckState::InstallFailed;});
        Require(launches == 0 && prepares == 1, "checkpoint failure prevents installation");
        prepare_ok = true;
        DownloadAvailableUpdate(owner);
        Until([&]{return launches == 1;});
        Require(GetCheckStatus().state == CheckState::InstallFailed && cancellations == 2,
                "UAC or launch cancellation rolls back restart checkpoint");
        Require(parameters.find(L"/SILENT") != std::wstring::npos &&
                parameters.find(L"/NOCANCEL") != std::wstring::npos &&
                parameters.find(L"/AICLIUPDATE") != std::wstring::npos &&
                parameters.find(L"/DIR=\"") != std::wstring::npos,
                "installer disables cancellation and uses existing install directory and automatic relaunch mode");
        launch_ok = true;
        DownloadAvailableUpdate(owner);
        Until([]{return IsInstalling();});
        const auto cancellations_before_install_cancel = cancellations;
        CancelDownloadOrInstall();
        CheckForUpdates();
        DownloadAvailableUpdate(owner);
        Pump();
        Require(IsInstalling() && cancellations == cancellations_before_install_cancel,
                "cancel requests during installation cannot interrupt it or discard the restart checkpoint");
        Require(launches == 2 && prepares == 3, "double clicks cannot launch a second installer");
        Require(unexpected_windows == 0, "update flow never opens a notification or progress dialog");
        UnhookWindowsHookEx(hook);
        hook = nullptr;
        Stop();
        testing::SetServices({});
        DestroyWindow(owner);
        owner = nullptr;
        std::filesystem::remove_all(root);
        std::cout << "Update badges, download/cancellation/install flow and restart checkpoint tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (hook) UnhookWindowsHookEx(hook);
        Stop();
        if (owner) DestroyWindow(owner);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
