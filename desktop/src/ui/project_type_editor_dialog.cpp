#include "ui/project_type_editor_dialog.h"
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
constexpr wchar_t kProjectTypeEditorClassName[] = L"AicliProjectTypeEditorDialog";
enum : int
{
    kProjectTypeNameInput = 2420,
    kProjectTypeCommandInput = 2421,
    kProjectTypeSave = 2422,
    kProjectTypeCancel = 2423,
    kProjectTypeClose = 2424,
    kProjectTypeDefault = 2425,
};

struct ProjectTypeEditorState
{
    HWND window{};
    HWND owner{};
    HWND name_input{};
    HWND command_input{};
    HWND default_toggle{};
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
    std::wstring initial_command;
    std::wstring name;
    std::wstring command;
    bool initial_default{};
    bool make_default{};
    float default_toggle_progress{};
    float default_toggle_from{};
    ULONGLONG default_toggle_started{};
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
constexpr UINT_PTR kProjectTypeEditorButtonSubclassId = 0xA1C17;
constexpr UINT_PTR kProjectTypeEditorInputSubclassId = 0xA1C18;
constexpr UINT_PTR kProjectTypeEditorToggleTimerId = 0xA1C19;
constexpr ULONGLONG kProjectTypeEditorToggleDurationMs = 180;

bool AdvanceProjectTypeDefaultToggle(ProjectTypeEditorState &state)
{
    if (!state.default_toggle_started)
    {
        state.default_toggle_progress = state.make_default ? 1.0f : 0.0f;
        return true;
    }
    const auto elapsed = GetTickCount64() - state.default_toggle_started;
    const float t = std::clamp(static_cast<float>(elapsed) /
                                   static_cast<float>(kProjectTypeEditorToggleDurationMs),
                               0.0f, 1.0f);
    const float inverse = 1.0f - t;
    const float eased = 1.0f - inverse * inverse * inverse;
    const float target = state.make_default ? 1.0f : 0.0f;
    state.default_toggle_progress =
        state.default_toggle_from + (target - state.default_toggle_from) * eased;
    if (t < 1.0f)
        return false;
    state.default_toggle_progress = target;
    state.default_toggle_started = 0;
    return true;
}

void AnimateProjectTypeDefaultToggle(ProjectTypeEditorState &state)
{
    AdvanceProjectTypeDefaultToggle(state);
    state.default_toggle_from = state.default_toggle_progress;
    state.make_default = !state.make_default;
    state.default_toggle_started = GetTickCount64();
    SetTimer(state.window, kProjectTypeEditorToggleTimerId, 15, nullptr);
    InvalidateRect(state.default_toggle, nullptr, FALSE);
}

LRESULT CALLBACK ProjectTypeEditorButtonProc(HWND control, UINT message, WPARAM wparam,
                                             LPARAM lparam, UINT_PTR, DWORD_PTR reference)
{
    auto *state = reinterpret_cast<ProjectTypeEditorState *>(reference);
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
        RemoveWindowSubclass(control, ProjectTypeEditorButtonProc,
                             kProjectTypeEditorButtonSubclassId);
        break;
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK ProjectTypeEditorInputProc(HWND control, UINT message, WPARAM wparam,
                                            LPARAM lparam, UINT_PTR, DWORD_PTR reference)
{
    auto *state = reinterpret_cast<ProjectTypeEditorState *>(reference);
    if (state)
    {
        if (message == WM_KEYDOWN)
        {
            if (wparam == VK_RETURN)
            {
                if (control == state->name_input)
                {
                    SetFocus(state->command_input);
                    SendMessageW(state->command_input, EM_SETSEL, 0, -1);
                }
                else
                {
                    SendMessageW(state->window, WM_COMMAND,
                                 MAKEWPARAM(kProjectTypeSave, BN_CLICKED),
                                 reinterpret_cast<LPARAM>(state->save));
                }
                return 0;
            }
            if (wparam == VK_ESCAPE)
            {
                CloseModalSurface(state->window);
                return 0;
            }
        }
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
        {
            InvalidateRect(state->window, &state->panel, FALSE);
        }
    }
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(control, ProjectTypeEditorInputProc,
                             kProjectTypeEditorInputSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

void LayoutProjectTypeEditorDialog(ProjectTypeEditorState &state)
{
    RECT client{};
    GetClientRect(state.window, &client);
    const int client_width = static_cast<int>(client.right);
    const int client_height = static_cast<int>(client.bottom);
    const auto S = [&](int value) { return DpiScale(state.window, value); };
    const int width = std::min(S(640), std::max(S(500), client_width - S(120)));
    const int height = S(420);
    const int left = (client_width - width) / 2;
    const int top = std::max(S(24), (client_height - height) / 2);
    state.panel = {left, top, left + width, top + height};
    LayoutStyledInput(state.name_input,
                      {left + S(30), top + S(120), left + width - S(30), top + S(155)}, S(2), S(8));
    LayoutStyledInput(state.command_input,
                      {left + S(30), top + S(204), left + width - S(30), top + S(239)}, S(2), S(8));
    MoveWindow(state.default_toggle, left + S(30), top + S(282), width - S(60), S(48), TRUE);
    MoveWindow(state.cancel, left + width - S(238), top + S(356), S(100), S(36), TRUE);
    MoveWindow(state.save, left + width - S(126), top + S(356), S(96), S(36), TRUE);
    MoveWindow(state.close, left + width - S(42), top + S(5), S(32), S(32), TRUE);
    SetWindowRgn(state.cancel, CreateRoundRectRgn(0, 0, S(101), S(39), S(9), S(9)), TRUE);
    SetWindowRgn(state.save, CreateRoundRectRgn(0, 0, S(109), S(39), S(9), S(9)), TRUE);
    // The default card paints its own antialiased rounded edge. A GDI window
    // region clips the blended corner pixels and is the source of the jagged
    // outline that was visible around this control.
    SetWindowRgn(state.default_toggle, nullptr, FALSE);
    SetWindowRgn(state.close, CreateRoundRectRgn(0, 0, S(33), S(33), S(7), S(7)), TRUE);
    // Match the window region to the 16 px RoundRect curve painted below.
    // A smaller region admits four corner pixels outside the painted panel.
    SetModalSurfaceRegion(state.window, state.panel, S(16));
    InvalidateRect(state.window, nullptr, FALSE);
}

void PaintProjectTypeEditorDialog(ProjectTypeEditorState &state, HDC dc)
{
    RECT client{};
    GetClientRect(state.window, &client);
    (void)client;

    const auto S = [&](int value) { return DpiScale(state.window, value); };
    // Seed every pixel admitted by the rounded window region. Without this,
    // the modal's initial white surface can remain visible at the four
    // extreme corner samples before the anti-aliased edge is painted.
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
    RECT subtitle{state.panel.left + S(30), state.panel.top + S(50), state.panel.right - S(30),
                  state.panel.top + S(80)};
    DrawTextW(dc, state.subtitle.c_str(), -1, &subtitle,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    auto label = [&](const wchar_t *value, int y) {
        RECT rect{state.panel.left + S(30), state.panel.top + S(y), state.panel.right - S(30),
                  state.panel.top + S(y + 24)};
        DrawTextW(dc, value, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    label(L"类型名称", 90);
    label(L"默认启动命令", 174);

    auto outline = [&](HWND input, RECT rect) {
        const COLORREF color = GetFocus() == input ? state.accent : state.panel_edge;
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ previous_pen = SelectObject(dc, pen);
        HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, S(10), S(10));
        SelectObject(dc, previous_brush);
        SelectObject(dc, previous_pen);
        DeleteObject(pen);
    };
    outline(state.name_input, {state.panel.left + S(30), state.panel.top + S(120),
                               state.panel.right - S(30), state.panel.top + S(155)});
    outline(state.command_input, {state.panel.left + S(30), state.panel.top + S(204),
                                  state.panel.right - S(30), state.panel.top + S(239)});

    SetTextColor(dc, state.muted);
    SelectObject(dc, state.small_font);
    RECT hint{state.panel.left + S(30), state.panel.top + S(248), state.panel.right - S(30),
              state.panel.top + S(276)};
    DrawTextW(dc, L"启动命令会作为新建项目的默认值，项目仍可单独覆盖。", -1, &hint,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    HPEN separator = CreatePen(PS_SOLID, 1, state.panel_edge);
    HGDIOBJ old_separator = SelectObject(dc, separator);
    MoveToEx(dc, state.panel.left + 1, state.panel.top + S(42), nullptr);
    LineTo(dc, state.panel.right - 1, state.panel.top + S(42));
    SelectObject(dc, old_separator);
    DeleteObject(separator);
}

void DrawProjectTypeEditorButton(ProjectTypeEditorState &state, const DRAWITEMSTRUCT &item)
{
    RECT rect = item.rcItem;
    if (item.CtlID == kProjectTypeDefault)
    {
        BufferedWindowPaint buffered(item.hDC, rect, rect);
        HDC dc = buffered.dc();
        const bool hovered = state.hovered == item.hwndItem;
        HBRUSH clear = CreateSolidBrush(state.panel_color);
        FillRect(dc, &rect, clear);
        DeleteObject(clear);

        const COLORREF fill = hovered ? state.field_focus : state.field;
        const auto S = [&](int value) { return DpiScale(state.window, value); };
        RECT card{rect.left + S(1), rect.top + S(1), rect.right - S(1), rect.bottom - S(1)};
        DrawSmoothRoundedRect(dc, card, static_cast<float>(S(8)), fill, state.panel_edge,
                              static_cast<float>(std::max(1, S(1))));

        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, state.font);
        SetTextColor(dc, state.text);
        RECT title{rect.left + S(14), rect.top + S(4), rect.right - S(74), rect.top + S(27)};
        DrawTextW(dc, L"默认项目类型", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, state.small_font);
        SetTextColor(dc, state.muted);
        RECT detail{rect.left + S(14), rect.top + S(23), rect.right - S(74), rect.bottom - S(2)};
        DrawTextW(dc, L"新建项目时自动选中；设置后会替换原默认类型", -1, &detail,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT track{rect.right - S(58), rect.top + S(14), rect.right - S(14), rect.top + S(36)};
        const BYTE blend =
            static_cast<BYTE>(std::clamp(state.default_toggle_progress * 255.0f, 0.0f, 255.0f));
        const COLORREF track_color = BlendBackdropColor(state.panel_edge, state.accent, blend);

        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        Gdiplus::RectF track_bounds{static_cast<float>(track.left), static_cast<float>(track.top),
                                    static_cast<float>(track.right - track.left),
                                    static_cast<float>(track.bottom - track.top)};
        Gdiplus::GraphicsPath track_path;
        AddRoundedPath(track_path, track_bounds, track_bounds.Height / 2.0f);
        Gdiplus::SolidBrush track_brush(SmoothColor(track_color));
        graphics.FillPath(&track_brush, &track_path);

        const float inset = static_cast<float>(S(2));
        const float knob = static_cast<float>(S(18));
        const float travel = std::max(0.0f, track_bounds.Width - knob - inset * 2.0f);
        const float knob_left = track_bounds.X + inset + travel * state.default_toggle_progress;
        Gdiplus::SolidBrush knob_brush(SmoothColor(RGB(255, 255, 255)));
        graphics.FillEllipse(&knob_brush, knob_left, track_bounds.Y + inset, knob, knob);
        return;
    }
    const bool primary = item.CtlID == kProjectTypeSave;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hovered = state.hovered == item.hwndItem;
    COLORREF fill = primary ? state.accent : state.field;
    COLORREF edge = fill;
    if (hovered || pressed)
    {
        fill = primary ? state.palette.accent_strong : state.field_focus;
        edge = fill;
    }
    // Keep the area outside the rounded button transparent to the modal
    // surface, including while the pointer is hovering the control.
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

LRESULT CALLBACK ProjectTypeEditorWindowProc(HWND h, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto *state = reinterpret_cast<ProjectTypeEditorState *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        state = static_cast<ProjectTypeEditorState *>(create->lpCreateParams);
        state->window = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(h, message, wparam, lparam);
    switch (message)
    {
    case WM_CREATE:
        state->name_input =
            CreateStyledInput(h, kProjectTypeNameInput, state->font, L"输入类型名称");
        state->command_input =
            CreateStyledInput(h, kProjectTypeCommandInput, state->font, L"输入默认启动命令");
        SetStyledInputTheme(state->name_input, state->palette);
        SetStyledInputTheme(state->command_input, state->palette);
        SetStyledInputText(state->name_input, state->initial_name);
        SetStyledInputText(state->command_input, state->initial_command);
        for (HWND input : {state->name_input, state->command_input})
        {
            SetWindowSubclass(input, ProjectTypeEditorInputProc, kProjectTypeEditorInputSubclassId,
                              reinterpret_cast<DWORD_PTR>(state));
        }
        state->default_toggle = CreateWindowExW(
            0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectTypeDefault)),
            GetModuleHandleW(nullptr), nullptr);
        state->cancel = CreateWindowExW(
            0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectTypeCancel)),
            GetModuleHandleW(nullptr), nullptr);
        state->save = CreateWindowExW(
            0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectTypeSave)),
            GetModuleHandleW(nullptr), nullptr);
        SetWindowTextW(state->save, L"确定");
        state->close =
            CreateWindowExW(0, L"BUTTON", L"×", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 1, 1, h,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectTypeClose)),
                            GetModuleHandleW(nullptr), nullptr);
        for (HWND button : {state->default_toggle, state->cancel, state->save, state->close})
        {
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            SetWindowSubclass(button, ProjectTypeEditorButtonProc,
                              kProjectTypeEditorButtonSubclassId,
                              reinterpret_cast<DWORD_PTR>(state));
        }
        LayoutProjectTypeEditorDialog(*state);
        SetFocus(state->name_input);
        SendMessageW(state->name_input, EM_SETSEL, 0, -1);
        return 0;
    case WM_SIZE:
        LayoutProjectTypeEditorDialog(*state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(h, &paint);
        {
            BufferedWindowPaint buffered(dc, state->panel, paint.rcPaint);
            PaintProjectTypeEditorDialog(*state, buffered.dc());
        }
        EndPaint(h, &paint);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        HWND control = reinterpret_cast<HWND>(lparam);
        const bool focused = GetFocus() == control;
        SetTextColor(dc, state->text);
        SetBkColor(dc, focused ? state->field_focus : state->field);
        return reinterpret_cast<LRESULT>(focused ? state->field_focus_brush : state->field_brush);
    }
    case WM_DRAWITEM: {
        const auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(lparam);
        if (item && item->CtlType == ODT_BUTTON)
        {
            DrawProjectTypeEditorButton(*state, *item);
        }
        return TRUE;
    }
    case WM_TIMER:
        if (wparam == kProjectTypeEditorToggleTimerId)
        {
            if (AdvanceProjectTypeDefaultToggle(*state))
            {
                KillTimer(h, kProjectTypeEditorToggleTimerId);
            }
            InvalidateRect(state->default_toggle, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kProjectTypeDefault)
        {
            AnimateProjectTypeDefaultToggle(*state);
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED &&
            (LOWORD(wparam) == kProjectTypeCancel || LOWORD(wparam) == kProjectTypeClose))
        {
            CloseModalSurface(h);
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kProjectTypeSave)
        {
            state->name = GetStyledInputText(state->name_input);
            state->command = GetStyledInputText(state->command_input);
            if (state->name.find_first_not_of(L" \t\r\n") == std::wstring::npos)
            {
                MessageBoxW(h, L"类型名称不能为空", L"AI CLI", MB_OK | MB_ICONWARNING);
                SetFocus(state->name_input);
                return 0;
            }
            if (state->command.find_first_not_of(L" \t\r\n") == std::wstring::npos)
            {
                MessageBoxW(h, L"默认启动命令不能为空", L"AI CLI", MB_OK | MB_ICONWARNING);
                SetFocus(state->command_input);
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
    case WM_DESTROY:
        KillTimer(h, kProjectTypeEditorToggleTimerId);
        return 0;
    }
    return DefWindowProcW(h, message, wparam, lparam);
}

} // namespace

bool ShowProjectTypeEditorDialog(HWND owner, const std::wstring &title,
                                 const std::wstring &subtitle, const std::wstring &initial_name,
                                 const std::wstring &initial_command, bool initial_default,
                                 bool light_theme, std::wstring &name, std::wstring &command,
                                 bool &make_default)
{
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = ProjectTypeEditorWindowProc;
        klass.lpszClassName = kProjectTypeEditorClassName;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = nullptr;
        RegisterClassW(&klass);
        registered = true;
    }

    ProjectTypeEditorState state;
    state.owner = owner;
    state.title = title;
    state.subtitle = subtitle;
    state.initial_name = initial_name;
    state.initial_command = initial_command;
    state.initial_default = initial_default;
    state.make_default = initial_default;
    state.default_toggle_progress = initial_default ? 1.0f : 0.0f;
    state.default_toggle_from = state.default_toggle_progress;
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
    state.window =
        CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, kProjectTypeEditorClassName, L"",
                        WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, owner, nullptr,
                        GetModuleHandleW(nullptr), &state);
    if (state.window)
    {
        workspace.AttachSurface(state.window);
        aicli::ui::ActivateModalSurface(state.window);
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
    {
        name = state.name;
        command = state.command;
        make_default = state.make_default;
    }
    aicli::ui::RestoreModalOwner(owner);
    return state.accepted;
}

} // namespace aicli::ui
