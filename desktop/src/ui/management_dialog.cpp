#include "ui/management_dialog.h"

#include "ui/styled_input.h"
#include "ui/theme.h"
#include "ui/layout_metrics.h"
#include "ui/modal_host.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")

namespace aicli::ui {
namespace {

constexpr wchar_t kClassName[] = L"AicliManagementDialog";
constexpr wchar_t kSettingsContentClassName[] = L"AicliSettingsContent";
constexpr wchar_t kGroupNameClassName[] = L"AicliGroupNameDialog";
constexpr wchar_t kProjectTypeEditorClassName[] = L"AicliProjectTypeEditorDialog";
constexpr UINT kPopulateMessage = WM_APP + 60;
enum : int {
    kList = 2301,
    kName = 2302,
    kCommand = 2303,
    kListen = 2304,
    kPort = 2305,
    kLocal = 2306,
    kRemote = 2307,
    kTheme = 2308,
    kAdd = 2310,
    kSave = 2311,
    kDelete = 2312,
    kSelect = 2313,
    kClose = 2314,
    kTypes = 2315,
    kUnbind = 2316,
    kFirewall = 2317,
    kThemeLight = 2318,
    kSettingsAppearance = 2320,
    kSettingsConnection = 2321,
    kSettingsProjectTypes = 2322,
    kSettingsGroups = 2323,
};

enum : int {
    kGroupNameInput = 2410,
    kGroupNameSave = 2411,
    kGroupNameCancel = 2412,
};

enum : int {
    kProjectTypeNameInput = 2420,
    kProjectTypeCommandInput = 2421,
    kProjectTypeSave = 2422,
    kProjectTypeCancel = 2423,
};

enum class SettingsPage {
    Appearance,
    Connection,
    ProjectTypes,
    Groups,
};

COLORREF BlendBackdrop(COLORREF base, COLORREF overlay, BYTE alpha) {
    const auto blend = [alpha](BYTE from, BYTE to) -> BYTE {
        return static_cast<BYTE>((static_cast<unsigned>(from) * (255u - alpha) +
                                  static_cast<unsigned>(to) * alpha) / 255u);
    };
    return RGB(blend(GetRValue(base), GetRValue(overlay)),
               blend(GetGValue(base), GetGValue(overlay)),
               blend(GetBValue(base), GetBValue(overlay)));
}

const ThemePalette& DarkPalette() {
    static const ThemePalette palette = MakeThemePalette(ThemeMode::Dark);
    return palette;
}

struct State {
    HWND window{};
    HWND owner{};
    ManagementMode mode{};
    DataStore* store{};
    agent::RemoteAgent* agent{};
    AgentRestartCallback restart_agent;
    ThemeChangedCallback theme_changed;
    ManagementDialogResult result;
    RECT panel{};
    RECT content_rect{};
    HFONT font{};
    HFONT title_font{};
    HFONT small_font{};
    HWND list{};
    HWND name{};
    HWND command{};
    HWND listen{};
    HWND port{};
    HWND local{};
    HWND remote{};
    HWND theme{};
    HWND theme_light{};
    HWND add{};
    HWND save{};
    HWND remove{};
    HWND select{};
    HWND close{};
    HWND types{};
    HWND unbind{};
    HWND firewall{};
    HWND content{};
    HWND settings_appearance{};
    HWND settings_connection{};
    HWND settings_project_types{};
    HWND settings_groups{};
    HWND title_close{};
    HWND hovered_control{};
    std::vector<ProjectGroup> groups;
    std::vector<ProjectType> types_data;
    std::vector<DeviceRecord> devices;
    ThemePalette palette;
    bool light_theme{};
    SettingsPage settings_page{SettingsPage::Appearance};
    int content_scroll{};
    int content_extent{};
    COLORREF background_color{};
    COLORREF panel_color{};
    COLORREF panel_edge{};
    COLORREF text_color{};
    COLORREF muted_color{};
    COLORREF field_color{};
    COLORREF field_focus_color{};
    COLORREF accent_color{};
    HBRUSH field_brush{};
    HBRUSH field_focus_brush{};
    HBRUSH list_brush{};
};

struct GroupNameState {
    HWND window{};
    HWND owner{};
    HWND input{};
    HWND save{};
    HWND cancel{};
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

struct ProjectTypeEditorState {
    HWND window{};
    HWND owner{};
    HWND name_input{};
    HWND command_input{};
    HWND save{};
    HWND cancel{};
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

bool ShowProjectTypeEditorDialog(HWND owner,
                                 const std::wstring& title,
                                 const std::wstring& subtitle,
                                 const std::wstring& initial_name,
                                 const std::wstring& initial_command,
                                 bool light_theme,
                                 std::wstring& name,
                                 std::wstring& command);

State* GetState(HWND h) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

State* FindState(HWND h) {
    for (HWND current = h; current; current = GetParent(current)) {
        if (auto* state = GetState(current)) return state;
    }
    return nullptr;
}

bool IsSettingsProjectTypes(const State& state) {
    return state.mode == ManagementMode::ProjectTypes ||
           (state.mode == ManagementMode::Settings &&
            state.settings_page == SettingsPage::ProjectTypes);
}

bool IsSettingsGroups(const State& state) {
    return state.mode == ManagementMode::Groups ||
           (state.mode == ManagementMode::Settings &&
            state.settings_page == SettingsPage::Groups);
}

bool IsSettingsNavigationId(int id) {
    return id == kSettingsAppearance || id == kSettingsConnection ||
           id == kSettingsProjectTypes || id == kSettingsGroups;
}

bool IsActiveSettingsNavigation(const State& state, int id) {
    if (state.mode != ManagementMode::Settings) return false;
    if (id == kSettingsAppearance) return state.settings_page == SettingsPage::Appearance;
    if (id == kSettingsConnection) return state.settings_page == SettingsPage::Connection;
    if (id == kSettingsProjectTypes) return state.settings_page == SettingsPage::ProjectTypes;
    if (id == kSettingsGroups) return state.settings_page == SettingsPage::Groups;
    return false;
}

LRESULT CALLBACK SettingsContentProc(HWND h, UINT message, WPARAM wparam, LPARAM lparam);
void LayoutSettingsContent(State& state);
void UpdateSettingsPage(State& state, SettingsPage page);
void RefreshList(State& state);
void ReadSelection(State& state);

constexpr UINT_PTR kManagementButtonSubclassId = 0xA1C14;

LRESULT CALLBACK ManagementButtonProc(HWND control, UINT message, WPARAM wparam,
                                      LPARAM lparam, UINT_PTR, DWORD_PTR) {
    State* state = FindState(control);
    switch (message) {
    case WM_ERASEBKGND:
        // Every button in the management surface is owner-drawn.  Letting
        // the stock BUTTON class erase first briefly exposes its default
        // white brush in the dark theme before WM_DRAWITEM paints the themed
        // surface, which is visible as a one-frame flash while the modal
        // opens or a button receives focus.
        return 1;
    case WM_MOUSEMOVE:
        if (state && state->hovered_control != control) {
            HWND previous = state->hovered_control;
            state->hovered_control = control;
            if (previous) InvalidateRect(previous, nullptr, FALSE);
            InvalidateRect(control, nullptr, FALSE);
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
            TrackMouseEvent(&tracking);
        }
        break;
    case WM_MOUSELEAVE:
        if (state && state->hovered_control == control) state->hovered_control = nullptr;
        InvalidateRect(control, nullptr, FALSE);
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(control, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        if (state && state->hovered_control == control) state->hovered_control = nullptr;
        RemoveWindowSubclass(control, ManagementButtonProc, kManagementButtonSubclassId);
        break;
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

void ApplyTheme(State& state, bool light) {
    state.palette = MakeThemePalette(light);
    state.light_theme = state.palette.light();
    state.background_color = state.palette.window;
    state.panel_color = state.palette.panel;
    state.panel_edge = state.palette.border;
    state.text_color = state.palette.text;
    state.muted_color = state.palette.muted;
    state.field_color = state.palette.field;
    state.field_focus_color = state.palette.field_focus;
    state.accent_color = state.palette.accent;
    if (state.field_brush) DeleteObject(state.field_brush);
    if (state.field_focus_brush) DeleteObject(state.field_focus_brush);
    if (state.list_brush) DeleteObject(state.list_brush);
    state.field_brush = CreateSolidBrush(state.field_color);
    state.field_focus_brush = CreateSolidBrush(state.field_focus_color);
    state.list_brush = CreateSolidBrush(state.palette.list_surface);
    for (HWND field : {state.name, state.command, state.listen, state.port,
                       state.local, state.remote}) {
        if (field) SetStyledInputTheme(field, state.palette);
    }
}

constexpr UINT_PTR kGroupNameButtonSubclassId = 0xA1C15;
constexpr UINT_PTR kGroupNameInputSubclassId = 0xA1C16;

LRESULT CALLBACK GroupNameButtonProc(HWND control, UINT message, WPARAM wparam,
                                     LPARAM lparam, UINT_PTR, DWORD_PTR reference) {
    auto* state = reinterpret_cast<GroupNameState*>(reference);
    switch (message) {
    case WM_MOUSEMOVE:
        if (state && state->hovered != control) {
            HWND previous = state->hovered;
            state->hovered = control;
            if (previous) InvalidateRect(previous, nullptr, FALSE);
            InvalidateRect(control, nullptr, FALSE);
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
            TrackMouseEvent(&tracking);
        }
        break;
    case WM_MOUSELEAVE:
        if (state && state->hovered == control) state->hovered = nullptr;
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

LRESULT CALLBACK GroupNameInputProc(HWND control, UINT message, WPARAM wparam,
                                    LPARAM lparam, UINT_PTR, DWORD_PTR reference) {
    auto* state = reinterpret_cast<GroupNameState*>(reference);
    if (message == WM_KEYDOWN && state) {
        if (wparam == VK_RETURN) {
            SendMessageW(state->window, WM_COMMAND,
                         MAKEWPARAM(kGroupNameSave, BN_CLICKED),
                         reinterpret_cast<LPARAM>(state->save));
            return 0;
        }
        if (wparam == VK_ESCAPE) {
            DestroyWindow(state->window);
            return 0;
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control, GroupNameInputProc, kGroupNameInputSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

void LayoutGroupNameDialog(GroupNameState& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int client_width = static_cast<int>(client.right);
    const int client_height = static_cast<int>(client.bottom);
    const int width = std::min(540, std::max(430, client_width - 120));
    const int height = 270;
    const int left = (client_width - width) / 2;
    const int top = std::max(24, (client_height - height) / 2);
    state.panel = {left, top, left + width, top + height};
    LayoutStyledInput(state.input, {left + 30, top + 126, left + width - 30, top + 168}, 2, 9);
    MoveWindow(state.cancel, left + width - 250, top + 208, 100, 38, TRUE);
    MoveWindow(state.save, left + width - 138, top + 208, 108, 38, TRUE);
    SetWindowRgn(state.cancel, CreateRoundRectRgn(0, 0, 101, 39, 9, 9), TRUE);
    SetWindowRgn(state.save, CreateRoundRectRgn(0, 0, 109, 39, 9, 9), TRUE);
    SetModalSurfaceRegion(state.window, state.panel, 16);
    InvalidateRect(state.window, nullptr, FALSE);
}

void PaintGroupNameDialog(GroupNameState& state, HDC dc) {
    RECT client{};
    GetClientRect(state.window, &client);
    // The layered ModalWorkspaceScope scrim is behind this content window.
    // Leave the host background untouched so the dimmed owner remains visible.
    (void)client;

    HBRUSH panel_brush = CreateSolidBrush(state.panel_color);
    HPEN panel_pen = CreatePen(PS_SOLID, 1, state.panel_edge);
    HGDIOBJ old_brush = SelectObject(dc, panel_brush);
    HGDIOBJ old_pen = SelectObject(dc, panel_pen);
    RoundRect(dc, state.panel.left, state.panel.top, state.panel.right, state.panel.bottom, 16, 16);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(panel_brush);
    DeleteObject(panel_pen);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, state.title_font);
    SetTextColor(dc, state.text);
    RECT title{state.panel.left + 30, state.panel.top + 24,
               state.panel.right - 30, state.panel.top + 54};
    DrawTextW(dc, state.title.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, state.small_font);
    SetTextColor(dc, state.muted);
    RECT subtitle{state.panel.left + 30, state.panel.top + 58,
                  state.panel.right - 30, state.panel.top + 88};
    DrawTextW(dc, state.subtitle.c_str(), -1, &subtitle,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT label{state.panel.left + 30, state.panel.top + 98,
               state.panel.right - 30, state.panel.top + 122};
    DrawTextW(dc, L"分组名称", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const COLORREF outline = GetFocus() == state.input ? state.accent : state.panel_edge;
    HPEN input_pen = CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ previous_pen = SelectObject(dc, input_pen);
    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, state.panel.left + 30, state.panel.top + 126,
              state.panel.right - 30, state.panel.top + 168, 10, 10);
    SelectObject(dc, previous_brush);
    SelectObject(dc, previous_pen);
    DeleteObject(input_pen);
}

void DrawGroupNameButton(GroupNameState& state, const DRAWITEMSTRUCT& item) {
    RECT rect = item.rcItem;
    const bool primary = item.CtlID == kGroupNameSave;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hovered = state.hovered == item.hwndItem;
    COLORREF fill = primary ? state.accent : state.field;
    COLORREF edge = primary ? state.accent : state.panel_edge;
    if (hovered || pressed) {
        fill = primary ? state.palette.accent_strong : state.field_focus;
        edge = primary ? RGB(91, 157, 236) : state.muted;
    }
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    FillRect(item.hDC, &rect, brush);
    HGDIOBJ old_brush = SelectObject(item.hDC, brush);
    HGDIOBJ old_pen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1, 9, 9);
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

LRESULT CALLBACK GroupNameWindowProc(HWND h, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<GroupNameState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<GroupNameState*>(create->lpCreateParams);
        state->window = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(h, message, wparam, lparam);
    switch (message) {
    case WM_CREATE:
        state->input = CreateStyledInput(h, kGroupNameInput, state->font, L"输入分组名称");
        SetStyledInputTheme(state->input, state->palette);
        SetStyledInputText(state->input, state->initial_name);
        SetWindowSubclass(state->input, GroupNameInputProc, kGroupNameInputSubclassId,
                          reinterpret_cast<DWORD_PTR>(state));
        state->cancel = CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGroupNameCancel)),
            GetModuleHandleW(nullptr), nullptr);
        state->save = CreateWindowExW(0, L"BUTTON", L"保存",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGroupNameSave)),
            GetModuleHandleW(nullptr), nullptr);
        for (HWND button : {state->cancel, state->save}) {
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
        PaintGroupNameDialog(*state, dc);
        EndPaint(h, &paint);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, state->text);
        SetBkColor(dc, GetFocus() == state->input ? state->field_focus : state->field);
        return reinterpret_cast<LRESULT>(GetFocus() == state->input
            ? state->field_focus_brush : state->field_brush);
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (item && item->CtlType == ODT_BUTTON) DrawGroupNameButton(*state, *item);
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kGroupNameCancel) {
            DestroyWindow(h);
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kGroupNameSave) {
            state->value = GetStyledInputText(state->input);
            if (state->value.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
                MessageBoxW(h, L"分组名称不能为空", L"AI CLI", MB_OK | MB_ICONWARNING);
                SetFocus(state->input);
                return 0;
            }
            state->accepted = true;
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, message, wparam, lparam);
}

constexpr UINT_PTR kProjectTypeEditorButtonSubclassId = 0xA1C17;
constexpr UINT_PTR kProjectTypeEditorInputSubclassId = 0xA1C18;

LRESULT CALLBACK ProjectTypeEditorButtonProc(HWND control, UINT message, WPARAM wparam,
                                              LPARAM lparam, UINT_PTR,
                                              DWORD_PTR reference) {
    auto* state = reinterpret_cast<ProjectTypeEditorState*>(reference);
    switch (message) {
    case WM_MOUSEMOVE:
        if (state && state->hovered != control) {
            HWND previous = state->hovered;
            state->hovered = control;
            if (previous) InvalidateRect(previous, nullptr, FALSE);
            InvalidateRect(control, nullptr, FALSE);
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
            TrackMouseEvent(&tracking);
        }
        break;
    case WM_MOUSELEAVE:
        if (state && state->hovered == control) state->hovered = nullptr;
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
                                             LPARAM lparam, UINT_PTR,
                                             DWORD_PTR reference) {
    auto* state = reinterpret_cast<ProjectTypeEditorState*>(reference);
    if (state) {
        if (message == WM_KEYDOWN) {
            if (wparam == VK_RETURN) {
                if (control == state->name_input) {
                    SetFocus(state->command_input);
                    SendMessageW(state->command_input, EM_SETSEL, 0, -1);
                } else {
                    SendMessageW(state->window, WM_COMMAND,
                                 MAKEWPARAM(kProjectTypeSave, BN_CLICKED),
                                 reinterpret_cast<LPARAM>(state->save));
                }
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                DestroyWindow(state->window);
                return 0;
            }
        }
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
            InvalidateRect(state->window, &state->panel, FALSE);
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control, ProjectTypeEditorInputProc,
                             kProjectTypeEditorInputSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

void LayoutProjectTypeEditorDialog(ProjectTypeEditorState& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int client_width = static_cast<int>(client.right);
    const int client_height = static_cast<int>(client.bottom);
    const int width = std::min(640, std::max(500, client_width - 120));
    const int height = 390;
    const int left = (client_width - width) / 2;
    const int top = std::max(24, (client_height - height) / 2);
    state.panel = {left, top, left + width, top + height};
    LayoutStyledInput(state.name_input,
                      {left + 30, top + 132, left + width - 30, top + 174}, 2, 9);
    LayoutStyledInput(state.command_input,
                      {left + 30, top + 222, left + width - 30, top + 264}, 2, 9);
    MoveWindow(state.cancel, left + width - 250, top + 326, 100, 38, TRUE);
    MoveWindow(state.save, left + width - 138, top + 326, 108, 38, TRUE);
    SetWindowRgn(state.cancel, CreateRoundRectRgn(0, 0, 101, 39, 9, 9), TRUE);
    SetWindowRgn(state.save, CreateRoundRectRgn(0, 0, 109, 39, 9, 9), TRUE);
    SetModalSurfaceRegion(state.window, state.panel, 16);
    InvalidateRect(state.window, nullptr, FALSE);
}

void PaintProjectTypeEditorDialog(ProjectTypeEditorState& state, HDC dc) {
    RECT client{};
    GetClientRect(state.window, &client);
    (void)client;

    HBRUSH panel_brush = CreateSolidBrush(state.panel_color);
    HPEN panel_pen = CreatePen(PS_SOLID, 1, state.panel_edge);
    HGDIOBJ old_brush = SelectObject(dc, panel_brush);
    HGDIOBJ old_pen = SelectObject(dc, panel_pen);
    RoundRect(dc, state.panel.left, state.panel.top, state.panel.right,
              state.panel.bottom, 16, 16);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(panel_brush);
    DeleteObject(panel_pen);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, state.title_font);
    SetTextColor(dc, state.text);
    RECT title{state.panel.left + 30, state.panel.top + 24,
               state.panel.right - 30, state.panel.top + 54};
    DrawTextW(dc, state.title.c_str(), -1, &title,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, state.small_font);
    SetTextColor(dc, state.muted);
    RECT subtitle{state.panel.left + 30, state.panel.top + 58,
                  state.panel.right - 30, state.panel.top + 88};
    DrawTextW(dc, state.subtitle.c_str(), -1, &subtitle,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    auto label = [&](const wchar_t* value, int y) {
        RECT rect{state.panel.left + 30, state.panel.top + y,
                  state.panel.right - 30, state.panel.top + y + 24};
        DrawTextW(dc, value, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    label(L"类型名称", 102);
    label(L"默认启动命令", 192);

    auto outline = [&](HWND input, RECT rect) {
        const COLORREF color = GetFocus() == input ? state.accent : state.panel_edge;
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ previous_pen = SelectObject(dc, pen);
        HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 10, 10);
        SelectObject(dc, previous_brush);
        SelectObject(dc, previous_pen);
        DeleteObject(pen);
    };
    outline(state.name_input,
            {state.panel.left + 30, state.panel.top + 132,
             state.panel.right - 30, state.panel.top + 174});
    outline(state.command_input,
            {state.panel.left + 30, state.panel.top + 222,
             state.panel.right - 30, state.panel.top + 264});

    SetTextColor(dc, state.muted);
    RECT hint{state.panel.left + 30, state.panel.top + 278,
              state.panel.right - 30, state.panel.top + 308};
    DrawTextW(dc, L"启动命令会作为新建项目的默认值，项目仍可单独覆盖。", -1,
              &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void DrawProjectTypeEditorButton(ProjectTypeEditorState& state,
                                 const DRAWITEMSTRUCT& item) {
    RECT rect = item.rcItem;
    const bool primary = item.CtlID == kProjectTypeSave;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hovered = state.hovered == item.hwndItem;
    COLORREF fill = primary ? state.accent : state.field;
    COLORREF edge = primary ? state.accent : state.panel_edge;
    if (hovered || pressed) {
        fill = primary ? state.palette.accent_strong : state.field_focus;
        edge = primary ? RGB(91, 157, 236) : state.muted;
    }
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    FillRect(item.hDC, &rect, brush);
    HGDIOBJ old_brush = SelectObject(item.hDC, brush);
    HGDIOBJ old_pen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, rect.left + 1, rect.top + 1,
              rect.right - 1, rect.bottom - 1, 9, 9);
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

LRESULT CALLBACK ProjectTypeEditorWindowProc(HWND h, UINT message,
                                              WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<ProjectTypeEditorState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<ProjectTypeEditorState*>(create->lpCreateParams);
        state->window = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(h, message, wparam, lparam);
    switch (message) {
    case WM_CREATE:
        state->name_input = CreateStyledInput(h, kProjectTypeNameInput,
                                              state->font, L"输入类型名称");
        state->command_input = CreateStyledInput(h, kProjectTypeCommandInput,
                                                 state->font, L"输入默认启动命令");
        SetStyledInputTheme(state->name_input, state->palette);
        SetStyledInputTheme(state->command_input, state->palette);
        SetStyledInputText(state->name_input, state->initial_name);
        SetStyledInputText(state->command_input, state->initial_command);
        for (HWND input : {state->name_input, state->command_input}) {
            SetWindowSubclass(input, ProjectTypeEditorInputProc,
                              kProjectTypeEditorInputSubclassId,
                              reinterpret_cast<DWORD_PTR>(state));
        }
        state->cancel = CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectTypeCancel)),
            GetModuleHandleW(nullptr), nullptr);
        state->save = CreateWindowExW(0, L"BUTTON", L"保存",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectTypeSave)),
            GetModuleHandleW(nullptr), nullptr);
        for (HWND button : {state->cancel, state->save}) {
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
        PaintProjectTypeEditorDialog(*state, dc);
        EndPaint(h, &paint);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        HWND control = reinterpret_cast<HWND>(lparam);
        const bool focused = GetFocus() == control;
        SetTextColor(dc, state->text);
        SetBkColor(dc, focused ? state->field_focus : state->field);
        return reinterpret_cast<LRESULT>(focused
            ? state->field_focus_brush : state->field_brush);
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (item && item->CtlType == ODT_BUTTON) {
            DrawProjectTypeEditorButton(*state, *item);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kProjectTypeCancel) {
            DestroyWindow(h);
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kProjectTypeSave) {
            state->name = GetStyledInputText(state->name_input);
            state->command = GetStyledInputText(state->command_input);
            if (state->name.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
                MessageBoxW(h, L"类型名称不能为空", L"AI CLI", MB_OK | MB_ICONWARNING);
                SetFocus(state->name_input);
                return 0;
            }
            if (state->command.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
                MessageBoxW(h, L"默认启动命令不能为空", L"AI CLI", MB_OK | MB_ICONWARNING);
                SetFocus(state->command_input);
                return 0;
            }
            state->accepted = true;
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, message, wparam, lparam);
}

HWND MakeButton(State& state, const wchar_t* text, int id, HWND parent = nullptr) {
    if (!parent) parent = state.window;
    HWND button = CreateWindowExW(0, L"BUTTON", text,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  0, 0, 1, 1, parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    if (button) {
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        SetWindowSubclass(button, ManagementButtonProc, kManagementButtonSubclassId, 0);
    }
    return button;
}

HWND MakeField(State& state, int id, const wchar_t* placeholder, HWND parent = nullptr) {
    HWND field=CreateStyledInput(parent ? parent : state.window, id, state.font, placeholder);
    SetStyledInputTheme(field,state.palette);
    return field;
}

void SetField(HWND field, const std::wstring& value) {
    if (field) SetStyledInputText(field, value);
}

std::wstring Field(HWND field) {
    return field ? GetStyledInputText(field) : std::wstring{};
}

void AddLine(HDC dc, const wchar_t* text, int x, int y, int width, HFONT font, COLORREF color) {
    if (!text) return;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    SelectObject(dc, font);
    RECT rect{x, y, x + width, y + 24};
    DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void DrawButton(const DRAWITEMSTRUCT* item) {
    if (!item) return;
    State* state = FindState(item->hwndItem);
    const COLORREF text_color = state ? state->text_color : DarkPalette().text;
    const COLORREF muted_color = state ? state->muted_color : DarkPalette().muted;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool hovered = state && state->hovered_control == item->hwndItem;
    const bool active_navigation = state && IsActiveSettingsNavigation(*state, item->CtlID);
    const bool active_theme = state &&
        ((item->CtlID == kTheme && !state->light_theme) ||
         (item->CtlID == kThemeLight && state->light_theme));
    const bool active = active_navigation || active_theme;
    RECT rect = item->rcItem;
    const bool light = state && state->light_theme;

    const bool title_close = state && state->mode == ManagementMode::Settings &&
                             state->title_close == item->hwndItem;
    if (title_close && state) {
        // This is an icon-only control.  Paint the parent panel through the
        // idle state and a subtle tinted hit target on hover; never let the
        // stock button background show through in the dark theme.
        const bool hot = hovered || pressed;
        const COLORREF fill = hot
            ? (light ? RGB(239, 242, 247) : RGB(48, 54, 64))
            : state->panel_color;
        HBRUSH clear = CreateSolidBrush(fill);
        FillRect(item->hDC, &rect, clear);
        DeleteObject(clear);
        if (hot) {
            HPEN edge_pen = CreatePen(PS_SOLID, 1,
                                      light ? RGB(211, 218, 228) : RGB(69, 77, 90));
            HGDIOBJ old_pen = SelectObject(item->hDC, edge_pen);
            HGDIOBJ old_brush = SelectObject(item->hDC, GetStockObject(HOLLOW_BRUSH));
            RoundRect(item->hDC, rect.left + 1, rect.top + 1,
                      rect.right - 1, rect.bottom - 1, 8, 8);
            SelectObject(item->hDC, old_brush);
            SelectObject(item->hDC, old_pen);
            DeleteObject(edge_pen);
        }
        SetBkMode(item->hDC, TRANSPARENT);
        SelectObject(item->hDC, state->title_font);
        SetTextColor(item->hDC, pressed
                                  ? (light ? RGB(170, 44, 58) : RGB(255, 145, 155))
                                  : (hovered
                                         ? (light ? RGB(184, 54, 67) : RGB(255, 132, 143))
                                         : state->muted_color));
        RECT close_rect = rect;
        DrawTextW(item->hDC, L"\x00D7", -1, &close_rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    COLORREF fill = light ? RGB(245, 247, 250) : RGB(38, 42, 50);
    COLORREF edge = light ? RGB(210, 217, 226) : RGB(58, 65, 76);
    if (hovered) {
        fill = light ? RGB(234, 240, 248) : RGB(44, 51, 62);
        edge = light ? RGB(171, 189, 213) : RGB(73, 92, 119);
    }
    if (pressed) {
        fill = light ? RGB(218, 230, 246) : RGB(37, 76, 128);
        edge = light ? RGB(121, 158, 207) : RGB(82, 143, 220);
    }
    if (active) {
        fill = light ? RGB(226, 237, 251) : RGB(31, 57, 87);
        edge = light ? RGB(107, 153, 213) : RGB(59, 111, 178);
    }
    if (item->CtlID == kDelete || item->CtlID == kUnbind) {
        fill = light ? RGB(253, 240, 242) : RGB(56, 40, 45);
        edge = light ? RGB(225, 170, 178) : RGB(116, 65, 77);
        if (hovered || pressed) {
            fill = light ? RGB(249, 222, 226) : RGB(82, 43, 50);
            edge = light ? RGB(211, 124, 139) : RGB(151, 70, 84);
        }
    }
    if (disabled) {
        fill = light ? RGB(235, 238, 243) : RGB(32, 35, 41);
        edge = light ? RGB(218, 223, 231) : RGB(50, 54, 62);
    }
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    // Owner-drawn buttons keep their previous pixels outside the inner
    // RoundRect. Clear the complete clipped button surface first so switching
    // from the light palette to the dark palette cannot leave a square white
    // halo around the rounded border.
    FillRect(item->hDC, &rect, brush);
    HGDIOBJ old_brush = SelectObject(item->hDC, brush);
    HGDIOBJ old_pen = SelectObject(item->hDC, pen);
    RoundRect(item->hDC, rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1, 9, 9);
    SelectObject(item->hDC, old_brush);
    SelectObject(item->hDC, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t text[256]{};
    GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
    if (active_navigation && state) {
        RECT marker{rect.left + 1, rect.top + 10, rect.left + 4, rect.bottom - 10};
        HBRUSH marker_brush = CreateSolidBrush(state->accent_color);
        FillRect(item->hDC, &marker, marker_brush);
        DeleteObject(marker_brush);
    }

    SetBkMode(item->hDC, TRANSPARENT);
    if (IsSettingsNavigationId(item->CtlID) && state) {
        const wchar_t* subtitle = item->CtlID == kSettingsAppearance ? L"主题与显示" :
                                  item->CtlID == kSettingsConnection ? L"地址与 Agent" :
                                  item->CtlID == kSettingsProjectTypes ? L"CLI 启动配置" :
                                  L"项目归类管理";
        SelectObject(item->hDC, state->font);
        SetTextColor(item->hDC, disabled ? muted_color : text_color);
        RECT title_rect{rect.left + 16, rect.top + 7, rect.right - 28, rect.top + 29};
        DrawTextW(item->hDC, text, -1, &title_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(item->hDC, state->small_font);
        SetTextColor(item->hDC, muted_color);
        RECT subtitle_rect{rect.left + 16, rect.top + 27, rect.right - 28, rect.bottom - 4};
        DrawTextW(item->hDC, subtitle, -1, &subtitle_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(item->hDC, active_navigation ? state->accent_color : muted_color);
        SelectObject(item->hDC, state->font);
        RECT chevron{rect.right - 27, rect.top, rect.right - 8, rect.bottom};
        DrawTextW(item->hDC, L">", -1, &chevron, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    SetTextColor(item->hDC, disabled ? muted_color :
                 (active_theme && state ? state->accent_color : text_color));
    RECT text_rect = rect;
    DrawTextW(item->hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawListItem(const DRAWITEMSTRUCT* item) {
    if (!item || item->itemID == static_cast<UINT>(-1)) return;
    State* state = FindState(item->hwndItem);
    if (!state) return;
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    HBRUSH fill = CreateSolidBrush(selected ? state->palette.selection
                                            : state->palette.list_surface);
    FillRect(item->hDC, &item->rcItem, fill);
    DeleteObject(fill);
    wchar_t text[512]{};
    SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, selected ? state->text_color : state->palette.list_text);
    SelectObject(item->hDC, state->font);
    RECT rect{item->rcItem.left + 12, item->rcItem.top, item->rcItem.right - 12, item->rcItem.bottom};
    DrawTextW(item->hDC, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void Layout(State& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int width = client.right;
    const int height = client.bottom;
    const int panel_width = state.mode == ManagementMode::Settings
        ? std::min(980, std::max(760, width - 80))
        : std::min(760, std::max(600, width - 100));
    const int panel_height = state.mode == ManagementMode::Settings
        ? std::min(680, std::max(520, height - 64))
        : state.mode == ManagementMode::ProjectTypes ? 590 : 520;
    const int left = (width - panel_width) / 2;
    const int top = std::max(24, (height - panel_height) / 2);
    state.panel = {left, top, left + panel_width, top + panel_height};
    const int x = left + 34;
    const int right = left + panel_width - 34;
    const int button_y = state.panel.bottom - 52;

    auto place = [](HWND control, RECT rect) {
        if (!control) return;
        MoveWindow(control, rect.left + 2, rect.top + 2,
                   std::max(1L, rect.right - rect.left - 4),
                   std::max(1L, rect.bottom - rect.top - 4), TRUE);
    };
    auto button = [](HWND control, int x, int y, int w, int h) {
        if (control) {
            MoveWindow(control, x, y, w, h, TRUE);
            SetWindowRgn(control, CreateRoundRectRgn(0, 0, w + 1, h + 1, 9, 9), TRUE);
        }
    };

    if (state.mode == ManagementMode::Settings) {
        constexpr int sidebar_width = 214;
        const int nav_x = left + 18;
        const int nav_width = sidebar_width - 34;
        button(state.settings_appearance, nav_x, top + 116, nav_width, 54);
        button(state.settings_connection, nav_x, top + 180, nav_width, 54);
        button(state.settings_project_types, nav_x, top + 244, nav_width, 54);
        button(state.settings_groups, nav_x, top + 308, nav_width, 54);
        button(state.title_close, state.panel.right - 62, top + 18, 36, 32);

        state.content_rect = {left + sidebar_width + 18, top + 88,
                              state.panel.right - 18, state.panel.bottom - 18};
        if (state.content) {
            const int content_width = state.content_rect.right - state.content_rect.left;
            const int content_height = state.content_rect.bottom - state.content_rect.top;
            MoveWindow(state.content, state.content_rect.left, state.content_rect.top,
                       content_width, content_height, TRUE);
            SetWindowRgn(state.content,
                         CreateRoundRectRgn(0, 0, content_width + 1, content_height + 1, 12, 12),
                         TRUE);
            LayoutSettingsContent(state);
        }
    } else if (state.mode == ManagementMode::Devices) {
        if (state.list) MoveWindow(state.list, x, top + 120, right - x, 280, TRUE);
        button(state.unbind, right - 270, button_y, 126, 36);
        button(state.close, right - 126, button_y, 126, 36);
    } else if (state.mode == ManagementMode::ProjectTypes) {
        if (state.list) MoveWindow(state.list, x, top + 120, right - x, 300, TRUE);
        button(state.add, x, button_y, 108, 36);
        button(state.save, x + 118, button_y, 118, 36);
        button(state.remove, x + 246, button_y, 108, 36);
        button(state.close, right - 118, button_y, 118, 36);
    } else {
        if (state.list) MoveWindow(state.list, x, top + 120, right - x, 215, TRUE);
        place(state.name, {x, top + 362, right, top + 398});
        if (state.command) place(state.command, {x, top + 436, right, top + 472});
        const int y = button_y;
        button(state.add, x, y, 108, 36);
        button(state.save, x + 118, y, 108, 36);
        button(state.remove, x + 236, y, 108, 36);
        if (state.select) button(state.select, right - 236, y, 108, 36);
        button(state.close, right - 118, y, 118, 36);
    }
    SetModalSurfaceRegion(state.window, state.panel, 14);
    InvalidateRect(state.window, nullptr, FALSE);
}

void Paint(State& state, HDC dc) {
    RECT client{};
    GetClientRect(state.window, &client);
    // ModalWorkspaceScope owns the translucent scrim.  Painting a solid
    // backdrop here would make the modal opaque and hide the live workspace.
    (void)client;
    HBRUSH panel_brush = CreateSolidBrush(state.panel_color);
    HPEN panel_pen = CreatePen(PS_SOLID, 1, state.panel_edge);
    HGDIOBJ old_brush = SelectObject(dc, panel_brush);
    HGDIOBJ old_pen = SelectObject(dc, panel_pen);
    RoundRect(dc, state.panel.left, state.panel.top, state.panel.right, state.panel.bottom, 14, 14);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(panel_brush);
    DeleteObject(panel_pen);

    const int x = state.panel.left + 34;
    const wchar_t* title = L"管理";
    const wchar_t* subtitle = L"保存后立即写入本机 SQLite";
    if (state.mode == ManagementMode::Settings) { title = L"设置"; subtitle = L"选择左侧入口配置 AI CLI"; }
    else if (state.mode == ManagementMode::Groups) { title = L"分组管理"; subtitle = L"选择当前分组，或添加、重命名和删除空分组"; }
    else if (state.mode == ManagementMode::ProjectTypes) { title = L"项目类型"; subtitle = L"Codex / Claude 为预置类型，所有类型都可编辑"; }
    else if (state.mode == ManagementMode::Devices) { title = L"已绑定设备"; subtitle = L"解绑后该手机的令牌和实时连接立即失效"; }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, state.text_color);
    SelectObject(dc, state.title_font);
    const int title_right = state.mode == ManagementMode::Settings
        ? state.panel.right - 86 : state.panel.right - 34;
    RECT title_rect{x, state.panel.top + 24, title_right, state.panel.top + 54};
    DrawTextW(dc, title, -1, &title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, state.muted_color);
    SelectObject(dc, state.small_font);
    RECT sub_rect{x, state.panel.top + 57, title_right, state.panel.top + 82};
    DrawTextW(dc, subtitle, -1, &sub_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (state.mode == ManagementMode::Settings) {
        const int divider_x = state.panel.left + 214;
        HPEN divider = CreatePen(PS_SOLID, 1, state.panel_edge);
        HGDIOBJ old_divider = SelectObject(dc, divider);
        MoveToEx(dc, divider_x, state.panel.top + 18, nullptr);
        LineTo(dc, divider_x, state.panel.bottom - 18);
        SelectObject(dc, old_divider);
        DeleteObject(divider);
        SetTextColor(dc, state.muted_color);
        SelectObject(dc, state.small_font);
        RECT shortcut_rect{state.panel.left + 18, state.panel.top + 88,
                           divider_x - 18, state.panel.top + 111};
        DrawTextW(dc, L"快捷设置", -1, &shortcut_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    auto label = [&](const wchar_t* text, int y) {
        SetTextColor(dc, state.palette.muted);
        SelectObject(dc, state.small_font);
        RECT rect{x, state.panel.top + y, state.panel.right - 34, state.panel.top + y + 22};
        DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    if (state.mode == ManagementMode::Groups) {
        label(L"分组列表", 88);
        label(L"分组名称", 346);
    } else if (state.mode == ManagementMode::ProjectTypes) {
        label(L"项目类型列表", 88);
        SetTextColor(dc, state.muted_color);
        SelectObject(dc, state.small_font);
        RECT hint{x, state.panel.top + 436, state.panel.right - 34,
                  state.panel.top + 468};
        DrawTextW(dc, L"新增和修改会打开独立模态框；Codex 和 Claude 虽为预置类型，也可手工调整，但不能删除。", -1,
                  &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        label(L"已绑定手机（双击或选择后点击解绑）", 88);
    }
}

int SettingsContentExtent(SettingsPage page) {
    switch (page) {
    case SettingsPage::Appearance: return 390;
    case SettingsPage::Connection: return 590;
    case SettingsPage::ProjectTypes: return 590;
    case SettingsPage::Groups: return 590;
    }
    return 390;
}

void SetSettingsScroll(State& state, int position) {
    if (!state.content) return;
    RECT client{};
    GetClientRect(state.content, &client);
    const int page = std::max(1L, client.bottom - client.top);
    const int maximum = std::max(0, state.content_extent - page);
    const int next = std::clamp(position, 0, maximum);
    if (state.content_scroll == next) return;
    state.content_scroll = next;
    LayoutSettingsContent(state);
}

void LayoutSettingsContent(State& state) {
    if (!state.content || state.mode != ManagementMode::Settings) return;
    RECT client{};
    GetClientRect(state.content, &client);
    const int width = std::max(1L, client.right - client.left);
    const int height = std::max(1L, client.bottom - client.top);
    state.content_extent = SettingsContentExtent(state.settings_page);
    state.content_scroll = std::clamp(state.content_scroll, 0,
        std::max(0, state.content_extent - height));

    SCROLLINFO scroll{sizeof(scroll)};
    scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0;
    scroll.nMax = std::max(0, state.content_extent - 1);
    scroll.nPage = static_cast<UINT>(height);
    scroll.nPos = state.content_scroll;
    SetScrollInfo(state.content, SB_VERT, &scroll, TRUE);

    const int x = 28;
    const int right = std::max(x + 160, width - 34);
    const int available = right - x;
    const int offset = state.content_scroll;
    auto field = [&](HWND control, int y, int h = 40) {
        if (!control) return;
        LayoutStyledInput(control, RECT{x, y - offset, right, y + h - offset}, 2, 8);
    };
    auto button = [&](HWND control, int bx, int y, int w, int h = 38) {
        if (!control) return;
        MoveWindow(control, bx, y - offset, w, h, TRUE);
        SetWindowRgn(control, CreateRoundRectRgn(0, 0, w + 1, h + 1, 9, 9), TRUE);
    };

    if (state.settings_page == SettingsPage::Appearance) {
        const int gap = 10;
        // Give the theme choices the same generous inset as the rest of the
        // card content so the first button never appears attached to its edge.
        const int inset = 24;
        const int available_button_width = std::max(96, (available - inset * 2 - gap) / 2);
        const int button_width = std::min(124, available_button_width);
        button(state.theme, x + inset, 198, button_width, 36);
        button(state.theme_light, x + inset + button_width + gap, 198, button_width, 36);
    } else if (state.settings_page == SettingsPage::Connection) {
        field(state.listen, 130);
        field(state.port, 216);
        field(state.local, 302);
        field(state.remote, 388);
        button(state.firewall, x, 484, 158);
        button(state.save, right - 174, 484, 174);
    } else if (state.settings_page == SettingsPage::ProjectTypes) {
        if (state.list) {
            MoveWindow(state.list, x, 126 - offset, available, 330, TRUE);
            SetWindowRgn(state.list,
                         CreateRoundRectRgn(0, 0, available + 1, 331, 9, 9), TRUE);
        }
        button(state.add, x, 492, 122);
        button(state.save, x + 134, 492, 118);
        button(state.remove, x + 264, 492, 104);
    } else {
        if (state.list) {
            MoveWindow(state.list, x, 126 - offset, available, 330, TRUE);
            SetWindowRgn(state.list,
                         CreateRoundRectRgn(0, 0, available + 1, 331, 9, 9), TRUE);
        }
        button(state.add, x, 492, 122);
        button(state.save, x + 134, 492, 118);
        button(state.remove, x + 264, 492, 104);
    }
    InvalidateRect(state.content, nullptr, FALSE);
}

void PaintSettingsContent(State& state, HDC dc) {
    RECT client{};
    GetClientRect(state.content, &client);
    HBRUSH background = CreateSolidBrush(state.background_color);
    FillRect(dc, &client, background);
    DeleteObject(background);

    POINT previous_origin{};
    SetViewportOrgEx(dc, 0, -state.content_scroll, &previous_origin);
    SetBkMode(dc, TRANSPARENT);
    const int width = client.right;
    const int x = 28;
    const int right = std::max(x + 160, width - 34);

    const wchar_t* title = L"界面外观";
    const wchar_t* subtitle = L"主题切换会立即作用于主窗口、设置页和原生终端。";
    if (state.settings_page == SettingsPage::Connection) {
        title = L"网络连接";
        subtitle = L"配置手机访问使用的 HTTPS/WSS Agent；保存网络设置时重启 Agent。";
    } else if (state.settings_page == SettingsPage::ProjectTypes) {
        title = L"项目类型";
        subtitle = L"Codex 和 Claude 为预置类型；所有类型都可在这里维护名称和启动方式。";
    } else if (state.settings_page == SettingsPage::Groups) {
        title = L"分组管理";
        subtitle = L"用一个名称输入框新建或修改分组；默认分组始终保留。";
    }

    SelectObject(dc, state.title_font);
    SetTextColor(dc, state.text_color);
    RECT title_rect{x, 20, right, 52};
    DrawTextW(dc, title, -1, &title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, state.small_font);
    SetTextColor(dc, state.muted_color);
    RECT subtitle_rect{x, 54, right, 84};
    DrawTextW(dc, subtitle, -1, &subtitle_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    auto label = [&](const wchar_t* text, int y) {
        SelectObject(dc, state.small_font);
        SetTextColor(dc, state.palette.muted);
        RECT rect{x, y, right, y + 24};
        DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    auto card = [&](RECT rect) {
        HBRUSH brush = CreateSolidBrush(state.panel_color);
        HPEN pen = CreatePen(PS_SOLID, 1, state.panel_edge);
        HGDIOBJ old_brush = SelectObject(dc, brush);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    };

    if (state.settings_page == SettingsPage::Appearance) {
        card({x, 106, right, 294});
        SelectObject(dc, state.font);
        SetTextColor(dc, state.text_color);
        RECT card_title{x + 24, 124, right - 24, 151};
        DrawTextW(dc, L"应用主题", -1, &card_title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT detail{x + 24, 151, right - 24, 184};
        DrawTextW(dc, L"点击后立即保存，无需再按“保存”。默认使用夜间模式。", -1, &detail,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(dc, state.muted_color);
        RECT hint{x, 318, right, 354};
        DrawTextW(dc, L"原生终端会保留 ANSI 颜色层次，并同步切换背景与默认前景色。", -1,
                  &hint, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else if (state.settings_page == SettingsPage::Connection) {
        label(L"监听地址（0.0.0.0 表示所有网卡）", 100);
        label(L"HTTPS/WSS 端口", 186);
        label(L"本机/内网访问地址（二维码优先使用）", 272);
        label(L"公网或 Relay 地址（可留空）", 358);
        SelectObject(dc, state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT hint{x, 538, right, 574};
        DrawTextW(dc, L"本机和公网地址必须使用 https://。端口变化后请重新允许手机访问。", -1,
                  &hint, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else if (state.settings_page == SettingsPage::ProjectTypes) {
        label(L"项目类型列表", 96);
        SelectObject(dc, state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT hint{x, 548, right, 578};
        DrawTextW(dc, L"新增和修改会打开独立模态框；Codex 和 Claude 虽为预置类型，也可手工调整，但不能删除。", -1,
                  &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        label(L"分组列表", 96);
        SelectObject(dc, state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT hint{x, 548, right, 578};
        DrawTextW(dc, L"添加和修改名称会打开独立输入模态层；非空分组不能删除。", -1,
                  &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SetViewportOrgEx(dc, previous_origin.x, previous_origin.y, nullptr);
}

void UpdateSettingsPage(State& state, SettingsPage page) {
    if (state.mode != ManagementMode::Settings) return;
    state.settings_page = page;
    state.content_scroll = 0;
    const bool appearance = page == SettingsPage::Appearance;
    const bool connection = page == SettingsPage::Connection;
    const bool project_types = page == SettingsPage::ProjectTypes;
    const bool groups = page == SettingsPage::Groups;
    auto visible = [](HWND control, bool show) {
        if (control) ShowWindow(control, show ? SW_SHOW : SW_HIDE);
    };
    visible(state.theme, appearance);
    visible(state.theme_light, appearance);
    visible(state.listen, connection);
    visible(state.port, connection);
    visible(state.local, connection);
    visible(state.remote, connection);
    visible(state.firewall, connection);
    visible(state.save, connection || project_types || groups);
    visible(state.list, project_types || groups);
    visible(state.name, false);
    visible(state.command, false);
    visible(state.add, project_types || groups);
    visible(state.remove, project_types || groups);
    if (state.save) EnableWindow(state.save, TRUE);
    if (state.remove) EnableWindow(state.remove, TRUE);
    if (project_types) {
        SetWindowTextW(state.add, L"新增类型");
        SetWindowTextW(state.save, L"修改类型");
        SetWindowTextW(state.remove, L"删除");
        RefreshList(state);
        ReadSelection(state);
    } else if (connection) {
        SetWindowTextW(state.save, L"保存并重启 Agent");
    } else if (groups) {
        SetWindowTextW(state.add, L"添加分组");
        SetWindowTextW(state.save, L"修改名称");
        SetWindowTextW(state.remove, L"删除");
        RefreshList(state);
        ReadSelection(state);
    }
    LayoutSettingsContent(state);
    InvalidateRect(state.settings_appearance, nullptr, FALSE);
    InvalidateRect(state.settings_connection, nullptr, FALSE);
    InvalidateRect(state.settings_project_types, nullptr, FALSE);
    InvalidateRect(state.settings_groups, nullptr, FALSE);
    InvalidateRect(state.content, nullptr, FALSE);
}

LRESULT CALLBACK SettingsContentProc(HWND h, UINT message, WPARAM wparam, LPARAM lparam) {
    State* state = GetState(h);
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<State*>(create->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(h, message, wparam, lparam);
    switch (message) {
    case WM_SIZE:
        LayoutSettingsContent(*state);
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        SetSettingsScroll(*state, state->content_scroll - (delta / WHEEL_DELTA) * 52);
        return 0;
    }
    case WM_VSCROLL: {
        RECT client{};
        GetClientRect(h, &client);
        int next = state->content_scroll;
        switch (LOWORD(wparam)) {
        case SB_LINEUP: next -= 40; break;
        case SB_LINEDOWN: next += 40; break;
        case SB_PAGEUP: next -= std::max(80L, client.bottom - 60); break;
        case SB_PAGEDOWN: next += std::max(80L, client.bottom - 60); break;
        case SB_TOP: next = 0; break;
        case SB_BOTTOM: next = state->content_extent; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO info{sizeof(info)};
            info.fMask = SIF_TRACKPOS;
            GetScrollInfo(h, SB_VERT, &info);
            next = info.nTrackPos;
            break;
        }
        default: return 0;
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
        PaintSettingsContent(*state, dc);
        EndPaint(h, &paint);
        return 0;
    }
    }
    return DefWindowProcW(h, message, wparam, lparam);
}

void RefreshList(State& state) {
    if (!state.list) return;
    SendMessageW(state.list, LB_RESETCONTENT, 0, 0);
    if (IsSettingsGroups(state)) {
        state.groups = state.store->LoadGroups();
        const auto projects = state.store->LoadProjects();
        for (const auto& value : state.groups) {
            const auto count = std::count_if(projects.begin(), projects.end(), [&](const Project& project) {
                const std::wstring group = project.group.empty() ? L"默认分组" : project.group;
                return group == value.name;
            });
            const std::wstring label = value.name + L" (" + std::to_wstring(count) + L")";
            LRESULT row = SendMessageW(state.list, LB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(label.c_str()));
            if (row != LB_ERR) SendMessageW(state.list, LB_SETITEMDATA, row,
                                             static_cast<LPARAM>(&value - state.groups.data()));
        }
    } else if (IsSettingsProjectTypes(state)) {
        state.types_data = state.store->LoadProjectTypes();
        for (const auto& value : state.types_data) {
            LRESULT row = SendMessageW(state.list, LB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(value.name.c_str()));
            if (row != LB_ERR) SendMessageW(state.list, LB_SETITEMDATA, row,
                                             static_cast<LPARAM>(&value - state.types_data.data()));
        }
    } else if (state.mode == ManagementMode::Devices) {
        state.devices = state.store->LoadDevices(true);
        for (const auto& value : state.devices) {
            std::wstring text = (value.active ? L"●  " : L"○  ") + value.name + L"  ·  " +
                                value.model + L"  ·  " + value.phone_id;
            LRESULT row = SendMessageW(state.list, LB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(text.c_str()));
            if (row != LB_ERR) SendMessageW(state.list, LB_SETITEMDATA, row,
                                             static_cast<LPARAM>(&value - state.devices.data()));
        }
    }
    if (SendMessageW(state.list, LB_GETCOUNT, 0, 0) > 0) SendMessageW(state.list, LB_SETCURSEL, 0, 0);
}

void ReadSelection(State& state) {
    if (!state.list) return;
    LRESULT row = SendMessageW(state.list, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR) return;
    const auto index = static_cast<size_t>(SendMessageW(state.list, LB_GETITEMDATA, row, 0));
    if (IsSettingsProjectTypes(state) && index < state.types_data.size()) {
        SetField(state.name, state.types_data[index].name);
        SetField(state.command, state.types_data[index].default_command);
        if (state.save) EnableWindow(state.save, TRUE);
        if (state.remove) EnableWindow(state.remove,
                                       state.types_data[index].builtin ? FALSE : TRUE);
    }
    if (IsSettingsGroups(state) && index < state.groups.size()) {
        SetField(state.name, state.groups[index].name);
    }
}

bool SelectedIndex(HWND list, size_t& index) {
    if (!list) return false;
    const LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR) return false;
    index = static_cast<size_t>(SendMessageW(list, LB_GETITEMDATA, row, 0));
    return true;
}

void ShowError(HWND owner, const std::wstring& message) {
    if (!message.empty()) MessageBoxW(owner, message.c_str(), L"AI CLI", MB_OK | MB_ICONWARNING);
}

std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t current : value) {
        if (current == L'\\') {
            ++slashes;
        } else if (current == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(current);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool ConfigureFirewall(HWND owner, const std::wstring& port, std::wstring* error) {
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) {
        if (error) *error = L"无法确定 AI CLI 程序路径";
        return false;
    }
    const std::filesystem::path script =
        std::filesystem::path(executable).parent_path() / L"configure-agent-firewall.ps1";
    if (!std::filesystem::exists(script)) {
        if (error) *error = L"缺少防火墙配置脚本，请重新运行部署脚本";
        return false;
    }
    wchar_t system_directory[MAX_PATH]{};
    const UINT system_length = GetSystemDirectoryW(system_directory, MAX_PATH);
    const std::filesystem::path powershell = system_length > 0 && system_length < MAX_PATH
        ? std::filesystem::path(system_directory) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe"
        : std::filesystem::path(L"powershell.exe");
    const std::wstring parameters = L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
        QuoteArgument(script.wstring()) + L" -Port " + port + L" -Program " +
        QuoteArgument(executable);
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.hwnd = owner;
    launch.lpVerb = L"runas";
    launch.lpFile = powershell.c_str();
    launch.lpParameters = parameters.c_str();
    launch.nShow = SW_HIDE;
    if (!ShellExecuteExW(&launch)) {
        if (error) *error = GetLastError() == ERROR_CANCELLED
            ? L"已取消管理员授权，防火墙没有修改"
            : L"无法启动管理员防火墙配置";
        return false;
    }
    WaitForSingleObject(launch.hProcess, INFINITE);
    DWORD exit_code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(launch.hProcess, &exit_code);
    CloseHandle(launch.hProcess);
    if (exit_code != 0) {
        if (error) *error = L"防火墙配置失败，请确认当前账户可执行管理员操作";
        return false;
    }
    return true;
}

void CreateControls(State& state) {
    if (state.mode == ManagementMode::Settings) {
        state.content = CreateWindowExW(
            WS_EX_CONTROLPARENT, kSettingsContentClassName, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL,
            0, 0, 1, 1, state.window, nullptr, GetModuleHandleW(nullptr), &state);
        state.settings_appearance = MakeButton(state, L"外观", kSettingsAppearance);
        state.settings_connection = MakeButton(state, L"网络连接", kSettingsConnection);
        state.settings_project_types = MakeButton(state, L"项目类型", kSettingsProjectTypes);
        state.settings_groups = MakeButton(state, L"分组管理", kSettingsGroups);
        state.listen = MakeField(state, kListen, L"例如 0.0.0.0", state.content);
        state.port = MakeField(state, kPort, L"例如 4280", state.content);
        state.local = MakeField(state, kLocal, L"例如 https://192.168.1.10:4280", state.content);
        state.remote = MakeField(state, kRemote, L"例如 https://relay.example.com/aicli", state.content);
        const auto theme = state.store->GetSetting(L"theme", L"dark");
        state.light_theme = theme == L"light";
        state.theme = MakeButton(state, L"夜间模式", kTheme, state.content);
        state.theme_light = MakeButton(state, L"白天模式", kThemeLight, state.content);
        state.save = MakeButton(state, L"保存并重启 Agent", kSave, state.content);
        state.firewall = MakeButton(state, L"允许手机访问", kFirewall, state.content);
        state.list = CreateWindowExW(0, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                         LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                                         LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                     0, 0, 1, 1, state.content,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
                                     GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state.list, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        state.add = MakeButton(state, L"新增类型", kAdd, state.content);
        state.remove = MakeButton(state, L"删除", kDelete, state.content);
        state.title_close = MakeButton(state, L"\x00D7", kClose);
        UpdateSettingsPage(state, SettingsPage::Appearance);
    } else if (state.mode == ManagementMode::Groups) {
        state.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                         LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                     0, 0, 1, 1, state.window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
                                     GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state.list, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        state.name = MakeField(state, kName, L"输入新分组名称，或修改选中分组");
        state.add = MakeButton(state, L"新建分组", kAdd);
        state.save = MakeButton(state, L"保存修改", kSave);
        state.remove = MakeButton(state, L"删除", kDelete);
        state.select = MakeButton(state, L"切换到此分组", kSelect);
        state.close = MakeButton(state, L"关闭", kClose);
        RefreshList(state);
    } else if (state.mode == ManagementMode::ProjectTypes) {
        state.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                         LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                     0, 0, 1, 1, state.window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
                                     GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state.list, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        state.add = MakeButton(state, L"新增", kAdd);
        state.save = MakeButton(state, L"修改", kSave);
        state.remove = MakeButton(state, L"删除", kDelete);
        state.close = MakeButton(state, L"关闭", kClose);
        RefreshList(state);
        ReadSelection(state);
    } else {
        state.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                         LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                     0, 0, 1, 1, state.window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
                                     GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state.list, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        state.unbind = MakeButton(state, L"解绑设备", kUnbind);
        state.close = MakeButton(state, L"关闭", kClose);
        RefreshList(state);
    }
    Layout(state);
    PostMessageW(state.window, kPopulateMessage, 0, 0);
}

void PopulateControls(State& state) {
    if (state.mode == ManagementMode::Settings) {
        std::wstring listen_value = state.store->GetSetting(L"listen_address", L"0.0.0.0");
        std::wstring port_value = state.store->GetSetting(L"listen_port", L"4280");
        if (listen_value.empty()) listen_value = L"0.0.0.0";
        if (port_value.empty()) port_value = L"4280";
        SetField(state.local, state.store->GetSetting(L"local_address"));
        SetField(state.remote, state.store->GetSetting(L"remote_address"));
        SetField(state.listen, listen_value);
        SetField(state.port, port_value);
    } else {
        ReadSelection(state);
    }
}

LRESULT CALLBACK WindowProc(HWND h, UINT message, WPARAM wparam, LPARAM lparam) {
    State* state = GetState(h);
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<State*>(create->lpCreateParams);
        state->window = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(h, message, wparam, lparam);
    switch (message) {
    case WM_CREATE:
        CreateControls(*state);
        return 0;
    case WM_SIZE:
        Layout(*state);
        return 0;
    case kPopulateMessage:
        PopulateControls(*state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        Paint(*state, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, state->text_color);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        HWND control = reinterpret_cast<HWND>(lparam);
        SetTextColor(dc, state->text_color);
        const bool focused = GetFocus() == control;
        SetBkColor(dc, focused ? state->field_focus_color : state->field_color);
        return reinterpret_cast<LRESULT>(focused ? state->field_focus_brush
                                                  : state->field_brush);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, state->text_color);
        SetBkColor(dc, state->panel_color);
        return reinterpret_cast<LRESULT>(state->list_brush);
    }
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (measure && measure->CtlID == kList) {
            measure->itemHeight = 38;
            return TRUE;
        }
        return FALSE;
    }
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (!draw) return TRUE;
        if (draw->CtlType == ODT_BUTTON) DrawButton(draw);
        else if (draw->CtlType == ODT_LISTBOX) DrawListItem(draw);
        return TRUE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        const int notification = HIWORD(wparam);
        if (id == kList && (notification == LBN_SELCHANGE || notification == LBN_DBLCLK)) {
            ReadSelection(*state);
            if (notification == LBN_DBLCLK && state->mode == ManagementMode::Groups) {
                size_t index{};
                if (SelectedIndex(state->list, index) && index < state->groups.size()) {
                    state->result.accepted = true;
                    state->result.selected_group = state->groups[index].name;
                    DestroyWindow(h);
                }
            }
            if (notification == LBN_DBLCLK && IsSettingsProjectTypes(*state) &&
                IsWindowEnabled(state->save)) {
                SendMessageW(h, WM_COMMAND, MAKEWPARAM(kSave, BN_CLICKED),
                             reinterpret_cast<LPARAM>(state->save));
            }
            return 0;
        }
        if (notification != BN_CLICKED) return 0;
        std::wstring error;
        if (id == kClose) {
            DestroyWindow(h);
            return 0;
        }
        if (state->mode == ManagementMode::Settings) {
            if (id == kSettingsAppearance) {
                UpdateSettingsPage(*state, SettingsPage::Appearance);
                return 0;
            }
            if (id == kSettingsConnection) {
                UpdateSettingsPage(*state, SettingsPage::Connection);
                return 0;
            }
            if (id == kSettingsProjectTypes) {
                UpdateSettingsPage(*state, SettingsPage::ProjectTypes);
                return 0;
            }
            if (id == kSettingsGroups) {
                UpdateSettingsPage(*state, SettingsPage::Groups);
                return 0;
            }
            if (id == kTheme || id == kThemeLight) {
                const bool light = id == kThemeLight;
                if (state->light_theme != light) {
                    ApplyTheme(*state, light);
                    state->store->SetSetting(L"theme", light ? L"light" : L"dark");
                    if (state->theme_changed) state->theme_changed(light);
                    RedrawWindow(state->window, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_ALLCHILDREN);
                }
                return 0;
            }
            if (state->settings_page == SettingsPage::Connection && id == kFirewall) {
                const std::wstring port = Field(state->port).empty() ? L"4280" : Field(state->port);
                try {
                    size_t consumed{};
                    const int number = std::stoi(port, &consumed);
                    if (consumed != port.size() || number < 1 || number > 65535) {
                        throw std::out_of_range("port");
                    }
                } catch (...) {
                    ShowError(h, L"请先填写有效的 HTTPS 端口");
                    return 0;
                }
                if (!ConfigureFirewall(h, port, &error)) {
                    ShowError(h, error);
                    return 0;
                }
                state->store->SetSetting(L"firewall_port", port);
                MessageBoxW(h, L"已允许已绑定手机访问当前 HTTPS/WSS 端口。\n端口变化后请重新执行一次。",
                            L"AI CLI", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (state->settings_page == SettingsPage::Connection && id == kSave) {
                const std::wstring listen = Field(state->listen).empty() ? L"0.0.0.0" : Field(state->listen);
                const std::wstring port = Field(state->port).empty() ? L"4280" : Field(state->port);
                const std::wstring local = Field(state->local);
                const std::wstring remote = Field(state->remote);
                try {
                    size_t consumed{};
                    const int number = std::stoi(port, &consumed);
                    if (consumed != port.size() || number < 1 || number > 65535) throw std::out_of_range("port");
                } catch (...) {
                    ShowError(h, L"HTTPS 端口必须是 1 到 65535 的数字");
                    return 0;
                }
                const auto secure_address = [](const std::wstring& value) {
                    return value.empty() || value.rfind(L"https://", 0) == 0;
                };
                if (!secure_address(local) || !secure_address(remote)) {
                    ShowError(h, L"本机地址和公网/Relay 地址必须使用 https://");
                    return 0;
                }
                state->store->SetSetting(L"listen_address", listen);
                state->store->SetSetting(L"listen_port", port);
                state->store->SetSetting(L"local_address", local);
                state->store->SetSetting(L"remote_address", remote);
                agent::AgentConfig config;
                config.bind_address = listen;
                config.port = port;
                config.local_address = local;
                config.remote_address = remote;
                config.data_directory = aicli::DefaultDataDirectory();
                if (state->restart_agent && !state->restart_agent(config, &error)) {
                    ShowError(h, error.empty() ? L"Agent 重启失败" : error);
                    return 0;
                }
                state->result.accepted = true;
                DestroyWindow(h);
                return 0;
            }
            if (state->settings_page == SettingsPage::ProjectTypes) {
                if (id == kAdd) {
                    std::wstring name;
                    std::wstring command;
                    if (!ShowProjectTypeEditorDialog(
                            h, L"新增项目类型",
                            L"为新的 AI CLI 配置名称和默认启动命令。",
                            L"", L"", state->light_theme, name, command)) return 0;
                    if (!state->store->AddProjectType(name, command, &error)) {
                        ShowError(h, error);
                    } else {
                        RefreshList(*state);
                        ReadSelection(*state);
                    }
                } else if (id == kSave) {
                    size_t index{};
                    if (!SelectedIndex(state->list, index) || index >= state->types_data.size()) {
                        ShowError(h, L"请选择要修改的类型");
                    } else {
                        const auto current = state->types_data[index];
                        std::wstring name;
                        std::wstring command;
                        if (!ShowProjectTypeEditorDialog(
                                h, L"修改项目类型",
                                L"保存后，类型名称和默认启动命令会立即更新。",
                                current.name, current.default_command,
                                state->light_theme, name, command)) return 0;
                        if (!state->store->UpdateProjectType(
                                current.id, name, command, &error)) {
                            ShowError(h, error);
                        } else {
                            RefreshList(*state);
                            ReadSelection(*state);
                        }
                    }
                } else if (id == kDelete) {
                    size_t index{};
                    if (!SelectedIndex(state->list, index) || index >= state->types_data.size()) {
                        ShowError(h, L"请选择要删除的类型");
                    } else if (!state->store->DeleteProjectType(state->types_data[index].id, &error)) {
                        ShowError(h, error);
                    } else {
                        RefreshList(*state);
                        ReadSelection(*state);
                    }
                }
                return 0;
            }
            if (state->settings_page == SettingsPage::Groups) {
                if (id == kAdd) {
                    std::wstring name;
                    if (!ShowGroupNameDialog(h, L"添加分组", L"创建后可在标题栏下拉框中直接切换。",
                                             L"", state->light_theme, name)) return 0;
                    if (!state->store->AddGroup(name, &error)) {
                        ShowError(h, error);
                    } else {
                        RefreshList(*state);
                    }
                } else if (id == kSave) {
                    size_t index{};
                    if (!SelectedIndex(state->list, index) || index >= state->groups.size()) {
                        ShowError(h, L"请选择要修改的分组");
                    } else {
                        std::wstring name;
                        if (!ShowGroupNameDialog(h, L"修改分组名称",
                                                 L"保存后已有项目会同步更新到新名称。",
                                                 state->groups[index].name,
                                                 state->light_theme, name)) return 0;
                        if (!state->store->RenameGroup(state->groups[index].id, name, &error)) {
                            ShowError(h, error);
                        } else {
                            RefreshList(*state);
                        }
                    }
                } else if (id == kDelete) {
                    size_t index{};
                    if (!SelectedIndex(state->list, index) || index >= state->groups.size()) {
                        ShowError(h, L"请选择要删除的分组");
                    } else if (!state->store->DeleteGroup(state->groups[index].id, &error)) {
                        ShowError(h, error);
                    } else {
                        RefreshList(*state);
                    }
                }
                return 0;
            }
            return 0;
        }
        if (state->mode == ManagementMode::Groups) {
            if (id == kAdd) {
                if (!state->store->AddGroup(Field(state->name), &error)) ShowError(h, error);
                else { RefreshList(*state); SetField(state->name, L""); }
            } else if (id == kSave) {
                size_t index{};
                if (!SelectedIndex(state->list, index) || index >= state->groups.size()) ShowError(h, L"请选择要重命名的分组");
                else if (!state->store->RenameGroup(state->groups[index].id, Field(state->name), &error)) ShowError(h, error);
                else RefreshList(*state);
            } else if (id == kDelete) {
                size_t index{};
                if (!SelectedIndex(state->list, index) || index >= state->groups.size()) ShowError(h, L"请选择要删除的分组");
                else if (!state->store->DeleteGroup(state->groups[index].id, &error)) ShowError(h, error);
                else RefreshList(*state);
            } else if (id == kSelect) {
                size_t index{};
                if (!SelectedIndex(state->list, index) || index >= state->groups.size()) ShowError(h, L"请选择分组");
                else { state->result.accepted = true; state->result.selected_group = state->groups[index].name; DestroyWindow(h); }
            }
            return 0;
        }
        if (state->mode == ManagementMode::ProjectTypes) {
            if (id == kAdd) {
                std::wstring name;
                std::wstring command;
                if (!ShowProjectTypeEditorDialog(
                        h, L"新增项目类型",
                        L"为新的 AI CLI 配置名称和默认启动命令。",
                        L"", L"", state->light_theme, name, command)) return 0;
                if (!state->store->AddProjectType(name, command, &error)) ShowError(h, error);
                else { RefreshList(*state); ReadSelection(*state); }
            } else if (id == kSave) {
                size_t index{};
                if (!SelectedIndex(state->list, index) || index >= state->types_data.size()) ShowError(h, L"请选择要修改的类型");
                else {
                    const auto current = state->types_data[index];
                    std::wstring name;
                    std::wstring command;
                    if (!ShowProjectTypeEditorDialog(
                            h, L"修改项目类型",
                            L"保存后，类型名称和默认启动命令会立即更新。",
                            current.name, current.default_command,
                            state->light_theme, name, command)) return 0;
                    if (!state->store->UpdateProjectType(current.id, name, command, &error)) ShowError(h, error);
                    else { RefreshList(*state); ReadSelection(*state); }
                }
            } else if (id == kDelete) {
                size_t index{};
                if (!SelectedIndex(state->list, index) || index >= state->types_data.size()) ShowError(h, L"请选择要删除的类型");
                else if (!state->store->DeleteProjectType(state->types_data[index].id, &error)) ShowError(h, error);
                else { RefreshList(*state); ReadSelection(*state); }
            }
            return 0;
        }
        if (state->mode == ManagementMode::Devices && id == kUnbind) {
            size_t index{};
            if (!SelectedIndex(state->list, index) || index >= state->devices.size()) ShowError(h, L"请选择要解绑的设备");
            else if (!state->devices[index].active) ShowError(h, L"该设备已经解绑");
            else if (!state->agent->RevokeDevice(state->devices[index].phone_id)) ShowError(h, L"解绑失败");
            else RefreshList(*state);
            return 0;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(h, message, wparam, lparam);
}

bool ShowProjectTypeEditorDialog(HWND owner,
                                 const std::wstring& title,
                                 const std::wstring& subtitle,
                                 const std::wstring& initial_name,
                                 const std::wstring& initial_command,
                                 bool light_theme,
                                 std::wstring& name,
                                 std::wstring& command) {
    static bool registered = false;
    if (!registered) {
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
    state.font = CreateFontW(-15, 0, 0, 0, 400, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state.title_font = CreateFontW(-21, 0, 0, 0, 600, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state.small_font = CreateFontW(-13, 0, 0, 0, 400, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    ModalWorkspaceScope workspace(owner, -1, &state.palette);
    state.field_brush = CreateSolidBrush(state.field);
    state.field_focus_brush = CreateSolidBrush(state.field_focus);
    state.window = CreateWindowExW(
        WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, kProjectTypeEditorClassName, L"",
        WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 1, 1, owner, nullptr,
        GetModuleHandleW(nullptr), &state);
    if (state.window) {
        workspace.AttachSurface(state.window);
        aicli::ui::ActivateModalSurface(state.window);
        MSG message{};
        while (IsWindow(state.window) && GetMessageW(&message, nullptr, 0, 0)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (state.field_brush) DeleteObject(state.field_brush);
    if (state.field_focus_brush) DeleteObject(state.field_focus_brush);
    if (state.font) DeleteObject(state.font);
    if (state.title_font) DeleteObject(state.title_font);
    if (state.small_font) DeleteObject(state.small_font);
    if (state.accepted) {
        name = state.name;
        command = state.command;
    }
    aicli::ui::RestoreModalOwner(owner);
    return state.accepted;
}

} // namespace

ManagementDialogResult ShowManagementDialog(HWND owner, ManagementMode mode,
                                            DataStore& store, agent::RemoteAgent& agent,
                                            AgentRestartCallback restart_agent,
                                            ThemeChangedCallback theme_changed) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = WindowProc;
        klass.lpszClassName = kClassName;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = nullptr;
        RegisterClassW(&klass);
        WNDCLASSW content_class{};
        content_class.hInstance = GetModuleHandleW(nullptr);
        content_class.lpfnWndProc = SettingsContentProc;
        content_class.lpszClassName = kSettingsContentClassName;
        content_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        content_class.hbrBackground = nullptr;
        RegisterClassW(&content_class);
        registered = true;
    }
    State state;
    state.owner = owner;
    state.mode = mode;
    state.store = &store;
    state.agent = &agent;
    state.restart_agent = std::move(restart_agent);
    state.theme_changed = std::move(theme_changed);
    ApplyTheme(state, store.GetSetting(L"theme", L"dark") == L"light");
    state.font = CreateFontW(-15, 0, 0, 0, 400, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state.title_font = CreateFontW(-22, 0, 0, 0, 600, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state.small_font = CreateFontW(-13, 0, 0, 0, 400, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    const int host_title_height = ModalTopForOwner(owner);
    ModalWorkspaceScope workspace(owner, host_title_height, &state.palette);
    state.window = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, kClassName, L"",
                                   WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                   0, 0, 1, 1, owner, nullptr,
                                   GetModuleHandleW(nullptr), &state);
    if (state.window) {
        workspace.AttachSurface(state.window);
        PopulateControls(state);
        aicli::ui::ActivateModalSurface(state.window);
        MSG message{};
        while (IsWindow(state.window) && GetMessageW(&message, nullptr, 0, 0)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (state.font) DeleteObject(state.font);
    if (state.title_font) DeleteObject(state.title_font);
    if (state.small_font) DeleteObject(state.small_font);
    if (state.field_brush) DeleteObject(state.field_brush);
    if (state.field_focus_brush) DeleteObject(state.field_focus_brush);
    if (state.list_brush) DeleteObject(state.list_brush);
    aicli::ui::RestoreModalOwner(owner);
    return state.result;
}

bool ShowGroupNameDialog(HWND owner, const std::wstring& title,
                         const std::wstring& subtitle,
                         const std::wstring& initial_name,
                         bool light_theme,
                         std::wstring& name) {
    static bool registered = false;
    if (!registered) {
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
    state.font = CreateFontW(-15, 0, 0, 0, 400, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state.title_font = CreateFontW(-21, 0, 0, 0, 600, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state.small_font = CreateFontW(-13, 0, 0, 0, 400, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    ModalWorkspaceScope workspace(owner, -1, &state.palette);
    state.field_brush = CreateSolidBrush(state.field);
    state.field_focus_brush = CreateSolidBrush(state.field_focus);
    state.window = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, kGroupNameClassName, L"",
        WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 1, 1, owner, nullptr,
        GetModuleHandleW(nullptr), &state);
    if (state.window) {
        workspace.AttachSurface(state.window);
        aicli::ui::ActivateModalSurface(state.window, state.input);
        MSG message{};
        while (IsWindow(state.window) && GetMessageW(&message, nullptr, 0, 0)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (state.field_brush) DeleteObject(state.field_brush);
    if (state.field_focus_brush) DeleteObject(state.field_focus_brush);
    if (state.font) DeleteObject(state.font);
    if (state.title_font) DeleteObject(state.title_font);
    if (state.small_font) DeleteObject(state.small_font);
    if (state.accepted) name = state.value;
    aicli::ui::RestoreModalOwner(owner);
    return state.accepted;
}

} // namespace aicli::ui
