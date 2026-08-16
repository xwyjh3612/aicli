#include "ui/rounded_select_popup.h"

#include "ui/drawing_utils.h"

#include <windowsx.h>

#include <algorithm>
#include <utility>

namespace aicli::ui {
namespace {

constexpr wchar_t kPopupClassName[] = L"AicliRoundedSelectPopup";

int Scale(int value, int dpi) noexcept {
    return MulDiv(value, std::max(96, dpi), 96);
}

float ScaleF(float value, int dpi) noexcept {
    return value * static_cast<float>(std::max(96, dpi)) / 96.0F;
}

struct PopupState {
    HWND window{};
    RoundedSelectPopupOptions options;
    std::vector<RoundedSelectEntry> entries;
    int row_height{};
    int padding{};
    int separator_height{};
    int viewport_height{};
    int content_height{};
    int scroll_offset{};
    int hover_index{-1};
    int keyboard_index{-1};
    bool done{};
    bool accepted{};
    bool closing{};
    bool scrollbar_pointer_down{};
    bool scrollbar_dragging{};
    int scrollbar_drag_offset{};
    std::wstring result;

    int EntryTop(int index) const {
        int top = 0;
        for (int current = 0; current < index; ++current) {
            if (entries[current].separator_before) top += separator_height;
            top += row_height;
        }
        if (index >= 0 && index < static_cast<int>(entries.size()) &&
            entries[index].separator_before) {
            top += separator_height;
        }
        return top;
    }

    int EntryBottom(int index) const { return EntryTop(index) + row_height; }
    int MaxScroll() const { return std::max(0, content_height - viewport_height); }
    bool HasScrollbar() const { return MaxScroll() > 0; }

    void ClampScroll() {
        scroll_offset = std::clamp(scroll_offset, 0, MaxScroll());
    }

    void EnsureVisible(int index) {
        if (index < 0 || index >= static_cast<int>(entries.size())) return;
        const int top = EntryTop(index);
        const int bottom = EntryBottom(index);
        if (top < scroll_offset) scroll_offset = top;
        else if (bottom > scroll_offset + viewport_height)
            scroll_offset = bottom - viewport_height;
        ClampScroll();
    }

    RECT ScrollbarThumb(const RECT& client) const {
        const int track_top = padding + Scale(4, options.dpi);
        const int track_bottom = client.bottom - padding - Scale(4, options.dpi);
        const int track_height = std::max(1, track_bottom - track_top);
        const int thumb_height = std::min(
            track_height,
            std::max(Scale(26, options.dpi),
                     static_cast<int>((static_cast<long long>(track_height) *
                                       viewport_height) /
                                      std::max(1, content_height))));
        const int travel = std::max(0, track_height - thumb_height);
        const int thumb_top = track_top +
            (MaxScroll() > 0
                 ? static_cast<int>((static_cast<long long>(travel) * scroll_offset) /
                                    MaxScroll())
                 : 0);
        return {client.right - Scale(7, options.dpi), thumb_top,
                client.right - Scale(2, options.dpi), thumb_top + thumb_height};
    }

    int HitTest(POINT point, const RECT& client) const {
        if (point.x < 0 || point.x >= client.right ||
            point.y < padding || point.y >= client.bottom - padding) {
            return -1;
        }
        if (HasScrollbar() && point.x >= client.right - Scale(12, options.dpi))
            return -1;
        const int content_y = point.y - padding + scroll_offset;
        for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
            if (content_y >= EntryTop(index) && content_y < EntryBottom(index))
                return index;
        }
        return -1;
    }
};

PopupState* StateFor(HWND window) {
    return reinterpret_cast<PopupState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
}

void Invalidate(PopupState* state) {
    if (state && state->window) InvalidateRect(state->window, nullptr, FALSE);
}

void SetScroll(PopupState* state, int position) {
    if (!state || !state->HasScrollbar()) return;
    const int previous = state->scroll_offset;
    state->scroll_offset = position;
    state->ClampScroll();
    if (state->scroll_offset != previous) Invalidate(state);
}

void Finish(PopupState* state, bool accepted, int index = -1) {
    if (!state || state->closing) return;
    state->closing = true;
    if (accepted && index >= 0 && index < static_cast<int>(state->entries.size())) {
        state->accepted = true;
        state->result = state->entries[index].value;
    }
    if (GetCapture() == state->window) ReleaseCapture();
    if (state->window && IsWindow(state->window)) DestroyWindow(state->window);
}

void PaintPopup(HWND window, PopupState* state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    if (!dc || !state) {
        if (dc) EndPaint(window, &paint);
        return;
    }

    RECT client{};
    GetClientRect(window, &client);
    const int width = std::max(1L, client.right - client.left);
    const int height = std::max(1L, client.bottom - client.top);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ previous_bitmap = SelectObject(memory, bitmap);

    const bool light = state->options.palette.light();
    const COLORREF popup_background = light ? RGB(250, 252, 255) : RGB(16, 24, 39);
    const COLORREF popup_border = light ? RGB(205, 214, 226) : RGB(40, 52, 74);
    const COLORREF hover_background = light ? RGB(239, 245, 253) : RGB(24, 36, 59);
    const COLORREF selected_background = light ? RGB(229, 239, 252) : RGB(27, 44, 75);
    const COLORREF selected_hover = light ? RGB(220, 234, 251) : RGB(31, 51, 87);
    const COLORREF popup_text = light ? RGB(35, 43, 56) : RGB(229, 234, 242);
    const COLORREF popup_muted = light ? RGB(111, 126, 145) : RGB(129, 144, 168);

    HBRUSH background = CreateSolidBrush(popup_background);
    FillRect(memory, &client, background);
    DeleteObject(background);
    DrawSmoothRoundedRect(memory, client, ScaleF(8.0F, state->options.dpi),
                          popup_background, popup_border, 1.0F);

    const int saved = SaveDC(memory);
    IntersectClipRect(memory, Scale(1, state->options.dpi), state->padding,
                      width - Scale(1, state->options.dpi),
                      height - state->padding);
    SetBkMode(memory, TRANSPARENT);
    const int row_right = width - Scale(state->HasScrollbar() ? 11 : 5,
                                        state->options.dpi);

    for (int index = 0; index < static_cast<int>(state->entries.size()); ++index) {
        const auto& entry = state->entries[index];
        const int row_top = state->padding + state->EntryTop(index) -
                            state->scroll_offset;
        const int row_bottom = row_top + state->row_height;
        if (entry.separator_before) {
            const int separator_y = row_top - state->separator_height / 2;
            if (separator_y >= state->padding &&
                separator_y < height - state->padding) {
                HPEN separator = CreatePen(
                    PS_SOLID, std::max(1, Scale(1, state->options.dpi)),
                    light ? RGB(222, 228, 236) : RGB(43, 56, 77));
                HGDIOBJ previous_pen = SelectObject(memory, separator);
                MoveToEx(memory, Scale(12, state->options.dpi), separator_y, nullptr);
                LineTo(memory, row_right - Scale(7, state->options.dpi), separator_y);
                SelectObject(memory, previous_pen);
                DeleteObject(separator);
            }
        }
        if (row_bottom <= state->padding || row_top >= height - state->padding)
            continue;

        const bool hovered = index == state->hover_index;
        if (entry.selected || hovered) {
            const COLORREF fill = entry.selected
                ? (hovered ? selected_hover : selected_background)
                : hover_background;
            RECT row{Scale(5, state->options.dpi),
                     row_top + Scale(1, state->options.dpi), row_right,
                     row_bottom - Scale(1, state->options.dpi)};
            DrawSmoothRoundedRect(memory, row, ScaleF(6.0F, state->options.dpi),
                                  fill, fill, 0.0F);
        }

        const int left = Scale(12, state->options.dpi);
        int label_left = left;
        if (!entry.icon.empty()) {
            RECT icon_rect{left, row_top, left + Scale(22, state->options.dpi),
                           row_bottom};
            SetTextColor(memory, entry.selected ? state->options.palette.accent
                                                : popup_muted);
            SelectObject(memory, state->options.icon_font);
            DrawTextW(memory, entry.icon.c_str(), -1, &icon_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            label_left = icon_rect.right + Scale(7, state->options.dpi);
        }

        RECT check_rect{row_right - Scale(25, state->options.dpi), row_top,
                        row_right - Scale(5, state->options.dpi), row_bottom};
        if (entry.selected) {
            SetTextColor(memory, state->options.palette.accent);
            SelectObject(memory, state->options.icon_font);
            DrawTextW(memory, L"\xE73E", -1, &check_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        int detail_left = check_rect.left;
        if (!entry.detail.empty()) {
            SIZE detail_size{};
            SelectObject(memory, state->options.detail_font);
            GetTextExtentPoint32W(memory, entry.detail.c_str(),
                                  static_cast<int>(entry.detail.size()),
                                  &detail_size);
            const int detail_right = check_rect.left - Scale(6, state->options.dpi);
            detail_left = detail_right - detail_size.cx;
            RECT detail_rect{detail_left, row_top, detail_right, row_bottom};
            SetTextColor(memory, popup_muted);
            DrawTextW(memory, entry.detail.c_str(), -1, &detail_rect,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }

        RECT label_rect{label_left, row_top,
                        detail_left - Scale(9, state->options.dpi), row_bottom};
        SetTextColor(memory, popup_text);
        SelectObject(memory, state->options.text_font);
        DrawTextW(memory, entry.label.c_str(), -1, &label_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    RestoreDC(memory, saved);

    if (state->HasScrollbar()) {
        RECT thumb = state->ScrollbarThumb(client);
        const COLORREF thumb_color = light ? RGB(151, 163, 180) : RGB(94, 111, 137);
        DrawSmoothRoundedRect(memory, thumb, ScaleF(2.5F, state->options.dpi),
                              thumb_color, thumb_color, 0.0F);
    }

    BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, previous_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(window, &paint);
}

LRESULT CALLBACK PopupWindowProc(HWND window, UINT message,
                                 WPARAM wparam, LPARAM lparam) {
    auto* state = StateFor(window);
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<PopupState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
        if (state) state->window = window;
        return TRUE;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintPopup(window, state);
        return 0;
    case WM_LBUTTONDOWN: {
        if (!state || !state->HasScrollbar()) return 0;
        RECT client{};
        GetClientRect(window, &client);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (point.x < client.right - Scale(12, state->options.dpi)) return 0;
        state->scrollbar_pointer_down = true;
        const RECT thumb = state->ScrollbarThumb(client);
        if (PtInRect(&thumb, point)) {
            state->scrollbar_dragging = true;
            state->scrollbar_drag_offset = point.y - thumb.top;
        } else {
            SetScroll(state, state->scroll_offset +
                (point.y < thumb.top ? -state->viewport_height
                                     : state->viewport_height));
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!state) return 0;
        RECT client{};
        GetClientRect(window, &client);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (state->scrollbar_dragging) {
            const RECT thumb = state->ScrollbarThumb(client);
            const int track_top = state->padding + Scale(4, state->options.dpi);
            const int track_bottom = client.bottom - state->padding -
                                     Scale(4, state->options.dpi);
            const int travel = std::max(
                1, static_cast<int>(track_bottom - track_top) -
                       static_cast<int>(thumb.bottom - thumb.top));
            const int thumb_top = std::clamp(
                static_cast<int>(point.y - state->scrollbar_drag_offset),
                static_cast<int>(track_top),
                static_cast<int>(track_top + travel));
            SetScroll(state, static_cast<int>(
                (static_cast<long long>(thumb_top - track_top) *
                 state->MaxScroll()) / travel));
            return 0;
        }
        const int next = state->HitTest(point, client);
        if (next != state->hover_index) {
            state->hover_index = next;
            Invalidate(state);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (state) {
            const int steps = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
            SetScroll(state, state->scroll_offset -
                             steps * state->row_height * 2);
        }
        return 0;
    case WM_LBUTTONUP: {
        if (!state) return 0;
        if (state->scrollbar_pointer_down) {
            state->scrollbar_pointer_down = false;
            state->scrollbar_dragging = false;
            return 0;
        }
        RECT client{};
        GetClientRect(window, &client);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int hit = state->HitTest(point, client);
        Finish(state, hit >= 0, hit);
        return 0;
    }
    case WM_RBUTTONUP:
    case WM_CANCELMODE:
        Finish(state, false);
        return 0;
    case WM_CAPTURECHANGED:
        if (state && !state->closing &&
            reinterpret_cast<HWND>(lparam) != window) {
            Finish(state, false);
        }
        return 0;
    case WM_DESTROY:
        if (state) {
            state->window = nullptr;
            state->done = true;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool EnsurePopupClass() {
    static ATOM atom{};
    if (atom) return true;
    WNDCLASSEXW descriptor{sizeof(descriptor)};
    descriptor.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    descriptor.lpfnWndProc = PopupWindowProc;
    descriptor.hInstance = GetModuleHandleW(nullptr);
    descriptor.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    descriptor.lpszClassName = kPopupClassName;
    atom = RegisterClassExW(&descriptor);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int MeasureWidth(const RoundedSelectPopupOptions& options,
                 const std::vector<RoundedSelectEntry>& entries) {
    RECT anchor{};
    if (options.use_screen_anchor) anchor = options.screen_anchor;
    else GetWindowRect(options.anchor, &anchor);
    const int anchor_width = std::max(1L, anchor.right - anchor.left);
    if (options.match_anchor_width) return anchor_width;

    HDC dc = GetDC(options.owner);
    if (!dc) return std::max(anchor_width, Scale(218, options.dpi));
    int widest = 0;
    for (const auto& entry : entries) {
        SIZE label{}, detail{};
        HGDIOBJ previous = SelectObject(dc, options.text_font);
        GetTextExtentPoint32W(dc, entry.label.c_str(),
                              static_cast<int>(entry.label.size()), &label);
        if (!entry.detail.empty()) {
            SelectObject(dc, options.detail_font);
            GetTextExtentPoint32W(dc, entry.detail.c_str(),
                                  static_cast<int>(entry.detail.size()), &detail);
        }
        SelectObject(dc, previous);
        const int icon_width = entry.icon.empty() ? 0 : Scale(29, options.dpi);
        widest = std::max(widest,
            Scale(12 + 9 + 6 + 25 + 8, options.dpi) + icon_width +
            static_cast<int>(label.cx + detail.cx));
    }
    ReleaseDC(options.owner, dc);
    return std::clamp(std::max(widest, anchor_width),
                      Scale(options.minimum_width, options.dpi),
                      Scale(options.maximum_width, options.dpi));
}

} // namespace

std::optional<std::wstring> ShowRoundedSelectPopup(
    const RoundedSelectPopupOptions& options,
    std::vector<RoundedSelectEntry> entries) {
    if (!options.owner || !options.anchor || entries.empty() || !EnsurePopupClass())
        return std::nullopt;

    PopupState state;
    state.options = options;
    state.entries = std::move(entries);
    state.row_height = Scale(options.row_height, options.dpi);
    state.padding = Scale(6, options.dpi);
    state.separator_height = Scale(9, options.dpi);
    for (int index = 0; index < static_cast<int>(state.entries.size()); ++index) {
        if (state.entries[index].separator_before)
            state.content_height += state.separator_height;
        state.content_height += state.row_height;
        if (state.entries[index].selected) state.keyboard_index = index;
    }
    if (state.keyboard_index < 0) state.keyboard_index = 0;
    const int maximum_content =
        state.row_height * std::max(1, options.maximum_visible_rows) +
        (state.entries.size() > 2 ? state.separator_height : 0);
    state.viewport_height = std::min(state.content_height, maximum_content);
    state.EnsureVisible(state.keyboard_index);

    const int width = MeasureWidth(options, state.entries);
    const int height = state.viewport_height + state.padding * 2;
    RECT anchor{};
    if (options.use_screen_anchor) anchor = options.screen_anchor;
    else GetWindowRect(options.anchor, &anchor);
    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    GetMonitorInfoW(monitor, &monitor_info);
    const RECT work = monitor_info.rcWork;
    int x = anchor.left;
    int y = anchor.bottom + Scale(5, options.dpi);
    if (options.placement == RoundedPopupPlacement::Right) {
        x = anchor.right + Scale(5, options.dpi);
        y = anchor.top;
        if (x + width > work.right)
            x = anchor.left - width - Scale(5, options.dpi);
    } else {
        if (x + width > work.right) x = work.right - width;
        if (y + height > work.bottom)
            y = anchor.top - height - Scale(5, options.dpi);
    }
    x = std::clamp(x, static_cast<int>(work.left),
                   static_cast<int>(work.right) - width);
    y = std::clamp(y, static_cast<int>(work.top),
                   static_cast<int>(work.bottom) - height);

    HWND popup = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kPopupClassName, L"", WS_POPUP, x, y, width, height,
        options.owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!popup) return std::nullopt;
    ApplyRoundedRegion(popup, width, height, Scale(9, options.dpi));

    if (options.open_state_changed) options.open_state_changed(true);
    ShowWindow(popup, SW_SHOWNOACTIVATE);
    UpdateWindow(popup);
    SetCapture(popup);

    MSG message{};
    while (!state.done) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (message.message == WM_KEYDOWN) {
            if (message.wParam == VK_ESCAPE) {
                Finish(&state, false);
                continue;
            }
            if (message.wParam == VK_UP || message.wParam == VK_DOWN) {
                const int direction = message.wParam == VK_UP ? -1 : 1;
                state.keyboard_index = std::clamp(
                    state.keyboard_index + direction, 0,
                    static_cast<int>(state.entries.size()) - 1);
                state.hover_index = state.keyboard_index;
                state.EnsureVisible(state.keyboard_index);
                Invalidate(&state);
                continue;
            }
            if (message.wParam == VK_RETURN || message.wParam == VK_SPACE) {
                Finish(&state, true, state.keyboard_index);
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (state.window && IsWindow(state.window)) Finish(&state, false);
    if (options.open_state_changed) options.open_state_changed(false);
    if (state.accepted) return state.result;
    return std::nullopt;
}

} // namespace aicli::ui
