#include "ui/internal/management_dialog_state.h"
#include "ui/project_type_editor_dialog.h"
#include "ui/modal_host.h"
#include "ui/redraw_scope.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <filesystem>
#include <iterator>
#include <stdexcept>

namespace aicli::ui::management
{

void RefreshList(State &state)
{
    if (!state.list)
        return;
    ChildRedrawScope redraw(state.list);
    SendMessageW(state.list, LB_RESETCONTENT, 0, 0);
    if (IsSettingsGroups(state))
    {
        state.groups = state.store->LoadGroups();
        const auto projects = state.store->LoadProjects();
        const auto default_group =
            std::find_if(state.groups.begin(), state.groups.end(),
                         [](const ProjectGroup &group) { return group.builtin; });
        const std::wstring default_group_name =
            default_group != state.groups.end() ? default_group->name : L"默认分组";
        for (const auto &value : state.groups)
        {
            const auto count =
                std::count_if(projects.begin(), projects.end(), [&](const Project &project) {
                    const std::wstring group =
                        project.group.empty() ? default_group_name : project.group;
                    return group == value.name;
                });
            const std::wstring label = value.name + L" (" + std::to_wstring(count) + L")";
            LRESULT row =
                SendMessageW(state.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (row != LB_ERR)
                SendMessageW(state.list, LB_SETITEMDATA, row,
                             static_cast<LPARAM>(&value - state.groups.data()));
        }
    }
    else if (IsSettingsProjectTypes(state))
    {
        state.types_data = state.store->LoadProjectTypes();
        state.default_project_type_id = state.store->ConfiguredDefaultProjectTypeId();
        for (const auto &value : state.types_data)
        {
            LRESULT row = SendMessageW(state.list, LB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(value.name.c_str()));
            if (row != LB_ERR)
                SendMessageW(state.list, LB_SETITEMDATA, row,
                             static_cast<LPARAM>(&value - state.types_data.data()));
        }
    }
    else if (state.mode == ManagementMode::Devices ||
             (state.mode == ManagementMode::Settings &&
              state.settings_page == SettingsPage::Devices))
    {
        state.devices = state.store->LoadDevices(true);
        state.device_online_states.clear();
        state.device_online_states.reserve(state.devices.size());
        for (const auto &value : state.devices)
        {
            state.device_online_states.push_back(state.agent &&
                                                 state.agent->IsDeviceOnline(value.phone_id));
            std::wstring text = value.name;
            LRESULT row =
                SendMessageW(state.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            if (row != LB_ERR)
                SendMessageW(state.list, LB_SETITEMDATA, row,
                             static_cast<LPARAM>(&value - state.devices.data()));
        }
    }
    if (SendMessageW(state.list, LB_GETCOUNT, 0, 0) > 0)
        SendMessageW(state.list, LB_SETCURSEL, 0, 0);
    if (state.mode == ManagementMode::Settings && state.settings_page == SettingsPage::Devices)
    {
        const bool has_devices = !state.devices.empty();
        const bool has_active_device =
            std::any_of(state.devices.begin(), state.devices.end(),
                        [](const DeviceRecord &device) { return device.active; });
        if (state.bind_empty)
            ShowWindow(state.bind_empty, has_devices ? SW_HIDE : SW_SHOW);
        if (state.unbind)
            ShowWindow(state.unbind, has_active_device ? SW_SHOW : SW_HIDE);
    }
    state.list_scrollbar.Sync();
}

void ReadSelection(State &state)
{
    if (!state.list)
        return;
    LRESULT row = SendMessageW(state.list, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR)
    {
        if (state.mode == ManagementMode::Settings &&
            state.settings_page == SettingsPage::Devices && state.unbind)
            EnableWindow(state.unbind, FALSE);
        return;
    }
    const auto index = static_cast<size_t>(SendMessageW(state.list, LB_GETITEMDATA, row, 0));
    if (IsSettingsProjectTypes(state) && index < state.types_data.size())
    {
        SetField(state.name, state.types_data[index].name);
        SetField(state.command, state.types_data[index].default_command);
        if (state.save)
            EnableWindow(state.save, TRUE);
        if (state.remove)
            EnableWindow(state.remove, state.types_data[index].builtin ? FALSE : TRUE);
    }
    if (IsSettingsGroups(state) && index < state.groups.size())
    {
        SetField(state.name, state.groups[index].name);
    }
    if (state.mode == ManagementMode::Settings && state.settings_page == SettingsPage::Devices &&
        state.unbind)
        EnableWindow(state.unbind, index < state.devices.size() && state.devices[index].active);
}

bool SelectedIndex(HWND list, size_t &index)
{
    if (!list)
        return false;
    const LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR)
        return false;
    index = static_cast<size_t>(SendMessageW(list, LB_GETITEMDATA, row, 0));
    return true;
}

void ShowError(HWND owner, const std::wstring &message)
{
    if (!message.empty())
        MessageBoxW(owner, message.c_str(), L"AI CLI", MB_OK | MB_ICONWARNING);
}

bool SetDefaultProjectTypeByName(DataStore &store, const std::wstring &name, std::wstring *error)
{
    const auto types = store.LoadProjectTypes();
    const auto found = std::find_if(types.begin(), types.end(), [&](const ProjectType &type) {
        return _wcsicmp(type.name.c_str(), name.c_str()) == 0;
    });
    if (found == types.end())
    {
        if (error)
            *error = L"新项目类型已保存，但无法将它设为默认";
        return false;
    }
    return store.SetDefaultProjectType(found->id, error);
}

std::wstring QuoteArgument(const std::wstring &value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t current : value)
    {
        if (current == L'\\')
        {
            ++slashes;
        }
        else if (current == L'\"')
        {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
        }
        else
        {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(current);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool ConfigureFirewall(HWND owner, const std::wstring &port, std::wstring *error)
{
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))))
    {
        if (error)
            *error = L"无法确定 AI CLI 程序路径";
        return false;
    }
    const std::filesystem::path script =
        std::filesystem::path(executable).parent_path() / L"configure-agent-firewall.ps1";
    if (!std::filesystem::exists(script))
    {
        if (error)
            *error = L"缺少防火墙配置脚本，请重新运行部署脚本";
        return false;
    }
    wchar_t system_directory[MAX_PATH]{};
    const UINT system_length = GetSystemDirectoryW(system_directory, MAX_PATH);
    const std::filesystem::path powershell = system_length > 0 && system_length < MAX_PATH
                                                 ? std::filesystem::path(system_directory) /
                                                       L"WindowsPowerShell" / L"v1.0" /
                                                       L"powershell.exe"
                                                 : std::filesystem::path(L"powershell.exe");
    const std::wstring parameters = L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
                                    QuoteArgument(script.wstring()) + L" -Port " + port +
                                    L" -Program " + QuoteArgument(executable);
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.hwnd = owner;
    launch.lpVerb = L"runas";
    launch.lpFile = powershell.c_str();
    launch.lpParameters = parameters.c_str();
    launch.nShow = SW_HIDE;
    if (!ShellExecuteExW(&launch))
    {
        if (error)
            *error = GetLastError() == ERROR_CANCELLED ? L"已取消管理员授权，防火墙没有修改"
                                                       : L"无法启动管理员防火墙配置";
        return false;
    }
    WaitForSingleObject(launch.hProcess, INFINITE);
    DWORD exit_code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(launch.hProcess, &exit_code);
    CloseHandle(launch.hProcess);
    if (exit_code != 0)
    {
        if (error)
            *error = L"防火墙配置失败，请确认当前账户可执行管理员操作";
        return false;
    }
    return true;
}

LRESULT HandleCommand(State &context, WPARAM wparam)
{
    State *state = &context;
    const HWND h = state->window;

    const int id = LOWORD(wparam);
    const int notification = HIWORD(wparam);
    if (id == kList && (notification == LBN_SELCHANGE || notification == LBN_DBLCLK))
    {
        ReadSelection(*state);
        if (state->mode == ManagementMode::Devices)
            InvalidateRect(h, &state->panel, FALSE);
        if (state->mode == ManagementMode::Settings &&
            state->settings_page == SettingsPage::Devices)
            InvalidateRect(state->content, nullptr, FALSE);
        if (notification == LBN_DBLCLK && state->mode == ManagementMode::Groups)
        {
            size_t index{};
            if (SelectedIndex(state->list, index) && index < state->groups.size())
            {
                state->result.accepted = true;
                state->result.selected_group = state->groups[index].name;
                CloseModalSurface(h);
            }
        }
        if (notification == LBN_DBLCLK && IsSettingsProjectTypes(*state) &&
            IsWindowEnabled(state->save))
        {
            SendMessageW(h, WM_COMMAND, MAKEWPARAM(kSave, BN_CLICKED),
                         reinterpret_cast<LPARAM>(state->save));
        }
        return 0;
    }
    if (notification != BN_CLICKED)
        return 0;
    std::wstring error;
    if (id == kClose)
    {
        CloseModalSurface(h);
        return 0;
    }
    if (state->mode == ManagementMode::Settings)
    {
        if (id == kCheckUpdate)
        {
            aicli::update::CheckForUpdates();
            RefreshUpdateStatus(*state, true);
            return 0;
        }
        if (id == kDownloadUpdate)
        {
            aicli::update::DownloadAvailableUpdate(state->window);
            return 0;
        }
        if (id == kSettingsAppearance)
        {
            UpdateSettingsPage(*state, SettingsPage::Appearance);
            return 0;
        }
        if (id == kSettingsConnection)
        {
            UpdateSettingsPage(*state, SettingsPage::Connection);
            return 0;
        }
        if (id == kSettingsProjectTypes)
        {
            UpdateSettingsPage(*state, SettingsPage::ProjectTypes);
            return 0;
        }
        if (id == kSettingsGroups)
        {
            UpdateSettingsPage(*state, SettingsPage::Groups);
            return 0;
        }
        if (id == kSettingsDevices)
        {
            UpdateSettingsPage(*state, SettingsPage::Devices);
            return 0;
        }
        if (id == kSettingsAbout)
        {
            UpdateSettingsPage(*state, SettingsPage::About);
            return 0;
        }
        if (state->settings_page == SettingsPage::Devices && (id == kSelect || id == kBindEmpty))
        {
            state->result.open_binding = true;
            CloseModalSurface(h);
            return 0;
        }
        if (state->settings_page == SettingsPage::Devices && id == kUnbind)
        {
            size_t index{};
            if (!SelectedIndex(state->list, index) || index >= state->devices.size())
            {
                ShowError(h, L"请选择要解绑的设备");
            }
            else if (!state->devices[index].active)
            {
                ShowError(h, L"该设备已经解绑");
            }
            else if (!state->agent->RevokeDevice(state->devices[index].phone_id))
            {
                ShowError(h, L"解绑失败");
            }
            else
            {
                RefreshList(*state);
                ReadSelection(*state);
                InvalidateRect(state->content, nullptr, FALSE);
            }
            return 0;
        }
        if (id == kTheme || id == kThemeLight)
        {
            const bool light = id == kThemeLight;
            if (state->light_theme != light)
            {
                ApplyTheme(*state, light);
                state->store->SetSetting(L"theme", light ? L"light" : L"dark");
                if (state->theme_changed)
                    state->theme_changed(light);
                RedrawWindow(state->window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            }
            return 0;
        }
        if (id == kTaskbarWorking || id == kKeepAwake || id == kTaskbarStyleProgress ||
            id == kTaskbarStyleOverlay || id == kTaskbarStyleIconDots ||
            id == kTaskbarScopeCurrent || id == kTaskbarScopeAll)
        {
            if (id == kTaskbarWorking)
            {
                const bool enabled =
                    state->store->GetSetting(L"taskbar_working_enabled", L"1") != L"0";
                state->store->SetSetting(L"taskbar_working_enabled", enabled ? L"0" : L"1");
                SetWindowTextW(state->taskbar_working, enabled ? L"关闭" : L"开启");
            }
            else if (id == kKeepAwake)
            {
                const bool enabled =
                    state->store->GetSetting(L"keep_awake_while_working", L"0") != L"0";
                state->store->SetSetting(L"keep_awake_while_working", enabled ? L"0" : L"1");
                SetWindowTextW(state->keep_awake, enabled ? L"关闭" : L"开启");
            }
            else if (id == kTaskbarStyleProgress || id == kTaskbarStyleOverlay ||
                     id == kTaskbarStyleIconDots)
            {
                state->store->SetSetting(L"taskbar_working_style",
                                         id == kTaskbarStyleOverlay    ? L"overlay"
                                         : id == kTaskbarStyleIconDots ? L"icon_dots"
                                                                       : L"progress");
            }
            else
            {
                state->store->SetSetting(L"taskbar_working_scope",
                                         id == kTaskbarScopeCurrent ? L"current" : L"all");
            }
            for (HWND control :
                 {state->taskbar_working, state->keep_awake, state->taskbar_style_progress,
                  state->taskbar_style_overlay, state->taskbar_style_icon_dots,
                  state->taskbar_scope_current, state->taskbar_scope_all})
            {
                InvalidateRect(control, nullptr, FALSE);
            }
            if (state->appearance_changed)
                state->appearance_changed();
            return 0;
        }
        if (state->settings_page == SettingsPage::Connection && id == kMobileAccess)
        {
            state->mobile_access_enabled = !state->mobile_access_enabled;
            SetWindowTextW(state->mobile_access, state->mobile_access_enabled ? L"开启" : L"关闭");
            InvalidateRect(state->mobile_access, nullptr, FALSE);
            return 0;
        }
        if (state->settings_page == SettingsPage::Connection && id == kFirewall)
        {
            const std::wstring port = Field(state->port).empty() ? L"4280" : Field(state->port);
            try
            {
                size_t consumed{};
                const int number = std::stoi(port, &consumed);
                if (consumed != port.size() || number < 1 || number > 65535)
                {
                    throw std::out_of_range("port");
                }
            }
            catch (...)
            {
                ShowError(h, L"请先填写有效的 HTTPS 端口");
                return 0;
            }
            if (!ConfigureFirewall(h, port, &error))
            {
                ShowError(h, error);
                return 0;
            }
            state->store->SetSetting(L"firewall_port", port);
            MessageBoxW(h, L"已允许已绑定手机访问当前 HTTPS/WSS 端口。\n端口变化后请重新执行一次。",
                        L"AI CLI", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (state->settings_page == SettingsPage::Connection && id == kMobileWorkspaceBrowse)
        {
            IFileDialog *dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&dialog))) &&
                dialog)
            {
                DWORD options = 0;
                dialog->GetOptions(&options);
                dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                   FOS_PATHMUSTEXIST);
                if (SUCCEEDED(dialog->Show(h)))
                {
                    IShellItem *item = nullptr;
                    if (SUCCEEDED(dialog->GetResult(&item)) && item)
                    {
                        PWSTR value = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value)) && value)
                        {
                            SetField(state->mobile_workspace_root, value);
                            CoTaskMemFree(value);
                        }
                        item->Release();
                    }
                }
                dialog->Release();
            }
            return 0;
        }
        if (state->settings_page == SettingsPage::Connection && id == kSave)
        {
            const std::wstring computer_name = Field(state->computer_name);
            const std::wstring listen =
                Field(state->listen).empty() ? L"0.0.0.0" : Field(state->listen);
            const std::wstring port = Field(state->port).empty() ? L"4280" : Field(state->port);
            const std::wstring local = Field(state->local);
            const std::wstring remote = Field(state->remote);
            const std::wstring mobile_workspace_root = Field(state->mobile_workspace_root);
            if (computer_name.empty())
            {
                ShowError(h, L"本机名称不能为空");
                return 0;
            }
            if (computer_name.size() > 64)
            {
                ShowError(h, L"本机名称不能超过 64 个字符");
                return 0;
            }
            try
            {
                size_t consumed{};
                const int number = std::stoi(port, &consumed);
                if (consumed != port.size() || number < 1 || number > 65535)
                    throw std::out_of_range("port");
            }
            catch (...)
            {
                ShowError(h, L"HTTPS 端口必须是 1 到 65535 的数字");
                return 0;
            }
            const auto secure_address = [](const std::wstring &value) {
                return value.empty() || value.rfind(L"https://", 0) == 0;
            };
            if (!secure_address(local) || !secure_address(remote))
            {
                ShowError(h, L"本机地址和公网/Relay 地址必须使用 https://");
                return 0;
            }
            const std::filesystem::path mobile_root_path(mobile_workspace_root);
            if ((!mobile_workspace_root.empty() && !mobile_root_path.is_absolute()) ||
                (state->mobile_access_enabled && mobile_workspace_root.empty()))
            {
                ShowError(h, state->mobile_access_enabled && mobile_workspace_root.empty()
                                 ? L"开启手机访问前请选择手机工作目录"
                                 : L"手机工作目录必须是有效的绝对路径");
                return 0;
            }
            state->store->SetSetting(L"computer_name", computer_name);
            state->store->SetSetting(L"listen_address", listen);
            state->store->SetSetting(L"listen_port", port);
            state->store->SetSetting(L"local_address", local);
            state->store->SetSetting(L"remote_address", remote);
            state->store->SetSetting(L"mobile_workspace_root",
                                     mobile_workspace_root.empty()
                                         ? L""
                                         : mobile_root_path.lexically_normal().wstring());
            agent::SetMobileAccessEnabled(*state->store, state->mobile_access_enabled);
            agent::AgentConfig config;
            config.computer_name = computer_name;
            config.bind_address = listen;
            config.port = port;
            config.local_address = local;
            config.remote_address = remote;
            config.data_directory = aicli::DefaultDataDirectory();
            if (state->restart_agent && !state->restart_agent(config, &error))
            {
                ShowError(h, error.empty() ? L"Agent 重启失败" : error);
                return 0;
            }
            state->result.accepted = true;
            CloseModalSurface(h);
            return 0;
        }
        if (state->settings_page == SettingsPage::ProjectTypes)
        {
            if (id == kAdd)
            {
                std::wstring name;
                std::wstring command;
                bool make_default = false;
                if (!ShowProjectTypeEditorDialog(
                        h, L"新增项目类型", L"为新的 AI CLI 配置名称和默认启动命令。", L"", L"",
                        false, state->light_theme, name, command, make_default))
                    return 0;
                if (!state->store->AddProjectType(name, command, &error))
                {
                    ShowError(h, error);
                }
                else
                {
                    if (make_default && !SetDefaultProjectTypeByName(*state->store, name, &error))
                    {
                        ShowError(h, error);
                    }
                    RefreshList(*state);
                    ReadSelection(*state);
                }
            }
            else if (id == kSave)
            {
                size_t index{};
                if (!SelectedIndex(state->list, index) || index >= state->types_data.size())
                {
                    ShowError(h, L"请选择要修改的类型");
                }
                else
                {
                    const auto current = state->types_data[index];
                    std::wstring name;
                    std::wstring command;
                    const bool was_default =
                        current.id == state->store->ConfiguredDefaultProjectTypeId();
                    bool make_default = was_default;
                    if (!ShowProjectTypeEditorDialog(
                            h, L"修改项目类型", L"保存后，类型名称和默认启动命令会立即更新。",
                            current.name, current.default_command, make_default, state->light_theme,
                            name, command, make_default))
                        return 0;
                    if (!state->store->UpdateProjectType(current.id, name, command, &error))
                    {
                        ShowError(h, error);
                    }
                    else
                    {
                        if (make_default)
                        {
                            if (!state->store->SetDefaultProjectType(current.id, &error))
                            {
                                ShowError(h, error);
                            }
                        }
                        else if (was_default && !state->store->ClearDefaultProjectType(&error))
                        {
                            ShowError(h, error);
                        }
                        RefreshList(*state);
                        ReadSelection(*state);
                    }
                }
            }
            else if (id == kDelete)
            {
                size_t index{};
                if (!SelectedIndex(state->list, index) || index >= state->types_data.size())
                {
                    ShowError(h, L"请选择要删除的类型");
                }
                else if (!state->store->DeleteProjectType(state->types_data[index].id, &error))
                {
                    ShowError(h, error);
                }
                else
                {
                    RefreshList(*state);
                    ReadSelection(*state);
                }
            }
            return 0;
        }
        if (state->settings_page == SettingsPage::Groups)
        {
            if (id == kAdd)
            {
                std::wstring name;
                if (!ShowGroupNameDialog(h, L"添加分组", L"创建后可在标题栏下拉框中直接切换。", L"",
                                         state->light_theme, name))
                    return 0;
                if (!state->store->AddGroup(name, &error))
                {
                    ShowError(h, error);
                }
                else
                {
                    RefreshList(*state);
                }
            }
            else if (id == kSave)
            {
                size_t index{};
                if (!SelectedIndex(state->list, index) || index >= state->groups.size())
                {
                    ShowError(h, L"请选择要修改的分组");
                }
                else
                {
                    std::wstring name;
                    if (!ShowGroupNameDialog(h, L"修改分组名称",
                                             L"保存后已有项目会同步更新到新名称。",
                                             state->groups[index].name, state->light_theme, name))
                        return 0;
                    if (!state->store->RenameGroup(state->groups[index].id, name, &error))
                    {
                        ShowError(h, error);
                    }
                    else
                    {
                        RefreshList(*state);
                    }
                }
            }
            else if (id == kDelete)
            {
                size_t index{};
                if (!SelectedIndex(state->list, index) || index >= state->groups.size())
                {
                    ShowError(h, L"请选择要删除的分组");
                }
                else if (!state->store->DeleteGroup(state->groups[index].id, &error))
                {
                    ShowError(h, error);
                }
                else
                {
                    RefreshList(*state);
                }
            }
            return 0;
        }
        return 0;
    }
    if (state->mode == ManagementMode::Groups)
    {
        if (id == kAdd)
        {
            if (!state->store->AddGroup(Field(state->name), &error))
                ShowError(h, error);
            else
            {
                RefreshList(*state);
                SetField(state->name, L"");
            }
        }
        else if (id == kSave)
        {
            size_t index{};
            if (!SelectedIndex(state->list, index) || index >= state->groups.size())
                ShowError(h, L"请选择要重命名的分组");
            else if (!state->store->RenameGroup(state->groups[index].id, Field(state->name),
                                                &error))
                ShowError(h, error);
            else
                RefreshList(*state);
        }
        else if (id == kDelete)
        {
            size_t index{};
            if (!SelectedIndex(state->list, index) || index >= state->groups.size())
                ShowError(h, L"请选择要删除的分组");
            else if (!state->store->DeleteGroup(state->groups[index].id, &error))
                ShowError(h, error);
            else
                RefreshList(*state);
        }
        else if (id == kSelect)
        {
            size_t index{};
            if (!SelectedIndex(state->list, index) || index >= state->groups.size())
                ShowError(h, L"请选择分组");
            else
            {
                state->result.accepted = true;
                state->result.selected_group = state->groups[index].name;
                CloseModalSurface(h);
            }
        }
        return 0;
    }
    if (state->mode == ManagementMode::ProjectTypes)
    {
        if (id == kAdd)
        {
            std::wstring name;
            std::wstring command;
            bool make_default = false;
            if (!ShowProjectTypeEditorDialog(
                    h, L"新增项目类型", L"为新的 AI CLI 配置名称和默认启动命令。", L"", L"", false,
                    state->light_theme, name, command, make_default))
                return 0;
            if (!state->store->AddProjectType(name, command, &error))
                ShowError(h, error);
            else
            {
                if (make_default && !SetDefaultProjectTypeByName(*state->store, name, &error))
                    ShowError(h, error);
                RefreshList(*state);
                ReadSelection(*state);
            }
        }
        else if (id == kSave)
        {
            size_t index{};
            if (!SelectedIndex(state->list, index) || index >= state->types_data.size())
                ShowError(h, L"请选择要修改的类型");
            else
            {
                const auto current = state->types_data[index];
                std::wstring name;
                std::wstring command;
                const bool was_default =
                    current.id == state->store->ConfiguredDefaultProjectTypeId();
                bool make_default = was_default;
                if (!ShowProjectTypeEditorDialog(
                        h, L"修改项目类型", L"保存后，类型名称和默认启动命令会立即更新。",
                        current.name, current.default_command, make_default, state->light_theme,
                        name, command, make_default))
                    return 0;
                if (!state->store->UpdateProjectType(current.id, name, command, &error))
                    ShowError(h, error);
                else
                {
                    if (make_default)
                    {
                        if (!state->store->SetDefaultProjectType(current.id, &error))
                        {
                            ShowError(h, error);
                        }
                    }
                    else if (was_default && !state->store->ClearDefaultProjectType(&error))
                    {
                        ShowError(h, error);
                    }
                    RefreshList(*state);
                    ReadSelection(*state);
                }
            }
        }
        else if (id == kDelete)
        {
            size_t index{};
            if (!SelectedIndex(state->list, index) || index >= state->types_data.size())
                ShowError(h, L"请选择要删除的类型");
            else if (!state->store->DeleteProjectType(state->types_data[index].id, &error))
                ShowError(h, error);
            else
            {
                RefreshList(*state);
                ReadSelection(*state);
            }
        }
        return 0;
    }
    if (state->mode == ManagementMode::Devices && id == kUnbind)
    {
        size_t index{};
        if (!SelectedIndex(state->list, index) || index >= state->devices.size())
            ShowError(h, L"请选择要解绑的设备");
        else if (!state->devices[index].active)
            ShowError(h, L"该设备已经解绑");
        else if (!state->agent->RevokeDevice(state->devices[index].phone_id))
            ShowError(h, L"解绑失败");
        else
            RefreshList(*state);
        return 0;
    }
    return 0;
}

} // namespace aicli::ui::management
