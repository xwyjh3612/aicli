#include "ui/styled_input.h"

#include <commctrl.h>
#include <algorithm>
#include <memory>

#include "ui/drawing_utils.h"

namespace aicli::ui {
namespace {

constexpr UINT_PTR kStyledInputSubclassId = 0xA1C13;
constexpr wchar_t kStyledInputProperty[] = L"Aicli.StyledInput";

struct StyledInputState {
    std::wstring placeholder;
    std::wstring leading_icon;
    COLORREF placeholder_color{RGB(126, 136, 151)};
    COLORREF focus_border_color{RGB(72, 137, 224)};
    COLORREF background_color{RGB(32, 36, 43)};
    COLORREF focus_background_color{RGB(43, 48, 57)};
    bool focus_border_only{};
    bool legacy_painting{};
    bool parent_frame{};
    bool parent_frame_valid{};
    int radius{5};
    int horizontal_margin{10};
    int right_margin{10};
    int leading_icon_slot{22};
    int leading_icon_slot_px{};
    HFONT leading_icon_font{};
    UINT leading_icon_font_dpi{};
    int parent_frame_content_inset{2};
    int parent_frame_content_inset_px{};
    RECT parent_frame_rect{};

    ~StyledInputState() {
        if (leading_icon_font) DeleteObject(leading_icon_font);
    }
};

StyledInputState* State(HWND control) {
    return reinterpret_cast<StyledInputState*>(GetPropW(control, kStyledInputProperty));
}

HFONT LeadingIconFont(HWND control, StyledInputState& state) {
    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(control));
    if (state.leading_icon_font && state.leading_icon_font_dpi == dpi)
        return state.leading_icon_font;
    if (state.leading_icon_font) DeleteObject(state.leading_icon_font);
    state.leading_icon_font = CreateFontW(
        -MulDiv(13, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
    state.leading_icon_font_dpi = dpi;
    return state.leading_icon_font;
}

void ApplyRoundedRegion(HWND control) {
    const auto* state = State(control);
    if (!state) return;
    RECT client{};
    GetClientRect(control, &client);
    HRGN region = CreateRoundRectRgn(0, 0, std::max(1L, client.right) + 1,
                                     std::max(1L, client.bottom) + 1,
                                     state->radius, state->radius);
    if (!SetWindowRgn(control, region, TRUE)) DeleteObject(region);
}

void CenterText(HWND control) {
    RECT client{};
    GetClientRect(control, &client);
    HDC dc = GetDC(control);
    if (!dc) return;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    const auto previous = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    if (previous) SelectObject(dc, previous);
    ReleaseDC(control, dc);
    const int height = client.bottom - client.top;
    const int text_height = std::min(height, std::max(1, static_cast<int>(metrics.tmHeight)));
    const int top = std::max(0, (height - text_height) / 2);
    const auto* state = State(control);
    const int left_margin = state
        ? std::max(1, state->horizontal_margin -
                          state->parent_frame_content_inset_px)
        : 10;
    const int right_margin = state
        ? std::max(1, state->right_margin -
                          state->parent_frame_content_inset_px)
        : 10;
    RECT format{left_margin, top,
                std::max<LONG>(left_margin + 1, client.right - right_margin),
                std::min(height, top + text_height + 1)};
    SendMessageW(control, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&format));
}

// The underline/outline around an edit is painted by its parent.  Invalidating
// the whole parent on every focus transition made a click look like a full
// screen flash, especially when the parent is a modal host covering the
// workspace.  Restrict the repaint to the small frame around this control.
void InvalidateParentFrame(HWND control) {
    HWND parent = GetParent(control);
    if (!parent) return;
    RECT frame{};
    const auto* state = State(control);
    if (state && state->parent_frame && state->parent_frame_valid) {
        frame = state->parent_frame_rect;
    } else {
        GetWindowRect(control, &frame);
        MapWindowPoints(HWND_DESKTOP, parent,
                        reinterpret_cast<POINT*>(&frame), 2);
    }
    InflateRect(&frame, 3, 3);
    RedrawWindow(parent, &frame, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
}

void DrawPlaceholderAndFocusBorder(HWND control, HDC dc,
                                   const StyledInputState& state) {
    if (!state.leading_icon.empty() && !state.parent_frame) {
        RECT icon_rect{};
        GetClientRect(control, &icon_rect);
        const UINT dpi = std::max<UINT>(96, GetDpiForWindow(control));
        const int inset = state.parent_frame_content_inset_px;
        icon_rect.left = std::max(0, state.right_margin - inset);
        icon_rect.right = std::max<LONG>(
            icon_rect.left + 1,
            state.horizontal_margin - inset - MulDiv(
                3, dpi, 96));
        OffsetRect(&icon_rect, 0, MulDiv(2, dpi, 96));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, state.placeholder_color);
        auto& mutable_state = const_cast<StyledInputState&>(state);
        const HFONT icon_font = LeadingIconFont(control, mutable_state);
        const auto previous = icon_font ? SelectObject(dc, icon_font) : nullptr;
        DrawTextW(dc, state.leading_icon.c_str(), -1, &icon_rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (previous) SelectObject(dc, previous);
    }
    if (GetWindowTextLengthW(control) == 0 && !state.placeholder.empty()) {
        RECT rect{};
        GetClientRect(control, &rect);
        rect.left += state.leading_icon.empty()
            ? state.horizontal_margin
            : std::max(1, state.horizontal_margin -
                              state.parent_frame_content_inset_px);
        rect.right -= state.right_margin;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, state.placeholder_color);
        const auto font = reinterpret_cast<HFONT>(
            SendMessageW(control, WM_GETFONT, 0, 0));
        const auto old = font ? SelectObject(dc, font) : nullptr;
        DrawTextW(dc, state.placeholder.c_str(), -1, &rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (old) SelectObject(dc, old);
    }

    if (state.focus_border_only && !state.parent_frame &&
        GetFocus() == control) {
        RECT rect{};
        GetClientRect(control, &rect);
        HPEN pen = CreatePen(PS_SOLID, 1, state.focus_border_color);
        const auto old_pen = SelectObject(dc, pen);
        const auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        if (state.legacy_painting) {
            RoundRect(dc, 0, 0, std::max(1L, rect.right),
                      std::max(1L, rect.bottom), state.radius, state.radius);
        } else {
            InflateRect(&rect, -1, -1);
            RoundRect(dc, rect.left, rect.top,
                      std::max(rect.left + 1, rect.right),
                      std::max(rect.top + 1, rect.bottom),
                      state.radius, state.radius);
        }
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
}

LRESULT PaintStyledInput(HWND control, const StyledInputState& state) {
    if (state.legacy_painting) {
        const auto result = DefSubclassProc(control, WM_PAINT, 0, 0);
        HDC dc = GetDC(control);
        if (dc) {
            DrawPlaceholderAndFocusBorder(control, dc, state);
            ReleaseDC(control, dc);
        }
        return result;
    }
    // Paint one complete field-colour surface first, then let the native EDIT
    // draw its text, selection, caret and IME state on top. WM_PRINTCLIENT did
    // not request WM_CTLCOLOREDIT on every Windows build, leaving its text
    // formatting rectangle in the system beige colour while only the top and
    // bottom bands used our theme.
    RECT client{};
    GetClientRect(control, &client);
    const COLORREF background = GetFocus() == control
        ? state.focus_background_color
        : state.background_color;
    HDC background_dc = GetDC(control);
    if (background_dc) {
        HBRUSH background_brush = CreateSolidBrush(background);
        FillRect(background_dc, &client, background_brush);
        DeleteObject(background_brush);
        ReleaseDC(control, background_dc);
    }

    const auto result = DefSubclassProc(control, WM_PAINT, 0, 0);
    HDC overlay_dc = GetDC(control);
    if (overlay_dc) {
        DrawPlaceholderAndFocusBorder(control, overlay_dc, state);
        ReleaseDC(control, overlay_dc);
    }
    return result;
}

LRESULT CALLBACK StyledInputProc(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_SIZE:
        CenterText(control);
        break;
    case WM_SHOWWINDOW: {
        const auto result = DefSubclassProc(control, message, wparam, lparam);
        if (wparam) {
            // Settings pages create their fields while hidden, then reveal
            // them when a navigation item is selected. Reapply the region
            // after the native EDIT has become visible; otherwise its first
            // presented frame can retain the stock square rectangle until a
            // later focus message.
            CenterText(control);
            if (const auto* state = State(control);
                state && state->parent_frame) {
                SetWindowRgn(control, nullptr, FALSE);
            } else {
                ApplyRoundedRegion(control);
            }
            InvalidateParentFrame(control);
            RedrawWindow(control, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        }
        return result;
    }
    case WM_SETTEXT:
        {
            const auto result = DefSubclassProc(control, message, wparam, lparam);
            CenterText(control);
            InvalidateRect(control, nullptr, FALSE);
            return result;
        }
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        const auto result = DefSubclassProc(control, message, wparam, lparam);
        CenterText(control);
        if (const auto* state = State(control); state && state->legacy_painting) {
            RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
            ApplyRoundedRegion(control);
            InvalidateParentFrame(control);
            return result;
        }
        // Paint the complete field synchronously without a separate erase
        // frame. The old and new controls therefore each reach the compositor
        // with their placeholder already present.
        RedrawWindow(control, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        if (const auto* state = State(control);
            state && state->parent_frame) {
            SetWindowRgn(control, nullptr, FALSE);
        } else {
            ApplyRoundedRegion(control);
        }
        InvalidateParentFrame(control);
        return result;
    }
    case WM_KEYDOWN:
    case WM_CHAR:
        if (wparam == VK_RETURN || wparam == L'\n') return 0;
        break;
    case WM_ERASEBKGND:
        if (const auto* state = State(control)) {
            if (state->legacy_painting)
                return DefSubclassProc(control, message, wparam, lparam);
            RECT client{};
            GetClientRect(control, &client);
            const COLORREF background = GetFocus() == control
                ? state->focus_background_color
                : state->background_color;
            HBRUSH brush = CreateSolidBrush(background);
            FillRect(reinterpret_cast<HDC>(wparam), &client, brush);
            DeleteObject(brush);
        }
        return 1;
    case WM_PAINT:
        if (const auto* state = State(control)) {
            return PaintStyledInput(control, *state);
        }
        break;
    case WM_NCDESTROY: {
        auto* state = State(control);
        RemovePropW(control, kStyledInputProperty);
        delete state;
        RemoveWindowSubclass(control, StyledInputProc, kStyledInputSubclassId);
        break;
    }
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

}

HWND CreateStyledInput(HWND parent, int control_id, HFONT font,
                       const std::wstring& placeholder, bool read_only, bool tab_stop) {
    DWORD style = WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOHSCROLL;
    if (read_only) style |= ES_READONLY;
    if (tab_stop) style |= WS_TABSTOP;
    HWND control = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 1, 1, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (!control) return nullptr;
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    auto state = std::make_unique<StyledInputState>();
    const UINT dpi = parent ? GetDpiForWindow(parent) : 96;
    state->horizontal_margin = MulDiv(10, dpi ? dpi : 96, 96);
    state->right_margin = state->horizontal_margin;
    SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                  MAKELPARAM(state->horizontal_margin, state->right_margin));
    state->placeholder = placeholder;
    SetPropW(control, kStyledInputProperty, state.release());
    SetWindowSubclass(control, StyledInputProc, kStyledInputSubclassId, 0);
    CenterText(control);
    return control;
}

void SetStyledInputPlaceholder(HWND control, const std::wstring& placeholder) {
    if (auto* state = State(control)) {
        state->placeholder = placeholder;
        InvalidateRect(control, nullptr, FALSE);
    }
}

void SetStyledInputLeadingIcon(HWND control, const std::wstring& icon,
                               int slot_width) {
    if (auto* state = State(control)) {
        state->leading_icon = icon;
        state->leading_icon_slot = std::max(0, slot_width);
        CenterText(control);
        InvalidateRect(control, nullptr, FALSE);
    }
}

std::wstring GetStyledInputText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length + 1), L'\0');
    GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<size_t>(length));
    return result;
}

void SetStyledInputText(HWND control, const std::wstring& value) {
    SetWindowTextW(control, value.c_str());
    CenterText(control);
    InvalidateRect(control, nullptr, FALSE);
}

void SetStyledInputTheme(HWND control, const ThemePalette& palette) {
    if (auto* state = State(control)) {
        state->placeholder_color = palette.muted;
        state->focus_border_color = palette.accent;
        state->background_color = palette.field;
        state->focus_background_color = palette.field_focus;
        RedrawWindow(control, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        if (state->parent_frame) InvalidateParentFrame(control);
    }
}

void SetStyledInputFocusBorderOnly(HWND control, bool enabled) {
    if (auto* state = State(control)) {
        state->focus_border_only = enabled;
        InvalidateRect(control, nullptr, FALSE);
    }
}

void SetStyledInputLegacyPainting(HWND control, bool enabled) {
    if (auto* state = State(control)) {
        state->legacy_painting = enabled;
        InvalidateRect(control, nullptr, FALSE);
    }
}

void SetStyledInputParentFrame(HWND control, bool enabled, int content_inset) {
    if (auto* state = State(control)) {
        state->parent_frame = enabled;
        state->parent_frame_content_inset = std::max(0, content_inset);
        state->parent_frame_content_inset_px = 0;
        state->parent_frame_valid = false;
        // The smaller inner EDIT is deliberately rectangular. The parent owns
        // the stable rounded silhouette, while the native control keeps all
        // caret, selection, clipboard and IME behavior.
        if (enabled) SetWindowRgn(control, nullptr, TRUE);
        InvalidateParentFrame(control);
    }
}

void LayoutStyledInput(HWND control, const RECT& outer, int inset, int radius) {
    if (!control) return;
    StyledInputState* state = State(control);
    HWND parent = GetParent(control);
    if (state) {
        state->radius = radius;
        const UINT dpi = parent ? GetDpiForWindow(parent) : 96;
        state->parent_frame_content_inset_px = state->parent_frame
            ? MulDiv(state->parent_frame_content_inset,
                     dpi ? dpi : 96, 96)
            : 0;
        const int base_margin = MulDiv(10, dpi ? dpi : 96, 96);
        state->leading_icon_slot_px = state->leading_icon.empty()
            ? 0
            : MulDiv(state->leading_icon_slot, dpi ? dpi : 96, 96);
        const int icon_slot = state->parent_frame ? 0 : state->leading_icon_slot_px;
        const int left_margin = base_margin + icon_slot;
        if (state->horizontal_margin != left_margin ||
            state->right_margin != base_margin) {
            state->horizontal_margin = left_margin;
            state->right_margin = base_margin;
            SendMessageW(control, EM_SETMARGINS,
                         EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(left_margin, base_margin));
        }
    }
    RECT visual{outer.left + inset, outer.top + inset,
                outer.right - inset, outer.bottom - inset};
    const int content_inset = state ? state->parent_frame_content_inset_px : 0;
    const int leading_slot = state && state->parent_frame
        ? state->leading_icon_slot_px
        : 0;
    RECT desired{visual.left + content_inset + leading_slot,
                 visual.top + content_inset,
                 visual.right - content_inset, visual.bottom - content_inset};
    const int width = std::max(1L, desired.right - desired.left);
    const int height = std::max(1L, desired.bottom - desired.top);
    desired.right = desired.left + width;
    desired.bottom = desired.top + height;
    RECT current{};
    GetWindowRect(control, &current);
    POINT points[2] = {{current.left, current.top}, {current.right, current.bottom}};
    if (parent) MapWindowPoints(HWND_DESKTOP, parent, points, 2);
    const RECT actual{points[0].x, points[0].y, points[1].x, points[1].y};
    const bool geometry_changed = !EqualRect(&actual, &desired);
    const bool frame_changed = state && state->parent_frame &&
        (!state->parent_frame_valid ||
         !EqualRect(&state->parent_frame_rect, &visual));
    if (frame_changed) {
        if (state->parent_frame_valid && parent) {
            RECT previous = state->parent_frame_rect;
            InflateRect(&previous, 2, 2);
            InvalidateRect(parent, &previous, FALSE);
        }
        state->parent_frame_rect = visual;
        state->parent_frame_valid = true;
    }
    if (geometry_changed) {
        SetWindowPos(control, nullptr, desired.left, desired.top, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                         SWP_NOSENDCHANGING);
    }
    if (state && state->parent_frame) {
        SetWindowRgn(control, nullptr, FALSE);
    } else {
        ApplyRoundedRegion(control);
    }
    if (!geometry_changed && !frame_changed) return;
    // Changing a rectangular child to a rounded region exposes its four
    // corner pixels. Repaint the parent immediately so those pixels do not
    // retain the old square EDIT background until the field receives focus.
    InvalidateParentFrame(control);
    CenterText(control);
    RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
}

void PaintStyledInputParentFrame(HWND control, HDC parent_dc) {
    const auto* state = State(control);
    if (!state || !state->parent_frame || !state->parent_frame_valid ||
        !parent_dc) {
        return;
    }
    const bool focused = GetFocus() == control;
    const COLORREF fill = focused
        ? state->focus_background_color
        : state->background_color;
    const COLORREF stroke = focused
        ? state->focus_border_color
        : fill;
    // SetWindowRgn interprets the old radius as an ellipse diameter. Preserve
    // that visual curvature when drawing the deterministic parent surface.
    const float radius = std::max(1.0F, state->radius / 2.0F);
    DrawSmoothRoundedRect(parent_dc, state->parent_frame_rect, radius,
                          fill, stroke, focused ? 1.0F : 0.0F);
    if (!state->leading_icon.empty() && state->leading_icon_slot_px > 0) {
        const UINT dpi = std::max<UINT>(96, GetDpiForWindow(control));
        RECT icon_rect{
            state->parent_frame_rect.left + state->parent_frame_content_inset_px,
            state->parent_frame_rect.top,
            state->parent_frame_rect.left + state->parent_frame_content_inset_px +
                state->leading_icon_slot_px,
            state->parent_frame_rect.bottom};
        OffsetRect(&icon_rect, MulDiv(4, dpi, 96), 0);
        OffsetRect(&icon_rect, 0, MulDiv(2, dpi, 96));
        SetBkMode(parent_dc, TRANSPARENT);
        SetTextColor(parent_dc, state->placeholder_color);
        auto& mutable_state = const_cast<StyledInputState&>(*state);
        const HFONT icon_font = LeadingIconFont(control, mutable_state);
        const auto previous = icon_font
            ? SelectObject(parent_dc, icon_font)
            : nullptr;
        DrawTextW(parent_dc, state->leading_icon.c_str(), -1, &icon_rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (previous) SelectObject(parent_dc, previous);
    }
}

bool IsStyledInput(HWND control) {
    return State(control) != nullptr;
}

}
