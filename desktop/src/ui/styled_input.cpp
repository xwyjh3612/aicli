#include "ui/styled_input.h"

#include <commctrl.h>
#include <algorithm>
#include <memory>

namespace aicli::ui {
namespace {

constexpr UINT_PTR kStyledInputSubclassId = 0xA1C13;
constexpr wchar_t kStyledInputProperty[] = L"Aicli.StyledInput";

struct StyledInputState {
    std::wstring placeholder;
    COLORREF placeholder_color{RGB(126, 136, 151)};
    COLORREF focus_border_color{RGB(72, 137, 224)};
    COLORREF background_color{RGB(32, 36, 43)};
    COLORREF focus_background_color{RGB(43, 48, 57)};
    bool focus_border_only{};
    int radius{5};
    int horizontal_margin{10};
};

StyledInputState* State(HWND control) {
    return reinterpret_cast<StyledInputState*>(GetPropW(control, kStyledInputProperty));
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
    const int margin = state ? state->horizontal_margin : 10;
    RECT format{margin, top, std::max<LONG>(margin + 1, client.right - margin),
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
    GetWindowRect(control, &frame);
    MapWindowPoints(HWND_DESKTOP, parent,
                    reinterpret_cast<POINT*>(&frame), 2);
    InflateRect(&frame, 3, 3);
    RedrawWindow(parent, &frame, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
}

void DrawPlaceholderAndFocusBorder(HWND control, HDC dc,
                                   const StyledInputState& state) {
    if (GetWindowTextLengthW(control) == 0 && !state.placeholder.empty()) {
        RECT rect{};
        GetClientRect(control, &rect);
        rect.left += state.horizontal_margin;
        rect.right -= state.horizontal_margin;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, state.placeholder_color);
        const auto font = reinterpret_cast<HFONT>(
            SendMessageW(control, WM_GETFONT, 0, 0));
        const auto old = font ? SelectObject(dc, font) : nullptr;
        DrawTextW(dc, state.placeholder.c_str(), -1, &rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (old) SelectObject(dc, old);
    }

    if (state.focus_border_only && GetFocus() == control) {
        RECT rect{};
        GetClientRect(control, &rect);
        HPEN pen = CreatePen(PS_SOLID, 1, state.focus_border_color);
        const auto old_pen = SelectObject(dc, pen);
        const auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 0, 0, std::max(1L, rect.right),
                  std::max(1L, rect.bottom), state.radius, state.radius);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
}

LRESULT PaintStyledInput(HWND control, const StyledInputState& state) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(control, &paint);
    if (!target) return 0;

    RECT client{};
    GetClientRect(control, &client);
    const int width = std::max(1L, client.right - client.left);
    const int height = std::max(1L, client.bottom - client.top);
    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, width, height) : nullptr;
    if (!buffer || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        EndPaint(control, &paint);

        // Allocation failure is exceptionally rare for these small fields.
        // Fall back to the native paint path and retain placeholder support.
        const auto result = DefSubclassProc(control, WM_PAINT, 0, 0);
        HDC dc = GetDC(control);
        if (dc) {
            DrawPlaceholderAndFocusBorder(control, dc, state);
            ReleaseDC(control, dc);
        }
        return result;
    }

    const auto old_bitmap = SelectObject(buffer, bitmap);
    const COLORREF background = GetFocus() == control
        ? state.focus_background_color
        : state.background_color;
    HBRUSH background_brush = CreateSolidBrush(background);
    FillRect(buffer, &client, background_brush);
    DeleteObject(background_brush);

    // Render the native EDIT (text, selection and IME state) into the same
    // off-screen frame as our placeholder. This prevents the background-only
    // frame that was briefly visible when focus moved between two empty
    // fields.
    DefSubclassProc(control, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(buffer),
                    PRF_CLIENT);
    DrawPlaceholderAndFocusBorder(control, buffer, state);
    BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);

    SelectObject(buffer, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    EndPaint(control, &paint);
    return 0;
}

LRESULT CALLBACK StyledInputProc(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_SIZE:
        CenterText(control);
        ApplyRoundedRegion(control);
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
            ApplyRoundedRegion(control);
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
        // Paint the complete field synchronously without a separate erase
        // frame. The old and new controls therefore each reach the compositor
        // with their placeholder already present.
        RedrawWindow(control, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        ApplyRoundedRegion(control);
        InvalidateParentFrame(control);
        return result;
    }
    case WM_KEYDOWN:
    case WM_CHAR:
        if (wparam == VK_RETURN || wparam == L'\n') return 0;
        break;
    case WM_ERASEBKGND:
        // WM_PAINT composes the entire field off-screen, including its
        // background. Suppressing the independent erase is what removes the
        // one-frame placeholder flash.
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
    SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(state->horizontal_margin, state->horizontal_margin));
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
    }
}

void SetStyledInputFocusBorderOnly(HWND control, bool enabled) {
    if (auto* state = State(control)) {
        state->focus_border_only = enabled;
        InvalidateRect(control, nullptr, FALSE);
    }
}

void LayoutStyledInput(HWND control, const RECT& outer, int inset, int radius) {
    if (!control) return;
    if (auto* state = State(control)) {
        state->radius = radius;
        HWND parent = GetParent(control);
        const UINT dpi = parent ? GetDpiForWindow(parent) : 96;
        const int margin = MulDiv(10, dpi ? dpi : 96, 96);
        if (state->horizontal_margin != margin) {
            state->horizontal_margin = margin;
            SendMessageW(control, EM_SETMARGINS,
                         EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(margin, margin));
        }
    }
    const int width = std::max(1L, outer.right - outer.left - inset * 2);
    const int height = std::max(1L, outer.bottom - outer.top - inset * 2);
    RECT current{};
    GetWindowRect(control, &current);
    POINT points[2] = {{current.left, current.top}, {current.right, current.bottom}};
    HWND parent = GetParent(control);
    if (parent) MapWindowPoints(HWND_DESKTOP, parent, points, 2);
    const RECT desired{outer.left + inset, outer.top + inset,
                       outer.left + inset + width, outer.top + inset + height};
    const RECT actual{points[0].x, points[0].y, points[1].x, points[1].y};
    const bool geometry_changed = !EqualRect(&actual, &desired);
    if (geometry_changed) {
        SetWindowPos(control, nullptr, desired.left, desired.top, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                         SWP_NOSENDCHANGING);
    }
    ApplyRoundedRegion(control);
    if (!geometry_changed) return;
    // Changing a rectangular child to a rounded region exposes its four
    // corner pixels. Repaint the parent immediately so those pixels do not
    // retain the old square EDIT background until the field receives focus.
    InvalidateParentFrame(control);
    CenterText(control);
    RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
}

bool IsStyledInput(HWND control) {
    return State(control) != nullptr;
}

}
