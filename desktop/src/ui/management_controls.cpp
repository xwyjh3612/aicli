#include "ui/internal/management_dialog_state.h"
#include "ui/styled_input.h"
#include "ui/modal_host.h"
#include "core/text_utils.h"
#include "features/mobile_project_creation.h"
#include <commctrl.h>
#include <algorithm>

namespace aicli::ui::management
{

constexpr UINT_PTR kManagementButtonSubclassId = 0xA1C14;
State *GetState(HWND h)
{
    return reinterpret_cast<State *>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

State *FindState(HWND h)
{
    for (HWND current = h; current; current = GetParent(current))
    {
        if (auto *state = GetState(current))
            return state;
    }
    return nullptr;
}

LRESULT CALLBACK ManagementButtonProc(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                      UINT_PTR, DWORD_PTR)
{
    State *state = FindState(control);
    switch (message)
    {
    case WM_ERASEBKGND:
        // Every button in the management surface is owner-drawn.  Letting
        // the stock BUTTON class erase first briefly exposes its default
        // white brush in the dark theme before WM_DRAWITEM paints the themed
        // surface, which is visible as a one-frame flash while the modal
        // opens or a button receives focus.
        return 1;
    case WM_MOUSEMOVE:
        if (state && state->hovered_control != control)
        {
            HWND previous = state->hovered_control;
            state->hovered_control = control;
            if (previous)
                InvalidateRect(previous, nullptr, FALSE);
            InvalidateRect(control, nullptr, FALSE);
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
            TrackMouseEvent(&tracking);
        }
        break;
    case WM_MOUSELEAVE:
        if (state && state->hovered_control == control)
            state->hovered_control = nullptr;
        InvalidateRect(control, nullptr, FALSE);
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(control, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        if (state && state->hovered_control == control)
            state->hovered_control = nullptr;
        RemoveWindowSubclass(control, ManagementButtonProc, kManagementButtonSubclassId);
        break;
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

void ApplyTheme(State &state, bool light)
{
    state.palette = MakeThemePalette(light);
    state.light_theme = state.palette.light();
    state.background_color = state.palette.window;
    state.panel_color = state.palette.panel;
    state.panel_edge = state.palette.border;
    state.text_color = state.palette.text;
    state.muted_color = state.palette.muted;
    state.field_color = state.palette.field;
    state.field_focus_color = state.palette.field_focus;
    state.accent_color = state.palette.accent;
    if (state.field_brush)
        DeleteObject(state.field_brush);
    if (state.field_focus_brush)
        DeleteObject(state.field_focus_brush);
    if (state.list_brush)
        DeleteObject(state.list_brush);
    state.field_brush = CreateSolidBrush(state.field_color);
    state.field_focus_brush = CreateSolidBrush(state.field_focus_color);
    state.list_brush = CreateSolidBrush(state.palette.list_surface);
    for (HWND field : {state.name, state.command, state.computer_name, state.listen, state.port,
                       state.local, state.remote, state.mobile_workspace_root})
    {
        if (field)
            SetStyledInputTheme(field, state.palette);
    }
    state.content_scrollbar.SetTheme(state.palette, state.background_color);
    state.list_scrollbar.SetTheme(state.palette, state.palette.list_surface);
    if (state.content)
    {
        RedrawWindow(state.content, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
}

HWND MakeButton(State &state, const wchar_t *text, int id, HWND parent)
{
    if (!parent)
        parent = state.window;
    HWND button = CreateWindowExW(
        0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (button)
    {
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        SetWindowSubclass(button, ManagementButtonProc, kManagementButtonSubclassId, 0);
    }
    return button;
}

HWND MakeField(State &state, int id, const wchar_t *placeholder, HWND parent)
{
    HWND field = CreateStyledInput(parent ? parent : state.window, id, state.font, placeholder);
    SetStyledInputTheme(field, state.palette);
    return field;
}

void SetField(HWND field, const std::wstring &value)
{
    if (field)
        SetStyledInputText(field, value);
}

std::wstring Field(HWND field)
{
    return field ? GetStyledInputText(field) : std::wstring{};
}

void CreateControls(State &state)
{
    if (state.mode == ManagementMode::Settings)
    {
        state.content =
            CreateWindowExW(WS_EX_CONTROLPARENT, kSettingsContentClassName, L"",
                            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                            state.window, nullptr, GetModuleHandleW(nullptr), &state);
        state.content_scrollbar.Attach(
            state.content,
            [&state] {
                RECT client{};
                GetClientRect(state.content, &client);
                return OverlayScrollMetrics{
                    state.content_extent,
                    static_cast<int>(std::max(1L, client.bottom - client.top)),
                    state.content_scroll};
            },
            [&state](int position) { SetSettingsScroll(state, position); });
        state.content_scrollbar.SetTheme(state.palette, state.background_color);
        state.settings_appearance = MakeButton(state, L"外观", kSettingsAppearance);
        state.settings_connection = MakeButton(state, L"本机与网络", kSettingsConnection);
        state.settings_project_types = MakeButton(state, L"项目类型", kSettingsProjectTypes);
        state.settings_groups = MakeButton(state, L"分组管理", kSettingsGroups);
        state.settings_devices = MakeButton(state, L"设备管理", kSettingsDevices);
        state.settings_about = MakeButton(state, L"关于", kSettingsAbout);
        state.computer_name =
            MakeField(state, kComputerName, L"显示在手机端，例如：开发主机", state.content);
        state.listen = MakeField(state, kListen, L"0.0.0.0 表示所有网卡", state.content);
        state.port = MakeField(state, kPort, L"例如 4280", state.content);
        state.local = MakeField(state, kLocal, L"二维码优先使用，例如：https://192.168.1.10:4280",
                                state.content);
        state.remote = MakeField(state, kRemote, L"可留空，例如：https://relay.example.com/aicli",
                                 state.content);
        state.mobile_workspace_root =
            MakeField(state, kMobileWorkspaceRoot, L"选择手机工作目录", state.content);
        state.mobile_workspace_browse =
            MakeButton(state, L"选择", kMobileWorkspaceBrowse, state.content);
        state.mobile_access_enabled = agent::IsMobileAccessEnabled(*state.store);
        state.mobile_access = MakeButton(state, state.mobile_access_enabled ? L"开启" : L"关闭",
                                         kMobileAccess, state.content);
        const auto theme = state.store->GetSetting(L"theme", L"dark");
        state.light_theme = theme == L"light";
        state.theme = MakeButton(state, L"夜间模式", kTheme, state.content);
        state.theme_light = MakeButton(state, L"白天模式", kThemeLight, state.content);
        state.taskbar_working = MakeButton(
            state,
            state.store->GetSetting(L"taskbar_working_enabled", L"1") == L"0" ? L"关闭" : L"开启",
            kTaskbarWorking, state.content);
        state.taskbar_style_progress =
            MakeButton(state, L"进度条", kTaskbarStyleProgress, state.content);
        state.taskbar_style_overlay =
            MakeButton(state, L"图标转圈", kTaskbarStyleOverlay, state.content);
        state.taskbar_style_icon_dots =
            MakeButton(state, L"图标三点", kTaskbarStyleIconDots, state.content);
        state.taskbar_scope_current =
            MakeButton(state, L"当前项目", kTaskbarScopeCurrent, state.content);
        state.taskbar_scope_all = MakeButton(state, L"所有项目", kTaskbarScopeAll, state.content);
        state.keep_awake = MakeButton(
            state,
            state.store->GetSetting(L"keep_awake_while_working", L"0") == L"0" ? L"关闭" : L"开启",
            kKeepAwake, state.content);
        state.save = MakeButton(state, L"保存设置", kSave, state.content);
        state.firewall = MakeButton(state, L"配置防火墙", kFirewall, state.content);
        for (HWND control :
             {state.computer_name, state.listen, state.port, state.local, state.remote,
              state.mobile_workspace_root, state.mobile_workspace_browse, state.mobile_access,
              state.theme, state.theme_light, state.taskbar_working, state.keep_awake,
              state.taskbar_style_progress, state.taskbar_style_overlay,
              state.taskbar_style_icon_dots, state.taskbar_scope_current, state.taskbar_scope_all,
              state.save, state.firewall})
        {
            if (control && state.settings_content_font)
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(state.settings_content_font), TRUE);
        }
        state.list = CreateWindowExW(
            0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN | LBS_NOTIFY |
                LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            0, 0, 1, 1, state.content, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state.list, WM_SETFONT,
                     reinterpret_cast<WPARAM>(
                         state.settings_content_font ? state.settings_content_font : state.font),
                     TRUE);
        state.list_scrollbar.Attach(
            state.list,
            [&state] {
                RECT client{};
                GetClientRect(state.list, &client);
                const int item_height =
                    std::max(1, static_cast<int>(SendMessageW(state.list, LB_GETITEMHEIGHT, 0, 0)));
                return OverlayScrollMetrics{
                    static_cast<int>(SendMessageW(state.list, LB_GETCOUNT, 0, 0)),
                    std::max(1, static_cast<int>(client.bottom - client.top) / item_height),
                    static_cast<int>(SendMessageW(state.list, LB_GETTOPINDEX, 0, 0))};
            },
            [&state](int position) { SendMessageW(state.list, LB_SETTOPINDEX, position, 0); });
        state.list_scrollbar.SetTheme(state.palette, state.palette.list_surface);
        state.add = MakeButton(state, L"新增类型", kAdd, state.content);
        state.remove = MakeButton(state, L"删除", kDelete, state.content);
        state.select = MakeButton(state, L"绑定手机", kSelect, state.content);
        state.unbind = MakeButton(state, L"解绑设备", kUnbind, state.content);
        state.bind_empty = MakeButton(state, L"绑定第一台手机", kBindEmpty, state.content);
        state.check_update = MakeButton(state, L"检查更新", kCheckUpdate, state.content);
        state.download_update = MakeButton(state, L"下载更新", kDownloadUpdate, state.content);
        state.title_close = MakeButton(state, L"\x00D7", kClose);
        for (HWND control : {state.computer_name,
                             state.listen,
                             state.port,
                             state.local,
                             state.remote,
                             state.mobile_workspace_root,
                             state.mobile_workspace_browse,
                             state.mobile_access,
                             state.theme,
                             state.theme_light,
                             state.taskbar_working,
                             state.taskbar_style_progress,
                             state.taskbar_style_overlay,
                             state.taskbar_style_icon_dots,
                             state.taskbar_scope_current,
                             state.taskbar_scope_all,
                             state.keep_awake,
                             state.save,
                             state.firewall,
                             state.list,
                             state.add,
                             state.remove,
                             state.select,
                             state.unbind,
                             state.bind_empty,
                             state.check_update,
                             state.download_update})
        {
            if (control)
            {
                SetWindowSubclass(control, SettingsWheelProc, kSettingsWheelSubclassId,
                                  reinterpret_cast<DWORD_PTR>(&state));
            }
        }
        UpdateSettingsPage(state, state.settings_page);
    }
    else if (state.mode == ManagementMode::Groups)
    {
        state.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                         LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                     0, 0, 1, 1, state.window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
                                     GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state.list, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        state.name = MakeField(state, kName, L"输入新分组名称，或修改选中分组");
        state.add = MakeButton(state, L"新建分组", kAdd);
        state.save = MakeButton(state, L"保存修改", kSave);
        state.remove = MakeButton(state, L"删除", kDelete);
        state.select = MakeButton(state, L"切换到此分组", kSelect);
        state.close = MakeButton(state, L"关闭", kClose);
        RefreshList(state);
    }
    else if (state.mode == ManagementMode::ProjectTypes)
    {
        state.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                         LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                     0, 0, 1, 1, state.window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
                                     GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state.list, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        state.add = MakeButton(state, L"新增", kAdd);
        state.save = MakeButton(state, L"修改", kSave);
        state.remove = MakeButton(state, L"删除", kDelete);
        state.close = MakeButton(state, L"关闭", kClose);
        RefreshList(state);
        ReadSelection(state);
    }
    else
    {
        state.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                         LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                     0, 0, 1, 1, state.window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
                                     GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state.list, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        state.unbind = MakeButton(state, L"解绑设备", kUnbind);
        state.title_close = MakeButton(state, L"\x00D7", kClose);
        RefreshList(state);
    }
    Layout(state);
    PostMessageW(state.window, kPopulateMessage, 0, 0);
}

void PopulateControls(State &state)
{
    if (state.mode == ManagementMode::Settings)
    {
        SetField(state.computer_name,
                 state.store->GetSetting(L"computer_name", aicli::text::LocalComputerName()));
        std::wstring listen_value = state.store->GetSetting(L"listen_address", L"0.0.0.0");
        std::wstring port_value = state.store->GetSetting(L"listen_port", L"4280");
        if (listen_value.empty())
            listen_value = L"0.0.0.0";
        if (port_value.empty())
            port_value = L"4280";
        SetField(state.local, state.store->GetSetting(L"local_address"));
        SetField(state.remote, state.store->GetSetting(L"remote_address"));
        SetField(state.mobile_workspace_root,
                 mobile_projects::WorkspaceDirectory(*state.store).wstring());
        SetField(state.listen, listen_value);
        SetField(state.port, port_value);
    }
    else
    {
        ReadSelection(state);
    }
}

} // namespace aicli::ui::management
