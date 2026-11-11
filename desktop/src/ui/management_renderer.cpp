#include "ui/internal/management_dialog_state.h"
#include "ui/drawing_utils.h"
#include "ui/layout_metrics.h"
#include "ui/modal_host.h"
#include <algorithm>
#include <ctime>
#include <iterator>

namespace aicli::ui::management
{

COLORREF BlendBackdrop(COLORREF base, COLORREF overlay, BYTE alpha)
{
    const auto blend = [alpha](BYTE from, BYTE to) -> BYTE {
        return static_cast<BYTE>(
            (static_cast<unsigned>(from) * (255u - alpha) + static_cast<unsigned>(to) * alpha) /
            255u);
    };
    return RGB(blend(GetRValue(base), GetRValue(overlay)),
               blend(GetGValue(base), GetGValue(overlay)),
               blend(GetBValue(base), GetBValue(overlay)));
}

std::wstring FormatDeviceTime(std::int64_t timestamp)
{
    if (timestamp <= 0)
        return L"从未";
    const std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm local{};
    if (localtime_s(&local, &value) != 0)
        return L"未知";
    wchar_t text[32]{};
    if (wcsftime(text, std::size(text), L"%Y-%m-%d %H:%M", &local) == 0)
        return L"未知";
    return text;
}

const ThemePalette &DarkPalette()
{
    static const ThemePalette palette = MakeThemePalette(ThemeMode::Dark);
    return palette;
}

void DrawButton(const DRAWITEMSTRUCT *item)
{
    if (!item)
        return;
    RECT client{};
    GetClientRect(item->hwndItem, &client);
    BufferedWindowPaint buffered(item->hDC, client, item->rcItem);
    DRAWITEMSTRUCT buffered_item = *item;
    buffered_item.hDC = buffered.dc();
    item = &buffered_item;
    State *state = FindState(item->hwndItem);
    const COLORREF text_color = state ? state->text_color : DarkPalette().text;
    const COLORREF muted_color = state ? state->muted_color : DarkPalette().muted;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool hovered = state && state->hovered_control == item->hwndItem;
    const bool active_navigation = state && IsActiveSettingsNavigation(*state, item->CtlID);
    const bool active_theme =
        state &&
        ((item->CtlID == kTheme && !state->light_theme) ||
         (item->CtlID == kThemeLight && state->light_theme) ||
         (item->CtlID == kTaskbarWorking &&
          state->store->GetSetting(L"taskbar_working_enabled", L"1") != L"0") ||
         (item->CtlID == kKeepAwake &&
          state->store->GetSetting(L"keep_awake_while_working", L"0") != L"0") ||
         (item->CtlID == kTaskbarStyleProgress &&
          state->store->GetSetting(L"taskbar_working_style", L"progress") != L"overlay" &&
          state->store->GetSetting(L"taskbar_working_style", L"progress") != L"icon_dots") ||
         (item->CtlID == kTaskbarStyleOverlay &&
          state->store->GetSetting(L"taskbar_working_style", L"progress") == L"overlay") ||
         (item->CtlID == kTaskbarStyleIconDots &&
          state->store->GetSetting(L"taskbar_working_style", L"progress") == L"icon_dots") ||
         (item->CtlID == kTaskbarScopeCurrent &&
          state->store->GetSetting(L"taskbar_working_scope", L"all") == L"current") ||
         (item->CtlID == kTaskbarScopeAll &&
          state->store->GetSetting(L"taskbar_working_scope", L"all") != L"current") ||
         (item->CtlID == kMobileAccess && state->mobile_access_enabled));
    const bool active = active_navigation || active_theme;
    RECT rect = item->rcItem;
    const bool light = state && state->light_theme;
    const auto S = [&](int value) { return state ? DpiScale(state->window, value) : value; };

    const bool title_close = state && state->title_close == item->hwndItem;
    if (title_close && state)
    {
        // Keep the idle control visually part of the title area, then add a
        // compact rounded hover layer without restoring the old square halo.
        HBRUSH clear =
            CreateSolidBrush(state->mode == ManagementMode::Settings ? SettingsHeaderColor(*state)
                                                                     : state->panel_color);
        FillRect(item->hDC, &rect, clear);
        DeleteObject(clear);
        if (hovered || pressed)
        {
            const COLORREF close_fill = pressed ? (light ? RGB(241, 218, 222) : RGB(70, 39, 45))
                                                : (light ? RGB(246, 232, 235) : RGB(54, 38, 43));
            HBRUSH hover_brush = CreateSolidBrush(close_fill);
            HGDIOBJ old_brush = SelectObject(item->hDC, hover_brush);
            HGDIOBJ old_pen = SelectObject(item->hDC, GetStockObject(NULL_PEN));
            RoundRect(item->hDC, rect.left + S(3), rect.top + S(3), rect.right - S(3),
                      rect.bottom - S(3), S(8), S(8));
            SelectObject(item->hDC, old_pen);
            SelectObject(item->hDC, old_brush);
            DeleteObject(hover_brush);
        }
        SetBkMode(item->hDC, TRANSPARENT);
        SelectObject(item->hDC, state->title_font);
        SetTextColor(item->hDC, pressed ? (light ? RGB(170, 44, 58) : RGB(255, 145, 155))
                                        : (hovered ? (light ? RGB(184, 54, 67) : RGB(255, 132, 143))
                                                   : state->muted_color));
        RECT close_rect = rect;
        OffsetRect(&close_rect, 0, -S(2));
        DrawTextW(item->hDC, L"\x00D7", -1, &close_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    const bool settings_navigation = state && IsSettingsNavigationId(item->CtlID);
    if (settings_navigation)
    {
        // The navigation buttons are transparent sidebar items. Clear the
        // complete owner-draw surface with the sidebar color, then draw only
        // a rounded interaction layer. This removes the square halo around
        // every item while retaining a clear hover/selection response.
        HBRUSH clear = CreateSolidBrush(state->panel_color);
        FillRect(item->hDC, &rect, clear);
        DeleteObject(clear);

        {
            COLORREF nav_fill = active_navigation
                                    ? (light ? RGB(226, 237, 251) : RGB(31, 57, 87))
                                    : (hovered ? (light ? RGB(239, 243, 248) : RGB(32, 37, 45))
                                               : (light ? RGB(246, 248, 251) : RGB(25, 29, 37)));
            if (pressed)
                nav_fill = light ? RGB(218, 230, 246) : RGB(37, 50, 68);
            HBRUSH nav_brush = CreateSolidBrush(nav_fill);
            HGDIOBJ old_brush = SelectObject(item->hDC, nav_brush);
            HGDIOBJ old_pen = SelectObject(item->hDC, GetStockObject(NULL_PEN));
            RoundRect(item->hDC, rect.left + S(2), rect.top + S(2), rect.right - S(2),
                      rect.bottom - S(2), S(8), S(8));
            SelectObject(item->hDC, old_pen);
            SelectObject(item->hDC, old_brush);
            DeleteObject(nav_brush);
        }

        wchar_t text[256]{};
        GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
        const wchar_t *icon = item->CtlID == kSettingsAppearance     ? L"\xE790"
                              : item->CtlID == kSettingsConnection   ? L"\xE774"
                              : item->CtlID == kSettingsProjectTypes ? L"\xE756"
                              : item->CtlID == kSettingsGroups       ? L"\xE8B7"
                                                                     : L"\xE8EA";
        SetBkMode(item->hDC, TRANSPARENT);
        SelectObject(item->hDC, state->settings_icon_font ? state->settings_icon_font
                                                          : state->settings_nav_font);
        SetTextColor(item->hDC, active_navigation ? state->accent_color : muted_color);
        RECT icon_rect{rect.left + S(13), rect.top, rect.left + S(37), rect.bottom};
        DrawTextW(item->hDC, icon, -1, &icon_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item->hDC, state->settings_nav_font ? state->settings_nav_font : state->font);
        SetTextColor(item->hDC, disabled ? muted_color : text_color);
        RECT title_rect{rect.left + S(44), rect.top, rect.right - S(28), rect.bottom};
        DrawTextW(item->hDC, text, -1, &title_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(item->hDC, active_navigation ? state->accent_color : muted_color);
        SelectObject(item->hDC, state->settings_nav_font ? state->settings_nav_font : state->font);
        RECT chevron{rect.right - S(27), rect.top, rect.right - S(8), rect.bottom};
        DrawTextW(item->hDC, L">", -1, &chevron, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    const bool taskbar_toggle =
        state && (item->CtlID == kTaskbarWorking || item->CtlID == kKeepAwake);
    if (taskbar_toggle)
    {
        // A binary setting reads more naturally as a compact switch than as
        // a second action button. Keep the full HWND rectangular so the
        // anti-aliased track can blend into the card without clipped edges.
        HBRUSH clear = CreateSolidBrush(state->panel_color);
        FillRect(item->hDC, &rect, clear);
        DeleteObject(clear);

        COLORREF track = active_theme ? (light ? RGB(70, 137, 218) : RGB(47, 103, 166))
                                      : (light ? RGB(199, 207, 218) : RGB(53, 59, 70));
        if (hovered)
        {
            track = active_theme ? (light ? RGB(61, 128, 211) : RGB(55, 119, 188))
                                 : (light ? RGB(185, 195, 208) : RGB(63, 71, 84));
        }
        if (pressed)
        {
            track = active_theme ? (light ? RGB(51, 113, 191) : RGB(42, 91, 147))
                                 : (light ? RGB(174, 185, 199) : RGB(46, 52, 62));
        }
        const COLORREF track_edge = active_theme ? (light ? RGB(55, 119, 202) : RGB(70, 132, 202))
                                                 : (light ? RGB(180, 189, 201) : RGB(70, 78, 91));
        RECT track_rect{rect.left + S(1), rect.top + S(1), rect.right - S(1), rect.bottom - S(1)};
        DrawSmoothRoundedRect(item->hDC, track_rect, static_cast<float>(S(13)), track, track_edge,
                              1.0f);

        const int knob_size = S(18);
        const int knob_margin = S(4);
        const int knob_left =
            active_theme ? rect.right - knob_margin - knob_size : rect.left + knob_margin;
        RECT knob{knob_left, rect.top + (rect.bottom - rect.top - knob_size) / 2,
                  knob_left + knob_size, rect.top + (rect.bottom - rect.top + knob_size) / 2};
        const COLORREF knob_fill = light ? RGB(255, 255, 255) : RGB(232, 238, 247);
        const COLORREF knob_edge = light ? RGB(225, 230, 237) : RGB(195, 207, 222);
        DrawSmoothRoundedRect(item->hDC, knob, static_cast<float>(knob_size / 2), knob_fill,
                              knob_edge, 1.0f);
        return;
    }

    const bool segmented_choice =
        state && (item->CtlID == kTheme || item->CtlID == kThemeLight ||
                  item->CtlID == kTaskbarStyleProgress || item->CtlID == kTaskbarStyleOverlay ||
                  item->CtlID == kTaskbarStyleIconDots || item->CtlID == kTaskbarScopeCurrent ||
                  item->CtlID == kTaskbarScopeAll);
    if (segmented_choice)
    {
        // Segmented settings share one rounded base. Each child paints only
        // its selected or hover interaction layer.
        const COLORREF selector_fill = light ? RGB(238, 242, 247) : RGB(25, 29, 37);
        const COLORREF hover_fill = light ? RGB(229, 235, 243) : RGB(34, 40, 51);
        const COLORREF selected_fill = light ? RGB(226, 237, 251) : RGB(32, 58, 90);
        const COLORREF selected_edge = light ? RGB(190, 210, 236) : RGB(48, 82, 122);
        const COLORREF selected_text = light ? RGB(39, 102, 190) : RGB(221, 235, 255);

        HBRUSH clear = CreateSolidBrush(selector_fill);
        FillRect(item->hDC, &rect, clear);
        DeleteObject(clear);

        COLORREF segment_fill = selector_fill;
        COLORREF segment_edge = selector_fill;
        float edge_width = 0.0f;
        if (active_theme)
        {
            segment_fill = pressed ? (light ? RGB(214, 229, 247) : RGB(28, 51, 79)) : selected_fill;
            segment_edge = selected_edge;
            edge_width = 1.0f;
        }
        else if (hovered || pressed)
        {
            segment_fill = pressed ? (light ? RGB(220, 228, 238) : RGB(39, 46, 59)) : hover_fill;
        }

        if (active_theme || hovered || pressed)
        {
            RECT layer{rect.left + S(1), rect.top + S(1), rect.right - S(1), rect.bottom - S(1)};
            DrawSmoothRoundedRect(item->hDC, layer, static_cast<float>(S(7)), segment_fill,
                                  segment_edge, edge_width);
        }

        wchar_t text[256]{};
        GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
        const wchar_t *icon = item->CtlID == kTheme                  ? L"\xE708"
                              : item->CtlID == kThemeLight           ? L"\xE706"
                              : item->CtlID == kTaskbarStyleOverlay  ? L"\xE895"
                              : item->CtlID == kTaskbarStyleIconDots ? L"\xE712"
                              : item->CtlID == kTaskbarScopeCurrent  ? L"\xE8B7"
                              : item->CtlID == kTaskbarScopeAll      ? L"\xE8A5"
                                                                     : nullptr;
        const HFONT icon_font = state->settings_icon_font ? state->settings_icon_font : state->font;
        const HFONT text_font =
            state->settings_content_font ? state->settings_content_font : state->font;
        SIZE icon_size{};
        SIZE text_size{};
        if (icon)
        {
            SelectObject(item->hDC, icon_font);
            GetTextExtentPoint32W(item->hDC, icon, 1, &icon_size);
        }
        else
        {
            icon_size.cx = S(15);
        }
        SelectObject(item->hDC, text_font);
        GetTextExtentPoint32W(item->hDC, text, static_cast<int>(wcslen(text)), &text_size);
        const int gap = S(7);
        const int content_width = icon_size.cx + gap + text_size.cx;
        const int segment_width = static_cast<int>(rect.right - rect.left);
        const int content_left = rect.left + std::max(0, (segment_width - content_width) / 2);

        SetBkMode(item->hDC, TRANSPARENT);
        const COLORREF foreground =
            disabled ? muted_color
                     : (active_theme ? selected_text : (hovered ? text_color : muted_color));
        SetTextColor(item->hDC, foreground);
        RECT icon_rect{content_left, rect.top, content_left + icon_size.cx, rect.bottom};
        if (icon)
        {
            SelectObject(item->hDC, icon_font);
            DrawTextW(item->hDC, icon, 1, &icon_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        else
        {
            // Compact vector progress glyph; it stays crisp at every DPI and
            // avoids relying on an unrelated font symbol.
            const int glyph_height = S(5);
            const int glyph_top = rect.top + (rect.bottom - rect.top - glyph_height) / 2;
            RECT progress_track{icon_rect.left, glyph_top, icon_rect.right,
                                glyph_top + glyph_height};
            const COLORREF progress_back = active_theme
                                               ? BlendBackdrop(selected_fill, foreground, 54)
                                               : BlendBackdrop(selector_fill, foreground, 44);
            DrawSmoothRoundedRect(item->hDC, progress_track, static_cast<float>(S(2)),
                                  progress_back, progress_back, 0.0f);
            RECT progress_fill = progress_track;
            const int progress_width = static_cast<int>(progress_track.right - progress_track.left);
            progress_fill.right = progress_fill.left + std::max(S(5), progress_width * 3 / 5);
            DrawSmoothRoundedRect(item->hDC, progress_fill, static_cast<float>(S(2)), foreground,
                                  foreground, 0.0f);
        }
        SelectObject(item->hDC, text_font);
        RECT text_rect{icon_rect.right + gap, rect.top, rect.right, rect.bottom};
        DrawTextW(item->hDC, text, -1, &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    COLORREF fill = light ? RGB(245, 247, 250) : RGB(38, 42, 50);
    COLORREF edge = light ? RGB(210, 217, 226) : RGB(58, 65, 76);
    if (hovered)
    {
        fill = light ? RGB(234, 240, 248) : RGB(44, 51, 62);
        edge = light ? RGB(171, 189, 213) : RGB(73, 92, 119);
    }
    if (pressed)
    {
        fill = light ? RGB(218, 230, 246) : RGB(37, 76, 128);
        edge = light ? RGB(121, 158, 207) : RGB(82, 143, 220);
    }
    if (active)
    {
        fill = light ? RGB(226, 237, 251) : RGB(31, 57, 87);
        edge = light ? RGB(107, 153, 213) : RGB(59, 111, 178);
    }
    if (item->CtlID == kDelete)
    {
        fill = light ? RGB(253, 240, 242) : RGB(56, 40, 45);
        edge = light ? RGB(225, 170, 178) : RGB(116, 65, 77);
        if (hovered || pressed)
        {
            fill = light ? RGB(249, 222, 226) : RGB(82, 43, 50);
            edge = light ? RGB(211, 124, 139) : RGB(151, 70, 84);
        }
    }
    if (item->CtlID == kUnbind && state)
    {
        fill = state->background_color;
        edge = state->palette.danger;
        if (hovered || pressed)
            fill = light ? RGB(253, 238, 241) : RGB(56, 35, 42);
    }
    if (disabled)
    {
        fill = light ? RGB(235, 238, 243) : RGB(32, 35, 41);
        edge = light ? RGB(218, 223, 231) : RGB(50, 54, 62);
    }
    // Every management button sits on a modal surface. Clear the complete
    // owner-draw rectangle with that surface color before painting the
    // rounded button; filling it with the button color leaves square corners
    // behind on hover/press, notably for Unbind and Close.
    if (state)
    {
        const COLORREF clear_color = GetParent(item->hwndItem) == state->content
                                         ? state->background_color
                                         : state->panel_color;
        HBRUSH clear = CreateSolidBrush(clear_color);
        FillRect(item->hDC, &rect, clear);
        DeleteObject(clear);
    }
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ old_brush = SelectObject(item->hDC, brush);
    HGDIOBJ old_pen = SelectObject(item->hDC, pen);
    RoundRect(item->hDC, rect.left + S(1), rect.top + S(1), rect.right - S(1), rect.bottom - S(1),
              S(9), S(9));
    SelectObject(item->hDC, old_brush);
    SelectObject(item->hDC, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t text[256]{};
    GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
    if (active_navigation && state)
    {
        RECT marker{rect.left + S(1), rect.top + S(10), rect.left + S(4), rect.bottom - S(10)};
        HBRUSH marker_brush = CreateSolidBrush(state->accent_color);
        FillRect(item->hDC, &marker, marker_brush);
        DeleteObject(marker_brush);
    }

    SetBkMode(item->hDC, TRANSPARENT);
    if (IsSettingsNavigationId(item->CtlID) && state)
    {
        const wchar_t *subtitle = item->CtlID == kSettingsAppearance     ? L"主题与显示"
                                  : item->CtlID == kSettingsConnection   ? L"地址与 Agent"
                                  : item->CtlID == kSettingsProjectTypes ? L"CLI 启动配置"
                                  : item->CtlID == kSettingsGroups       ? L"项目归类管理"
                                  : item->CtlID == kSettingsDevices      ? L"手机授权状态"
                                                                         : L"版本与终端环境";
        SelectObject(item->hDC, state->font);
        SetTextColor(item->hDC, disabled ? muted_color : text_color);
        RECT title_rect{rect.left + S(16), rect.top + S(7), rect.right - S(28), rect.top + S(29)};
        DrawTextW(item->hDC, text, -1, &title_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(item->hDC, state->small_font);
        SetTextColor(item->hDC, muted_color);
        RECT subtitle_rect{rect.left + S(16), rect.top + S(27), rect.right - S(28),
                           rect.bottom - S(4)};
        DrawTextW(item->hDC, subtitle, -1, &subtitle_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(item->hDC, active_navigation ? state->accent_color : muted_color);
        SelectObject(item->hDC, state->font);
        RECT chevron{rect.right - S(27), rect.top, rect.right - S(8), rect.bottom};
        DrawTextW(item->hDC, L">", -1, &chevron, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    SetTextColor(item->hDC,
                 disabled ? muted_color
                          : (item->CtlID == kUnbind && state
                                 ? state->palette.danger
                                 : (active_theme && state ? state->accent_color : text_color)));
    if (state)
    {
        const bool settings_content =
            state->mode == ManagementMode::Settings && GetParent(item->hwndItem) == state->content;
        SelectObject(item->hDC, settings_content && state->settings_content_font
                                    ? state->settings_content_font
                                    : state->font);
    }
    RECT text_rect = rect;
    DrawTextW(item->hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawListItem(const DRAWITEMSTRUCT *item)
{
    if (!item || item->itemID == static_cast<UINT>(-1))
        return;
    RECT client{};
    GetClientRect(item->hwndItem, &client);
    BufferedWindowPaint buffered(item->hDC, client, item->rcItem);
    DRAWITEMSTRUCT buffered_item = *item;
    buffered_item.hDC = buffered.dc();
    item = &buffered_item;
    State *state = FindState(item->hwndItem);
    if (!state)
        return;
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    wchar_t text[512]{};
    SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, selected ? state->text_color : state->palette.list_text);
    const int inset = DpiScale(state->window, 12);
    RECT rect{item->rcItem.left + inset, item->rcItem.top, item->rcItem.right - inset,
              item->rcItem.bottom};
    const size_t data_index =
        static_cast<size_t>(SendMessageW(item->hwndItem, LB_GETITEMDATA, item->itemID, 0));
    const bool device_item = (state->mode == ManagementMode::Devices ||
                              (state->mode == ManagementMode::Settings &&
                               state->settings_page == SettingsPage::Devices)) &&
                             data_index < state->devices.size();
    if (device_item)
    {
        const auto &device = state->devices[data_index];
        const bool online = state->agent && state->agent->IsDeviceOnline(device.phone_id);
        const COLORREF status =
            online ? state->palette.success
                   : (device.active ? state->palette.muted : state->palette.danger);
        const bool hovered = static_cast<int>(item->itemID) == state->device_hover_row;
        HBRUSH clear = CreateSolidBrush(state->palette.list_surface);
        FillRect(item->hDC, &item->rcItem, clear);
        DeleteObject(clear);
        RECT card{item->rcItem.left + DpiScale(state->window, 3),
                  item->rcItem.top + DpiScale(state->window, 2),
                  item->rcItem.right - DpiScale(state->window, 3),
                  item->rcItem.bottom - DpiScale(state->window, 2)};
        const COLORREF card_fill =
            selected ? (state->light_theme ? RGB(232, 241, 253) : RGB(24, 40, 62))
                     : (hovered ? (state->light_theme ? RGB(244, 247, 251) : RGB(24, 31, 42))
                                : state->palette.list_surface);
        const COLORREF card_edge =
            selected ? state->accent_color : (hovered ? state->panel_edge : card_fill);
        DrawSmoothRoundedRect(item->hDC, card, static_cast<float>(DpiScale(state->window, 6)),
                              card_fill, card_edge, 1.0f);
        const int icon_size = DpiScale(state->window, 30);
        RECT icon_rect{card.left + DpiScale(state->window, 10),
                       (card.top + card.bottom - icon_size) / 2,
                       card.left + DpiScale(state->window, 10) + icon_size,
                       (card.top + card.bottom + icon_size) / 2};
        const COLORREF icon_fill = selected
                                       ? (state->light_theme ? RGB(215, 231, 251) : RGB(31, 58, 88))
                                       : state->palette.surface;
        DrawSmoothRoundedRect(item->hDC, icon_rect, static_cast<float>(DpiScale(state->window, 7)),
                              icon_fill, icon_fill, 0.0f);
        SelectObject(item->hDC,
                     state->settings_icon_font ? state->settings_icon_font : state->font);
        SetTextColor(item->hDC, selected ? state->accent_color : state->muted_color);
        DrawTextW(item->hDC, L"\xE8EA", -1, &icon_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        const int text_left = icon_rect.right + DpiScale(state->window, 10);
        SelectObject(item->hDC,
                     state->settings_content_font ? state->settings_content_font : state->font);
        SetTextColor(item->hDC, state->text_color);
        RECT name_rect{text_left, card.top + DpiScale(state->window, 4),
                       card.right - DpiScale(state->window, 58),
                       card.top + DpiScale(state->window, 30)};
        DrawTextW(item->hDC, device.name.c_str(), -1, &name_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(item->hDC,
                     state->settings_small_font ? state->settings_small_font : state->small_font);
        SetTextColor(item->hDC, state->muted_color);
        std::wstring detail = device.model;
        if (!device.phone_id.empty())
            detail +=
                L"  ·  " + device.phone_id.substr(0, std::min<size_t>(8, device.phone_id.size()));
        RECT detail_rect{text_left, card.top + DpiScale(state->window, 28),
                         card.right - DpiScale(state->window, 12),
                         card.bottom - DpiScale(state->window, 3)};
        DrawTextW(item->hDC, detail.c_str(), -1, &detail_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        HBRUSH dot = CreateSolidBrush(status);
        HGDIOBJ old_dot_brush = SelectObject(item->hDC, dot);
        HGDIOBJ old_dot_pen = SelectObject(item->hDC, GetStockObject(NULL_PEN));
        const int dot_size = DpiScale(state->window, 7);
        const int dot_top = card.top + DpiScale(state->window, 13);
        Ellipse(item->hDC, card.right - DpiScale(state->window, 48), dot_top,
                card.right - DpiScale(state->window, 48) + dot_size, dot_top + dot_size);
        SelectObject(item->hDC, old_dot_pen);
        SelectObject(item->hDC, old_dot_brush);
        DeleteObject(dot);
        SetTextColor(item->hDC, status);
        RECT status_rect{card.right - DpiScale(state->window, 36), card.top,
                         card.right - DpiScale(state->window, 8),
                         card.top + DpiScale(state->window, 32)};
        DrawTextW(item->hDC, online ? L"在线" : (device.active ? L"离线" : L"撤销"), -1,
                  &status_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    HBRUSH fill =
        CreateSolidBrush(selected ? state->palette.selection : state->palette.list_surface);
    FillRect(item->hDC, &item->rcItem, fill);
    DeleteObject(fill);
    if (IsSettingsProjectTypes(*state) && data_index < state->types_data.size())
    {
        const auto &type = state->types_data[data_index];
        const bool is_default = type.id == state->default_project_type_id;
        SelectObject(item->hDC, state->font);
        const int badge_width = is_default ? DpiScale(state->window, 48) : 0;
        RECT name_rect{rect.left, rect.top + DpiScale(state->window, 4), rect.right - badge_width,
                       rect.top + DpiScale(state->window, 27)};
        DrawTextW(item->hDC, type.name.c_str(), -1, &name_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (is_default)
        {
            RECT badge{rect.right - DpiScale(state->window, 44),
                       rect.top + DpiScale(state->window, 7), rect.right,
                       rect.top + DpiScale(state->window, 25)};
            const COLORREF badge_fill = state->light_theme ? RGB(226, 238, 253) : RGB(24, 43, 68);
            DrawSmoothRoundedRect(item->hDC, badge, static_cast<float>(DpiScale(state->window, 5)),
                                  badge_fill, state->accent_color, 1.0f);
            SelectObject(item->hDC, state->project_type_badge_font ? state->project_type_badge_font
                                                                   : state->small_font);
            SetTextColor(item->hDC, state->accent_color);
            DrawTextW(item->hDC, L"默认", -1, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(item->hDC, state->small_font);
        SetTextColor(item->hDC, state->muted_color);
        RECT command_rect{rect.left, rect.top + DpiScale(state->window, 26), rect.right,
                          rect.bottom - DpiScale(state->window, 3)};
        DrawTextW(item->hDC, type.default_command.c_str(), -1, &command_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    SelectObject(item->hDC, state->font);
    DrawTextW(item->hDC, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void Layout(State &state)
{
    RECT client{};
    GetClientRect(state.window, &client);
    const int width = client.right;
    const int height = client.bottom;
    const auto S = [&](int value) { return DpiScale(state.window, value); };
    const int panel_width = state.mode == ManagementMode::Settings
                                ? std::min(S(980), std::max(S(760), width - S(80)))
                                : std::min(S(760), std::max(S(600), width - S(100)));
    const int panel_height = state.mode == ManagementMode::Settings
                                 ? std::min(S(680), std::max(S(520), height - S(64)))
                             : state.mode == ManagementMode::ProjectTypes ? S(590)
                                                                          : S(520);
    const int left = (width - panel_width) / 2;
    const int top = std::max(S(24), (height - panel_height) / 2);
    state.panel = {left, top, left + panel_width, top + panel_height};
    const int x = left + S(34);
    const int right = left + panel_width - S(34);
    const int button_y = state.panel.bottom - S(52);

    auto place = [](HWND control, RECT rect) {
        if (!control)
            return;
        MoveWindow(control, rect.left + 2, rect.top + 2, std::max(1L, rect.right - rect.left - 4),
                   std::max(1L, rect.bottom - rect.top - 4), TRUE);
    };
    auto button = [](HWND control, int x, int y, int w, int h) {
        if (control)
        {
            MoveWindow(control, x, y, w, h, TRUE);
            SetWindowRgn(control, CreateRoundRectRgn(0, 0, w + 1, h + 1, 9, 9), TRUE);
        }
    };

    if (state.mode == ManagementMode::Settings)
    {
        const int sidebar_width = S(214);
        const int nav_x = left + S(18);
        const int nav_width = sidebar_width - S(34);
        button(state.settings_appearance, nav_x, top + S(62), nav_width, S(46));
        button(state.settings_connection, nav_x, top + S(110), nav_width, S(46));
        button(state.settings_project_types, nav_x, top + S(158), nav_width, S(46));
        button(state.settings_groups, nav_x, top + S(206), nav_width, S(46));
        button(state.settings_devices, nav_x, top + S(254), nav_width, S(46));
        button(state.settings_about, nav_x, top + S(302), nav_width, S(46));
        button(state.title_close, state.panel.right - S(54), top + S(4), S(32), S(32));

        // The content surface owns the full right pane.  Its painted content
        // keeps its own text alignment, without an extra framed 16 px inset.
        state.content_rect = {left + sidebar_width + S(1), top + S(kSettingsHeaderHeight) + S(1),
                              state.panel.right - S(1), state.panel.bottom - S(1)};
        if (state.content)
        {
            const int content_width = state.content_rect.right - state.content_rect.left;
            const int content_height = state.content_rect.bottom - state.content_rect.top;
            MoveWindow(state.content, state.content_rect.left, state.content_rect.top,
                       content_width, content_height, TRUE);
            SetWindowRgn(state.content, nullptr, TRUE);
            LayoutSettingsContent(state);
        }
    }
    else if (state.mode == ManagementMode::Devices ||
             (state.mode == ManagementMode::Settings &&
              state.settings_page == SettingsPage::Devices))
    {
        const int list_width = S(280);
        if (state.list)
            MoveWindow(state.list, x, top + S(104), list_width, S(334), TRUE);
        button(state.unbind, right - S(126), button_y, S(126), S(36));
        button(state.title_close, state.panel.right - S(48), top + S(5), S(32), S(32));
    }
    else if (state.mode == ManagementMode::ProjectTypes)
    {
        if (state.list)
            MoveWindow(state.list, x, top + S(120), right - x, S(300), TRUE);
        button(state.add, x, button_y, S(108), S(36));
        button(state.save, x + S(118), button_y, S(118), S(36));
        button(state.remove, x + S(246), button_y, S(108), S(36));
        button(state.close, right - S(118), button_y, S(118), S(36));
    }
    else
    {
        if (state.list)
            MoveWindow(state.list, x, top + S(120), right - x, S(215), TRUE);
        place(state.name, {x, top + S(362), right, top + S(398)});
        if (state.command)
            place(state.command, {x, top + S(436), right, top + S(472)});
        const int y = button_y;
        button(state.add, x, y, S(108), S(36));
        button(state.save, x + S(118), y, S(108), S(36));
        button(state.remove, x + S(236), y, S(108), S(36));
        if (state.select)
            button(state.select, right - S(236), y, S(108), S(36));
        button(state.close, right - S(118), y, S(118), S(36));
    }
    SetModalSurfaceRegion(state.window, state.panel, S(14));
    InvalidateRect(state.window, nullptr, FALSE);
}

void Paint(State &state, HDC dc)
{
    RECT client{};
    GetClientRect(state.window, &client);
    // ModalWorkspaceScope owns the translucent scrim.  Painting a solid
    // backdrop here would make the modal opaque and hide the live workspace.
    (void)client;
    const auto S = [&](int value) { return DpiScale(state.window, value); };
    HBRUSH panel_brush = CreateSolidBrush(state.panel_color);
    // Initialize the entire rounded surface before composing its text/edges.
    FillRect(dc, &state.panel, panel_brush);
    const unsigned edge_alpha = state.light_theme ? 118u : 150u;
    const auto blend_channel = [edge_alpha](BYTE base, BYTE edge) -> BYTE {
        return static_cast<BYTE>((static_cast<unsigned>(base) * (255u - edge_alpha) +
                                  static_cast<unsigned>(edge) * edge_alpha) /
                                 255u);
    };
    const COLORREF subtle_edge =
        RGB(blend_channel(GetRValue(state.panel_color), GetRValue(state.panel_edge)),
            blend_channel(GetGValue(state.panel_color), GetGValue(state.panel_edge)),
            blend_channel(GetBValue(state.panel_color), GetBValue(state.panel_edge)));
    HPEN panel_pen = CreatePen(PS_SOLID, 1, subtle_edge);
    HGDIOBJ old_brush = SelectObject(dc, panel_brush);
    HGDIOBJ old_pen = SelectObject(dc, panel_pen);
    RoundRect(dc, state.panel.left, state.panel.top, state.panel.right, state.panel.bottom, S(10),
              S(10));
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(panel_brush);
    DeleteObject(panel_pen);

    if (state.mode == ManagementMode::Settings)
    {
        const int header_bottom = state.panel.top + S(kSettingsHeaderHeight);
        const int saved = SaveDC(dc);
        HRGN panel_clip = CreateRoundRectRgn(state.panel.left + 1, state.panel.top + 1,
                                             state.panel.right, state.panel.bottom, S(10), S(10));
        SelectClipRgn(dc, panel_clip);
        DeleteObject(panel_clip);
        HBRUSH header_brush = CreateSolidBrush(SettingsHeaderColor(state));
        RECT header{state.panel.left + 1, state.panel.top + 1, state.panel.right - 1,
                    header_bottom};
        FillRect(dc, &header, header_brush);
        DeleteObject(header_brush);
        RestoreDC(dc, saved);

        HPEN header_separator = CreatePen(PS_SOLID, 1, subtle_edge);
        HGDIOBJ old_separator = SelectObject(dc, header_separator);
        MoveToEx(dc, state.panel.left + 1, header_bottom, nullptr);
        LineTo(dc, state.panel.right - 1, header_bottom);
        SelectObject(dc, old_separator);
        DeleteObject(header_separator);
    }

    const int x = state.panel.left + S(34);
    const wchar_t *title = L"管理";
    std::wstring dynamic_title;
    const wchar_t *subtitle = L"保存后立即写入本机 SQLite";
    if (state.mode == ManagementMode::Settings)
    {
        title = L"设置";
        subtitle = L"";
    }
    else if (state.mode == ManagementMode::Groups)
    {
        title = L"分组管理";
        subtitle = L"选择当前分组，或添加、重命名和删除空分组";
    }
    else if (state.mode == ManagementMode::ProjectTypes)
    {
        title = L"项目类型";
        subtitle = L"Codex / Claude 为预置类型，所有类型都可编辑";
    }
    else if (state.mode == ManagementMode::Devices)
    {
        dynamic_title = L"已绑定设备  " + std::to_wstring(state.devices.size());
        title = dynamic_title.c_str();
        subtitle = L"解绑后该手机的令牌和实时连接立即失效";
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, state.text_color);
    SelectObject(dc, state.settings_title_font ? state.settings_title_font : state.title_font);
    const int title_right = state.mode == ManagementMode::Settings ? state.panel.right - S(86)
                                                                   : state.panel.right - S(34);
    RECT title_rect = state.mode == ManagementMode::Settings
                          ? RECT{state.panel.left + S(24), state.panel.top, title_right,
                                 state.panel.top + S(kSettingsHeaderHeight)}
                          : RECT{x, state.panel.top + S(24), title_right, state.panel.top + S(54)};
    DrawTextW(dc, title, -1, &title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, state.muted_color);
    SelectObject(dc, state.small_font);
    if (state.mode != ManagementMode::Settings)
    {
        RECT sub_rect{x, state.panel.top + S(57), title_right, state.panel.top + S(82)};
        DrawTextW(dc, subtitle, -1, &sub_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    if (state.mode == ManagementMode::Settings)
    {
        const int divider_x = state.panel.left + S(214);
        HPEN divider = CreatePen(PS_SOLID, 1, state.panel_edge);
        HGDIOBJ old_divider = SelectObject(dc, divider);
        MoveToEx(dc, divider_x, state.panel.top + S(kSettingsHeaderHeight) + S(1), nullptr);
        LineTo(dc, divider_x, state.panel.bottom - S(18));
        SelectObject(dc, old_divider);
        DeleteObject(divider);
        SetTextColor(dc, state.muted_color);
        SelectObject(dc, state.small_font);
        return;
    }

    auto label = [&](const wchar_t *text, int y) {
        SetTextColor(dc, state.palette.muted);
        SelectObject(dc, state.small_font);
        RECT rect{x, state.panel.top + S(y), state.panel.right - S(34),
                  state.panel.top + S(y + 22)};
        DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    if (state.mode == ManagementMode::Groups)
    {
        label(L"分组列表", 88);
        label(L"分组名称", 346);
    }
    else if (state.mode == ManagementMode::ProjectTypes)
    {
        label(L"项目类型列表", 88);
        SetTextColor(dc, state.muted_color);
        SelectObject(dc, state.small_font);
        RECT hint{x, state.panel.top + S(436), state.panel.right - S(34), state.panel.top + S(468)};
        DrawTextW(
            dc,
            L"新增和修改会打开独立模态框；Codex 和 Claude 虽为预置类型，也可手工调整，但不能删除。",
            -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    else
    {
        label(L"已绑定手机", 76);
        const int divider_x = x + S(300);
        HPEN divider = CreatePen(PS_SOLID, 1, state.panel_edge);
        HGDIOBJ old_device_pen = SelectObject(dc, divider);
        MoveToEx(dc, divider_x, state.panel.top + S(104), nullptr);
        LineTo(dc, divider_x, state.panel.bottom - S(72));
        SelectObject(dc, old_device_pen);
        DeleteObject(divider);
        size_t index{};
        const bool has_device = SelectedIndex(state.list, index) && index < state.devices.size();
        const int detail_x = divider_x + S(28);
        const int detail_right = state.panel.right - S(34);
        if (!has_device)
        {
            SelectObject(dc, state.font);
            SetTextColor(dc, state.muted_color);
            RECT empty{detail_x, state.panel.top + S(180), detail_right, state.panel.top + S(260)};
            DrawTextW(dc, L"选择一台设备查看授权信息", -1, &empty,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        else
        {
            const auto &device = state.devices[index];
            SelectObject(dc, state.title_font);
            SetTextColor(dc, state.text_color);
            RECT name{detail_x, state.panel.top + S(112), detail_right, state.panel.top + S(144)};
            DrawTextW(dc, device.name.c_str(), -1, &name,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, state.small_font);
            auto detail_row = [&](const wchar_t *key, const std::wstring &value, int y) {
                SetTextColor(dc, state.muted_color);
                RECT key_rect{detail_x, state.panel.top + S(y), detail_x + S(92),
                              state.panel.top + S(y + 28)};
                DrawTextW(dc, key, -1, &key_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SetTextColor(dc, state.text_color);
                RECT value_rect{detail_x + S(96), state.panel.top + S(y), detail_right,
                                state.panel.top + S(y + 28)};
                DrawTextW(dc, value.c_str(), -1, &value_rect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            };
            detail_row(L"设备型号", device.model, 158);
            detail_row(L"设备 ID", device.phone_id, 196);
            detail_row(L"绑定时间", FormatDeviceTime(device.paired_at), 234);
            detail_row(L"最近在线", FormatDeviceTime(device.last_seen), 272);
            detail_row(L"授权状态", device.active ? L"已授权" : L"已解绑", 310);
        }
    }
}

void AddLine(HDC dc, const wchar_t *text, int x, int y, int width, HFONT font, COLORREF color)
{
    if (!text)
        return;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    SelectObject(dc, font);
    RECT rect{x, y, x + width, y + 24};
    DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

} // namespace aicli::ui::management
