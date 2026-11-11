#include "ui/management_dialog.h"
#include "ui/internal/management_dialog_state.h"
#include "ui/drawing_utils.h"
#include "ui/modal_host.h"
#include <utility>

namespace aicli::ui
{
namespace management
{

constexpr wchar_t kClassName[] = L"AicliManagementDialog";

LRESULT CALLBACK WindowProc(HWND h, UINT message, WPARAM wparam, LPARAM lparam)
{
    State *state = GetState(h);
    if (message == WM_NCCREATE)
    {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        state = static_cast<State *>(create->lpCreateParams);
        state->window = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(h, message, wparam, lparam);
    switch (message)
    {
    case WM_CREATE:
        CreateControls(*state);
        if (state->mode == ManagementMode::Settings)
            SetTimer(h, kDevicePresenceTimerId, 2000, nullptr);
        return 0;
    case WM_SIZE:
        Layout(*state);
        return 0;
    case kPopulateMessage:
        PopulateControls(*state);
        return 0;
    case WM_TIMER:
        if (wparam == kUpdateStatusTimerId)
        {
            RefreshUpdateStatus(*state);
            return 0;
        }
        if (wparam == kDevicePresenceTimerId)
        {
            if (state->mode == ManagementMode::Settings &&
                state->settings_page == SettingsPage::Devices)
            {
                if (state->device_online_states.size() != state->devices.size())
                {
                    state->device_online_states.assign(state->devices.size(), 0);
                }
                size_t selected_index{};
                const bool has_selection = SelectedIndex(state->list, selected_index);
                bool selected_status_changed = false;
                for (size_t index = 0; index < state->devices.size(); ++index)
                {
                    const bool online = state->agent && state->agent->IsDeviceOnline(
                                                            state->devices[index].phone_id);
                    if (state->device_online_states[index] == static_cast<unsigned char>(online))
                        continue;
                    state->device_online_states[index] = static_cast<unsigned char>(online);
                    RECT row{};
                    if (state->list &&
                        SendMessageW(state->list, LB_GETITEMRECT, static_cast<WPARAM>(index),
                                     reinterpret_cast<LPARAM>(&row)) != LB_ERR)
                        InvalidateRect(state->list, &row, FALSE);
                    selected_status_changed =
                        selected_status_changed || (has_selection && selected_index == index);
                }
                if (selected_status_changed && state->content)
                    InvalidateRect(state->content, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        {
            BufferedWindowPaint buffered(dc, state->panel, ps.rcPaint);
            Paint(*state, buffered.dc());
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, state->text_color);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        HWND control = reinterpret_cast<HWND>(lparam);
        SetTextColor(dc, state->text_color);
        const bool focused = GetFocus() == control;
        SetBkColor(dc, focused ? state->field_focus_color : state->field_color);
        return reinterpret_cast<LRESULT>(focused ? state->field_focus_brush : state->field_brush);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, state->text_color);
        SetBkColor(dc, state->panel_color);
        return reinterpret_cast<LRESULT>(state->list_brush);
    }
    case WM_MEASUREITEM: {
        auto *measure = reinterpret_cast<MEASUREITEMSTRUCT *>(lparam);
        if (measure && measure->CtlID == kList)
        {
            measure->itemHeight = DpiScale(h, (state->mode == ManagementMode::Devices ||
                                               (state->mode == ManagementMode::Settings &&
                                                state->settings_page == SettingsPage::Devices))
                                                  ? 64
                                                  : (IsSettingsProjectTypes(*state) ? 52 : 44));
            return TRUE;
        }
        return FALSE;
    }
    case WM_DRAWITEM: {
        auto *draw = reinterpret_cast<DRAWITEMSTRUCT *>(lparam);
        if (!draw)
            return TRUE;
        if (draw->CtlType == ODT_BUTTON)
            DrawButton(draw);
        else if (draw->CtlType == ODT_LISTBOX)
            DrawListItem(draw);
        return TRUE;
    }
    case WM_COMMAND:
        return HandleCommand(*state, wparam);
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            CloseModalSurface(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        CloseModalSurface(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, kDevicePresenceTimerId);
        KillTimer(h, kUpdateStatusTimerId);
        state->list_scrollbar.Detach();
        state->content_scrollbar.Detach();
        return 0;
    }
    return DefWindowProcW(h, message, wparam, lparam);
}

} // namespace management

ManagementDialogResult ShowManagementDialog(HWND owner, ManagementMode mode, DataStore &store,
                                            agent::RemoteAgent &agent,
                                            AgentRestartCallback restart_agent,
                                            ThemeChangedCallback theme_changed,
                                            AppearanceChangedCallback appearance_changed)
{
    using namespace management;
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = WindowProc;
        klass.lpszClassName = kClassName;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = nullptr;
        RegisterClassW(&klass);
        WNDCLASSW content_class{};
        content_class.hInstance = GetModuleHandleW(nullptr);
        content_class.lpfnWndProc = SettingsContentProc;
        content_class.lpszClassName = kSettingsContentClassName;
        content_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        content_class.hbrBackground = nullptr;
        RegisterClassW(&content_class);
        registered = true;
    }
    State state;
    state.owner = owner;
    state.mode = mode == ManagementMode::Devices ? ManagementMode::Settings : mode;
    state.settings_page =
        mode == ManagementMode::Devices ? SettingsPage::Devices : SettingsPage::Appearance;
    state.store = &store;
    state.agent = &agent;
    state.restart_agent = std::move(restart_agent);
    state.theme_changed = std::move(theme_changed);
    state.appearance_changed = std::move(appearance_changed);
    ApplyTheme(state, store.GetSetting(L"theme", L"dark") == L"light");
    state.font = CreateDialogFont(owner, 15, FW_NORMAL);
    state.title_font = CreateDialogFont(owner, 22, FW_SEMIBOLD);
    state.small_font = CreateDialogFont(owner, 13, FW_NORMAL);
    state.project_type_badge_font = CreateDialogFont(owner, 10, FW_NORMAL);
    // Settings uses a compact, explicit type scale independent of the other
    // management dialogs: 16 px navigation, 14 px content headings/controls,
    // and 12 px supporting text.
    if (state.mode == ManagementMode::Settings)
    {
        state.settings_title_font = CreateDialogFont(owner, 16, FW_SEMIBOLD);
        state.settings_nav_font = CreateDialogFont(owner, 16, FW_MEDIUM);
        state.settings_content_font = CreateDialogFont(owner, 14, FW_MEDIUM);
        state.settings_small_font = CreateDialogFont(owner, 12, FW_NORMAL);
        state.settings_icon_font = CreateDialogIconFont(owner, 14);
    }
    const int host_title_height = ModalTopForOwner(owner);
    ModalWorkspaceScope workspace(owner, host_title_height, &state.palette);
    state.window = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, kClassName, L"",
                                   WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, owner,
                                   nullptr, GetModuleHandleW(nullptr), &state);
    if (state.window)
    {
        workspace.AttachSurface(state.window);
        PopulateControls(state);
        aicli::ui::ActivateModalSurface(state.window);
        MSG message{};
        while (IsWindow(state.window) && GetMessageW(&message, nullptr, 0, 0))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (state.font)
        DeleteObject(state.font);
    if (state.title_font)
        DeleteObject(state.title_font);
    if (state.settings_title_font)
        DeleteObject(state.settings_title_font);
    if (state.settings_nav_font)
        DeleteObject(state.settings_nav_font);
    if (state.settings_content_font)
        DeleteObject(state.settings_content_font);
    if (state.settings_small_font)
        DeleteObject(state.settings_small_font);
    if (state.settings_icon_font)
        DeleteObject(state.settings_icon_font);
    if (state.project_type_badge_font)
        DeleteObject(state.project_type_badge_font);
    if (state.small_font)
        DeleteObject(state.small_font);
    if (state.field_brush)
        DeleteObject(state.field_brush);
    if (state.field_focus_brush)
        DeleteObject(state.field_focus_brush);
    if (state.list_brush)
        DeleteObject(state.list_brush);
    aicli::ui::RestoreModalOwner(owner);
    return state.result;
}

} // namespace aicli::ui
