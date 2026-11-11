#include "app/native_remote_bridge.h"
#include "agent/conversation_bridge.h"
#include "agent/remote_agent.h"
#include "data/data_store.h"
#include "features/project_configuration.h"
#include "ui/group_name_dialog.h"
#include "ui/project_type_editor_dialog.h"
#include "ui/project_editor_dialog.h"
#include "ui/management_dialog.h"
#include "ui/internal/management_dialog_state.h"
#include "ui/modal_host.h"
#include "ui/styled_input.h"
#include "ui/drawing_utils.h"

#include <commctrl.h>
#include <objbase.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
namespace fs = std::filesystem;
namespace remote = aicli::app::native_remote;
namespace management = aicli::ui::management;
using namespace aicli::ui;

void Require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}

std::function<void()> tick;
std::string failure;
ULONGLONG deadline{};

HWND FindOwnWindow(const wchar_t *class_name)
{
    struct Search
    {
        const wchar_t *name;
        HWND found{};
    } search{class_name};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND window, LPARAM parameter) -> BOOL {
            auto &search = *reinterpret_cast<Search *>(parameter);
            wchar_t name[128]{};
            GetClassNameW(window, name, 128);
            if (wcscmp(name, search.name) == 0)
            {
                search.found = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD)
{
    try
    {
        Require(GetTickCount64() < deadline, "Modal test timed out");
        if (tick)
            tick();
    }
    catch (const std::exception &error)
    {
        failure = error.what();
        tick = {};
        for (const auto *name : {L"AicliGroupNameDialog", L"AicliProjectTypeEditorDialog",
                                 L"AicliProjectDialog", L"AicliManagementDialog"})
        {
            while (const auto window = FindOwnWindow(name))
                CloseModalSurface(window);
        }
    }
}

void Schedule(HWND owner, std::function<void()> callback)
{
    failure.clear();
    deadline = GetTickCount64() + 15000;
    tick = std::move(callback);
    SetTimer(owner, 1, 20, Drive);
}
void Finish(HWND owner)
{
    KillTimer(owner, 1);
    tick = {};
    Require(failure.empty(), failure.c_str());
}

void Click(HWND window, int id)
{
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
}
void Enter(HWND window, int id, const std::wstring &text)
{
    const auto control = GetDlgItem(window, id);
    Require(control != nullptr, "Dialog field exists");
    SetStyledInputText(control, text);
}

void TestDialogs(HWND owner, aicli::DataStore &store, aicli::agent::RemoteAgent &agent)
{
    std::wstring group;
    Schedule(owner, [&] {
        if (const auto window = FindOwnWindow(L"AicliGroupNameDialog"))
        {
            Require(GetStyledInputText(GetDlgItem(window, 2410)) == L"Original",
                    "Group initial value preserved");
            Enter(window, 2410, L"独立分组");
            Click(window, 2411);
        }
    });
    const auto accepted = ShowGroupNameDialog(owner, L"Test", L"Group", L"Original", false, group);
    Finish(owner);
    Require(accepted && group == L"独立分组", "Independent group editor returns edited name");

    std::wstring type, command;
    bool make_default{};
    Schedule(owner, [&] {
        if (const auto window = FindOwnWindow(L"AicliProjectTypeEditorDialog"))
        {
            Enter(window, 2420, L"Test CLI");
            Enter(window, 2421, L"echo custom-command");
            Click(window, 2425);
            Click(window, 2422);
        }
    });
    const auto type_accepted = ShowProjectTypeEditorDialog(
        owner, L"Test", L"Type", L"Initial", L"echo old", false, true, type, command, make_default);
    Finish(owner);
    Require(type_accepted && type == L"Test CLI" && command == L"echo custom-command" &&
                make_default,
            "Type editor returns name, command and default toggle");

    int stage{}, theme_changes{};
    Schedule(owner, [&] {
        if (stage == 2)
        {
            if (const auto child = FindOwnWindow(L"AicliGroupNameDialog"))
            {
                ++stage;
                Enter(child, 2410, L"扩展分组");
                Click(child, 2411);
            }
            return;
        }
        if (stage == 5)
        {
            if (const auto child = FindOwnWindow(L"AicliProjectTypeEditorDialog"))
            {
                ++stage;
                Enter(child, 2420, L"扩展 CLI");
                Enter(child, 2421, L"echo extension");
                Click(child, 2425);
                Click(child, 2422);
            }
            return;
        }
        const auto window = FindOwnWindow(L"AicliManagementDialog");
        if (!window)
            return;
        switch (stage++)
        {
        case 0:
            Click(window, management::kSettingsGroups);
            break;
        case 1:
            Click(window, management::kAdd);
            break;
        case 3:
            Click(window, management::kSettingsProjectTypes);
            break;
        case 4:
            Click(window, management::kAdd);
            break;
        case 6:
            Click(window, management::kSettingsAppearance);
            Click(window, management::kThemeLight);
            break;
        case 7:
            Click(window, management::kSettingsConnection);
            break;
        case 8:
            Click(window, management::kSettingsDevices);
            break;
        case 9:
            Click(window, management::kSettingsAbout);
            break;
        case 10:
            Click(window, management::kSettingsAppearance);
            break;
        default:
            Click(window, management::kClose);
            break;
        }
    });
    ShowManagementDialog(owner, ManagementMode::Settings, store, agent, {}, [&](bool light) {
        Require(light, "Theme callback receives selected theme");
        ++theme_changes;
    });
    Finish(owner);
    Require(stage >= 12 && theme_changes == 1 && store.GetSetting(L"theme") == L"light",
            "Settings pages and theme callback work across modules");
    const auto groups = store.LoadGroups();
    const auto types = store.LoadProjectTypes();
    Require(std::any_of(groups.begin(), groups.end(),
                        [](const auto &value) { return value.name == L"扩展分组"; }),
            "Nested group editor persists through management actions");
    Require(std::any_of(types.begin(), types.end(),
                        [](const auto &value) { return value.name == L"扩展 CLI"; }),
            "Nested type editor persists through management actions");
    std::cout << "Independent editors, all settings pages and nested management actions passed\n";
}

aicli::Project TestProjectEditors(HWND owner, aicli::DataStore &store, const fs::path &directory)
{
    const auto types = store.LoadProjectTypes();
    const auto groups = store.LoadGroups();
    const auto projects = store.LoadProjects();
    const auto group = groups.front().name;
    auto palette = MakeThemePalette(false);
    const auto font = CreateDialogFont(owner, 15, FW_NORMAL);
    ProjectEditorOptions options{
        store,
        projects,
        types,
        groups,
        palette,
        [&] { return ProjectEditorFonts{font, font, font, font, font, font}; },
        [&] { return group; },
        [&] { return group; },
        [] {}};
    ProjectEditorDialog first(options), second(options);
    aicli::Project outer, inner;
    int stage{};
    Schedule(owner, [&] {
        if (stage == 0 && first.window())
        {
            ++stage;
            Enter(first.window(), 2001, L"Outer draft");
            const auto result = second.Show(owner, inner);
            Require(result && inner.name == L"Inner draft",
                    "Nested project editor returns its own draft");
        }
        else if (stage == 1 && second.window())
        {
            ++stage;
            Enter(second.window(), 2001, L"Inner draft");
            Enter(second.window(), 2004, directory.wstring());
            Click(second.window(), 2006);
        }
        else if (stage == 2 && first.window())
        {
            ++stage;
            Require(GetStyledInputText(GetDlgItem(first.window(), 2001)) == L"Outer draft",
                    "Nested editor does not overwrite outer form state");
            Enter(first.window(), 2004, directory.wstring());
            Enter(first.window(), 2005, L"echo retained-custom-command");
            Enter(first.window(), 2009, L"独立备注");
            palette = MakeThemePalette(true);
            first.RefreshTheme();
            first.RefreshFonts();
            Click(first.window(), 2006);
        }
    });
    const auto accepted = first.Show(owner, outer);
    Finish(owner);
    Require(accepted && stage == 3 && outer.name == L"Outer draft" && outer.note == L"独立备注" &&
                outer.command == L"echo retained-custom-command",
            "Project draft and custom command survive nested form and theme changes");
    Require(first.window() == nullptr && second.window() == nullptr,
            "Project forms release their window state");
    Require(store.SaveProject(outer), "Save isolated project before editing");

    aicli::Project edited_project;
    Schedule(owner, [&] {
        if (first.window())
        {
            Enter(first.window(), 2001, L"Edited project");
            Click(first.window(), 2006);
        }
    });
    const auto saved_edit = first.Show(owner, edited_project, &outer);
    Finish(owner);
    Require(saved_edit && edited_project.id == outer.id &&
                edited_project.name == L"Edited project" && edited_project.note == outer.note &&
                edited_project.command == outer.command && store.SaveProject(edited_project),
            "Accepted edit keeps project identity and untouched fields");
    Require(store.LoadProjects().size() == 1,
            "Saving an edited project updates the original instead of creating a duplicate");
    outer = edited_project;

    aicli::Project cancelled;
    cancelled.id = L"keep-result";
    Schedule(owner, [&] {
        if (first.window())
        {
            Require(GetStyledInputText(GetDlgItem(first.window(), 2005)) == outer.command,
                    "Editing starts with saved custom command");
            Enter(first.window(), 2001, L"Discarded");
            Click(first.window(), 2007);
        }
    });
    const auto edited = first.Show(owner, cancelled, &outer);
    Finish(owner);
    Require(!edited && cancelled.id == L"keep-result", "Cancel preserves caller's result");
    DeleteObject(font);
    std::cout
        << "Instance-owned project forms, nested drafts, theme refresh and cancellation passed\n";
    return outer;
}

void TestRemoteMessages(aicli::DataStore &store, aicli::agent::RemoteAgent &agent,
                        const aicli::Project &project, const fs::path &directory)
{
    aicli::agent::ConversationBridge conversations(store, agent);
    remote::UiCallbacks ui;
    bool session{};
    int selected{}, started{}, submitted{}, written{}, keys{}, controls{}, pairings{}, statuses{},
        created{};
    std::wstring input;
    std::function<void()> on_start;
    ui.select_project = [&](const auto &id) {
        ++selected;
        return id == project.id;
    };
    ui.start_session = [&](bool) {
        ++started;
        session = true;
        if (on_start)
            on_start();
    };
    ui.has_session = [&](const auto &) { return session; };
    ui.write_input = [&](const auto &, const auto &text) {
        ++written;
        input = text;
        return true;
    };
    ui.submit_input = [&](const auto &, const auto &text) {
        ++submitted;
        input = text;
        return true;
    };
    ui.send_key = [&](const auto &, UINT key) {
        ++keys;
        return key == VK_LEFT;
    };
    ui.control = [&](const auto &value) {
        Require(value.action == L"pause", "Control action preserved");
        ++controls;
    };
    ui.pairing = [&](const auto &) { ++pairings; };
    ui.status = [&](bool, const auto &) { ++statuses; };
    ui.project_created = [&](const auto &result) {
        Require(result.success, "Created project callback receives result");
        ++created;
    };
    const auto dispatch = [&](UINT message, auto request) {
        const auto payload = reinterpret_cast<LPARAM>(new decltype(request)(request));
        Require(remote::HandleMessage(message, payload, ui, store, conversations),
                "Bridge handles owned message");
        Require(WaitForSingleObject(request->completed, 0) == WAIT_OBJECT_0,
                "Synchronous request completes");
    };
    auto text = std::make_shared<remote::RemoteInputMessage>(project.id, L"phone", L"hello\r\n");
    dispatch(remote::WM_REMOTE_INPUT, text);
    Require(text->accepted && started == 1 && submitted == 1 && written == 0 && input == L"hello",
            "Input starts missing session, strips submit newline and acknowledges success");
    auto plain = std::make_shared<remote::RemoteInputMessage>(project.id, L"phone", L"输入文本");
    dispatch(remote::WM_REMOTE_INPUT, plain);
    Require(plain->accepted && written == 1 && submitted == 1 && input == L"输入文本",
            "Input without trailing newline is written without submission");
    auto cancelled = std::make_shared<remote::RemoteInputMessage>(project.id, L"phone", L"discard");
    cancelled->cancelled = true;
    const int before = selected;
    dispatch(remote::WM_REMOTE_INPUT, cancelled);
    Require(!cancelled->accepted && selected == before && submitted == 1,
            "Expired input has no UI side effect");
    auto during_start =
        std::make_shared<remote::RemoteInputMessage>(project.id, L"phone", L"discard");
    session = false;
    on_start = [&] { during_start->cancelled = true; };
    dispatch(remote::WM_REMOTE_INPUT, during_start);
    on_start = {};
    Require(!during_start->accepted && submitted == 1,
            "Cancellation during terminal startup is respected");
    auto key = std::make_shared<remote::RemoteKeyMessage>(project.id, L"phone", L"left");
    dispatch(remote::WM_REMOTE_KEY, key);
    Require(key->accepted && keys == 1, "Named key maps to native key");
    auto unsupported = std::make_shared<remote::RemoteKeyMessage>(project.id, L"phone", L"unknown");
    dispatch(remote::WM_REMOTE_KEY, unsupported);
    Require(!unsupported->accepted && keys == 1, "Unsupported key is rejected without dispatch");
    store.SetSetting(L"mobile_workspace_root", directory.wstring());
    const auto options = aicli::mobile_projects::LoadCreationOptions(store);
    auto creation =
        std::make_shared<remote::RemoteCreateProjectMessage>(aicli::mobile_projects::CreateRequest{
            L"Remote child", options.default_type_id, options.default_group_id});
    dispatch(remote::WM_REMOTE_CREATE_PROJECT, creation);
    Require(creation->result.success && created == 1 &&
                fs::is_directory(creation->result.project.folder),
            "Remote creation persists and notifies the native UI");
    auto expired_creation =
        std::make_shared<remote::RemoteCreateProjectMessage>(aicli::mobile_projects::CreateRequest{
            L"Expired child", options.default_type_id, options.default_group_id});
    expired_creation->cancelled = true;
    dispatch(remote::WM_REMOTE_CREATE_PROJECT, expired_creation);
    Require(expired_creation->result.http_status == 504 && created == 1 &&
                !fs::exists(fs::path(options.root_directory) / L"Expired child"),
            "Expired creation does not create a project");
    remote::HandleMessage(
        remote::WM_REMOTE_CONTROL,
        reinterpret_cast<LPARAM>(new remote::RemoteControlMessage{project.id, L"phone", L"pause"}),
        ui, store, conversations);
    remote::HandleMessage(remote::WM_PAIRING_PENDING,
                          reinterpret_cast<LPARAM>(new aicli::agent::PendingPairing{}), ui, store,
                          conversations);
    remote::HandleMessage(
        remote::WM_AGENT_STATUS,
        reinterpret_cast<LPARAM>(new remote::AgentStatusMessage{false, L"offline"}), ui, store,
        conversations);
    Require(controls == 1 && pairings == 1 && statuses == 1,
            "Asynchronous notifications are forwarded");
    Require(!remote::HandleMessage(WM_APP + 999, 0, ui, store, conversations),
            "Other native messages remain with the window");
    std::cout
        << "Remote dispatch, request completion, cancellation, keys and project creation passed\n";
}
} // namespace

int wmain()
{
    fs::path root;
    HWND owner{};
    try
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        GdiplusRuntime drawing;
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        auto temp = fs::absolute(fs::temp_directory_path()).lexically_normal();
        if (temp.filename().empty())
            temp = temp.parent_path();
        root = temp / (L"aicli-native-modules-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64()));
        Require(root.parent_path() == temp,
                "Isolated data directory stays within temporary workspace");
        fs::create_directories(root / L"workspace");
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"AiCliNativeModuleTestOwner";
        RegisterClassW(&type);
        // All windows belong to this test and stay outside the visible desktop.
        owner = CreateWindowW(type.lpszClassName, L"", WS_POPUP | WS_VISIBLE, -30000, -30000, 1000,
                              750, nullptr, nullptr, type.hInstance, nullptr);
        Require(owner != nullptr, "Create isolated form owner");
        {
            aicli::DataStore store;
            Require(store.Initialize((root / L"test.db").wstring()),
                    "Initialize isolated database");
            store.SetSetting(L"theme", L"dark");
            store.SetSetting(L"mobile_access_enabled", L"0");
            aicli::agent::RemoteAgent agent;
            agent.AttachStore(&store);
            TestDialogs(owner, store, agent);
            const auto project = TestProjectEditors(owner, store, root / L"workspace");
            TestRemoteMessages(store, agent, project, root / L"workspace");
        }
        DestroyWindow(owner);
        owner = nullptr;
        fs::remove_all(root);
        CoUninitialize();
        std::cout << "Native module integration tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        if (owner)
        {
            KillTimer(owner, 1);
            DestroyWindow(owner);
        }
        std::cerr << error.what() << '\n';
        if (!root.empty())
            std::wcerr << L"Test artifacts: " << root.wstring() << '\n';
        return 1;
    }
}
