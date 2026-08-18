#include "ui/rounded_context_menu.h"

#include "ui/drawing_utils.h"

#include <windowsx.h>

#include <algorithm>
#include <utility>

namespace aicli::ui {
namespace {

constexpr wchar_t kContextMenuClassName[] = L"AicliRoundedContextMenu";

int Scale(int value, int dpi) noexcept {
    return MulDiv(value, std::max(96, dpi), 96);
}

float ScaleF(float value, int dpi) noexcept {
    return value * static_cast<float>(std::max(96, dpi)) / 96.0F;
}

struct MenuState {
    HWND window{};
    RoundedContextMenuOptions options;
    std::vector<RoundedContextMenuEntry> entries;
    int padding{};
    int separator_height{};
    int row_height{};
    int hover_index{-1};
    int keyboard_index{-1};
    bool closing{};
    bool done{};
    bool submenu_open{};
    std::optional<RoundedContextMenuResult> result;

    int EntryTop(int index) const {
        int top = padding;
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

    RECT EntryRect(int index) const {
        RECT client{};
        GetClientRect(window, &client);
        const int top = EntryTop(index);
        return {Scale(5, options.dpi), top + Scale(1, options.dpi),
                client.right - Scale(5, options.dpi),
                top + row_height - Scale(1, options.dpi)};
    }

    int HitTest(POINT point) const {
        RECT client{};
        GetClientRect(window, &client);
        if (!PtInRect(&client, point)) return -1;
        for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
            RECT row = EntryRect(index);
            if (point.y >= row.top && point.y < row.bottom) return index;
        }
        return -1;
    }

    int NextEnabled(int start, int direction) const {
        if (entries.empty()) return -1;
        int index = std::clamp(start, 0, static_cast<int>(entries.size()) - 1);
        for (int attempts = 0; attempts < static_cast<int>(entries.size()); ++attempts) {
            index = (index + direction + static_cast<int>(entries.size())) %
                    static_cast<int>(entries.size());
            if (entries[index].enabled) return index;
        }
        return -1;
    }
};

MenuState* StateFor(HWND window) {
    return reinterpret_cast<MenuState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void Finish(MenuState* state, int index = -1) {
    if (!state || state->closing) return;
    state->closing = true;
    if (index >= 0 && index < static_cast<int>(state->entries.size()) &&
        state->entries[index].enabled) {
        RECT row = state->EntryRect(index);
        MapWindowPoints(state->window, HWND_DESKTOP,
                        reinterpret_cast<POINT*>(&row), 2);
        state->result = RoundedContextMenuResult{
            state->entries[index].command, row};
    }
    if (GetCapture() == state->window) ReleaseCapture();
    if (state->window && IsWindow(state->window)) DestroyWindow(state->window);
}

void OpenSubmenu(MenuState* state, int index) {
    if (!state || state->closing || state->submenu_open || !state->window ||
        index < 0 || index >= static_cast<int>(state->entries.size()) ||
        !state->entries[index].enabled || !state->entries[index].submenu ||
        !state->options.submenu_handler) {
        return;
    }

    state->hover_index = index;
    state->keyboard_index = index;
    InvalidateRect(state->window, nullptr, FALSE);
    UpdateWindow(state->window);

    RECT item = state->EntryRect(index);
    MapWindowPoints(state->window, HWND_DESKTOP,
                    reinterpret_cast<POINT*>(&item), 2);
    state->submenu_open = true;
    const bool accepted = state->options.submenu_handler(
        state->entries[index].command, item);
    state->submenu_open = false;

    if (state->closing || !state->window || !IsWindow(state->window)) return;
    if (accepted) {
        Finish(state);
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    RECT parent{};
    GetWindowRect(state->window, &parent);
    if (!PtInRect(&parent, cursor)) {
        Finish(state);
        return;
    }
    SetCapture(state->window);
    ScreenToClient(state->window, &cursor);
    const int next = state->HitTest(cursor);
    state->hover_index = next;
    if (next >= 0 && state->entries[next].enabled)
        state->keyboard_index = next;
    InvalidateRect(state->window, nullptr, FALSE);
}

void PaintMenu(HWND window, MenuState* state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    if (!dc || !state) {
        if (dc) EndPaint(window, &paint);
        return;
    }
    RECT client{};
    GetClientRect(window, &client);
    const int width = std::max(1L, client.right);
    const int height = std::max(1L, client.bottom);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ previous_bitmap = SelectObject(memory, bitmap);

    const bool light = state->options.palette.light();
    const COLORREF background = light ? RGB(250, 252, 255) : RGB(16, 24, 39);
    const COLORREF edge = light ? RGB(205, 214, 226) : RGB(40, 52, 74);
    const COLORREF hover = light ? RGB(239, 245, 253) : RGB(24, 36, 59);
    const COLORREF danger_hover = light ? RGB(253, 235, 238) : RGB(58, 32, 40);
    const COLORREF text = light ? RGB(35, 43, 56) : RGB(229, 234, 242);
    const COLORREF muted = light ? RGB(132, 143, 158) : RGB(129, 144, 168);
    const COLORREF danger = light ? RGB(190, 62, 76) : RGB(229, 107, 118);

    HBRUSH clear = CreateSolidBrush(background);
    FillRect(memory, &client, clear);
    DeleteObject(clear);
    DrawSmoothRoundedRect(memory, client, ScaleF(8.0F, state->options.dpi),
                          background, edge, 1.0F);
    SetBkMode(memory, TRANSPARENT);

    for (int index = 0; index < static_cast<int>(state->entries.size()); ++index) {
        const auto& entry = state->entries[index];
        RECT row = state->EntryRect(index);
        if (entry.separator_before) {
            const int separator_y = row.top - state->separator_height / 2;
            HPEN separator = CreatePen(
                PS_SOLID, std::max(1, Scale(1, state->options.dpi)),
                light ? RGB(222, 228, 236) : RGB(43, 56, 77));
            HGDIOBJ previous_pen = SelectObject(memory, separator);
            MoveToEx(memory, Scale(12, state->options.dpi), separator_y, nullptr);
            LineTo(memory, client.right - Scale(12, state->options.dpi),
                   separator_y);
            SelectObject(memory, previous_pen);
            DeleteObject(separator);
        }

        const bool hovered = index == state->hover_index && entry.enabled;
        if (hovered) {
            const COLORREF fill = entry.danger ? danger_hover : hover;
            DrawSmoothRoundedRect(memory, row, ScaleF(6.0F, state->options.dpi),
                                  fill, fill, 0.0F);
        }

        const COLORREF row_text = !entry.enabled ? muted :
            (entry.danger ? danger : text);
        RECT icon{row.left + Scale(7, state->options.dpi), row.top,
                  row.left + Scale(30, state->options.dpi), row.bottom};
        SetTextColor(memory, !entry.enabled ? muted :
                     (entry.danger ? danger : (hovered ? text : muted)));
        SelectObject(memory, state->options.icon_font);
        DrawTextW(memory, entry.icon.c_str(), -1, &icon,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT label{icon.right + Scale(8, state->options.dpi), row.top,
                   row.right - Scale(entry.submenu ? 30 : 12, state->options.dpi),
                   row.bottom};
        SetTextColor(memory, row_text);
        SelectObject(memory, state->options.text_font);
        DrawTextW(memory, entry.label.c_str(), -1, &label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (entry.submenu) {
            RECT arrow{row.right - Scale(27, state->options.dpi), row.top,
                       row.right - Scale(7, state->options.dpi), row.bottom};
            SetTextColor(memory, entry.enabled ? muted :
                         (light ? RGB(174, 182, 193) : RGB(83, 96, 115)));
            SelectObject(memory, state->options.icon_font);
            DrawTextW(memory, L"\xE76C", -1, &arrow,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, previous_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(window, &paint);
}

LRESULT CALLBACK MenuWindowProc(HWND window, UINT message,
                                WPARAM wparam, LPARAM lparam) {
    auto* state = StateFor(window);
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<MenuState*>(create->lpCreateParams);
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
        PaintMenu(window, state);
        return 0;
    case WM_MOUSEMOVE: {
        if (!state) return 0;
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int next = state->HitTest(point);
        if (next != state->hover_index) {
            state->hover_index = next;
            if (next >= 0 && state->entries[next].enabled)
                state->keyboard_index = next;
            if (next >= 0 && state->entries[next].enabled &&
                state->entries[next].submenu && !state->submenu_open) {
                OpenSubmenu(state, next);
            }
            if (state->window && IsWindow(state->window))
                InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!state) return 0;
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int hit = state->HitTest(point);
        if (hit >= 0 && state->entries[hit].submenu &&
            state->entries[hit].enabled && state->options.submenu_handler) {
            OpenSubmenu(state, hit);
            return 0;
        }
        Finish(state, hit);
        return 0;
    }
    case WM_RBUTTONUP:
    case WM_CANCELMODE:
        if (state && state->submenu_open) return 0;
        Finish(state);
        return 0;
    case WM_CAPTURECHANGED:
        if (state && !state->closing && !state->submenu_open &&
            reinterpret_cast<HWND>(lparam) != window) Finish(state);
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

bool EnsureMenuClass() {
    static ATOM atom{};
    if (atom) return true;
    WNDCLASSEXW descriptor{sizeof(descriptor)};
    descriptor.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    descriptor.lpfnWndProc = MenuWindowProc;
    descriptor.hInstance = GetModuleHandleW(nullptr);
    descriptor.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    descriptor.lpszClassName = kContextMenuClassName;
    atom = RegisterClassExW(&descriptor);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

std::optional<RoundedContextMenuResult> ShowRoundedContextMenu(
    const RoundedContextMenuOptions& options,
    std::vector<RoundedContextMenuEntry> entries) {
    if (!options.owner || entries.empty() || !EnsureMenuClass())
        return std::nullopt;

    MenuState state;
    state.options = options;
    state.entries = std::move(entries);
    state.padding = Scale(6, options.dpi);
    state.separator_height = Scale(9, options.dpi);
    state.row_height = Scale(options.row_height, options.dpi);
    int content_height = state.padding * 2;
    for (int index = 0; index < static_cast<int>(state.entries.size()); ++index) {
        if (state.entries[index].separator_before)
            content_height += state.separator_height;
        content_height += state.row_height;
        if (state.keyboard_index < 0 && state.entries[index].enabled)
            state.keyboard_index = index;
    }

    const int width = Scale(options.width, options.dpi);
    const int height = content_height;
    RECT anchor{options.screen_position.x, options.screen_position.y,
                options.screen_position.x + 1, options.screen_position.y + 1};
    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{sizeof(monitor_info)};
    GetMonitorInfoW(monitor, &monitor_info);
    const RECT work = monitor_info.rcWork;
    int x = options.screen_position.x;
    int y = options.screen_position.y;
    if (x + width > work.right) x = work.right - width;
    if (y + height > work.bottom) y = work.bottom - height;
    x = std::max(static_cast<int>(work.left), x);
    y = std::max(static_cast<int>(work.top), y);

    HWND popup = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kContextMenuClassName, L"", WS_POPUP, x, y, width, height,
        options.owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!popup) return std::nullopt;
    ApplyRoundedRegion(popup, width, height, Scale(9, options.dpi));
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
            if (message.wParam == VK_ESCAPE || message.wParam == VK_LEFT) {
                Finish(&state);
                continue;
            }
            if (message.wParam == VK_UP || message.wParam == VK_DOWN) {
                const int direction = message.wParam == VK_UP ? -1 : 1;
                state.keyboard_index = state.NextEnabled(
                    state.keyboard_index, direction);
                state.hover_index = state.keyboard_index;
                InvalidateRect(state.window, nullptr, FALSE);
                continue;
            }
            if ((message.wParam == VK_RETURN || message.wParam == VK_SPACE ||
                 message.wParam == VK_RIGHT) &&
                state.keyboard_index >= 0 &&
                state.keyboard_index < static_cast<int>(state.entries.size()) &&
                state.entries[state.keyboard_index].submenu &&
                state.options.submenu_handler) {
                OpenSubmenu(&state, state.keyboard_index);
                continue;
            }
            if (message.wParam == VK_RETURN || message.wParam == VK_SPACE ||
                message.wParam == VK_RIGHT) {
                Finish(&state, state.keyboard_index);
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (state.window && IsWindow(state.window)) Finish(&state);
    return state.result;
}

} // namespace aicli::ui
