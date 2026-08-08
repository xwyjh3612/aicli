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
};

StyledInputState* State(HWND control) {
    return reinterpret_cast<StyledInputState*>(GetPropW(control, kStyledInputProperty));
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
    RECT format{10, top, std::max(11L, static_cast<LONG>(client.right - 10)),
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
    InvalidateRect(parent, &frame, FALSE);
}

LRESULT CALLBACK StyledInputProc(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_SIZE:
        CenterText(control);
        break;
    case WM_SETTEXT:
        {
            const auto result = DefSubclassProc(control, message, wparam, lparam);
            CenterText(control);
            InvalidateRect(control, nullptr, FALSE);
            return result;
        }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        CenterText(control);
        // A multiline EDIT only repaints its formatting rectangle when the
        // background is not erased.  Erase this control (not its full modal
        // parent) so the top and bottom bands use the focused field colour.
        RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
        InvalidateParentFrame(control);
        break;
    case WM_KEYDOWN:
    case WM_CHAR:
        if (wparam == VK_RETURN || wparam == L'\n') return 0;
        break;
    case WM_PAINT: {
        const auto result = DefSubclassProc(control, message, wparam, lparam);
        const auto* state = State(control);
        if (state && GetWindowTextLengthW(control) == 0 && !state->placeholder.empty()) {
            HDC dc = GetDC(control);
            if (dc) {
                RECT rect{};
                GetClientRect(control, &rect);
                rect.left += 10;
                rect.right -= 10;
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, state->placeholder_color);
                const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
                const auto old = font ? SelectObject(dc, font) : nullptr;
                DrawTextW(dc, state->placeholder.c_str(), -1, &rect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (old) SelectObject(dc, old);
                ReleaseDC(control, dc);
            }
        }
        return result;
    }
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
    SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(10, 10));
    auto state = std::make_unique<StyledInputState>();
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
        RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
    }
}

void LayoutStyledInput(HWND control, const RECT& outer, int inset, int radius) {
    if (!control) return;
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
    if (!geometry_changed) return;
    SetWindowPos(control, nullptr, desired.left, desired.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                     SWP_NOSENDCHANGING);
    SetWindowRgn(control, CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius), TRUE);
    CenterText(control);
    RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
}

bool IsStyledInput(HWND control) {
    return State(control) != nullptr;
}

}
