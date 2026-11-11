#include "ui/internal/management_dialog_state.h"
#include "aicli_build_identity.h"
#include "app/powershell_runtime.h"
#include "ui/styled_input.h"
#include "ui/drawing_utils.h"
#include "ui/layout_metrics.h"
#include "ui/redraw_scope.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>

namespace aicli::ui::management
{

bool IsSettingsProjectTypes(const State &state)
{
    return state.mode == ManagementMode::ProjectTypes ||
           (state.mode == ManagementMode::Settings &&
            state.settings_page == SettingsPage::ProjectTypes);
}

bool IsSettingsGroups(const State &state)
{
    return state.mode == ManagementMode::Groups ||
           (state.mode == ManagementMode::Settings && state.settings_page == SettingsPage::Groups);
}

bool IsSettingsNavigationId(int id)
{
    return id == kSettingsAppearance || id == kSettingsConnection || id == kSettingsProjectTypes ||
           id == kSettingsGroups || id == kSettingsDevices || id == kSettingsAbout;
}

bool IsActiveSettingsNavigation(const State &state, int id)
{
    if (state.mode != ManagementMode::Settings)
        return false;
    if (id == kSettingsAppearance)
        return state.settings_page == SettingsPage::Appearance;
    if (id == kSettingsConnection)
        return state.settings_page == SettingsPage::Connection;
    if (id == kSettingsProjectTypes)
        return state.settings_page == SettingsPage::ProjectTypes;
    if (id == kSettingsGroups)
        return state.settings_page == SettingsPage::Groups;
    if (id == kSettingsDevices)
        return state.settings_page == SettingsPage::Devices;
    if (id == kSettingsAbout)
        return state.settings_page == SettingsPage::About;
    return false;
}

HWND SettingsNavigationControl(const State &state, SettingsPage page)
{
    switch (page)
    {
    case SettingsPage::Appearance:
        return state.settings_appearance;
    case SettingsPage::Connection:
        return state.settings_connection;
    case SettingsPage::ProjectTypes:
        return state.settings_project_types;
    case SettingsPage::Groups:
        return state.settings_groups;
    case SettingsPage::Devices:
        return state.settings_devices;
    case SettingsPage::About:
        return state.settings_about;
    }
    return nullptr;
}

COLORREF SettingsHeaderColor(const State &state)
{
    return state.light_theme ? RGB(250, 251, 253) : RGB(22, 26, 33);
}

LRESULT CALLBACK SettingsWheelProc(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                   UINT_PTR, DWORD_PTR reference)
{
    auto *state = reinterpret_cast<State *>(reference);
    const bool device_list = state && control == state->list &&
                             state->mode == ManagementMode::Settings &&
                             state->settings_page == SettingsPage::Devices;
    if (message == WM_MOUSEMOVE && device_list)
    {
        const LRESULT hit = SendMessageW(control, LB_ITEMFROMPOINT, 0, lparam);
        const int next = HIWORD(static_cast<DWORD_PTR>(hit))
                             ? -1
                             : static_cast<int>(LOWORD(static_cast<DWORD_PTR>(hit)));
        if (next != state->device_hover_row)
        {
            const int previous = state->device_hover_row;
            state->device_hover_row = next;
            RECT row{};
            if (previous >= 0 && SendMessageW(control, LB_GETITEMRECT, previous,
                                              reinterpret_cast<LPARAM>(&row)) != LB_ERR)
                InvalidateRect(control, &row, FALSE);
            if (next >= 0 && SendMessageW(control, LB_GETITEMRECT, next,
                                          reinterpret_cast<LPARAM>(&row)) != LB_ERR)
                InvalidateRect(control, &row, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
        TrackMouseEvent(&tracking);
    }
    if (message == WM_MOUSELEAVE && device_list)
    {
        const int previous = state->device_hover_row;
        state->device_hover_row = -1;
        RECT row{};
        if (previous >= 0 && SendMessageW(control, LB_GETITEMRECT, previous,
                                          reinterpret_cast<LPARAM>(&row)) != LB_ERR)
            InvalidateRect(control, &row, FALSE);
        return 0;
    }
    if (message == WM_MOUSEWHEEL && device_list)
    {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        const int top = static_cast<int>(SendMessageW(control, LB_GETTOPINDEX, 0, 0));
        const int next = std::max(0, top - (delta / WHEEL_DELTA) * 3);
        SendMessageW(control, LB_SETTOPINDEX, next, 0);
        state->list_scrollbar.NotifyActivity();
        state->list_scrollbar.Sync();
        return 0;
    }
    if (message == WM_MOUSEWHEEL && state && state->content)
    {
        // LISTBOX consumes wheel input even when its own rows do not need to
        // move. The list is part of the scrolling settings document, so route
        // wheel input from both its empty surface and owner-drawn items to the
        // content viewport.
        return SendMessageW(state->content, WM_MOUSEWHEEL, wparam, lparam);
    }
    if (message == WM_ERASEBKGND && state && control == state->list)
    {
        // Moving an owner-drawn LISTBOX does not make the stock control erase
        // the unused area below its final item. With wheel scrolling that left
        // the old last row visible at its previous screen position, appearing
        // as a duplicated project type/group. Clear the complete list surface
        // before its real rows are drawn.
        HDC dc = reinterpret_cast<HDC>(wparam);
        RECT client{};
        GetClientRect(control, &client);
        HBRUSH background = CreateSolidBrush(state->palette.list_surface);
        FillRect(dc, &client, background);
        DeleteObject(background);
        return 1;
    }
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(control, SettingsWheelProc, kSettingsWheelSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

int SettingsContentExtent(SettingsPage page)
{
    switch (page)
    {
    case SettingsPage::Appearance:
        return 430;
    case SettingsPage::Connection:
        return 635;
    case SettingsPage::ProjectTypes:
        return 590;
    case SettingsPage::Groups:
        return 590;
    case SettingsPage::Devices:
        return 520;
    case SettingsPage::About:
        return 636;
    }
    return 390;
}

void SetSettingsScroll(State &state, int position)
{
    if (!state.content)
        return;
    RECT client{};
    GetClientRect(state.content, &client);
    const int page = std::max(1L, client.bottom - client.top);
    const int maximum = std::max(0, state.content_extent - page);
    const int next = std::clamp(position, 0, maximum);
    if (state.content_scroll == next)
        return;
    state.content_scroll = next;
    LayoutSettingsContent(state);
}

void LayoutSettingsContent(State &state)
{
    if (!state.content || state.mode != ManagementMode::Settings)
        return;
    if (state.content_layout_active)
        return;
    ChildRedrawScope redraw(state.content);
    state.content_layout_active = true;
    RECT client{};
    GetClientRect(state.content, &client);
    const auto S = [&](int value) { return DpiScale(state.window, value); };
    int height = std::max(1L, client.bottom - client.top);
    state.content_extent = S(SettingsContentExtent(state.settings_page));
    state.content_scroll =
        std::clamp(state.content_scroll, 0, std::max(0, state.content_extent - height));
    const int width = std::max(1L, client.right - client.left);

    const int x = S(20);
    const int right = std::max(x + S(160), width - S(20));
    const int available = right - x;
    const int offset = state.content_scroll;
    auto field = [&](HWND control, int left, int y, int h = 40, int field_right = -1) {
        if (!control)
            return;
        if (field_right < 0)
            field_right = right;
        LayoutStyledInput(control, RECT{left, S(y) - offset, field_right, S(y + h) - offset}, S(2),
                          S(8));
    };
    auto button = [&](HWND control, int bx, int y, int w, int h = 38) {
        if (!control)
            return;
        // Resize without presenting a stock square BUTTON frame. Apply the
        // region first, then publish one owner-drawn rounded frame.
        SetWindowPos(control, nullptr, bx, S(y) - offset, w, S(h),
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW |
                         SWP_NOSENDCHANGING);
        ApplyRoundedRegion(control, w, S(h), S(9));
        RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
    };
    auto list = [&](HWND control, int y, int h) {
        if (!control)
            return;
        SetWindowPos(control, nullptr, x, S(y) - offset, available, S(h),
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW |
                         SWP_NOSENDCHANGING);
        ApplyRoundedRegion(control, available, S(h), S(9));
        RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    };

    if (state.settings_page == SettingsPage::Appearance)
    {
        const int right_inset = S(18);
        const int right_edge = right - right_inset;
        const int selector_width = S(300);
        const int selector_inset = S(2);
        const int selector_left = right_edge - selector_width;
        const int selector_inner_width = selector_width - selector_inset * 2;
        const int left_width = selector_inner_width / 2;
        const int right_width = selector_inner_width - left_width;
        auto segment = [&](HWND control, int left, int y, int width) {
            if (!control)
                return;
            SetWindowPos(control, nullptr, left, S(y + 2) - offset, width, S(32),
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW |
                             SWP_NOSENDCHANGING);
            SetWindowRgn(control, nullptr, TRUE);
            RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
        };
        auto segmented_pair = [&](HWND left_control, HWND right_control, int y) {
            segment(left_control, selector_left + selector_inset, y, left_width);
            segment(right_control, selector_left + selector_inset + left_width, y, right_width);
        };
        auto segmented_triplet = [&](HWND first_control, HWND second_control, HWND third_control,
                                     int y) {
            const int first_width = selector_inner_width / 3;
            const int second_width = selector_inner_width / 3;
            const int third_width = selector_inner_width - first_width - second_width;
            const int first_left = selector_left + selector_inset;
            segment(first_control, first_left, y, first_width);
            segment(second_control, first_left + first_width, y, second_width);
            segment(third_control, first_left + first_width + second_width, y, third_width);
        };
        segmented_pair(state.theme, state.theme_light, 88);
        segmented_triplet(state.taskbar_style_progress, state.taskbar_style_overlay,
                          state.taskbar_style_icon_dots, 200);
        segmented_pair(state.taskbar_scope_current, state.taskbar_scope_all, 256);

        if (state.taskbar_working)
        {
            const int toggle_width = S(50);
            const int toggle_height = S(28);
            SetWindowPos(state.taskbar_working, nullptr, right_edge - toggle_width, S(148) - offset,
                         toggle_width, toggle_height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW |
                             SWP_NOSENDCHANGING);
            SetWindowRgn(state.taskbar_working, nullptr, TRUE);
            RedrawWindow(state.taskbar_working, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
        }
        if (state.keep_awake)
        {
            const int toggle_width = S(50);
            const int toggle_height = S(28);
            SetWindowPos(state.keep_awake, nullptr, right_edge - toggle_width, S(312) - offset,
                         toggle_width, toggle_height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW |
                             SWP_NOSENDCHANGING);
            SetWindowRgn(state.keep_awake, nullptr, TRUE);
            RedrawWindow(state.keep_awake, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
        }
    }
    else if (state.settings_page == SettingsPage::Connection)
    {
        const int label_width = S(120);
        const int field_x = x + label_width;
        field(state.computer_name, field_x, 104);
        field(state.listen, field_x, 158);
        field(state.port, field_x, 212);
        field(state.local, field_x, 266);
        field(state.remote, field_x, 320);
        const int browse_width = S(86);
        const int browse_gap = S(8);
        field(state.mobile_workspace_root, field_x, 374, 40, right - browse_width - browse_gap);
        button(state.mobile_workspace_browse, right - browse_width, 375, browse_width, 38);
        button(state.mobile_access, field_x, 429, S(112), 36);
        button(state.firewall, x, 484, S(158));
        button(state.save, right - S(174), 484, S(174));
    }
    else if (state.settings_page == SettingsPage::ProjectTypes)
    {
        list(state.list, 126, 330);
        button(state.add, x, 492, S(122));
        button(state.save, x + S(134), 492, S(118));
        button(state.remove, x + S(264), 492, S(104));
    }
    else if (state.settings_page == SettingsPage::Groups)
    {
        list(state.list, 126, 330);
        button(state.add, x, 492, S(122));
        button(state.save, x + S(134), 492, S(118));
        button(state.remove, x + S(264), 492, S(104));
    }
    else if (state.settings_page == SettingsPage::About)
    {
        button(state.check_update, right - S(150), 346, S(126));
        button(state.download_update, right - S(288), 346, S(126));
    }
    else if (state.settings_page == SettingsPage::Devices)
    {
        const int list_width = std::min(S(306), available / 2);
        if (state.list)
        {
            SetWindowPos(state.list, nullptr, x, S(126) - offset, list_width, S(350),
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW);
            ApplyRoundedRegion(state.list, list_width, S(350), S(8));
        }
        button(state.select, right - S(122), 80, S(122), 36);
        button(state.unbind, right - S(122), 444, S(122), 36);
        const int detail_x = x + list_width + S(26);
        const int bind_width = S(158);
        const int bind_left = detail_x + std::max(0, (right - detail_x - bind_width) / 2);
        button(state.bind_empty, bind_left, 270, bind_width, 36);
    }
    state.list_scrollbar.Layout();
    state.content_scrollbar.Layout();
    InvalidateRect(state.content, nullptr, FALSE);
    state.content_layout_active = false;
}

void PaintSettingsContent(State &state, HDC dc)
{
    RECT client{};
    GetClientRect(state.content, &client);
    HBRUSH background = CreateSolidBrush(state.background_color);
    FillRect(dc, &client, background);
    DeleteObject(background);

    POINT previous_origin{};
    SetViewportOrgEx(dc, 0, -state.content_scroll, &previous_origin);
    SetBkMode(dc, TRANSPARENT);
    const auto S = [&](int value) { return DpiScale(state.window, value); };
    const int width = client.right;
    const int x = S(20);
    const int right = std::max(x + S(160), width - S(20));

    const wchar_t *title = L"外观设置";
    const wchar_t *subtitle = L"";
    if (state.settings_page == SettingsPage::Connection)
    {
        title = L"本机与网络";
        subtitle = L"配置手机访问服务、访问地址和手机工作目录。";
    }
    else if (state.settings_page == SettingsPage::ProjectTypes)
    {
        title = L"项目类型";
        subtitle = L"Codex 和 Claude 为预置类型；所有类型都可在这里维护名称和启动方式。";
    }
    else if (state.settings_page == SettingsPage::Groups)
    {
        title = L"分组管理";
        subtitle = L"用一个名称输入框新建或修改分组；默认分组始终保留。";
    }
    else if (state.settings_page == SettingsPage::Devices)
    {
        title = L"设备管理";
        subtitle = L"查看已授权手机、最近连接状态，或撤销设备访问权限。";
    }
    else if (state.settings_page == SettingsPage::About)
    {
        title = L"关于";
        subtitle = L"查看软件版本和当前交互终端运行环境。";
    }

    SelectObject(dc, state.settings_content_font ? state.settings_content_font : state.title_font);
    SetTextColor(dc, state.text_color);
    RECT title_rect{x, S(20), right, S(52)};
    DrawTextW(dc, title, -1, &title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (subtitle[0] != L'\0')
    {
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT subtitle_rect{x, S(54), right, S(84)};
        DrawTextW(dc, subtitle, -1, &subtitle_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    auto label = [&](const wchar_t *text, int y) {
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.palette.muted);
        RECT rect{x, S(y), right, S(y + 24)};
        DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    auto card = [&](RECT rect) {
        HBRUSH brush = CreateSolidBrush(state.panel_color);
        HPEN pen = CreatePen(PS_SOLID, 1, state.panel_edge);
        HGDIOBJ old_brush = SelectObject(dc, brush);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, S(12), S(12));
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    };

    if (state.settings_page == SettingsPage::Appearance)
    {
        card({x, S(64), right, S(366)});
        const int selector_right = right - S(18);
        const int selector_left = selector_right - S(300);
        const COLORREF selector_fill = state.light_theme ? RGB(238, 242, 247) : RGB(25, 29, 37);
        const COLORREF selector_edge = state.light_theme ? RGB(215, 221, 230) : RGB(45, 51, 62);
        auto selector = [&](int y) {
            const RECT bounds{selector_left, S(y), selector_right, S(y + 36)};
            DrawSmoothRoundedRect(dc, bounds, static_cast<float>(S(8)), selector_fill,
                                  selector_edge, 1.0f);
        };
        selector(88);
        selector(200);
        selector(256);
        SelectObject(dc, state.settings_content_font ? state.settings_content_font : state.font);
        SetTextColor(dc, state.text_color);
        auto row_label = [&](const wchar_t *text, int y) {
            RECT rect{x + S(20), S(y), right - S(346), S(y + 36)};
            DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        };
        row_label(L"应用主题", 88);
        row_label(L"任务栏缓冲动画", 144);
        row_label(L"缓冲样式", 200);
        row_label(L"作用范围", 256);
        row_label(L"任务运行中保持唤醒", 312);
        HPEN divider = CreatePen(PS_SOLID, 1, state.panel_edge);
        HGDIOBJ old_pen = SelectObject(dc, divider);
        for (int y : {136, 192, 248, 304})
        {
            MoveToEx(dc, x + S(20), S(y), nullptr);
            LineTo(dc, right - S(20), S(y));
        }
        SelectObject(dc, old_pen);
        DeleteObject(divider);
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT sleep_hint{x, S(382), right, S(422)};
        DrawTextW(dc, L"最后一个任务结束后，重新等待系统设置的休眠时间。", -1, &sleep_hint,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    else if (state.settings_page == SettingsPage::Connection)
    {
        auto row_label = [&](const wchar_t *text, int y) {
            SelectObject(dc,
                         state.settings_content_font ? state.settings_content_font : state.font);
            SetTextColor(dc, state.text_color);
            RECT rect{x, S(y), x + S(120), S(y + 40)};
            DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        };
        row_label(L"本机名称", 104);
        row_label(L"监听地址", 158);
        row_label(L"HTTPS/WSS 端口", 212);
        row_label(L"内网访问地址", 266);
        row_label(L"公网/Relay 地址", 320);
        row_label(L"手机工作目录", 374);
        row_label(L"手机访问服务", 428);
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT hint{x, S(538), right, S(610)};
        DrawTextW(dc,
                  L"开启后才会监听 HTTPS/WSS "
                  L"端口；关闭会断开手机连接并清理临时授权。手机项目和附件分别保存在工作目录下。",
                  -1, &hint, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    else if (state.settings_page == SettingsPage::ProjectTypes)
    {
        label(L"项目类型列表", 96);
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT hint{x, S(548), right, S(578)};
        DrawTextW(
            dc,
            L"新增和修改会打开独立模态框；Codex 和 Claude 虽为预置类型，也可手工调整，但不能删除。",
            -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    else if (state.settings_page == SettingsPage::Groups)
    {
        label(L"分组列表", 96);
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT hint{x, S(548), right, S(578)};
        DrawTextW(dc, L"添加和修改名称会打开独立输入模态层；非空分组不能删除。", -1, &hint,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    else if (state.settings_page == SettingsPage::About)
    {
        card({x, S(104), right, S(312)});
        SelectObject(dc, state.title_font ? state.title_font : state.settings_content_font);
        SetTextColor(dc, state.text_color);
        RECT app_name{x + S(24), S(118), right - S(24), S(154)};
        DrawTextW(dc, L"AiCli", -1, &app_name, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT app_detail{x + S(24), S(150), right - S(24), S(176)};
        DrawTextW(dc, L"AiCli 桌面工作区", -1, &app_detail, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        HPEN divider = CreatePen(PS_SOLID, 1, state.panel_edge);
        HGDIOBJ old_divider = SelectObject(dc, divider);
        MoveToEx(dc, x + S(24), S(184), nullptr);
        LineTo(dc, right - S(24), S(184));
        SelectObject(dc, old_divider);
        DeleteObject(divider);

        const auto runtime = aicli::app::PowerShellRuntimeInfo();
        const std::wstring terminal = runtime.IsPowerShell7()
                                          ? runtime.DisplayName()
                                          : runtime.DisplayName() + L"（兼容模式）";
        auto row = [&](const wchar_t *key, const std::wstring &value, int y) {
            SelectObject(dc,
                         state.settings_small_font ? state.settings_small_font : state.small_font);
            SetTextColor(dc, state.muted_color);
            RECT key_rect{x + S(24), S(y), x + S(132), S(y + 34)};
            DrawTextW(dc, key, -1, &key_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc,
                         state.settings_content_font ? state.settings_content_font : state.font);
            SetTextColor(dc, state.text_color);
            RECT value_rect{x + S(136), S(y), right - S(24), S(y + 34)};
            DrawTextW(dc, value.c_str(), -1, &value_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        };
        row(L"软件名称", L"AiCli", 194);
        row(L"版本号", AICLI_VERSION, 232);
        row(L"终端环境", terminal, 270);

        SelectObject(dc, state.settings_content_font ? state.settings_content_font : state.font);
        SetTextColor(dc, state.text_color);
        RECT detection_title{x, S(488), right, S(516)};
        DrawTextW(dc, L"PowerShell 检测说明", -1, &detection_title,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.muted_color);
        std::wstring detection_note =
            runtime.IsPowerShell7()
                ? L"已自动使用检测到的 PowerShell 7。"
                : L"当前未检测到 PowerShell 7，正在使用 Windows PowerShell 5.1 兼容模式。";
        detection_note += L" AiCli 支持常规安装和 Microsoft Store 版；如未自动识别到，请把 "
                          L"pwsh.exe 所在文件夹加入 PATH。"
                          L"安装、升级或修改 PATH 后，请完全退出并重启 AiCli。";
        RECT hint{x, S(520), right, S(616)};
        DrawTextW(dc, detection_note.c_str(), -1, &hint, DT_LEFT | DT_TOP | DT_WORDBREAK);
        card({x, S(328), right, S(470)});
        SelectObject(dc, state.settings_content_font ? state.settings_content_font : state.font);
        SetTextColor(dc, state.text_color);
        RECT update_title{x + S(24), S(340), right - S(300), S(376)};
        DrawTextW(dc, L"软件更新", -1, &update_title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const auto update_state = state.update_status.state;
        SetTextColor(dc, update_state == aicli::update::CheckState::UpToDate ? state.palette.success
                         : update_state == aicli::update::CheckState::Available
                             ? state.palette.accent
                             : state.text_color);
        RECT update_result{x + S(24), S(394), right - S(24), S(434)};
        DrawTextW(dc, state.update_status.text.c_str(), -1, &update_result,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT update_note{x + S(24), S(436), right - S(24), S(462)};
        DrawTextW(dc, L"启动后自动检查，安装前会提示确认。", -1, &update_note,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    else
    {
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.palette.muted);
        const std::wstring list_title =
            L"已授权设备（" + std::to_wstring(state.devices.size()) + L"）";
        RECT list_title_rect{x, S(96), right, S(120)};
        DrawTextW(dc, list_title.c_str(), -1, &list_title_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const int list_width = std::min(S(306), (right - x) / 2);
        const int detail_x = x + list_width + S(26);
        size_t index{};
        const bool has_device = SelectedIndex(state.list, index) && index < state.devices.size();
        if (!has_device)
        {
            SelectObject(dc, state.settings_icon_font ? state.settings_icon_font : state.font);
            SetTextColor(dc, state.muted_color);
            RECT empty_icon{detail_x, S(166), right, S(218)};
            DrawTextW(dc, L"\xE8EA", -1, &empty_icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc,
                         state.settings_content_font ? state.settings_content_font : state.font);
            SetTextColor(dc, state.muted_color);
            RECT empty{detail_x, S(216), right, S(258)};
            DrawTextW(dc,
                      state.devices.empty() ? L"当前没有已绑定手机" : L"选择一台设备查看详细信息",
                      -1, &empty, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        }
        else
        {
            const auto &device = state.devices[index];
            const bool online = state.agent && state.agent->IsDeviceOnline(device.phone_id);
            SelectObject(dc,
                         state.settings_content_font ? state.settings_content_font : state.font);
            SetTextColor(dc, state.text_color);
            RECT device_name{detail_x, S(126), right - S(82), S(158)};
            DrawTextW(dc, device.name.c_str(), -1, &device_name,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            const COLORREF badge_text = online ? state.palette.success : state.palette.muted;
            const COLORREF badge_fill =
                online ? (state.light_theme ? RGB(229, 247, 239) : RGB(24, 54, 45))
                       : (state.light_theme ? RGB(239, 242, 246) : RGB(31, 36, 44));
            RECT badge{right - S(70), S(128), right, S(154)};
            DrawSmoothRoundedRect(dc, badge, static_cast<float>(S(7)), badge_fill, badge_fill,
                                  0.0f);
            SelectObject(dc,
                         state.settings_small_font ? state.settings_small_font : state.small_font);
            SetTextColor(dc, badge_text);
            DrawTextW(dc, online ? L"●  在线" : L"○  离线", -1, &badge,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SetTextColor(dc, state.muted_color);
            RECT section{detail_x, S(166), right, S(188)};
            DrawTextW(dc, L"设备信息", -1, &section, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc,
                         state.settings_small_font ? state.settings_small_font : state.small_font);
            auto row = [&](const wchar_t *key, const std::wstring &value, int y) {
                SetTextColor(dc, state.muted_color);
                RECT key_rect{detail_x, S(y), detail_x + S(88), S(y + 28)};
                DrawTextW(dc, key, -1, &key_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SetTextColor(dc, state.text_color);
                RECT value_rect{detail_x + S(92), S(y), right, S(y + 28)};
                DrawTextW(dc, value.c_str(), -1, &value_rect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            };
            row(L"设备型号", device.model, 194);
            row(L"设备 ID", device.phone_id, 230);
            row(L"绑定时间", FormatDeviceTime(device.paired_at), 266);
            row(L"最近在线", FormatDeviceTime(device.last_seen), 302);
            row(L"授权状态", device.active ? L"已授权" : L"已解绑", 338);
            HPEN divider = CreatePen(PS_SOLID, 1, state.panel_edge);
            HGDIOBJ old_divider = SelectObject(dc, divider);
            MoveToEx(dc, detail_x, S(382), nullptr);
            LineTo(dc, right, S(382));
            SelectObject(dc, old_divider);
            DeleteObject(divider);
            SelectObject(dc,
                         state.settings_content_font ? state.settings_content_font : state.font);
            SetTextColor(dc, state.text_color);
            RECT danger_title{detail_x, S(390), right, S(414)};
            DrawTextW(dc, L"撤销设备访问权限", -1, &danger_title,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc,
                         state.settings_small_font ? state.settings_small_font : state.small_font);
            SetTextColor(dc, state.muted_color);
            RECT danger_text{detail_x, S(416), right - S(138), S(476)};
            DrawTextW(dc, L"解绑后，该手机必须重新扫码并在电脑上确认。", -1, &danger_text,
                      DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    }
    SetViewportOrgEx(dc, previous_origin.x, previous_origin.y, nullptr);
}

void RefreshUpdateStatus(State &state, bool force)
{
    const auto status = aicli::update::GetCheckStatus();
    if (!force && status == state.update_status)
        return;
    state.update_status = status;
    const bool checking = status.state == aicli::update::CheckState::Checking;
    SetWindowTextW(state.check_update, checking ? L"正在检查…"
                                       : status.state == aicli::update::CheckState::Failed
                                           ? L"重试检查"
                                           : L"检查更新");
    EnableWindow(state.check_update, !checking);
    ShowWindow(state.download_update, state.settings_page == SettingsPage::About &&
                                              status.state == aicli::update::CheckState::Available
                                          ? SW_SHOW
                                          : SW_HIDE);
    InvalidateRect(state.content, nullptr, FALSE);
}

void UpdateSettingsPage(State &state, SettingsPage page)
{
    if (state.mode != ManagementMode::Settings)
        return;
    ChildRedrawScope redraw(state.content);
    const SettingsPage previous_page = state.settings_page;
    state.settings_page = page;
    state.content_scroll = 0;
    const bool appearance = page == SettingsPage::Appearance;
    const bool connection = page == SettingsPage::Connection;
    const bool project_types = page == SettingsPage::ProjectTypes;
    const bool groups = page == SettingsPage::Groups;
    const bool devices = page == SettingsPage::Devices;
    const bool has_active_device =
        std::any_of(state.devices.begin(), state.devices.end(),
                    [](const DeviceRecord &device) { return device.active; });
    state.device_hover_row = -1;
    if (state.list)
        SendMessageW(state.list, LB_SETITEMHEIGHT, 0,
                     DpiScale(state.window, devices ? 64 : (project_types ? 52 : 44)));
    auto visible = [](HWND control, bool show) {
        if (control)
            ShowWindow(control, show ? SW_SHOW : SW_HIDE);
    };
    visible(state.theme, appearance);
    visible(state.theme_light, appearance);
    visible(state.taskbar_working, appearance);
    visible(state.taskbar_style_progress, appearance);
    visible(state.taskbar_style_overlay, appearance);
    visible(state.taskbar_style_icon_dots, appearance);
    visible(state.taskbar_scope_current, appearance);
    visible(state.taskbar_scope_all, appearance);
    visible(state.keep_awake, appearance);
    visible(state.check_update, page == SettingsPage::About);
    visible(state.download_update, false);
    if (page == SettingsPage::About)
    {
        RefreshUpdateStatus(state, true);
        SetTimer(state.window, kUpdateStatusTimerId, 250, nullptr);
    }
    else
    {
        KillTimer(state.window, kUpdateStatusTimerId);
    }
    // The project/group/device list overlaps the connection form. Hide it
    // before revealing connection fields; otherwise each EDIT receives its
    // first WM_PAINT while still clipped by the higher-z-order list, and the
    // exposed text can remain only half painted after the list disappears.
    if (connection)
        visible(state.list, false);
    visible(state.computer_name, connection);
    visible(state.listen, connection);
    visible(state.port, connection);
    visible(state.local, connection);
    visible(state.remote, connection);
    visible(state.mobile_workspace_root, connection);
    visible(state.mobile_workspace_browse, connection);
    visible(state.mobile_access, connection);
    visible(state.firewall, connection);
    visible(state.save, connection || project_types || groups);
    if (!connection)
        visible(state.list, project_types || groups || devices);
    visible(state.name, false);
    visible(state.command, false);
    visible(state.add, project_types || groups);
    visible(state.remove, project_types || groups);
    visible(state.select, devices);
    visible(state.unbind, devices && has_active_device);
    visible(state.bind_empty, devices && state.devices.empty());
    if (state.save)
        EnableWindow(state.save, TRUE);
    if (state.remove)
        EnableWindow(state.remove, TRUE);
    if (project_types)
    {
        SetWindowTextW(state.add, L"新增类型");
        SetWindowTextW(state.save, L"修改类型");
        SetWindowTextW(state.remove, L"删除");
        RefreshList(state);
        ReadSelection(state);
    }
    else if (connection)
    {
        SetWindowTextW(state.save, L"保存设置");
    }
    else if (groups)
    {
        SetWindowTextW(state.add, L"添加分组");
        SetWindowTextW(state.save, L"修改名称");
        SetWindowTextW(state.remove, L"删除");
        RefreshList(state);
        ReadSelection(state);
    }
    else if (devices)
    {
        RefreshList(state);
        ReadSelection(state);
    }
    LayoutSettingsContent(state);
    if (HWND previous = SettingsNavigationControl(state, previous_page))
        InvalidateRect(previous, nullptr, FALSE);
    if (HWND current = SettingsNavigationControl(state, page);
        current && current != SettingsNavigationControl(state, previous_page))
        InvalidateRect(current, nullptr, FALSE);
    InvalidateRect(state.content, nullptr, FALSE);
    state.list_scrollbar.Sync();
}

LRESULT CALLBACK SettingsContentProc(HWND h, UINT message, WPARAM wparam, LPARAM lparam)
{
    State *state = GetState(h);
    if (message == WM_NCCREATE)
    {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        state = static_cast<State *>(create->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(h, message, wparam, lparam);
    switch (message)
    {
    case WM_SIZE:
        LayoutSettingsContent(*state);
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        state->content_scrollbar.NotifyActivity();
        SetSettingsScroll(*state, state->content_scroll - (delta / WHEEL_DELTA) * 52);
        return 0;
    }
    case WM_VSCROLL: {
        RECT client{};
        GetClientRect(h, &client);
        int next = state->content_scroll;
        switch (LOWORD(wparam))
        {
        case SB_LINEUP:
            next -= 40;
            break;
        case SB_LINEDOWN:
            next += 40;
            break;
        case SB_PAGEUP:
            next -= std::max(80L, client.bottom - 60);
            break;
        case SB_PAGEDOWN:
            next += std::max(80L, client.bottom - 60);
            break;
        case SB_TOP:
            next = 0;
            break;
        case SB_BOTTOM:
            next = state->content_extent;
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO info{sizeof(info)};
            info.fMask = SIF_TRACKPOS;
            GetScrollInfo(h, SB_VERT, &info);
            next = info.nTrackPos;
            break;
        }
        default:
            return 0;
        }
        SetSettingsScroll(*state, next);
        return 0;
    }
    case WM_COMMAND:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return SendMessageW(state->window, message, wparam, lparam);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(h, &paint);
        {
            RECT client{};
            GetClientRect(h, &client);
            BufferedWindowPaint buffered(dc, client, paint.rcPaint);
            PaintSettingsContent(*state, buffered.dc());
        }
        EndPaint(h, &paint);
        return 0;
    }
    }
    return DefWindowProcW(h, message, wparam, lparam);
}

} // namespace aicli::ui::management
