#include "ui/group_name_dialog.h"
#include "ui/dialog_metrics.h"
#include "ui/styled_input.h"
#include "ui/drawing_utils.h"
#include "ui/theme.h"
#include "ui/modal_host.h"
#include <commctrl.h>
#include <algorithm>
#include <iterator>

namespace aicli::ui
{
namespace
{
constexpr wchar_t kGroupNameClassName[] = L"AicliGroupNameDialog";
enum : int
{
    kGroupNameInput = 2410,
    kGroupNameSave = 2411,
    kGroupNameCancel = 2412,
    kGroupNameClose = 2413,
};

struct GroupNameState
{
    HWND window{};
    HWND owner{};
    HWND input{};
    HWND save{};
    HWND cancel{};
    HWND close{};
    HWND hovered{};
    RECT panel{};
    HFONT font{};
    HFONT title_font{};
    HFONT small_font{};
    HBRUSH field_brush{};
    HBRUSH field_focus_brush{};
    std::wstring title;
    std::wstring subtitle;
    std::wstring initial_name;
    std::wstring value;
    bool accepted{};
    ThemePalette palette;
    bool light_theme{};
    COLORREF background{};
    COLORREF panel_color{};
    COLORREF panel_edge{};
    COLORREF text{};
    COLORREF muted{};
    COLORREF field{};
    COLORREF field_focus{};
    COLORREF accent{};
};
constexpr UINT_PTR kGroupNameButtonSubclassId = 0xA1C15;
constexpr UINT_PTR kGroupNameInputSubclassId = 0xA1C16;

LRESULT CALLBACK GroupNameButtonProc(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR, DWORD_PTR reference)
{
    auto *state = reinterpret_cast<GroupNameState *>(reference);
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        if (state && state->hovered != control)
        {
            HWND previous = state->hovered;
            state->hovered = control;
            if (previous)
                InvalidateRect(previous, nullptr, FALSE);
            InvalidateRect(control, nullptr, FALSE);
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
            TrackMouseEvent(&tracking);
        }
        break;
    case WM_MOUSELEAVE:
        if (state && state->hovered == control)
            state->hovered = nullptr;
        InvalidateRect(control, nullptr, FALSE);
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(control, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(control, GroupNameButtonProc, kGroupNameButtonSubclassId);
        break;
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK GroupNameInputProc(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                    UINT_PTR, DWORD_PTR reference)
{
    auto *state = reinterpret_cast<GroupNameState *>(reference);
    if (message == WM_KEYDOWN && state)
    {
        if (wparam == VK_RETURN)
        {
            SendMessageW(state->window, WM_COMMAND, MAKEWPARAM(kGroupNameSave, BN_CLICKED),
                         reinterpret_cast<LPARAM>(state->save));
            return 0;
        }
        if (wparam == VK_ESCAPE)
        {
            CloseModalSurface(state->window);
            return 0;
        }
    }
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(control, GroupNameInputProc, kGroupNameInputSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

void LayoutGroupNameDialog(GroupNameState &state)
{
    RECT client{};
    GetClientRect(state.window, &client);
    const int client_width = static_cast<int>(client.right);
    const int client_height = static_cast<int>(client.bottom);
    const auto S = [&](int value) { return DpiScale(state.window, value); };
    const int width = std::min(S(460), std::max(S(400), client_width - S(120)));
    const int height = S(238);
    const int left = (client_width - width) / 2;
    const int top = std::max(S(24), (client_height - height) / 2);
    state.panel = {left, top, left + width, top + height};
    LayoutStyledInput(state.input, {left + S(24), top + S(112), left + width - S(24), top + S(147)},
                      S(2), S(8));
    MoveWindow(state.cancel, left + width - S(226), top + S(178), S(92), S(36), TRUE);
    MoveWindow(state.save, left + width - S(124), top + S(178), S(100), S(36), TRUE);
    MoveWindow(state.close, left + width - S(42), top + S(5), S(32), S(32), TRUE);
    SetWindowRgn(state.cancel, CreateRoundRectRgn(0, 0, S(93), S(37), S(8), S(8)), TRUE);
    SetWindowRgn(state.save, CreateRoundRectRgn(0, 0, S(101), S(37), S(8), S(8)), TRUE);
    SetWindowRgn(state.close, CreateRoundRectRgn(0, 0, S(33), S(33), S(7), S(7)), TRUE);
    // RoundRect below uses a 16 px ellipse diameter. Match the window region
    // to that curve so no unpainted corner pixels remain inside the surface.
    SetModalSurfaceRegion(state.window, state.panel, S(16));
    InvalidateRect(state.window, nullptr, FALSE);
}

void PaintGroupNameDialog(GroupNameState &state, HDC dc)
{
    RECT client{};
    GetClientRect(state.window, &client);
    // The layered ModalWorkspaceScope scrim is behind this content window.
    // Leave the host background untouched so the dimmed owner remains visible.
    (void)client;

    const auto S = [&](int value) { return DpiScale(state.window, value); };
    HBRUSH panel_background = CreateSolidBrush(state.panel_color);
    FillRect(dc, &state.panel, panel_background);
    DeleteObject(panel_background);
    HBRUSH panel_brush = CreateSolidBrush(state.panel_color);
    HPEN panel_pen = CreatePen(PS_SOLID, 1, state.panel_edge);
    HGDIOBJ old_brush = SelectObject(dc, panel_brush);
    HGDIOBJ old_pen = SelectObject(dc, panel_pen);
    RoundRect(dc, state.panel.left, state.panel.top, state.panel.right, state.panel.bottom, S(16),
              S(16));
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(panel_brush);
    DeleteObject(panel_pen);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, state.title_font);
    SetTextColor(dc, state.text);
    RECT title{state.panel.left + S(24), state.panel.top, state.panel.right - S(52),
               state.panel.top + S(42)};
    DrawTextW(dc, state.title.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, state.small_font);
    SetTextColor(dc, state.muted);
    RECT subtitle{state.panel.left + S(24), state.panel.top + S(50), state.panel.right - S(24),
                  state.panel.top + S(76)};
    DrawTextW(dc, state.subtitle.c_str(), -1, &subtitle,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT label{state.panel.left + S(24), state.panel.top + S(84), state.panel.right - S(24),
               state.panel.top + S(108)};
    DrawTextW(dc, L"分组名称", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const COLORREF outline = GetFocus() == state.input ? state.accent : state.panel_edge;
    HPEN input_pen = CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ previous_pen = SelectObject(dc, input_pen);
    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, state.panel.left + S(24), state.panel.top + S(112), state.panel.right - S(24),
              state.panel.top + S(147), S(8), S(8));
    SelectObject(dc, previous_brush);
    SelectObject(dc, previous_pen);
    DeleteObject(input_pen);
    HPEN separator = CreatePen(PS_SOLID, 1, state.panel_edge);
    HGDIOBJ old_separator = SelectObject(dc, separator);
    MoveToEx(dc, state.panel.left + 1, state.panel.top + S(42), nullptr);
    LineTo(dc, state.panel.right - 1, state.panel.top + S(42));
    SelectObject(dc, old_separator);
    DeleteObject(separator);
}

void DrawGroupNameButton(GroupNameState &state, const DRAWITEMSTRUCT &item)
{
    RECT rect = item.rcItem;
    const bool primary = item.CtlID == kGroupNameSave;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hovered = state.hovered == item.hwndItem;
    COLORREF fill = primary ? state.accent : state.field;
    COLORREF edge = fill;
    if (hovered || pressed)
    {
        fill = primary ? state.palette.accent_strong : state.field_focus;
        edge = fill;
    }
    // Clear the owner-draw item with the modal surface colour first.  Using
    // the button colour here exposes a square hover background outside the
    // rounded shape.
    HBRUSH clear = CreateSolidBrush(state.panel_color);
    FillRect(item.hDC, &rect, clear);
    DeleteObject(clear);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ old_brush = SelectObject(item.hDC, brush);
    HGDIOBJ old_pen = SelectObject(item.hDC, pen);
    const int radius = DpiScale(state.window, 9);
    RoundRect(item.hDC, rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1, radius,
              radius);
    SelectObject(item.hDC, old_brush);
    SelectObject(item.hDC, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, primary ? RGB(255, 255, 255) : state.text);
    SelectObject(item.hDC, state.font);
    DrawTextW(item.hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK GroupNameWindowProc(HWND h, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto *state = reinterpret_cast<GroupNameState *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        state = static_cast<GroupNameState *>(create->lpCreateParams);
        state->window = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(h, message, wparam, lparam);
    switch (message)
    {
    case WM_CREATE:
        state->input = CreateStyledInput(h, kGroupNameInput, state->font, L"输入分组名称");
        SetStyledInputTheme(state->input, state->palette);
        SetStyledInputText(state->input, state->initial_name);
        SetWindowSubclass(state->input, GroupNameInputProc, kGroupNameInputSubclassId,
                          reinterpret_cast<DWORD_PTR>(state));
        state->cancel = CreateWindowExW(
            0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGroupNameCancel)),
            GetModuleHandleW(nullptr), nullptr);
        state->save = CreateWindowExW(
            0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGroupNameSave)),
            GetModuleHandleW(nullptr), nullptr);
        SetWindowTextW(state->save, L"确定");
        state->close =
            CreateWindowExW(0, L"BUTTON", L"×", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 1, 1, h,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGroupNameClose)),
                            GetModuleHandleW(nullptr), nullptr);
        for (HWND button : {state->cancel, state->save, state->close})
        {
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            SetWindowSubclass(button, GroupNameButtonProc, kGroupNameButtonSubclassId,
                              reinterpret_cast<DWORD_PTR>(state));
        }
        LayoutGroupNameDialog(*state);
        SetFocus(state->input);
        SendMessageW(state->input, EM_SETSEL, 0, -1);
        return 0;
    case WM_SIZE:
        LayoutGroupNameDialog(*state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(h, &paint);
        {
            BufferedWindowPaint buffered(dc, state->panel, paint.rcPaint);
            PaintGroupNameDialog(*state, buffered.dc());
        }
        EndPaint(h, &paint);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, state->text);
        SetBkColor(dc, GetFocus() == state->input ? state->field_focus : state->field);
        return reinterpret_cast<LRESULT>(GetFocus() == state->input ? state->field_focus_brush
                                                                    : state->field_brush);
    }
    case WM_DRAWITEM: {
        const auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(lparam);
        if (item && item->CtlType == ODT_BUTTON)
            DrawGroupNameButton(*state, *item);
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED &&
            (LOWORD(wparam) == kGroupNameCancel || LOWORD(wparam) == kGroupNameClose))
        {
            CloseModalSurface(h);
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kGroupNameSave)
        {
            state->value = GetStyledInputText(state->input);
            if (state->value.find_first_not_of(L" \t\r\n") == std::wstring::npos)
            {
                MessageBoxW(h, L"分组名称不能为空", L"AI CLI", MB_OK | MB_ICONWARNING);
                SetFocus(state->input);
                return 0;
            }
            state->accepted = true;
            CloseModalSurface(h);
            return 0;
        }
        break;
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
    }
    return DefWindowProcW(h, message, wparam, lparam);
}

} // namespace

bool ShowGroupNameDialog(HWND owner, const std::wstring &title, const std::wstring &subtitle,
                         const std::wstring &initial_name, bool light_theme, std::wstring &name)
{
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = GroupNameWindowProc;
        klass.lpszClassName = kGroupNameClassName;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = nullptr;
        RegisterClassW(&klass);
        registered = true;
    }
    GroupNameState state;
    state.owner = owner;
    state.title = title;
    state.subtitle = subtitle;
    state.initial_name = initial_name;
    state.palette = MakeThemePalette(light_theme);
    state.light_theme = state.palette.light();
    state.background = state.palette.window;
    state.panel_color = state.palette.panel;
    state.panel_edge = state.palette.border;
    state.text = state.palette.text;
    state.muted = state.palette.muted;
    state.field = state.palette.field;
    state.field_focus = state.palette.field_focus;
    state.accent = state.palette.accent;
    state.font = CreateDialogFont(owner, 15, FW_NORMAL);
    state.title_font = CreateDialogFont(owner, 16, FW_SEMIBOLD);
    state.small_font = CreateDialogFont(owner, 12, FW_NORMAL);
    ModalWorkspaceScope workspace(owner, -1, &state.palette);
    state.field_brush = CreateSolidBrush(state.field);
    state.field_focus_brush = CreateSolidBrush(state.field_focus);
    state.window = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, kGroupNameClassName, L"",
                                   WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, owner,
                                   nullptr, GetModuleHandleW(nullptr), &state);
    if (state.window)
    {
        workspace.AttachSurface(state.window);
        aicli::ui::ActivateModalSurface(state.window, state.input);
        MSG message{};
        while (IsWindow(state.window) && GetMessageW(&message, nullptr, 0, 0))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (state.field_brush)
        DeleteObject(state.field_brush);
    if (state.field_focus_brush)
        DeleteObject(state.field_focus_brush);
    if (state.font)
        DeleteObject(state.font);
    if (state.title_font)
        DeleteObject(state.title_font);
    if (state.small_font)
        DeleteObject(state.small_font);
    if (state.accepted)
        name = state.value;
    aicli::ui::RestoreModalOwner(owner);
    return state.accepted;
}

} // namespace aicli::ui
