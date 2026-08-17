#include "ui/project_list_view.h"

#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ui/drawing_utils.h"

namespace aicli::ui {
namespace {

constexpr UINT kSetThemeMessage = WM_APP + 0x271;

struct ProjectListItem {
    std::wstring text;
    LPARAM data{};
};

struct ProjectListState {
    std::vector<ProjectListItem> items;
    ProjectListViewTheme theme{
        RGB(10, 17, 31), RGB(86, 100, 121),
        RGB(159, 159, 159), RGB(159, 159, 159)};
    HFONT font{};
    int item_height{40};
    int selected{-1};
    int scroll_offset{};
    int viewport_height{};
    int wheel_remainder{};
    bool redraw_enabled{true};
    bool mouse_inside{};
    bool scrollbar_hovered{};
    bool scrollbar_dragging{};
    int scrollbar_drag_offset{};
};

ProjectListState* State(HWND window) {
    return reinterpret_cast<ProjectListState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
}

int Scale(HWND window, int value) {
    return MulDiv(value, std::max<UINT>(96, GetDpiForWindow(window)), 96);
}

int ClientHeight(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    return std::max(0L, client.bottom - client.top);
}

int ContentHeight(const ProjectListState& state) {
    const long long height =
        static_cast<long long>(state.items.size()) * state.item_height;
    return static_cast<int>(std::min<long long>(height, INT_MAX));
}

int MaximumScroll(HWND window, const ProjectListState& state) {
    return std::max(0, ContentHeight(state) - ClientHeight(window));
}

void InvalidateScrollbarStrip(HWND window, BOOL erase = FALSE) {
    RECT client{};
    GetClientRect(window, &client);
    RECT strip{std::max(client.left, client.right - Scale(window, 14)),
               client.top, client.right, client.bottom};
    InvalidateRect(window, &strip, erase);
}

bool SetScrollOffset(HWND window, ProjectListState& state, int offset) {
    const int clamped = std::clamp(offset, 0, MaximumScroll(window, state));
    if (clamped == state.scroll_offset) return false;
    state.scroll_offset = clamped;
    if (state.redraw_enabled) InvalidateRect(window, nullptr, FALSE);
    return true;
}

void EnsureVisible(HWND window, ProjectListState& state, int row) {
    if (row < 0 || row >= static_cast<int>(state.items.size())) return;
    const int viewport = ClientHeight(window);
    const int top = row * state.item_height;
    const int bottom = top + state.item_height;
    if (top < state.scroll_offset) {
        SetScrollOffset(window, state, top);
    } else if (bottom > state.scroll_offset + viewport) {
        SetScrollOffset(window, state, bottom - viewport);
    }
}

void NotifyParent(HWND window, UINT notification) {
    HWND parent = GetParent(window);
    if (!parent) return;
    SendMessageW(parent, WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(window), notification),
                 reinterpret_cast<LPARAM>(window));
}

bool SetSelection(HWND window, ProjectListState& state, int row,
                  bool notify) {
    const int next = row >= 0 && row < static_cast<int>(state.items.size())
        ? row : -1;
    if (next == state.selected) return false;
    state.selected = next;
    if (next >= 0) EnsureVisible(window, state, next);
    if (state.redraw_enabled) InvalidateRect(window, nullptr, FALSE);
    if (notify) NotifyParent(window, LBN_SELCHANGE);
    return true;
}

int HitTestRow(HWND window, const ProjectListState& state, POINT point,
               bool* outside = nullptr) {
    RECT client{};
    GetClientRect(window, &client);
    const bool in_client = PtInRect(&client, point) != FALSE;
    const long long content_y =
        static_cast<long long>(point.y) + state.scroll_offset;
    const bool in_content = content_y >= 0 &&
        content_y < ContentHeight(state);
    if (outside) *outside = !in_client || !in_content;
    if (state.items.empty()) return -1;
    const int row = static_cast<int>(std::clamp<long long>(
        content_y / std::max(1, state.item_height), 0,
        static_cast<long long>(state.items.size() - 1)));
    return row;
}

bool ScrollbarVisible(HWND window, const ProjectListState& state) {
    return MaximumScroll(window, state) > 0 &&
        (state.mouse_inside || state.scrollbar_hovered ||
         state.scrollbar_dragging);
}

RECT ScrollbarThumbRect(HWND window, const ProjectListState& state) {
    RECT client{};
    GetClientRect(window, &client);
    const int track_top = Scale(window, 4);
    const int track_bottom = std::max(
        track_top + 1, static_cast<int>(client.bottom) - Scale(window, 4));
    const int track_height = track_bottom - track_top;
    const int content = std::max(1, ContentHeight(state));
    const int viewport = std::max(1L, client.bottom - client.top);
    const int thumb_height = std::min(
        track_height,
        std::max(Scale(window, 28), static_cast<int>(
            static_cast<long long>(track_height) * viewport / content)));
    const int maximum = MaximumScroll(window, state);
    const int travel = std::max(0, track_height - thumb_height);
    const int top = track_top + (maximum > 0
        ? static_cast<int>(static_cast<long long>(travel) *
                           state.scroll_offset / maximum)
        : 0);
    const int width = Scale(window,
        state.scrollbar_hovered || state.scrollbar_dragging ? 7 : 3);
    const int right = static_cast<int>(client.right);
    return {right - width, top, right, top + thumb_height};
}

RECT ScrollbarHitRect(HWND window, const ProjectListState& state) {
    RECT thumb = ScrollbarThumbRect(window, state);
    RECT client{};
    GetClientRect(window, &client);
    thumb.left = std::max(client.left,
                          client.right - Scale(window, 12));
    thumb.top = std::max(client.top, thumb.top - Scale(window, 2));
    thumb.bottom = std::min(client.bottom,
                            thumb.bottom + Scale(window, 2));
    return thumb;
}

void SetScrollFromPointer(HWND window, ProjectListState& state, int pointer_y) {
    RECT client{};
    GetClientRect(window, &client);
    const RECT thumb = ScrollbarThumbRect(window, state);
    const int track_top = Scale(window, 4);
    const int track_bottom = std::max(
        track_top + 1, static_cast<int>(client.bottom) - Scale(window, 4));
    const int travel = std::max(
        1, track_bottom - track_top -
           static_cast<int>(thumb.bottom - thumb.top));
    const int top = std::clamp(
        pointer_y - state.scrollbar_drag_offset,
        track_top, track_top + travel);
    const int maximum = MaximumScroll(window, state);
    SetScrollOffset(window, state, static_cast<int>(
        static_cast<long long>(top - track_top) * maximum / travel));
}

void DrawScrollbar(HWND window, ProjectListState& state, HDC dc) {
    if (!ScrollbarVisible(window, state)) return;
    const RECT thumb = ScrollbarThumbRect(window, state);
    const COLORREF color = state.scrollbar_dragging
        ? state.theme.scrollbar_drag
        : (state.scrollbar_hovered
            ? state.theme.scrollbar_hover
            : state.theme.scrollbar);
    const BYTE alpha = state.scrollbar_dragging
        ? 224 : (state.scrollbar_hovered ? 198 : 142);
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::GraphicsPath path;
    AddRoundedPath(path,
        Gdiplus::RectF(static_cast<float>(thumb.left),
                       static_cast<float>(thumb.top),
                       static_cast<float>(thumb.right - thumb.left),
                       static_cast<float>(thumb.bottom - thumb.top)),
        static_cast<float>(Scale(window, 4)));
    Gdiplus::SolidBrush brush(SmoothColor(color, alpha));
    graphics.FillPath(&brush, &path);
}

void Paint(HWND window, ProjectListState& state, HDC target,
           const RECT& dirty) {
    RECT client{};
    GetClientRect(window, &client);
    BufferedWindowPaint buffered(target, client, dirty);
    HDC dc = buffered.dc();
    HBRUSH background = CreateSolidBrush(state.theme.background);
    FillRect(dc, &client, background);
    DeleteObject(background);

    if (!state.items.empty()) {
        const int item_height = std::max(1, state.item_height);
        const int first = std::clamp(
            state.scroll_offset / item_height, 0,
            static_cast<int>(state.items.size() - 1));
        int y = first * item_height - state.scroll_offset;
        HWND parent = GetParent(window);
        for (int row = first;
             row < static_cast<int>(state.items.size()) && y < client.bottom;
             ++row, y += item_height) {
            DRAWITEMSTRUCT item{};
            item.CtlType = ODT_LISTBOX;
            item.CtlID = static_cast<UINT>(GetDlgCtrlID(window));
            item.itemID = static_cast<UINT>(row);
            item.itemAction = ODA_DRAWENTIRE;
            item.itemState = row == state.selected ? ODS_SELECTED : 0;
            if (GetFocus() == window && row == state.selected)
                item.itemState |= ODS_FOCUS;
            item.hwndItem = window;
            item.hDC = dc;
            item.rcItem = {client.left, y, client.right, y + item_height};
            item.itemData = static_cast<ULONG_PTR>(state.items[row].data);
            if (parent)
                SendMessageW(parent, WM_DRAWITEM, item.CtlID,
                             reinterpret_cast<LPARAM>(&item));
        }
    }
    DrawScrollbar(window, state, dc);
}

LRESULT HandleListMessage(HWND window, ProjectListState& state,
                          UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case LB_RESETCONTENT:
        state.items.clear();
        state.selected = -1;
        state.scroll_offset = 0;
        if (state.redraw_enabled) InvalidateRect(window, nullptr, FALSE);
        return 0;
    case LB_ADDSTRING: {
        const auto* text = reinterpret_cast<const wchar_t*>(lparam);
        state.items.push_back({text ? text : L"", 0});
        if (state.redraw_enabled) InvalidateRect(window, nullptr, FALSE);
        return static_cast<LRESULT>(state.items.size() - 1);
    }
    case LB_GETCOUNT:
        return static_cast<LRESULT>(state.items.size());
    case LB_SETITEMDATA: {
        const int row = static_cast<int>(wparam);
        if (row < 0 || row >= static_cast<int>(state.items.size()))
            return LB_ERR;
        state.items[row].data = lparam;
        return 0;
    }
    case LB_GETITEMDATA: {
        const int row = static_cast<int>(wparam);
        if (row < 0 || row >= static_cast<int>(state.items.size()))
            return LB_ERR;
        return state.items[row].data;
    }
    case LB_GETTEXTLEN: {
        const int row = static_cast<int>(wparam);
        if (row < 0 || row >= static_cast<int>(state.items.size()))
            return LB_ERR;
        return static_cast<LRESULT>(state.items[row].text.size());
    }
    case LB_GETTEXT: {
        const int row = static_cast<int>(wparam);
        auto* output = reinterpret_cast<wchar_t*>(lparam);
        if (!output || row < 0 ||
            row >= static_cast<int>(state.items.size())) return LB_ERR;
        lstrcpyW(output, state.items[row].text.c_str());
        return static_cast<LRESULT>(state.items[row].text.size());
    }
    case LB_SETCURSEL:
        SetSelection(window, state, static_cast<int>(wparam), false);
        return state.selected;
    case LB_GETCURSEL:
        return state.selected >= 0 ? state.selected : LB_ERR;
    case LB_SETITEMHEIGHT: {
        const int next = std::max(1, static_cast<int>(lparam));
        if (next == state.item_height) return 0;
        const int previous = std::max(1, state.item_height);
        state.scroll_offset = static_cast<int>(
            static_cast<long long>(state.scroll_offset) * next / previous);
        state.item_height = next;
        state.scroll_offset = std::clamp(
            state.scroll_offset, 0, MaximumScroll(window, state));
        if (state.redraw_enabled) InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case LB_GETITEMHEIGHT:
        return state.item_height;
    case LB_GETTOPINDEX:
        return state.item_height > 0
            ? state.scroll_offset / state.item_height : 0;
    case LB_SETTOPINDEX: {
        const int row = std::max(0, static_cast<int>(wparam));
        SetScrollOffset(window, state, row * state.item_height);
        return 0;
    }
    case LB_GETITEMRECT: {
        const int row = static_cast<int>(wparam);
        auto* rect = reinterpret_cast<RECT*>(lparam);
        if (!rect || row < 0 ||
            row >= static_cast<int>(state.items.size())) return LB_ERR;
        RECT client{};
        GetClientRect(window, &client);
        const int top = row * state.item_height - state.scroll_offset;
        *rect = {client.left, top, client.right,
                 top + state.item_height};
        return 0;
    }
    case LB_ITEMFROMPOINT: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        bool outside{};
        const int row = HitTestRow(window, state, point, &outside);
        return MAKELRESULT(row < 0 ? 0xffff : row, outside ? 1 : 0);
    }
    }
    return 0;
}

LRESULT CALLBACK ProjectListViewProc(HWND window, UINT message,
                                     WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* state = new ProjectListState();
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    auto* state = State(window);
    if (!state) return DefWindowProcW(window, message, wparam, lparam);

    if (message >= LB_ADDSTRING && message <= LB_MSGMAX)
        return HandleListMessage(window, *state, message, wparam, lparam);

    switch (message) {
    case kSetThemeMessage:
        if (lparam) {
            state->theme = *reinterpret_cast<ProjectListViewTheme*>(lparam);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_SETREDRAW:
        state->redraw_enabled = wparam != FALSE;
        return 0;
    case WM_SETFONT:
        state->font = reinterpret_cast<HFONT>(wparam);
        if (lparam && state->redraw_enabled)
            InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(state->font);
    case WM_SIZE: {
        const int old_max = std::max(
            0, ContentHeight(*state) - state->viewport_height);
        const bool was_at_bottom = old_max > 0 &&
            state->scroll_offset >= old_max;
        state->viewport_height = HIWORD(lparam);
        const int next_max = MaximumScroll(window, *state);
        state->scroll_offset = was_at_bottom
            ? next_max : std::min(state->scroll_offset, next_max);
        if (state->redraw_enabled) InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        Paint(window, *state, dc, paint.rcPaint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        state->wheel_remainder += GET_WHEEL_DELTA_WPARAM(wparam);
        const int steps = state->wheel_remainder / WHEEL_DELTA;
        state->wheel_remainder %= WHEEL_DELTA;
        if (steps != 0) {
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            const int amount = lines == WHEEL_PAGESCROLL
                ? std::max(1, ClientHeight(window))
                : std::max(1, static_cast<int>(lines)) *
                  std::max(1, state->item_height);
            SetScrollOffset(window, *state,
                            state->scroll_offset - steps * amount);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (UpdateProjectListScrollbarDrag(window, point)) return 0;
        const bool was_visible = ScrollbarVisible(window, *state);
        const bool was_hovered = state->scrollbar_hovered;
        state->mouse_inside = true;
        const RECT scrollbar_hit = ScrollbarHitRect(window, *state);
        state->scrollbar_hovered = ScrollbarVisible(window, *state) &&
            PtInRect(&scrollbar_hit, point);
        if (was_visible != ScrollbarVisible(window, *state) ||
            was_hovered != state->scrollbar_hovered)
            InvalidateScrollbarStrip(window);
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (!state->scrollbar_dragging) {
            state->mouse_inside = false;
            state->scrollbar_hovered = false;
            InvalidateScrollbarStrip(window);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window);
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        state->mouse_inside = true;
        if (BeginProjectListScrollbarDrag(window, point)) return 0;
        if (ScrollbarVisible(window, *state)) {
            const RECT thumb = ScrollbarThumbRect(window, *state);
            RECT client{};
            GetClientRect(window, &client);
            if (point.x >= client.right - Scale(window, 12)) {
                SetScrollOffset(window, *state,
                    state->scroll_offset +
                    (point.y < thumb.top ? -ClientHeight(window)
                                         : ClientHeight(window)));
                return 0;
            }
        }
        const int row = HitTestRow(window, *state, point);
        if (row >= 0) SetSelection(window, *state, row, true);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int row = HitTestRow(window, *state, point);
        if (row >= 0) {
            SetSelection(window, *state, row, true);
            NotifyParent(window, LBN_DBLCLK);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        EndProjectListScrollbarDrag(window);
        return 0;
    case WM_CAPTURECHANGED:
        CancelProjectListScrollbarDrag(window);
        return 0;
    case WM_KEYDOWN: {
        if (state->items.empty()) return 0;
        int row = state->selected >= 0 ? state->selected : 0;
        const int page = std::max(
            1, ClientHeight(window) / std::max(1, state->item_height));
        switch (wparam) {
        case VK_UP: --row; break;
        case VK_DOWN: ++row; break;
        case VK_PRIOR: row -= page; break;
        case VK_NEXT: row += page; break;
        case VK_HOME: row = 0; break;
        case VK_END: row = static_cast<int>(state->items.size()) - 1; break;
        case VK_RETURN:
            NotifyParent(window, LBN_DBLCLK);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
        row = std::clamp(row, 0, static_cast<int>(state->items.size()) - 1);
        SetSelection(window, *state, row, true);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (state->redraw_enabled) InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_CONTEXTMENU:
        if (HWND parent = GetParent(window))
            return SendMessageW(parent, WM_CONTEXTMENU,
                                reinterpret_cast<WPARAM>(window), lparam);
        return 0;
    case WM_NCDESTROY: {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete state;
        return DefWindowProcW(window, message, wparam, lparam);
    }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

bool BeginProjectListScrollbarDrag(HWND window, POINT point) {
    auto* state = State(window);
    if (!state) return false;
    state->mouse_inside = true;
    if (!ScrollbarVisible(window, *state)) return false;
    const RECT thumb = ScrollbarThumbRect(window, *state);
    const RECT hit = ScrollbarHitRect(window, *state);
    if (!PtInRect(&hit, point)) return false;
    state->scrollbar_dragging = true;
    state->scrollbar_hovered = true;
    state->scrollbar_drag_offset = std::clamp(
        static_cast<int>(point.y - thumb.top), 0,
        static_cast<int>(thumb.bottom - thumb.top));
    SetCapture(window);
    InvalidateScrollbarStrip(window);
    return true;
}

bool UpdateProjectListScrollbarDrag(HWND window, POINT point) {
    auto* state = State(window);
    if (!state || !state->scrollbar_dragging) return false;
    SetScrollFromPointer(window, *state, point.y);
    return true;
}

bool EndProjectListScrollbarDrag(HWND window) {
    auto* state = State(window);
    if (!state || !state->scrollbar_dragging) return false;
    state->scrollbar_dragging = false;
    if (GetCapture() == window) ReleaseCapture();
    InvalidateScrollbarStrip(window);
    return true;
}

void CancelProjectListScrollbarDrag(HWND window) {
    auto* state = State(window);
    if (!state || !state->scrollbar_dragging) return;
    state->scrollbar_dragging = false;
    state->scrollbar_hovered = false;
    InvalidateScrollbarStrip(window);
}

bool RegisterProjectListViewClass(HINSTANCE instance) {
    WNDCLASSEXW existing{sizeof(existing)};
    if (GetClassInfoExW(instance, kProjectListViewClassName, &existing))
        return true;
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    window_class.lpfnWndProc = ProjectListViewProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kProjectListViewClassName;
    return RegisterClassExW(&window_class) != 0;
}

HWND CreateProjectListView(HWND parent, int control_id, HINSTANCE instance) {
    return CreateWindowExW(
        0, kProjectListViewClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
        instance, nullptr);
}

void SetProjectListViewTheme(HWND window,
                             const ProjectListViewTheme& theme) {
    if (!window) return;
    SendMessageW(window, kSetThemeMessage, 0,
                 reinterpret_cast<LPARAM>(&theme));
}

} // namespace aicli::ui
