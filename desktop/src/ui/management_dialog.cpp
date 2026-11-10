#include "ui/management_dialog.h"

#include "aicli_build_identity.h"
#include "app/powershell_runtime.h"
#include "app/auto_update.h"
#include "ui/styled_input.h"
#include "ui/drawing_utils.h"
#include "ui/theme.h"
#include "ui/layout_metrics.h"
#include "ui/modal_host.h"
#include "ui/overlay_scrollbar.h"
#include "core/text_utils.h"
#include "features/mobile_project_creation.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace aicli::ui {
namespace {

constexpr wchar_t kClassName[] = L"AicliManagementDialog";
constexpr wchar_t kSettingsContentClassName[] = L"AicliSettingsContent";
constexpr wchar_t kGroupNameClassName[] = L"AicliGroupNameDialog";
constexpr wchar_t kProjectTypeEditorClassName[] = L"AicliProjectTypeEditorDialog";
constexpr UINT kPopulateMessage = WM_APP + 60;
constexpr int kSettingsHeaderHeight = 40;
constexpr UINT_PTR kDevicePresenceTimerId = 0xA1C32;

int DpiFor(HWND window) noexcept {
    return window ? std::max(96, static_cast<int>(GetDpiForWindow(window))) : 96;
}

int DpiScale(HWND window, int value) noexcept {
    return MulDiv(value, DpiFor(window), 96);
}

HFONT CreateDialogFont(HWND owner, int height, int weight) {
    return CreateFontW(-DpiScale(owner, height), 0, 0, 0, weight,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}
HFONT CreateDialogIconFont(HWND owner, int height) {
    return CreateFontW(-DpiScale(owner,height),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
                       DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe MDL2 Assets");
}
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
    kComputerName = 2319,
    kSettingsAppearance = 2320,
    kSettingsConnection = 2321,
    kSettingsProjectTypes = 2322,
    kSettingsGroups = 2323,
    kTaskbarWorking = 2324,
    kTaskbarScopeCurrent = 2325,
    kTaskbarScopeAll = 2326,
    kTaskbarStyleProgress = 2327,
    kTaskbarStyleOverlay = 2328,
    kMobileWorkspaceRoot = 2329,
    kMobileWorkspaceBrowse = 2330,
    kMobileAccess = 2331,
    kSettingsDevices = 2332,
    kBindEmpty = 2333,
    kTaskbarStyleIconDots = 2334,
    kSettingsAbout = 2335,
    kKeepAwake = 2336,
    kCheckUpdate = 2337,
};

enum : int {
    kGroupNameInput = 2410,
    kGroupNameSave = 2411,
    kGroupNameCancel = 2412,
    kGroupNameClose = 2413,
};

enum : int {
    kProjectTypeNameInput = 2420,
    kProjectTypeCommandInput = 2421,
    kProjectTypeSave = 2422,
    kProjectTypeCancel = 2423,
    kProjectTypeClose = 2424,
    kProjectTypeDefault = 2425,
};

enum class SettingsPage {
    Appearance,
    Connection,
    ProjectTypes,
    Groups,
    Devices,
    About,
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

std::wstring FormatDeviceTime(std::int64_t timestamp) {
    if(timestamp<=0)return L"从未";
    const std::time_t value=static_cast<std::time_t>(timestamp);
    std::tm local{};
    if(localtime_s(&local,&value)!=0)return L"未知";
    wchar_t text[32]{};
    if(wcsftime(text,std::size(text),L"%Y-%m-%d %H:%M",&local)==0)return L"未知";
    return text;
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
    AppearanceChangedCallback appearance_changed;
    ManagementDialogResult result;
    RECT panel{};
    RECT content_rect{};
    HFONT font{};
    HFONT title_font{};
    HFONT settings_title_font{};
    HFONT settings_nav_font{};
    HFONT settings_content_font{};
    HFONT settings_small_font{};
    HFONT settings_icon_font{};
    HFONT project_type_badge_font{};
    HFONT small_font{};
    HWND list{};
    HWND name{};
    HWND command{};
    HWND computer_name{};
    HWND listen{};
    HWND port{};
    HWND local{};
    HWND remote{};
    HWND mobile_workspace_root{};
    HWND mobile_workspace_browse{};
    HWND mobile_access{};
    HWND theme{};
    HWND theme_light{};
    HWND taskbar_working{};
    HWND keep_awake{};
    HWND taskbar_style_progress{};
    HWND taskbar_style_overlay{};
    HWND taskbar_style_icon_dots{};
    HWND taskbar_scope_current{};
    HWND taskbar_scope_all{};
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
    HWND settings_devices{};
    HWND settings_about{};
    HWND check_update{};
    HWND bind_empty{};
    HWND title_close{};
    HWND hovered_control{};
    std::vector<ProjectGroup> groups;
    std::vector<ProjectType> types_data;
    std::int64_t default_project_type_id{};
    std::vector<DeviceRecord> devices;
    std::vector<unsigned char> device_online_states;
    ThemePalette palette;
    bool light_theme{};
    bool mobile_access_enabled{};
    SettingsPage settings_page{SettingsPage::Appearance};
    OverlayScrollbar content_scrollbar;
    OverlayScrollbar list_scrollbar;
    int content_scroll{};
    int content_extent{};
    bool content_layout_active{};
    int device_hover_row{-1};
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

struct ProjectTypeEditorState {
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

bool ShowProjectTypeEditorDialog(HWND owner,
                                 const std::wstring& title,
                                 const std::wstring& subtitle,
                                 const std::wstring& initial_name,
                                 const std::wstring& initial_command,
                                 bool initial_default,
                                 bool light_theme,
                                 std::wstring& name,
                                 std::wstring& command,
                                 bool& make_default);

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
           id == kSettingsProjectTypes || id == kSettingsGroups ||
           id == kSettingsDevices || id == kSettingsAbout;
}

bool IsActiveSettingsNavigation(const State& state, int id) {
    if (state.mode != ManagementMode::Settings) return false;
    if (id == kSettingsAppearance) return state.settings_page == SettingsPage::Appearance;
    if (id == kSettingsConnection) return state.settings_page == SettingsPage::Connection;
    if (id == kSettingsProjectTypes) return state.settings_page == SettingsPage::ProjectTypes;
    if (id == kSettingsGroups) return state.settings_page == SettingsPage::Groups;
    if (id == kSettingsDevices) return state.settings_page == SettingsPage::Devices;
    if (id == kSettingsAbout) return state.settings_page == SettingsPage::About;
    return false;
}

HWND SettingsNavigationControl(const State& state, SettingsPage page) {
    switch (page) {
    case SettingsPage::Appearance: return state.settings_appearance;
    case SettingsPage::Connection: return state.settings_connection;
    case SettingsPage::ProjectTypes: return state.settings_project_types;
    case SettingsPage::Groups: return state.settings_groups;
    case SettingsPage::Devices: return state.settings_devices;
    case SettingsPage::About: return state.settings_about;
    }
    return nullptr;
}

COLORREF SettingsHeaderColor(const State& state) {
    return state.light_theme ? RGB(250, 251, 253) : RGB(22, 26, 33);
}

LRESULT CALLBACK SettingsContentProc(HWND h, UINT message, WPARAM wparam, LPARAM lparam);
void LayoutSettingsContent(State& state);
void UpdateSettingsPage(State& state, SettingsPage page);
void RefreshList(State& state);
void ReadSelection(State& state);
bool SelectedIndex(HWND list, size_t& index);

constexpr UINT_PTR kManagementButtonSubclassId = 0xA1C14;
constexpr UINT_PTR kSettingsWheelSubclassId = 0xA1C19;

LRESULT CALLBACK SettingsWheelProc(HWND control, UINT message, WPARAM wparam,
                                   LPARAM lparam, UINT_PTR, DWORD_PTR reference) {
    auto* state = reinterpret_cast<State*>(reference);
    const bool device_list=state&&control==state->list&&
        state->mode==ManagementMode::Settings&&
        state->settings_page==SettingsPage::Devices;
    if(message==WM_MOUSEMOVE&&device_list){
        const LRESULT hit=SendMessageW(control,LB_ITEMFROMPOINT,0,lparam);
        const int next=HIWORD(static_cast<DWORD_PTR>(hit))? -1:
            static_cast<int>(LOWORD(static_cast<DWORD_PTR>(hit)));
        if(next!=state->device_hover_row){
            const int previous=state->device_hover_row;state->device_hover_row=next;
            RECT row{};if(previous>=0&&SendMessageW(control,LB_GETITEMRECT,previous,reinterpret_cast<LPARAM>(&row))!=LB_ERR)InvalidateRect(control,&row,FALSE);
            if(next>=0&&SendMessageW(control,LB_GETITEMRECT,next,reinterpret_cast<LPARAM>(&row))!=LB_ERR)InvalidateRect(control,&row,FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,control,0};TrackMouseEvent(&tracking);
    }
    if(message==WM_MOUSELEAVE&&device_list){
        const int previous=state->device_hover_row;state->device_hover_row=-1;
        RECT row{};if(previous>=0&&SendMessageW(control,LB_GETITEMRECT,previous,reinterpret_cast<LPARAM>(&row))!=LB_ERR)InvalidateRect(control,&row,FALSE);
        return 0;
    }
    if(message==WM_MOUSEWHEEL&&device_list){
        const int delta=GET_WHEEL_DELTA_WPARAM(wparam);
        const int top=static_cast<int>(SendMessageW(control,LB_GETTOPINDEX,0,0));
        const int next=std::max(0,top-(delta/WHEEL_DELTA)*3);
        SendMessageW(control,LB_SETTOPINDEX,next,0);
        state->list_scrollbar.NotifyActivity();state->list_scrollbar.Sync();
        return 0;
    }
    if (message == WM_MOUSEWHEEL && state && state->content) {
        // LISTBOX consumes wheel input even when its own rows do not need to
        // move. The list is part of the scrolling settings document, so route
        // wheel input from both its empty surface and owner-drawn items to the
        // content viewport.
        return SendMessageW(state->content, WM_MOUSEWHEEL, wparam, lparam);
    }
    if (message == WM_ERASEBKGND && state && control == state->list) {
        // Moving an owner-drawn LISTBOX does not make the stock control erase
        // the unused area below its final item. With wheel scrolling that left
        // the old last row visible at its previous screen position, appearing
        // as a duplicated project type/group. Clear the complete list surface
        // before its real rows are drawn.
        HDC dc = reinterpret_cast<HDC>(wparam);
        RECT client{};
        GetClientRect(control, &client);
        HBRUSH background = CreateSolidBrush(state->palette.list_surface);
        FillRect(dc, &client, background);
        DeleteObject(background);
        return 1;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(control, SettingsWheelProc, kSettingsWheelSubclassId);
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

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
    for (HWND field : {state.name, state.command, state.computer_name, state.listen, state.port,
                       state.local, state.remote, state.mobile_workspace_root}) {
        if (field) SetStyledInputTheme(field, state.palette);
    }
    state.content_scrollbar.SetTheme(state.palette, state.background_color);
    state.list_scrollbar.SetTheme(state.palette,state.palette.list_surface);
    if (state.content) {
        RedrawWindow(state.content, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN);
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
    const auto S=[&](int value){return DpiScale(state.window,value);};
    const int width = std::min(S(460), std::max(S(400), client_width - S(120)));
    const int height = S(238);
    const int left = (client_width - width) / 2;
    const int top = std::max(S(24), (client_height - height) / 2);
    state.panel = {left, top, left + width, top + height};
    LayoutStyledInput(state.input, {left + S(24), top + S(112), left + width - S(24), top + S(147)}, S(2), S(8));
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

void PaintGroupNameDialog(GroupNameState& state, HDC dc) {
    RECT client{};
    GetClientRect(state.window, &client);
    // The layered ModalWorkspaceScope scrim is behind this content window.
    // Leave the host background untouched so the dimmed owner remains visible.
    (void)client;

    const auto S=[&](int value){return DpiScale(state.window,value);};
    HBRUSH panel_background = CreateSolidBrush(state.panel_color);
    FillRect(dc, &state.panel, panel_background);
    DeleteObject(panel_background);
    HBRUSH panel_brush = CreateSolidBrush(state.panel_color);
    HPEN panel_pen = CreatePen(PS_SOLID, 1, state.panel_edge);
    HGDIOBJ old_brush = SelectObject(dc, panel_brush);
    HGDIOBJ old_pen = SelectObject(dc, panel_pen);
    RoundRect(dc, state.panel.left, state.panel.top, state.panel.right, state.panel.bottom, S(16), S(16));
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(panel_brush);
    DeleteObject(panel_pen);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, state.title_font);
    SetTextColor(dc, state.text);
    RECT title{state.panel.left + S(24), state.panel.top,
               state.panel.right - S(52), state.panel.top + S(42)};
    DrawTextW(dc, state.title.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, state.small_font);
    SetTextColor(dc, state.muted);
    RECT subtitle{state.panel.left + S(24), state.panel.top + S(50),
                  state.panel.right - S(24), state.panel.top + S(76)};
    DrawTextW(dc, state.subtitle.c_str(), -1, &subtitle,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT label{state.panel.left + S(24), state.panel.top + S(84),
               state.panel.right - S(24), state.panel.top + S(108)};
    DrawTextW(dc, L"分组名称", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const COLORREF outline = GetFocus() == state.input ? state.accent : state.panel_edge;
    HPEN input_pen = CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ previous_pen = SelectObject(dc, input_pen);
    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, state.panel.left + S(24), state.panel.top + S(112),
              state.panel.right - S(24), state.panel.top + S(147), S(8), S(8));
    SelectObject(dc, previous_brush);
    SelectObject(dc, previous_pen);
    DeleteObject(input_pen);
    HPEN separator=CreatePen(PS_SOLID,1,state.panel_edge);
    HGDIOBJ old_separator=SelectObject(dc,separator);
    MoveToEx(dc,state.panel.left+1,state.panel.top+S(42),nullptr);
    LineTo(dc,state.panel.right-1,state.panel.top+S(42));
    SelectObject(dc,old_separator);DeleteObject(separator);
}

void DrawGroupNameButton(GroupNameState& state, const DRAWITEMSTRUCT& item) {
    RECT rect = item.rcItem;
    const bool primary = item.CtlID == kGroupNameSave;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hovered = state.hovered == item.hwndItem;
    COLORREF fill = primary ? state.accent : state.field;
    COLORREF edge = fill;
    if (hovered || pressed) {
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
    const int radius=DpiScale(state.window,9);
    RoundRect(item.hDC, rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1, radius, radius);
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
        SetWindowTextW(state->save,L"确定");
        state->close = CreateWindowExW(0,L"BUTTON",L"×",
            WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,1,1,h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGroupNameClose)),
            GetModuleHandleW(nullptr),nullptr);
        for (HWND button : {state->cancel, state->save, state->close}) {
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
        if (HIWORD(wparam) == BN_CLICKED &&
            (LOWORD(wparam) == kGroupNameCancel || LOWORD(wparam) == kGroupNameClose)) {
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
constexpr UINT_PTR kProjectTypeEditorToggleTimerId = 0xA1C19;
constexpr ULONGLONG kProjectTypeEditorToggleDurationMs = 180;

bool AdvanceProjectTypeDefaultToggle(ProjectTypeEditorState& state) {
    if (!state.default_toggle_started) {
        state.default_toggle_progress = state.make_default ? 1.0f : 0.0f;
        return true;
    }
    const auto elapsed = GetTickCount64() - state.default_toggle_started;
    const float t = std::clamp(
        static_cast<float>(elapsed) /
            static_cast<float>(kProjectTypeEditorToggleDurationMs),
        0.0f, 1.0f);
    const float inverse = 1.0f - t;
    const float eased = 1.0f - inverse * inverse * inverse;
    const float target = state.make_default ? 1.0f : 0.0f;
    state.default_toggle_progress =
        state.default_toggle_from +
        (target - state.default_toggle_from) * eased;
    if (t < 1.0f) return false;
    state.default_toggle_progress = target;
    state.default_toggle_started = 0;
    return true;
}

void AnimateProjectTypeDefaultToggle(ProjectTypeEditorState& state) {
    AdvanceProjectTypeDefaultToggle(state);
    state.default_toggle_from = state.default_toggle_progress;
    state.make_default = !state.make_default;
    state.default_toggle_started = GetTickCount64();
    SetTimer(state.window, kProjectTypeEditorToggleTimerId, 15, nullptr);
    InvalidateRect(state.default_toggle, nullptr, FALSE);
}

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
    const auto S=[&](int value){return DpiScale(state.window,value);};
    const int width = std::min(S(640), std::max(S(500), client_width - S(120)));
    const int height = S(420);
    const int left = (client_width - width) / 2;
    const int top = std::max(S(24), (client_height - height) / 2);
    state.panel = {left, top, left + width, top + height};
    LayoutStyledInput(state.name_input,
                      {left + S(30), top + S(120), left + width - S(30), top + S(155)}, S(2), S(8));
    LayoutStyledInput(state.command_input,
                      {left + S(30), top + S(204), left + width - S(30), top + S(239)}, S(2), S(8));
    MoveWindow(state.default_toggle, left + S(30), top + S(282),
               width - S(60), S(48), TRUE);
    MoveWindow(state.cancel, left + width - S(238), top + S(356), S(100), S(36), TRUE);
    MoveWindow(state.save, left + width - S(126), top + S(356), S(96), S(36), TRUE);
    MoveWindow(state.close, left + width - S(42), top + S(5), S(32), S(32), TRUE);
    SetWindowRgn(state.cancel, CreateRoundRectRgn(0, 0, S(101), S(39), S(9), S(9)), TRUE);
    SetWindowRgn(state.save, CreateRoundRectRgn(0, 0, S(109), S(39), S(9), S(9)), TRUE);
    // The default card paints its own antialiased rounded edge. A GDI window
    // region clips the blended corner pixels and is the source of the jagged
    // outline that was visible around this control.
    SetWindowRgn(state.default_toggle, nullptr, FALSE);
    SetWindowRgn(state.close, CreateRoundRectRgn(0,0,S(33),S(33),S(7),S(7)),TRUE);
    // Match the window region to the 16 px RoundRect curve painted below.
    // A smaller region admits four corner pixels outside the painted panel.
    SetModalSurfaceRegion(state.window, state.panel, S(16));
    InvalidateRect(state.window, nullptr, FALSE);
}

void PaintProjectTypeEditorDialog(ProjectTypeEditorState& state, HDC dc) {
    RECT client{};
    GetClientRect(state.window, &client);
    (void)client;

    const auto S=[&](int value){return DpiScale(state.window,value);};
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
    RoundRect(dc, state.panel.left, state.panel.top, state.panel.right,
              state.panel.bottom, S(16), S(16));
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(panel_brush);
    DeleteObject(panel_pen);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, state.title_font);
    SetTextColor(dc, state.text);
    RECT title{state.panel.left + S(24), state.panel.top,
               state.panel.right - S(52), state.panel.top + S(42)};
    DrawTextW(dc, state.title.c_str(), -1, &title,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, state.small_font);
    SetTextColor(dc, state.muted);
    RECT subtitle{state.panel.left + S(30), state.panel.top + S(50),
                  state.panel.right - S(30), state.panel.top + S(80)};
    DrawTextW(dc, state.subtitle.c_str(), -1, &subtitle,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    auto label = [&](const wchar_t* value, int y) {
        RECT rect{state.panel.left + S(30), state.panel.top + S(y),
                  state.panel.right - S(30), state.panel.top + S(y + 24)};
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
    outline(state.name_input,
            {state.panel.left + S(30), state.panel.top + S(120),
             state.panel.right - S(30), state.panel.top + S(155)});
    outline(state.command_input,
            {state.panel.left + S(30), state.panel.top + S(204),
             state.panel.right - S(30), state.panel.top + S(239)});

    SetTextColor(dc, state.muted);
    SelectObject(dc, state.small_font);
    RECT hint{state.panel.left + S(30), state.panel.top + S(248),
              state.panel.right - S(30), state.panel.top + S(276)};
    DrawTextW(dc, L"启动命令会作为新建项目的默认值，项目仍可单独覆盖。", -1,
              &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    HPEN separator=CreatePen(PS_SOLID,1,state.panel_edge);
    HGDIOBJ old_separator=SelectObject(dc,separator);
    MoveToEx(dc,state.panel.left+1,state.panel.top+S(42),nullptr);
    LineTo(dc,state.panel.right-1,state.panel.top+S(42));
    SelectObject(dc,old_separator);DeleteObject(separator);
}

void DrawProjectTypeEditorButton(ProjectTypeEditorState& state,
                                 const DRAWITEMSTRUCT& item) {
    RECT rect = item.rcItem;
    if (item.CtlID == kProjectTypeDefault) {
        BufferedWindowPaint buffered(item.hDC, rect, rect);
        HDC dc = buffered.dc();
        const bool hovered = state.hovered == item.hwndItem;
        HBRUSH clear = CreateSolidBrush(state.panel_color);
        FillRect(dc, &rect, clear);
        DeleteObject(clear);

        const COLORREF fill = hovered ? state.field_focus : state.field;
        const auto S = [&](int value) { return DpiScale(state.window, value); };
        RECT card{rect.left + S(1), rect.top + S(1),
                  rect.right - S(1), rect.bottom - S(1)};
        DrawSmoothRoundedRect(dc, card, static_cast<float>(S(8)), fill,
                              state.panel_edge,
                              static_cast<float>(std::max(1, S(1))));

        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, state.font);
        SetTextColor(dc, state.text);
        RECT title{rect.left + S(14), rect.top + S(4),
                   rect.right - S(74), rect.top + S(27)};
        DrawTextW(dc, L"默认项目类型", -1, &title,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, state.small_font);
        SetTextColor(dc, state.muted);
        RECT detail{rect.left + S(14), rect.top + S(23),
                    rect.right - S(74), rect.bottom - S(2)};
        DrawTextW(dc, L"新建项目时自动选中；设置后会替换原默认类型", -1,
                  &detail, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT track{rect.right - S(58), rect.top + S(14),
                   rect.right - S(14), rect.top + S(36)};
        const BYTE blend = static_cast<BYTE>(std::clamp(
            state.default_toggle_progress * 255.0f, 0.0f, 255.0f));
        const COLORREF track_color =
            BlendBackdropColor(state.panel_edge, state.accent, blend);

        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        Gdiplus::RectF track_bounds{
            static_cast<float>(track.left), static_cast<float>(track.top),
            static_cast<float>(track.right - track.left),
            static_cast<float>(track.bottom - track.top)};
        Gdiplus::GraphicsPath track_path;
        AddRoundedPath(track_path, track_bounds, track_bounds.Height / 2.0f);
        Gdiplus::SolidBrush track_brush(SmoothColor(track_color));
        graphics.FillPath(&track_brush, &track_path);

        const float inset = static_cast<float>(S(2));
        const float knob = static_cast<float>(S(18));
        const float travel = std::max(
            0.0f, track_bounds.Width - knob - inset * 2.0f);
        const float knob_left = track_bounds.X + inset +
            travel * state.default_toggle_progress;
        Gdiplus::SolidBrush knob_brush(SmoothColor(RGB(255, 255, 255)));
        graphics.FillEllipse(&knob_brush, knob_left, track_bounds.Y + inset,
                             knob, knob);
        return;
    }
    const bool primary = item.CtlID == kProjectTypeSave;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hovered = state.hovered == item.hwndItem;
    COLORREF fill = primary ? state.accent : state.field;
    COLORREF edge = fill;
    if (hovered || pressed) {
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
    const int radius=DpiScale(state.window,9);
    RoundRect(item.hDC, rect.left + 1, rect.top + 1,
              rect.right - 1, rect.bottom - 1, radius, radius);
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
        state->default_toggle = CreateWindowExW(
            0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectTypeDefault)),
            GetModuleHandleW(nullptr), nullptr);
        state->cancel = CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectTypeCancel)),
            GetModuleHandleW(nullptr), nullptr);
        state->save = CreateWindowExW(0, L"BUTTON", L"保存",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectTypeSave)),
            GetModuleHandleW(nullptr), nullptr);
        SetWindowTextW(state->save,L"确定");
        state->close=CreateWindowExW(0,L"BUTTON",L"×",
            WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,1,1,h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectTypeClose)),
            GetModuleHandleW(nullptr),nullptr);
        for (HWND button : {state->default_toggle, state->cancel, state->save, state->close}) {
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
    case WM_TIMER:
        if (wparam == kProjectTypeEditorToggleTimerId) {
            if (AdvanceProjectTypeDefaultToggle(*state)) {
                KillTimer(h, kProjectTypeEditorToggleTimerId);
            }
            InvalidateRect(state->default_toggle, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kProjectTypeDefault) {
            AnimateProjectTypeDefaultToggle(*state);
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED &&
            (LOWORD(wparam) == kProjectTypeCancel || LOWORD(wparam) == kProjectTypeClose)) {
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
    case WM_DESTROY:
        KillTimer(h, kProjectTypeEditorToggleTimerId);
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
    RECT client{};
    GetClientRect(item->hwndItem, &client);
    BufferedWindowPaint buffered(item->hDC, client, item->rcItem);
    DRAWITEMSTRUCT buffered_item = *item;
    buffered_item.hDC = buffered.dc();
    item = &buffered_item;
    State* state = FindState(item->hwndItem);
    const COLORREF text_color = state ? state->text_color : DarkPalette().text;
    const COLORREF muted_color = state ? state->muted_color : DarkPalette().muted;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool hovered = state && state->hovered_control == item->hwndItem;
    const bool active_navigation = state && IsActiveSettingsNavigation(*state, item->CtlID);
    const bool active_theme = state &&
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
    const auto S=[&](int value){return state?DpiScale(state->window,value):value;};

    const bool title_close = state &&
                              state->title_close == item->hwndItem;
    if (title_close && state) {
        // Keep the idle control visually part of the title area, then add a
        // compact rounded hover layer without restoring the old square halo.
        HBRUSH clear = CreateSolidBrush(state->mode==ManagementMode::Settings
                                            ? SettingsHeaderColor(*state)
                                            : state->panel_color);
        FillRect(item->hDC, &rect, clear);
        DeleteObject(clear);
        if (hovered || pressed) {
            const COLORREF close_fill = pressed
                ? (light ? RGB(241, 218, 222) : RGB(70, 39, 45))
                : (light ? RGB(246, 232, 235) : RGB(54, 38, 43));
            HBRUSH hover_brush = CreateSolidBrush(close_fill);
            HGDIOBJ old_brush = SelectObject(item->hDC, hover_brush);
            HGDIOBJ old_pen = SelectObject(item->hDC, GetStockObject(NULL_PEN));
            RoundRect(item->hDC, rect.left + S(3), rect.top + S(3),
                      rect.right - S(3), rect.bottom - S(3), S(8), S(8));
            SelectObject(item->hDC, old_pen);
            SelectObject(item->hDC, old_brush);
            DeleteObject(hover_brush);
        }
        SetBkMode(item->hDC, TRANSPARENT);
        SelectObject(item->hDC, state->title_font);
        SetTextColor(item->hDC, pressed
                                  ? (light ? RGB(170, 44, 58) : RGB(255, 145, 155))
                                  : (hovered
                                         ? (light ? RGB(184, 54, 67) : RGB(255, 132, 143))
                                         : state->muted_color));
        RECT close_rect = rect;
        OffsetRect(&close_rect, 0, -S(2));
        DrawTextW(item->hDC, L"\x00D7", -1, &close_rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    const bool settings_navigation = state &&
        IsSettingsNavigationId(item->CtlID);
    if (settings_navigation) {
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
                : (hovered
                       ? (light ? RGB(239, 243, 248) : RGB(32, 37, 45))
                       : (light ? RGB(246, 248, 251) : RGB(25, 29, 37)));
            if (pressed)
                nav_fill = light ? RGB(218, 230, 246) : RGB(37, 50, 68);
            HBRUSH nav_brush = CreateSolidBrush(nav_fill);
            HGDIOBJ old_brush = SelectObject(item->hDC, nav_brush);
            HGDIOBJ old_pen = SelectObject(item->hDC, GetStockObject(NULL_PEN));
            RoundRect(item->hDC, rect.left + S(2), rect.top + S(2),
                      rect.right - S(2), rect.bottom - S(2), S(8), S(8));
            SelectObject(item->hDC, old_pen);
            SelectObject(item->hDC, old_brush);
            DeleteObject(nav_brush);
        }

        wchar_t text[256]{};
        GetWindowTextW(item->hwndItem, text,
                       static_cast<int>(std::size(text)));
        const wchar_t* icon = item->CtlID == kSettingsAppearance ? L"\xE790" :
            item->CtlID == kSettingsConnection ? L"\xE774" :
            item->CtlID == kSettingsProjectTypes ? L"\xE756" :
            item->CtlID == kSettingsGroups ? L"\xE8B7" : L"\xE8EA";
        SetBkMode(item->hDC, TRANSPARENT);
        SelectObject(item->hDC,state->settings_icon_font?state->settings_icon_font:state->settings_nav_font);
        SetTextColor(item->hDC,active_navigation?state->accent_color:muted_color);
        RECT icon_rect{rect.left+S(13),rect.top,rect.left+S(37),rect.bottom};
        DrawTextW(item->hDC,icon,-1,&icon_rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(item->hDC, state->settings_nav_font ? state->settings_nav_font : state->font);
        SetTextColor(item->hDC, disabled ? muted_color : text_color);
        RECT title_rect{rect.left + S(44), rect.top,
                        rect.right - S(28), rect.bottom};
        DrawTextW(item->hDC, text, -1, &title_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(item->hDC, active_navigation
                                  ? state->accent_color : muted_color);
        SelectObject(item->hDC, state->settings_nav_font ? state->settings_nav_font : state->font);
        RECT chevron{rect.right - S(27), rect.top, rect.right - S(8), rect.bottom};
        DrawTextW(item->hDC, L">", -1, &chevron,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    const bool taskbar_toggle = state &&
        (item->CtlID == kTaskbarWorking || item->CtlID == kKeepAwake);
    if (taskbar_toggle) {
        // A binary setting reads more naturally as a compact switch than as
        // a second action button. Keep the full HWND rectangular so the
        // anti-aliased track can blend into the card without clipped edges.
        HBRUSH clear = CreateSolidBrush(state->panel_color);
        FillRect(item->hDC, &rect, clear);
        DeleteObject(clear);

        COLORREF track = active_theme
            ? (light ? RGB(70, 137, 218) : RGB(47, 103, 166))
            : (light ? RGB(199, 207, 218) : RGB(53, 59, 70));
        if (hovered) {
            track = active_theme
                ? (light ? RGB(61, 128, 211) : RGB(55, 119, 188))
                : (light ? RGB(185, 195, 208) : RGB(63, 71, 84));
        }
        if (pressed) {
            track = active_theme
                ? (light ? RGB(51, 113, 191) : RGB(42, 91, 147))
                : (light ? RGB(174, 185, 199) : RGB(46, 52, 62));
        }
        const COLORREF track_edge = active_theme
            ? (light ? RGB(55, 119, 202) : RGB(70, 132, 202))
            : (light ? RGB(180, 189, 201) : RGB(70, 78, 91));
        RECT track_rect{rect.left + S(1), rect.top + S(1),
                        rect.right - S(1), rect.bottom - S(1)};
        DrawSmoothRoundedRect(item->hDC, track_rect,
                              static_cast<float>(S(13)),
                              track, track_edge, 1.0f);

        const int knob_size = S(18);
        const int knob_margin = S(4);
        const int knob_left = active_theme
            ? rect.right - knob_margin - knob_size
            : rect.left + knob_margin;
        RECT knob{knob_left,
                  rect.top + (rect.bottom - rect.top - knob_size) / 2,
                  knob_left + knob_size,
                  rect.top + (rect.bottom - rect.top + knob_size) / 2};
        const COLORREF knob_fill = light ? RGB(255, 255, 255)
                                         : RGB(232, 238, 247);
        const COLORREF knob_edge = light ? RGB(225, 230, 237)
                                         : RGB(195, 207, 222);
        DrawSmoothRoundedRect(item->hDC, knob,
                              static_cast<float>(knob_size / 2),
                              knob_fill, knob_edge, 1.0f);
        return;
    }

    const bool segmented_choice = state &&
        (item->CtlID == kTheme || item->CtlID == kThemeLight ||
         item->CtlID == kTaskbarStyleProgress ||
         item->CtlID == kTaskbarStyleOverlay ||
         item->CtlID == kTaskbarStyleIconDots ||
         item->CtlID == kTaskbarScopeCurrent ||
         item->CtlID == kTaskbarScopeAll);
    if (segmented_choice) {
        // Segmented settings share one rounded base. Each child paints only
        // its selected or hover interaction layer.
        const COLORREF selector_fill = light ? RGB(238, 242, 247)
                                             : RGB(25, 29, 37);
        const COLORREF hover_fill = light ? RGB(229, 235, 243)
                                          : RGB(34, 40, 51);
        const COLORREF selected_fill = light ? RGB(226, 237, 251)
                                             : RGB(32, 58, 90);
        const COLORREF selected_edge = light ? RGB(190, 210, 236)
                                             : RGB(48, 82, 122);
        const COLORREF selected_text = light ? RGB(39, 102, 190)
                                             : RGB(221, 235, 255);

        HBRUSH clear = CreateSolidBrush(selector_fill);
        FillRect(item->hDC, &rect, clear);
        DeleteObject(clear);

        COLORREF segment_fill = selector_fill;
        COLORREF segment_edge = selector_fill;
        float edge_width = 0.0f;
        if (active_theme) {
            segment_fill = pressed
                ? (light ? RGB(214, 229, 247) : RGB(28, 51, 79))
                : selected_fill;
            segment_edge = selected_edge;
            edge_width = 1.0f;
        } else if (hovered || pressed) {
            segment_fill = pressed
                ? (light ? RGB(220, 228, 238) : RGB(39, 46, 59))
                : hover_fill;
        }

        if (active_theme || hovered || pressed) {
            RECT layer{rect.left + S(1), rect.top + S(1),
                       rect.right - S(1), rect.bottom - S(1)};
            DrawSmoothRoundedRect(item->hDC, layer,
                                  static_cast<float>(S(7)),
                                  segment_fill, segment_edge, edge_width);
        }

        wchar_t text[256]{};
        GetWindowTextW(item->hwndItem, text,
                       static_cast<int>(std::size(text)));
        const wchar_t* icon = item->CtlID == kTheme ? L"\xE708" :
                              item->CtlID == kThemeLight ? L"\xE706" :
                              item->CtlID == kTaskbarStyleOverlay ? L"\xE895" :
                              item->CtlID == kTaskbarStyleIconDots ? L"\xE712" :
                              item->CtlID == kTaskbarScopeCurrent ? L"\xE8B7" :
                              item->CtlID == kTaskbarScopeAll ? L"\xE8A5" : nullptr;
        const HFONT icon_font = state->settings_icon_font
            ? state->settings_icon_font : state->font;
        const HFONT text_font = state->settings_content_font
            ? state->settings_content_font : state->font;
        SIZE icon_size{};
        SIZE text_size{};
        if (icon) {
            SelectObject(item->hDC, icon_font);
            GetTextExtentPoint32W(item->hDC, icon, 1, &icon_size);
        } else {
            icon_size.cx = S(15);
        }
        SelectObject(item->hDC, text_font);
        GetTextExtentPoint32W(item->hDC, text,
                              static_cast<int>(wcslen(text)), &text_size);
        const int gap = S(7);
        const int content_width = icon_size.cx + gap + text_size.cx;
        const int segment_width = static_cast<int>(rect.right - rect.left);
        const int content_left = rect.left +
            std::max(0, (segment_width - content_width) / 2);

        SetBkMode(item->hDC, TRANSPARENT);
        const COLORREF foreground = disabled
            ? muted_color
            : (active_theme ? selected_text
                            : (hovered ? text_color : muted_color));
        SetTextColor(item->hDC, foreground);
        RECT icon_rect{content_left, rect.top,
                       content_left + icon_size.cx, rect.bottom};
        if (icon) {
            SelectObject(item->hDC, icon_font);
            DrawTextW(item->hDC, icon, 1, &icon_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else {
            // Compact vector progress glyph; it stays crisp at every DPI and
            // avoids relying on an unrelated font symbol.
            const int glyph_height = S(5);
            const int glyph_top = rect.top +
                (rect.bottom - rect.top - glyph_height) / 2;
            RECT progress_track{icon_rect.left, glyph_top,
                                icon_rect.right, glyph_top + glyph_height};
            const COLORREF progress_back = active_theme
                ? BlendBackdrop(selected_fill, foreground, 54)
                : BlendBackdrop(selector_fill, foreground, 44);
            DrawSmoothRoundedRect(item->hDC, progress_track,
                                  static_cast<float>(S(2)),
                                  progress_back, progress_back, 0.0f);
            RECT progress_fill = progress_track;
            const int progress_width = static_cast<int>(
                progress_track.right - progress_track.left);
            progress_fill.right = progress_fill.left +
                std::max(S(5), progress_width * 3 / 5);
            DrawSmoothRoundedRect(item->hDC, progress_fill,
                                  static_cast<float>(S(2)),
                                  foreground, foreground, 0.0f);
        }
        SelectObject(item->hDC, text_font);
        RECT text_rect{icon_rect.right + gap, rect.top,
                       rect.right, rect.bottom};
        DrawTextW(item->hDC, text, -1, &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
    if (item->CtlID == kDelete) {
        fill = light ? RGB(253, 240, 242) : RGB(56, 40, 45);
        edge = light ? RGB(225, 170, 178) : RGB(116, 65, 77);
        if (hovered || pressed) {
            fill = light ? RGB(249, 222, 226) : RGB(82, 43, 50);
            edge = light ? RGB(211, 124, 139) : RGB(151, 70, 84);
        }
    }
    if(item->CtlID==kUnbind&&state){
        fill=state->background_color;edge=state->palette.danger;
        if(hovered||pressed)fill=light?RGB(253,238,241):RGB(56,35,42);
    }
    if (disabled) {
        fill = light ? RGB(235, 238, 243) : RGB(32, 35, 41);
        edge = light ? RGB(218, 223, 231) : RGB(50, 54, 62);
    }
    // Every management button sits on a modal surface. Clear the complete
    // owner-draw rectangle with that surface color before painting the
    // rounded button; filling it with the button color leaves square corners
    // behind on hover/press, notably for Unbind and Close.
    if (state) {
        const COLORREF clear_color=GetParent(item->hwndItem)==state->content
            ?state->background_color:state->panel_color;
        HBRUSH clear = CreateSolidBrush(clear_color);
        FillRect(item->hDC, &rect, clear);
        DeleteObject(clear);
    }
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ old_brush = SelectObject(item->hDC, brush);
    HGDIOBJ old_pen = SelectObject(item->hDC, pen);
    RoundRect(item->hDC, rect.left + S(1), rect.top + S(1), rect.right - S(1), rect.bottom - S(1), S(9), S(9));
    SelectObject(item->hDC, old_brush);
    SelectObject(item->hDC, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t text[256]{};
    GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
    if (active_navigation && state) {
        RECT marker{rect.left + S(1), rect.top + S(10), rect.left + S(4), rect.bottom - S(10)};
        HBRUSH marker_brush = CreateSolidBrush(state->accent_color);
        FillRect(item->hDC, &marker, marker_brush);
        DeleteObject(marker_brush);
    }

    SetBkMode(item->hDC, TRANSPARENT);
    if (IsSettingsNavigationId(item->CtlID) && state) {
        const wchar_t* subtitle = item->CtlID == kSettingsAppearance ? L"主题与显示" :
                                  item->CtlID == kSettingsConnection ? L"地址与 Agent" :
                                  item->CtlID == kSettingsProjectTypes ? L"CLI 启动配置" :
                                  item->CtlID == kSettingsGroups ? L"项目归类管理" :
                                  item->CtlID == kSettingsDevices ? L"手机授权状态" :
                                  L"版本与终端环境";
        SelectObject(item->hDC, state->font);
        SetTextColor(item->hDC, disabled ? muted_color : text_color);
        RECT title_rect{rect.left + S(16), rect.top + S(7), rect.right - S(28), rect.top + S(29)};
        DrawTextW(item->hDC, text, -1, &title_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(item->hDC, state->small_font);
        SetTextColor(item->hDC, muted_color);
        RECT subtitle_rect{rect.left + S(16), rect.top + S(27), rect.right - S(28), rect.bottom - S(4)};
        DrawTextW(item->hDC, subtitle, -1, &subtitle_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(item->hDC, active_navigation ? state->accent_color : muted_color);
        SelectObject(item->hDC, state->font);
        RECT chevron{rect.right - S(27), rect.top, rect.right - S(8), rect.bottom};
        DrawTextW(item->hDC, L">", -1, &chevron, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    SetTextColor(item->hDC, disabled ? muted_color :
                 (item->CtlID==kUnbind&&state?state->palette.danger:
                   (active_theme && state ? state->accent_color : text_color)));
    if (state) {
        const bool settings_content = state->mode == ManagementMode::Settings &&
                                      GetParent(item->hwndItem) == state->content;
        SelectObject(item->hDC,
                     settings_content && state->settings_content_font
                         ? state->settings_content_font : state->font);
    }
    RECT text_rect = rect;
    DrawTextW(item->hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawListItem(const DRAWITEMSTRUCT* item) {
    if (!item || item->itemID == static_cast<UINT>(-1)) return;
    RECT client{};
    GetClientRect(item->hwndItem, &client);
    BufferedWindowPaint buffered(item->hDC, client, item->rcItem);
    DRAWITEMSTRUCT buffered_item = *item;
    buffered_item.hDC = buffered.dc();
    item = &buffered_item;
    State* state = FindState(item->hwndItem);
    if (!state) return;
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    wchar_t text[512]{};
    SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, selected ? state->text_color : state->palette.list_text);
    const int inset=DpiScale(state->window,12);
    RECT rect{item->rcItem.left + inset, item->rcItem.top, item->rcItem.right - inset, item->rcItem.bottom};
    const size_t data_index=static_cast<size_t>(SendMessageW(
        item->hwndItem,LB_GETITEMDATA,item->itemID,0));
    const bool device_item=(state->mode==ManagementMode::Devices||
        (state->mode==ManagementMode::Settings&&state->settings_page==SettingsPage::Devices))&&
        data_index<state->devices.size();
    if(device_item){
        const auto& device=state->devices[data_index];
        const bool online=state->agent&&state->agent->IsDeviceOnline(device.phone_id);
        const COLORREF status=online?state->palette.success:
            (device.active?state->palette.muted:state->palette.danger);
        const bool hovered=static_cast<int>(item->itemID)==state->device_hover_row;
        HBRUSH clear=CreateSolidBrush(state->palette.list_surface);FillRect(item->hDC,&item->rcItem,clear);DeleteObject(clear);
        RECT card{item->rcItem.left+DpiScale(state->window,3),item->rcItem.top+DpiScale(state->window,2),
                  item->rcItem.right-DpiScale(state->window,3),item->rcItem.bottom-DpiScale(state->window,2)};
        const COLORREF card_fill=selected
            ?(state->light_theme?RGB(232,241,253):RGB(24,40,62))
            :(hovered?(state->light_theme?RGB(244,247,251):RGB(24,31,42)):state->palette.list_surface);
        const COLORREF card_edge=selected?state->accent_color:
            (hovered?state->panel_edge:card_fill);
        DrawSmoothRoundedRect(item->hDC,card,static_cast<float>(DpiScale(state->window,6)),card_fill,card_edge,1.0f);
        const int icon_size=DpiScale(state->window,30);
        RECT icon_rect{card.left+DpiScale(state->window,10),(card.top+card.bottom-icon_size)/2,
                       card.left+DpiScale(state->window,10)+icon_size,(card.top+card.bottom+icon_size)/2};
        const COLORREF icon_fill=selected
            ?(state->light_theme?RGB(215,231,251):RGB(31,58,88)):state->palette.surface;
        DrawSmoothRoundedRect(item->hDC,icon_rect,static_cast<float>(DpiScale(state->window,7)),icon_fill,icon_fill,0.0f);
        SelectObject(item->hDC,state->settings_icon_font?state->settings_icon_font:state->font);
        SetTextColor(item->hDC,selected?state->accent_color:state->muted_color);
        DrawTextW(item->hDC,L"\xE8EA",-1,&icon_rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        const int text_left=icon_rect.right+DpiScale(state->window,10);
        SelectObject(item->hDC,state->settings_content_font?state->settings_content_font:state->font);
        SetTextColor(item->hDC,state->text_color);
        RECT name_rect{text_left,card.top+DpiScale(state->window,4),card.right-DpiScale(state->window,58),card.top+DpiScale(state->window,30)};
        DrawTextW(item->hDC,device.name.c_str(),-1,&name_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        SelectObject(item->hDC,state->settings_small_font?state->settings_small_font:state->small_font);SetTextColor(item->hDC,state->muted_color);
        std::wstring detail=device.model;
        if(!device.phone_id.empty())detail+=L"  ·  "+device.phone_id.substr(0,std::min<size_t>(8,device.phone_id.size()));
        RECT detail_rect{text_left,card.top+DpiScale(state->window,28),card.right-DpiScale(state->window,12),card.bottom-DpiScale(state->window,3)};
        DrawTextW(item->hDC,detail.c_str(),-1,&detail_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        HBRUSH dot=CreateSolidBrush(status);HGDIOBJ old_dot_brush=SelectObject(item->hDC,dot);HGDIOBJ old_dot_pen=SelectObject(item->hDC,GetStockObject(NULL_PEN));
        const int dot_size=DpiScale(state->window,7);const int dot_top=card.top+DpiScale(state->window,13);
        Ellipse(item->hDC,card.right-DpiScale(state->window,48),dot_top,
                card.right-DpiScale(state->window,48)+dot_size,dot_top+dot_size);
        SelectObject(item->hDC,old_dot_pen);SelectObject(item->hDC,old_dot_brush);DeleteObject(dot);
        SetTextColor(item->hDC,status);
        RECT status_rect{card.right-DpiScale(state->window,36),card.top,card.right-DpiScale(state->window,8),card.top+DpiScale(state->window,32)};
        DrawTextW(item->hDC,online?L"在线":(device.active?L"离线":L"撤销"),-1,&status_rect,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        return;
    }
    HBRUSH fill = CreateSolidBrush(selected ? state->palette.selection
                                            : state->palette.list_surface);
    FillRect(item->hDC, &item->rcItem, fill);
    DeleteObject(fill);
    if(IsSettingsProjectTypes(*state)&&data_index<state->types_data.size()){
        const auto& type=state->types_data[data_index];
        const bool is_default=type.id==state->default_project_type_id;
        SelectObject(item->hDC,state->font);
        const int badge_width=is_default?DpiScale(state->window,48):0;
        RECT name_rect{rect.left,rect.top+DpiScale(state->window,4),
                       rect.right-badge_width,rect.top+DpiScale(state->window,27)};
        DrawTextW(item->hDC,type.name.c_str(),-1,&name_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        if(is_default){
            RECT badge{rect.right-DpiScale(state->window,44),
                       rect.top+DpiScale(state->window,7),
                       rect.right,rect.top+DpiScale(state->window,25)};
            const COLORREF badge_fill=state->light_theme?RGB(226,238,253):RGB(24,43,68);
            DrawSmoothRoundedRect(item->hDC,badge,
                static_cast<float>(DpiScale(state->window,5)),
                badge_fill,state->accent_color,1.0f);
            SelectObject(item->hDC,state->project_type_badge_font
                ?state->project_type_badge_font:state->small_font);
            SetTextColor(item->hDC,state->accent_color);
            DrawTextW(item->hDC,L"默认",-1,&badge,
                      DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
        SelectObject(item->hDC,state->small_font);SetTextColor(item->hDC,state->muted_color);
        RECT command_rect{rect.left,rect.top+DpiScale(state->window,26),rect.right,rect.bottom-DpiScale(state->window,3)};
        DrawTextW(item->hDC,type.default_command.c_str(),-1,&command_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        return;
    }
    SelectObject(item->hDC, state->font);
    DrawTextW(item->hDC, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void Layout(State& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int width = client.right;
    const int height = client.bottom;
    const auto S=[&](int value){return DpiScale(state.window,value);};
    const int panel_width = state.mode == ManagementMode::Settings
        ? std::min(S(980), std::max(S(760), width - S(80)))
        : std::min(S(760), std::max(S(600), width - S(100)));
    const int panel_height = state.mode == ManagementMode::Settings
        ? std::min(S(680), std::max(S(520), height - S(64)))
        : state.mode == ManagementMode::ProjectTypes ? S(590) : S(520);
    const int left = (width - panel_width) / 2;
    const int top = std::max(S(24), (height - panel_height) / 2);
    state.panel = {left, top, left + panel_width, top + panel_height};
    const int x = left + S(34);
    const int right = left + panel_width - S(34);
    const int button_y = state.panel.bottom - S(52);

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
        state.content_rect = {left + sidebar_width + S(1),
                              top + S(kSettingsHeaderHeight) + S(1),
                              state.panel.right - S(1), state.panel.bottom - S(1)};
        if (state.content) {
            const int content_width = state.content_rect.right - state.content_rect.left;
            const int content_height = state.content_rect.bottom - state.content_rect.top;
            MoveWindow(state.content, state.content_rect.left, state.content_rect.top,
                       content_width, content_height, TRUE);
            SetWindowRgn(state.content, nullptr, TRUE);
            LayoutSettingsContent(state);
        }
    } else if (state.mode == ManagementMode::Devices ||
               (state.mode==ManagementMode::Settings&&
                state.settings_page==SettingsPage::Devices)) {
        const int list_width=S(280);
        if (state.list) MoveWindow(state.list, x, top + S(104), list_width, S(334), TRUE);
        button(state.unbind, right - S(126), button_y, S(126), S(36));
        button(state.title_close,state.panel.right-S(48),top+S(5),S(32),S(32));
    } else if (state.mode == ManagementMode::ProjectTypes) {
        if (state.list) MoveWindow(state.list, x, top + S(120), right - x, S(300), TRUE);
        button(state.add, x, button_y, S(108), S(36));
        button(state.save, x + S(118), button_y, S(118), S(36));
        button(state.remove, x + S(246), button_y, S(108), S(36));
        button(state.close, right - S(118), button_y, S(118), S(36));
    } else {
        if (state.list) MoveWindow(state.list, x, top + S(120), right - x, S(215), TRUE);
        place(state.name, {x, top + S(362), right, top + S(398)});
        if (state.command) place(state.command, {x, top + S(436), right, top + S(472)});
        const int y = button_y;
        button(state.add, x, y, S(108), S(36));
        button(state.save, x + S(118), y, S(108), S(36));
        button(state.remove, x + S(236), y, S(108), S(36));
        if (state.select) button(state.select, right - S(236), y, S(108), S(36));
        button(state.close, right - S(118), y, S(118), S(36));
    }
    SetModalSurfaceRegion(state.window, state.panel, S(14));
    InvalidateRect(state.window, nullptr, FALSE);
}

void Paint(State& state, HDC dc) {
    RECT client{};
    GetClientRect(state.window, &client);
    // ModalWorkspaceScope owns the translucent scrim.  Painting a solid
    // backdrop here would make the modal opaque and hide the live workspace.
    (void)client;
    const auto S=[&](int value){return DpiScale(state.window,value);};
    HBRUSH panel_brush = CreateSolidBrush(state.panel_color);
    const unsigned edge_alpha = state.light_theme ? 118u : 150u;
    const auto blend_channel = [edge_alpha](BYTE base, BYTE edge) -> BYTE {
        return static_cast<BYTE>(
            (static_cast<unsigned>(base) * (255u - edge_alpha) +
             static_cast<unsigned>(edge) * edge_alpha) / 255u);
    };
    const COLORREF subtle_edge = RGB(
        blend_channel(GetRValue(state.panel_color), GetRValue(state.panel_edge)),
        blend_channel(GetGValue(state.panel_color), GetGValue(state.panel_edge)),
        blend_channel(GetBValue(state.panel_color), GetBValue(state.panel_edge)));
    HPEN panel_pen = CreatePen(PS_SOLID, 1, subtle_edge);
    HGDIOBJ old_brush = SelectObject(dc, panel_brush);
    HGDIOBJ old_pen = SelectObject(dc, panel_pen);
    RoundRect(dc, state.panel.left, state.panel.top, state.panel.right, state.panel.bottom, S(10), S(10));
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(panel_brush);
    DeleteObject(panel_pen);

    if (state.mode == ManagementMode::Settings) {
        const int header_bottom = state.panel.top + S(kSettingsHeaderHeight);
        const int saved = SaveDC(dc);
        HRGN panel_clip = CreateRoundRectRgn(
            state.panel.left + 1, state.panel.top + 1,
            state.panel.right, state.panel.bottom, S(10), S(10));
        SelectClipRgn(dc, panel_clip);
        DeleteObject(panel_clip);
        HBRUSH header_brush = CreateSolidBrush(SettingsHeaderColor(state));
        RECT header{state.panel.left + 1, state.panel.top + 1,
                    state.panel.right - 1, header_bottom};
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
    const wchar_t* title = L"管理";
    std::wstring dynamic_title;
    const wchar_t* subtitle = L"保存后立即写入本机 SQLite";
    if (state.mode == ManagementMode::Settings) { title = L"设置"; subtitle = L""; }
    else if (state.mode == ManagementMode::Groups) { title = L"分组管理"; subtitle = L"选择当前分组，或添加、重命名和删除空分组"; }
    else if (state.mode == ManagementMode::ProjectTypes) { title = L"项目类型"; subtitle = L"Codex / Claude 为预置类型，所有类型都可编辑"; }
    else if (state.mode == ManagementMode::Devices) {
        dynamic_title=L"已绑定设备  "+std::to_wstring(state.devices.size());
        title=dynamic_title.c_str();
        subtitle = L"解绑后该手机的令牌和实时连接立即失效";
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, state.text_color);
    SelectObject(dc, state.settings_title_font ? state.settings_title_font : state.title_font);
    const int title_right = state.mode == ManagementMode::Settings
        ? state.panel.right - S(86) : state.panel.right - S(34);
    RECT title_rect = state.mode == ManagementMode::Settings
        ? RECT{state.panel.left + S(24), state.panel.top,
               title_right, state.panel.top + S(kSettingsHeaderHeight)}
        : RECT{x, state.panel.top + S(24), title_right, state.panel.top + S(54)};
    DrawTextW(dc, title, -1, &title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, state.muted_color);
    SelectObject(dc, state.small_font);
    if (state.mode != ManagementMode::Settings) {
        RECT sub_rect{x, state.panel.top + S(57), title_right, state.panel.top + S(82)};
        DrawTextW(dc, subtitle, -1, &sub_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    if (state.mode == ManagementMode::Settings) {
        const int divider_x = state.panel.left + S(214);
        HPEN divider = CreatePen(PS_SOLID, 1, state.panel_edge);
        HGDIOBJ old_divider = SelectObject(dc, divider);
        MoveToEx(dc, divider_x,
                 state.panel.top + S(kSettingsHeaderHeight) + S(1), nullptr);
        LineTo(dc, divider_x, state.panel.bottom - S(18));
        SelectObject(dc, old_divider);
        DeleteObject(divider);
        SetTextColor(dc, state.muted_color);
        SelectObject(dc, state.small_font);
        return;
    }

    auto label = [&](const wchar_t* text, int y) {
        SetTextColor(dc, state.palette.muted);
        SelectObject(dc, state.small_font);
        RECT rect{x, state.panel.top + S(y), state.panel.right - S(34), state.panel.top + S(y + 22)};
        DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    if (state.mode == ManagementMode::Groups) {
        label(L"分组列表", 88);
        label(L"分组名称", 346);
    } else if (state.mode == ManagementMode::ProjectTypes) {
        label(L"项目类型列表", 88);
        SetTextColor(dc, state.muted_color);
        SelectObject(dc, state.small_font);
        RECT hint{x, state.panel.top + S(436), state.panel.right - S(34),
                  state.panel.top + S(468)};
        DrawTextW(dc, L"新增和修改会打开独立模态框；Codex 和 Claude 虽为预置类型，也可手工调整，但不能删除。", -1,
                  &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        label(L"已绑定手机", 76);
        const int divider_x=x+S(300);
        HPEN divider=CreatePen(PS_SOLID,1,state.panel_edge);
        HGDIOBJ old_device_pen=SelectObject(dc,divider);
        MoveToEx(dc,divider_x,state.panel.top+S(104),nullptr);
        LineTo(dc,divider_x,state.panel.bottom-S(72));
        SelectObject(dc,old_device_pen);DeleteObject(divider);
        size_t index{};
        const bool has_device=SelectedIndex(state.list,index)&&index<state.devices.size();
        const int detail_x=divider_x+S(28);
        const int detail_right=state.panel.right-S(34);
        if(!has_device){
            SelectObject(dc,state.font);SetTextColor(dc,state.muted_color);
            RECT empty{detail_x,state.panel.top+S(180),detail_right,state.panel.top+S(260)};
            DrawTextW(dc,L"选择一台设备查看授权信息",-1,&empty,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }else{
            const auto& device=state.devices[index];
            SelectObject(dc,state.title_font);SetTextColor(dc,state.text_color);
            RECT name{detail_x,state.panel.top+S(112),detail_right,state.panel.top+S(144)};
            DrawTextW(dc,device.name.c_str(),-1,&name,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            SelectObject(dc,state.small_font);
            auto detail_row=[&](const wchar_t* key,const std::wstring& value,int y){
                SetTextColor(dc,state.muted_color);
                RECT key_rect{detail_x,state.panel.top+S(y),detail_x+S(92),state.panel.top+S(y+28)};
                DrawTextW(dc,key,-1,&key_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                SetTextColor(dc,state.text_color);
                RECT value_rect{detail_x+S(96),state.panel.top+S(y),detail_right,state.panel.top+S(y+28)};
                DrawTextW(dc,value.c_str(),-1,&value_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            };
            detail_row(L"设备型号",device.model,158);
            detail_row(L"设备 ID",device.phone_id,196);
            detail_row(L"绑定时间",FormatDeviceTime(device.paired_at),234);
            detail_row(L"最近在线",FormatDeviceTime(device.last_seen),272);
            detail_row(L"授权状态",device.active?L"已授权":L"已解绑",310);
        }
    }
}

int SettingsContentExtent(SettingsPage page) {
    switch (page) {
    case SettingsPage::Appearance: return 430;
    case SettingsPage::Connection: return 635;
    case SettingsPage::ProjectTypes: return 590;
    case SettingsPage::Groups: return 590;
    case SettingsPage::Devices: return 520;
    case SettingsPage::About: return 558;
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
    if (state.content_layout_active) return;
    state.content_layout_active = true;
    RECT client{};
    GetClientRect(state.content, &client);
    const auto S=[&](int value){return DpiScale(state.window,value);};
    int height = std::max(1L, client.bottom - client.top);
    state.content_extent = S(SettingsContentExtent(state.settings_page));
    state.content_scroll = std::clamp(state.content_scroll, 0,
        std::max(0, state.content_extent - height));
    const int width = std::max(1L, client.right - client.left);

    const int x = S(20);
    const int right = std::max(x + S(160), width - S(20));
    const int available = right - x;
    const int offset = state.content_scroll;
    auto field = [&](HWND control, int left, int y, int h = 40,
                     int field_right = -1) {
        if (!control) return;
        if (field_right < 0) field_right = right;
        LayoutStyledInput(control,
                          RECT{left, S(y) - offset, field_right,
                               S(y + h) - offset}, S(2), S(8));
    };
    auto button = [&](HWND control, int bx, int y, int w, int h = 38) {
        if (!control) return;
        // Resize without presenting a stock square BUTTON frame. Apply the
        // region first, then publish one owner-drawn rounded frame.
        SetWindowPos(control, nullptr, bx, S(y) - offset, w, S(h),
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                         SWP_NOREDRAW | SWP_NOSENDCHANGING);
        ApplyRoundedRegion(control, w, S(h), S(9));
        RedrawWindow(control, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOERASE);
    };
    auto list = [&](HWND control, int y, int h) {
        if (!control) return;
        SetWindowPos(control, nullptr, x, S(y) - offset, available, S(h),
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                         SWP_NOREDRAW | SWP_NOSENDCHANGING);
        ApplyRoundedRegion(control, available, S(h), S(9));
        RedrawWindow(control, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    };

    if (state.settings_page == SettingsPage::Appearance) {
        const int right_inset = S(18);
        const int right_edge = right - right_inset;
        const int selector_width = S(300);
        const int selector_inset = S(2);
        const int selector_left = right_edge - selector_width;
        const int selector_inner_width = selector_width - selector_inset * 2;
        const int left_width = selector_inner_width / 2;
        const int right_width = selector_inner_width - left_width;
        auto segment = [&](HWND control, int left, int y, int width) {
            if (!control) return;
            SetWindowPos(control, nullptr, left, S(y + 2) - offset,
                         width, S(32),
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                             SWP_NOREDRAW | SWP_NOSENDCHANGING);
            SetWindowRgn(control, nullptr, TRUE);
            RedrawWindow(control, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_NOERASE);
        };
        auto segmented_pair = [&](HWND left_control, HWND right_control, int y) {
            segment(left_control, selector_left + selector_inset, y, left_width);
            segment(right_control,
                    selector_left + selector_inset + left_width, y, right_width);
        };
        auto segmented_triplet = [&](HWND first_control, HWND second_control,
                                     HWND third_control, int y) {
            const int first_width = selector_inner_width / 3;
            const int second_width = selector_inner_width / 3;
            const int third_width = selector_inner_width - first_width - second_width;
            const int first_left = selector_left + selector_inset;
            segment(first_control, first_left, y, first_width);
            segment(second_control, first_left + first_width, y, second_width);
            segment(third_control, first_left + first_width + second_width,
                    y, third_width);
        };
        segmented_pair(state.theme, state.theme_light, 88);
        segmented_triplet(state.taskbar_style_progress,
                          state.taskbar_style_overlay,
                          state.taskbar_style_icon_dots, 200);
        segmented_pair(state.taskbar_scope_current,
                       state.taskbar_scope_all, 256);

        if (state.taskbar_working) {
            const int toggle_width = S(50);
            const int toggle_height = S(28);
            SetWindowPos(state.taskbar_working, nullptr,
                         right_edge - toggle_width, S(148) - offset,
                         toggle_width, toggle_height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                             SWP_NOREDRAW | SWP_NOSENDCHANGING);
            SetWindowRgn(state.taskbar_working, nullptr, TRUE);
            RedrawWindow(state.taskbar_working, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_NOERASE);
        }
        if (state.keep_awake) {
            const int toggle_width = S(50);
            const int toggle_height = S(28);
            SetWindowPos(state.keep_awake, nullptr,
                         right_edge - toggle_width, S(312) - offset,
                         toggle_width, toggle_height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                             SWP_NOREDRAW | SWP_NOSENDCHANGING);
            SetWindowRgn(state.keep_awake, nullptr, TRUE);
            RedrawWindow(state.keep_awake, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_NOERASE);
        }
    } else if (state.settings_page == SettingsPage::Connection) {
        const int label_width = S(120);
        const int field_x = x + label_width;
        field(state.computer_name, field_x, 104);
        field(state.listen, field_x, 158);
        field(state.port, field_x, 212);
        field(state.local, field_x, 266);
        field(state.remote, field_x, 320);
        const int browse_width = S(86);
        const int browse_gap = S(8);
        field(state.mobile_workspace_root, field_x, 374, 40,
              right - browse_width - browse_gap);
        button(state.mobile_workspace_browse, right - browse_width, 375,
               browse_width, 38);
        button(state.mobile_access, field_x, 429, S(112), 36);
        button(state.firewall, x, 484, S(158));
        button(state.save, right - S(174), 484, S(174));
    } else if (state.settings_page == SettingsPage::ProjectTypes) {
        list(state.list, 126, 330);
        button(state.add, x, 492, S(122));
        button(state.save, x + S(134), 492, S(118));
        button(state.remove, x + S(264), 492, S(104));
    } else if(state.settings_page==SettingsPage::Groups) {
        list(state.list, 126, 330);
        button(state.add, x, 492, S(122));
        button(state.save, x + S(134), 492, S(118));
        button(state.remove, x + S(264), 492, S(104));
    } else if (state.settings_page == SettingsPage::About) {
        button(state.check_update, x, 498, S(158));
    } else if (state.settings_page == SettingsPage::Devices) {
        const int list_width=std::min(S(306),available/2);
        if(state.list){
            SetWindowPos(state.list,nullptr,x,S(126)-offset,list_width,S(350),
                         SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_NOREDRAW);
            ApplyRoundedRegion(state.list,list_width,S(350),S(8));
        }
        button(state.select,right-S(122),80,S(122),36);
        button(state.unbind,right-S(122),444,S(122),36);
        const int detail_x=x+list_width+S(26);
        const int bind_width=S(158);
        const int bind_left=detail_x+
            std::max(0,(right-detail_x-bind_width)/2);
        button(state.bind_empty,bind_left,270,bind_width,36);
    }
    state.list_scrollbar.Layout();
    state.content_scrollbar.Layout();
    InvalidateRect(state.content, nullptr, FALSE);
    state.content_layout_active = false;
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
    const auto S=[&](int value){return DpiScale(state.window,value);};
    const int width = client.right;
    const int x = S(20);
    const int right = std::max(x + S(160), width - S(20));

    const wchar_t* title = L"外观设置";
    const wchar_t* subtitle = L"";
    if (state.settings_page == SettingsPage::Connection) {
        title = L"本机与网络";
        subtitle = L"配置手机访问服务、访问地址和手机工作目录。";
    } else if (state.settings_page == SettingsPage::ProjectTypes) {
        title = L"项目类型";
        subtitle = L"Codex 和 Claude 为预置类型；所有类型都可在这里维护名称和启动方式。";
    } else if (state.settings_page == SettingsPage::Groups) {
        title = L"分组管理";
        subtitle = L"用一个名称输入框新建或修改分组；默认分组始终保留。";
    } else if(state.settings_page==SettingsPage::Devices){
        title=L"设备管理";
        subtitle=L"查看已授权手机、最近连接状态，或撤销设备访问权限。";
    } else if(state.settings_page==SettingsPage::About){
        title=L"关于";
        subtitle=L"查看软件版本和当前交互终端运行环境。";
    }

    SelectObject(dc, state.settings_content_font ? state.settings_content_font : state.title_font);
    SetTextColor(dc, state.text_color);
    RECT title_rect{x, S(20), right, S(52)};
    DrawTextW(dc, title, -1, &title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (subtitle[0] != L'\0') {
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT subtitle_rect{x, S(54), right, S(84)};
        DrawTextW(dc, subtitle, -1, &subtitle_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    auto label = [&](const wchar_t* text, int y) {
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.palette.muted);
        RECT rect{x, S(y), right, S(y + 24)};
        DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    auto card = [&](RECT rect) {
        HBRUSH brush = CreateSolidBrush(state.panel_color);
        HPEN pen = CreatePen(PS_SOLID, 1, state.panel_edge);
        HGDIOBJ old_brush = SelectObject(dc, brush);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, S(12), S(12));
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    };

    if (state.settings_page == SettingsPage::Appearance) {
        card({x, S(64), right, S(366)});
        const int selector_right = right - S(18);
        const int selector_left = selector_right - S(300);
        const COLORREF selector_fill = state.light_theme
            ? RGB(238, 242, 247) : RGB(25, 29, 37);
        const COLORREF selector_edge = state.light_theme
            ? RGB(215, 221, 230) : RGB(45, 51, 62);
        auto selector = [&](int y) {
            const RECT bounds{selector_left, S(y),
                              selector_right, S(y + 36)};
            DrawSmoothRoundedRect(dc, bounds, static_cast<float>(S(8)),
                                  selector_fill, selector_edge, 1.0f);
        };
        selector(88);
        selector(200);
        selector(256);
        SelectObject(dc, state.settings_content_font ? state.settings_content_font : state.font);
        SetTextColor(dc, state.text_color);
        auto row_label = [&](const wchar_t* text, int y) {
            RECT rect{x + S(20), S(y), right - S(346), S(y + 36)};
            DrawTextW(dc, text, -1, &rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        };
        row_label(L"应用主题", 88);
        row_label(L"任务栏缓冲动画", 144);
        row_label(L"缓冲样式", 200);
        row_label(L"作用范围", 256);
        row_label(L"任务运行中保持唤醒", 312);
        HPEN divider = CreatePen(PS_SOLID, 1, state.panel_edge);
        HGDIOBJ old_pen = SelectObject(dc, divider);
        for (int y : {136, 192, 248, 304}) {
            MoveToEx(dc, x + S(20), S(y), nullptr);
            LineTo(dc, right - S(20), S(y));
        }
        SelectObject(dc, old_pen);
        DeleteObject(divider);
    } else if (state.settings_page == SettingsPage::Connection) {
        auto row_label = [&](const wchar_t* text, int y) {
            SelectObject(dc, state.settings_content_font ?
                                 state.settings_content_font : state.font);
            SetTextColor(dc, state.text_color);
            RECT rect{x, S(y), x + S(120), S(y + 40)};
            DrawTextW(dc, text, -1, &rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        };
        row_label(L"本机名称", 104);
        row_label(L"监听地址", 158);
        row_label(L"HTTPS/WSS 端口", 212);
        row_label(L"内网访问地址", 266);
        row_label(L"公网/Relay 地址", 320);
        row_label(L"手机工作目录", 374);
        row_label(L"手机访问服务", 428);
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT hint{x, S(538), right, S(610)};
        DrawTextW(dc, L"开启后才会监听 HTTPS/WSS 端口；关闭会断开手机连接并清理临时授权。手机项目和附件分别保存在工作目录下。", -1,
                  &hint, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else if (state.settings_page == SettingsPage::ProjectTypes) {
        label(L"项目类型列表", 96);
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT hint{x, S(548), right, S(578)};
        DrawTextW(dc, L"新增和修改会打开独立模态框；Codex 和 Claude 虽为预置类型，也可手工调整，但不能删除。", -1,
                  &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else if(state.settings_page==SettingsPage::Groups) {
        label(L"分组列表", 96);
        SelectObject(dc, state.settings_small_font ? state.settings_small_font : state.small_font);
        SetTextColor(dc, state.muted_color);
        RECT hint{x, S(548), right, S(578)};
        DrawTextW(dc, L"添加和修改名称会打开独立输入模态层；非空分组不能删除。", -1,
                  &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }else if(state.settings_page==SettingsPage::About){
        card({x,S(104),right,S(312)});
        SelectObject(dc,state.title_font?state.title_font:state.settings_content_font);
        SetTextColor(dc,state.text_color);
        RECT app_name{x+S(24),S(118),right-S(24),S(154)};
        DrawTextW(dc,L"AiCli",-1,&app_name,
                  DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        SelectObject(dc,state.settings_small_font?state.settings_small_font:state.small_font);
        SetTextColor(dc,state.muted_color);
        RECT app_detail{x+S(24),S(150),right-S(24),S(176)};
        DrawTextW(dc,L"AiCli 桌面工作区",-1,&app_detail,
                  DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        HPEN divider=CreatePen(PS_SOLID,1,state.panel_edge);
        HGDIOBJ old_divider=SelectObject(dc,divider);
        MoveToEx(dc,x+S(24),S(184),nullptr);
        LineTo(dc,right-S(24),S(184));
        SelectObject(dc,old_divider);
        DeleteObject(divider);

        const auto runtime=aicli::app::PowerShellRuntimeInfo();
        const std::wstring terminal=runtime.IsPowerShell7()
            ?runtime.DisplayName()
            :runtime.DisplayName()+L"（兼容模式）";
        auto row=[&](const wchar_t* key,const std::wstring& value,int y){
            SelectObject(dc,state.settings_small_font?state.settings_small_font:state.small_font);
            SetTextColor(dc,state.muted_color);
            RECT key_rect{x+S(24),S(y),x+S(132),S(y+34)};
            DrawTextW(dc,key,-1,&key_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            SelectObject(dc,state.settings_content_font?state.settings_content_font:state.font);
            SetTextColor(dc,state.text_color);
            RECT value_rect{x+S(136),S(y),right-S(24),S(y+34)};
            DrawTextW(dc,value.c_str(),-1,&value_rect,
                      DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        };
        row(L"软件名称",L"AiCli",194);
        row(L"版本号",AICLI_VERSION,232);
        row(L"终端环境",terminal,270);

        SelectObject(dc,state.settings_content_font?state.settings_content_font:state.font);
        SetTextColor(dc,state.text_color);
        RECT detection_title{x,S(326),right,S(354)};
        DrawTextW(dc,L"PowerShell 检测说明",-1,&detection_title,
                  DT_LEFT|DT_VCENTER|DT_SINGLELINE);

        SelectObject(dc,state.settings_small_font?state.settings_small_font:state.small_font);
        SetTextColor(dc,state.muted_color);
        std::wstring detection_note=runtime.IsPowerShell7()
            ?L"已自动使用检测到的 PowerShell 7。"
            :L"当前未检测到 PowerShell 7，正在使用 Windows PowerShell 5.1 兼容模式。";
        detection_note+=
            L" AiCli 支持常规安装和 Microsoft Store 版；如未自动识别到，请把 pwsh.exe 所在文件夹加入 PATH。"
            L"安装、升级或修改 PATH 后，请完全退出并重启 AiCli。";
        RECT hint{x,S(358),right,S(454)};
        DrawTextW(dc,detection_note.c_str(),-1,&hint,
                  DT_LEFT|DT_TOP|DT_WORDBREAK);
        RECT update_note{x,S(456),right,S(490)};
        DrawTextW(dc,L"启动后自动检查更新；安装前会提示确认。",-1,&update_note,
                  DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    }else{
        SelectObject(dc,state.settings_small_font?state.settings_small_font:state.small_font);
        SetTextColor(dc,state.palette.muted);
        const std::wstring list_title=L"已授权设备（"+std::to_wstring(state.devices.size())+L"）";
        RECT list_title_rect{x,S(96),right,S(120)};
        DrawTextW(dc,list_title.c_str(),-1,&list_title_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        const int list_width=std::min(S(306),(right-x)/2);
        const int detail_x=x+list_width+S(26);
        size_t index{};
        const bool has_device=SelectedIndex(state.list,index)&&index<state.devices.size();
        if(!has_device){
            SelectObject(dc,state.settings_icon_font?state.settings_icon_font:state.font);
            SetTextColor(dc,state.muted_color);
            RECT empty_icon{detail_x,S(166),right,S(218)};
            DrawTextW(dc,L"\xE8EA",-1,&empty_icon,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SelectObject(dc,state.settings_content_font?state.settings_content_font:state.font);
            SetTextColor(dc,state.muted_color);
            RECT empty{detail_x,S(216),right,S(258)};
            DrawTextW(dc,state.devices.empty()?L"当前没有已绑定手机":L"选择一台设备查看详细信息",
                      -1,&empty,DT_CENTER|DT_VCENTER|DT_WORDBREAK);
        }else{
            const auto& device=state.devices[index];
            const bool online=state.agent&&state.agent->IsDeviceOnline(device.phone_id);
            SelectObject(dc,state.settings_content_font?state.settings_content_font:state.font);
            SetTextColor(dc,state.text_color);
            RECT device_name{detail_x,S(126),right-S(82),S(158)};
            DrawTextW(dc,device.name.c_str(),-1,&device_name,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            const COLORREF badge_text=online?state.palette.success:state.palette.muted;
            const COLORREF badge_fill=online
                ?(state.light_theme?RGB(229,247,239):RGB(24,54,45))
                :(state.light_theme?RGB(239,242,246):RGB(31,36,44));
            RECT badge{right-S(70),S(128),right,S(154)};
            DrawSmoothRoundedRect(dc,badge,static_cast<float>(S(7)),badge_fill,badge_fill,0.0f);
            SelectObject(dc,state.settings_small_font?state.settings_small_font:state.small_font);
            SetTextColor(dc,badge_text);
            DrawTextW(dc,online?L"●  在线":L"○  离线",-1,&badge,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SetTextColor(dc,state.muted_color);
            RECT section{detail_x,S(166),right,S(188)};
            DrawTextW(dc,L"设备信息",-1,&section,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            SelectObject(dc,state.settings_small_font?state.settings_small_font:state.small_font);
            auto row=[&](const wchar_t* key,const std::wstring& value,int y){
                SetTextColor(dc,state.muted_color);
                RECT key_rect{detail_x,S(y),detail_x+S(88),S(y+28)};
                DrawTextW(dc,key,-1,&key_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                SetTextColor(dc,state.text_color);
                RECT value_rect{detail_x+S(92),S(y),right,S(y+28)};
                DrawTextW(dc,value.c_str(),-1,&value_rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            };
            row(L"设备型号",device.model,194);
            row(L"设备 ID",device.phone_id,230);
            row(L"绑定时间",FormatDeviceTime(device.paired_at),266);
            row(L"最近在线",FormatDeviceTime(device.last_seen),302);
            row(L"授权状态",device.active?L"已授权":L"已解绑",338);
            HPEN divider=CreatePen(PS_SOLID,1,state.panel_edge);HGDIOBJ old_divider=SelectObject(dc,divider);
            MoveToEx(dc,detail_x,S(382),nullptr);LineTo(dc,right,S(382));
            SelectObject(dc,old_divider);DeleteObject(divider);
            SelectObject(dc,state.settings_content_font?state.settings_content_font:state.font);
            SetTextColor(dc,state.text_color);
            RECT danger_title{detail_x,S(390),right,S(414)};
            DrawTextW(dc,L"撤销设备访问权限",-1,&danger_title,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            SelectObject(dc,state.settings_small_font?state.settings_small_font:state.small_font);
            SetTextColor(dc,state.muted_color);
            RECT danger_text{detail_x,S(416),right-S(138),S(476)};
            DrawTextW(dc,L"解绑后，该手机必须重新扫码并在电脑上确认。",-1,&danger_text,DT_LEFT|DT_TOP|DT_WORDBREAK);
        }
    }
    SetViewportOrgEx(dc, previous_origin.x, previous_origin.y, nullptr);
}

void UpdateSettingsPage(State& state, SettingsPage page) {
    if (state.mode != ManagementMode::Settings) return;
    const SettingsPage previous_page = state.settings_page;
    state.settings_page = page;
    state.content_scroll = 0;
    const bool appearance = page == SettingsPage::Appearance;
    const bool connection = page == SettingsPage::Connection;
    const bool project_types = page == SettingsPage::ProjectTypes;
    const bool groups = page == SettingsPage::Groups;
    const bool devices = page == SettingsPage::Devices;
    const bool has_active_device = std::any_of(
        state.devices.begin(), state.devices.end(),
        [](const DeviceRecord& device) { return device.active; });
    state.device_hover_row=-1;
    if(state.list)SendMessageW(state.list,LB_SETITEMHEIGHT,0,
        DpiScale(state.window,devices?64:(project_types?52:44)));
    auto visible = [](HWND control, bool show) {
        if (control) ShowWindow(control, show ? SW_SHOW : SW_HIDE);
    };
    visible(state.theme, appearance);
    visible(state.theme_light, appearance);
    visible(state.taskbar_working, appearance);
    visible(state.taskbar_style_progress, appearance);
    visible(state.taskbar_style_overlay, appearance);
    visible(state.taskbar_style_icon_dots, appearance);
    visible(state.taskbar_scope_current, appearance);
    visible(state.taskbar_scope_all, appearance);
    visible(state.keep_awake, appearance);
    visible(state.check_update, page == SettingsPage::About);
    // The project/group/device list overlaps the connection form. Hide it
    // before revealing connection fields; otherwise each EDIT receives its
    // first WM_PAINT while still clipped by the higher-z-order list, and the
    // exposed text can remain only half painted after the list disappears.
    if (connection) visible(state.list, false);
    visible(state.computer_name, connection);
    visible(state.listen, connection);
    visible(state.port, connection);
    visible(state.local, connection);
    visible(state.remote, connection);
    visible(state.mobile_workspace_root, connection);
    visible(state.mobile_workspace_browse, connection);
    visible(state.mobile_access, connection);
    visible(state.firewall, connection);
    visible(state.save, connection || project_types || groups);
    if (!connection) visible(state.list, project_types || groups || devices);
    visible(state.name, false);
    visible(state.command, false);
    visible(state.add, project_types || groups);
    visible(state.remove, project_types || groups);
    visible(state.select,devices);
    visible(state.unbind,devices&&has_active_device);
    visible(state.bind_empty,devices&&state.devices.empty());
    if (state.save) EnableWindow(state.save, TRUE);
    if (state.remove) EnableWindow(state.remove, TRUE);
    if (project_types) {
        SetWindowTextW(state.add, L"新增类型");
        SetWindowTextW(state.save, L"修改类型");
        SetWindowTextW(state.remove, L"删除");
        RefreshList(state);
        ReadSelection(state);
    } else if (connection) {
        SetWindowTextW(state.save, L"保存设置");
    } else if (groups) {
        SetWindowTextW(state.add, L"添加分组");
        SetWindowTextW(state.save, L"修改名称");
        SetWindowTextW(state.remove, L"删除");
        RefreshList(state);
        ReadSelection(state);
    }else if(devices){
        RefreshList(state);
        ReadSelection(state);
    }
    LayoutSettingsContent(state);
    if (connection) {
        for (HWND field : {state.computer_name, state.listen, state.port,
                           state.local, state.remote,
                           state.mobile_workspace_root}) {
            if (field && IsWindowVisible(field)) {
                RedrawWindow(field, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            }
        }
    }
    if (HWND previous = SettingsNavigationControl(state, previous_page))
        InvalidateRect(previous, nullptr, FALSE);
    if (HWND current = SettingsNavigationControl(state, page);
        current && current != SettingsNavigationControl(state, previous_page))
        InvalidateRect(current, nullptr, FALSE);
    InvalidateRect(state.content, nullptr, FALSE);
    state.list_scrollbar.Sync();
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
        state->content_scrollbar.NotifyActivity();
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
        {
            RECT client{};
            GetClientRect(h, &client);
            BufferedWindowPaint buffered(dc, client, paint.rcPaint);
            PaintSettingsContent(*state, buffered.dc());
        }
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
        const auto default_group = std::find_if(
            state.groups.begin(), state.groups.end(),
            [](const ProjectGroup& group) { return group.builtin; });
        const std::wstring default_group_name = default_group != state.groups.end()
            ? default_group->name : L"默认分组";
        for (const auto& value : state.groups) {
            const auto count = std::count_if(projects.begin(), projects.end(), [&](const Project& project) {
                const std::wstring group = project.group.empty()
                    ? default_group_name : project.group;
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
        state.default_project_type_id = state.store->ConfiguredDefaultProjectTypeId();
        for (const auto& value : state.types_data) {
            LRESULT row = SendMessageW(state.list, LB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(value.name.c_str()));
            if (row != LB_ERR) SendMessageW(state.list, LB_SETITEMDATA, row,
                                             static_cast<LPARAM>(&value - state.types_data.data()));
        }
    } else if (state.mode == ManagementMode::Devices ||
               (state.mode==ManagementMode::Settings&&
                state.settings_page==SettingsPage::Devices)) {
        state.devices = state.store->LoadDevices(true);
        state.device_online_states.clear();
        state.device_online_states.reserve(state.devices.size());
        for (const auto& value : state.devices) {
            state.device_online_states.push_back(
                state.agent && state.agent->IsDeviceOnline(value.phone_id));
            std::wstring text = value.name;
            LRESULT row = SendMessageW(state.list, LB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(text.c_str()));
            if (row != LB_ERR) SendMessageW(state.list, LB_SETITEMDATA, row,
                                             static_cast<LPARAM>(&value - state.devices.data()));
        }
    }
    if (SendMessageW(state.list, LB_GETCOUNT, 0, 0) > 0) SendMessageW(state.list, LB_SETCURSEL, 0, 0);
    if(state.mode==ManagementMode::Settings&&state.settings_page==SettingsPage::Devices){
        const bool has_devices=!state.devices.empty();
        const bool has_active_device=std::any_of(
            state.devices.begin(),state.devices.end(),
            [](const DeviceRecord& device){return device.active;});
        if(state.bind_empty)ShowWindow(state.bind_empty,has_devices?SW_HIDE:SW_SHOW);
        if(state.unbind)ShowWindow(state.unbind,has_active_device?SW_SHOW:SW_HIDE);
    }
    state.list_scrollbar.Sync();
}

void ReadSelection(State& state) {
    if (!state.list) return;
    LRESULT row = SendMessageW(state.list, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR) {
        if(state.mode==ManagementMode::Settings&&state.settings_page==SettingsPage::Devices&&state.unbind)
            EnableWindow(state.unbind,FALSE);
        return;
    }
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
    if(state.mode==ManagementMode::Settings&&state.settings_page==SettingsPage::Devices&&state.unbind)
        EnableWindow(state.unbind,index<state.devices.size()&&state.devices[index].active);
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

bool SetDefaultProjectTypeByName(DataStore& store,
                                 const std::wstring& name,
                                 std::wstring* error) {
    const auto types = store.LoadProjectTypes();
    const auto found = std::find_if(
        types.begin(), types.end(), [&](const ProjectType& type) {
            return _wcsicmp(type.name.c_str(), name.c_str()) == 0;
        });
    if (found == types.end()) {
        if (error) *error = L"新项目类型已保存，但无法将它设为默认";
        return false;
    }
    return store.SetDefaultProjectType(found->id, error);
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
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 1, 1, state.window, nullptr, GetModuleHandleW(nullptr), &state);
        state.content_scrollbar.Attach(
            state.content,
            [&state] {
                RECT client{};
                GetClientRect(state.content, &client);
                return OverlayScrollMetrics{
                    state.content_extent,
                    static_cast<int>(std::max(1L, client.bottom - client.top)),
                    state.content_scroll};
            },
            [&state](int position) { SetSettingsScroll(state, position); });
        state.content_scrollbar.SetTheme(state.palette, state.background_color);
        state.settings_appearance = MakeButton(state, L"外观", kSettingsAppearance);
        state.settings_connection = MakeButton(state, L"本机与网络", kSettingsConnection);
        state.settings_project_types = MakeButton(state, L"项目类型", kSettingsProjectTypes);
        state.settings_groups = MakeButton(state, L"分组管理", kSettingsGroups);
        state.settings_devices = MakeButton(state, L"设备管理", kSettingsDevices);
        state.settings_about = MakeButton(state, L"关于", kSettingsAbout);
        state.computer_name = MakeField(state, kComputerName, L"显示在手机端，例如：开发主机", state.content);
        state.listen = MakeField(state, kListen, L"0.0.0.0 表示所有网卡", state.content);
        state.port = MakeField(state, kPort, L"例如 4280", state.content);
        state.local = MakeField(state, kLocal, L"二维码优先使用，例如：https://192.168.1.10:4280", state.content);
        state.remote = MakeField(state, kRemote, L"可留空，例如：https://relay.example.com/aicli", state.content);
        state.mobile_workspace_root = MakeField(
            state, kMobileWorkspaceRoot, L"选择手机工作目录", state.content);
        state.mobile_workspace_browse = MakeButton(
            state, L"选择", kMobileWorkspaceBrowse, state.content);
        state.mobile_access_enabled = agent::IsMobileAccessEnabled(*state.store);
        state.mobile_access = MakeButton(
            state, state.mobile_access_enabled ? L"开启" : L"关闭",
            kMobileAccess, state.content);
        const auto theme = state.store->GetSetting(L"theme", L"dark");
        state.light_theme = theme == L"light";
        state.theme = MakeButton(state, L"夜间模式", kTheme, state.content);
        state.theme_light = MakeButton(state, L"白天模式", kThemeLight, state.content);
        state.taskbar_working = MakeButton(state,
            state.store->GetSetting(L"taskbar_working_enabled", L"1") == L"0"
                ? L"关闭" : L"开启",
            kTaskbarWorking, state.content);
        state.taskbar_style_progress = MakeButton(state, L"进度条", kTaskbarStyleProgress, state.content);
        state.taskbar_style_overlay = MakeButton(state, L"图标转圈", kTaskbarStyleOverlay, state.content);
        state.taskbar_style_icon_dots = MakeButton(
            state, L"图标三点", kTaskbarStyleIconDots, state.content);
        state.taskbar_scope_current = MakeButton(state, L"当前项目", kTaskbarScopeCurrent, state.content);
        state.taskbar_scope_all = MakeButton(state, L"所有项目", kTaskbarScopeAll, state.content);
        state.keep_awake = MakeButton(
            state,
            state.store->GetSetting(L"keep_awake_while_working", L"0") == L"0"
                ? L"关闭" : L"开启",
            kKeepAwake, state.content);
        state.save = MakeButton(state, L"保存设置", kSave, state.content);
        state.firewall = MakeButton(state, L"配置防火墙", kFirewall, state.content);
        for (HWND control : {state.computer_name, state.listen, state.port, state.local,
                              state.remote, state.mobile_workspace_root,
                              state.mobile_workspace_browse, state.mobile_access,
                              state.theme, state.theme_light,
                              state.taskbar_working, state.keep_awake,
                              state.taskbar_style_progress,
                              state.taskbar_style_overlay, state.taskbar_style_icon_dots,
                              state.taskbar_scope_current,
                              state.taskbar_scope_all, state.save,
                             state.firewall}) {
            if (control && state.settings_content_font)
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(state.settings_content_font), TRUE);
        }
        state.list = CreateWindowExW(0, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN |
                                         LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                                         LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                     0, 0, 1, 1, state.content,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
                                     GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state.list, WM_SETFONT,
                     reinterpret_cast<WPARAM>(state.settings_content_font ?
                                                   state.settings_content_font : state.font), TRUE);
        state.list_scrollbar.Attach(state.list,[&state]{
            RECT client{};GetClientRect(state.list,&client);
            const int item_height=std::max(1,static_cast<int>(SendMessageW(state.list,LB_GETITEMHEIGHT,0,0)));
            return OverlayScrollMetrics{
                static_cast<int>(SendMessageW(state.list,LB_GETCOUNT,0,0)),
                std::max(1,static_cast<int>(client.bottom-client.top)/item_height),
                static_cast<int>(SendMessageW(state.list,LB_GETTOPINDEX,0,0))};
        },[&state](int position){SendMessageW(state.list,LB_SETTOPINDEX,position,0);});
        state.list_scrollbar.SetTheme(state.palette,state.palette.list_surface);
        state.add = MakeButton(state, L"新增类型", kAdd, state.content);
        state.remove = MakeButton(state, L"删除", kDelete, state.content);
        state.select = MakeButton(state,L"绑定手机",kSelect,state.content);
        state.unbind = MakeButton(state,L"解绑设备",kUnbind,state.content);
        state.bind_empty=MakeButton(state,L"绑定第一台手机",kBindEmpty,state.content);
        state.check_update=MakeButton(state,L"检查更新",kCheckUpdate,state.content);
        state.title_close = MakeButton(state, L"\x00D7", kClose);
        for (HWND control : {state.computer_name, state.listen, state.port,
                              state.local, state.remote, state.mobile_workspace_root,
                              state.mobile_workspace_browse, state.mobile_access, state.theme,
                              state.theme_light, state.taskbar_working,
                              state.taskbar_style_progress, state.taskbar_style_overlay,
                              state.taskbar_style_icon_dots,
                              state.taskbar_scope_current, state.taskbar_scope_all,
                              state.keep_awake, state.save, state.firewall,
                             state.list, state.add, state.remove,state.select,state.unbind,
                             state.bind_empty, state.check_update}) {
            if (control) {
                SetWindowSubclass(control, SettingsWheelProc,
                                  kSettingsWheelSubclassId,
                                  reinterpret_cast<DWORD_PTR>(&state));
            }
        }
        UpdateSettingsPage(state, state.settings_page);
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
        state.title_close = MakeButton(state, L"\x00D7", kClose);
        RefreshList(state);
    }
    Layout(state);
    PostMessageW(state.window, kPopulateMessage, 0, 0);
}

void PopulateControls(State& state) {
    if (state.mode == ManagementMode::Settings) {
        SetField(state.computer_name, state.store->GetSetting(
            L"computer_name", aicli::text::LocalComputerName()));
        std::wstring listen_value = state.store->GetSetting(L"listen_address", L"0.0.0.0");
        std::wstring port_value = state.store->GetSetting(L"listen_port", L"4280");
        if (listen_value.empty()) listen_value = L"0.0.0.0";
        if (port_value.empty()) port_value = L"4280";
        SetField(state.local, state.store->GetSetting(L"local_address"));
        SetField(state.remote, state.store->GetSetting(L"remote_address"));
        SetField(state.mobile_workspace_root,
                 mobile_projects::WorkspaceDirectory(*state.store).wstring());
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
        if(state->mode==ManagementMode::Settings)
            SetTimer(h,kDevicePresenceTimerId,2000,nullptr);
        return 0;
    case WM_SIZE:
        Layout(*state);
        return 0;
    case kPopulateMessage:
        PopulateControls(*state);
        return 0;
    case WM_TIMER:
        if(wparam==kDevicePresenceTimerId){
            if(state->mode==ManagementMode::Settings&&
               state->settings_page==SettingsPage::Devices){
                if(state->device_online_states.size()!=state->devices.size()){
                    state->device_online_states.assign(state->devices.size(),0);
                }
                size_t selected_index{};
                const bool has_selection=SelectedIndex(state->list,selected_index);
                bool selected_status_changed=false;
                for(size_t index=0;index<state->devices.size();++index){
                    const bool online=state->agent&&
                        state->agent->IsDeviceOnline(state->devices[index].phone_id);
                    if(state->device_online_states[index]==static_cast<unsigned char>(online))
                        continue;
                    state->device_online_states[index]=static_cast<unsigned char>(online);
                    RECT row{};
                    if(state->list&&SendMessageW(state->list,LB_GETITEMRECT,
                            static_cast<WPARAM>(index),reinterpret_cast<LPARAM>(&row))!=LB_ERR)
                        InvalidateRect(state->list,&row,FALSE);
                    selected_status_changed=selected_status_changed||
                        (has_selection&&selected_index==index);
                }
                if(selected_status_changed&&state->content)
                    InvalidateRect(state->content,nullptr,FALSE);
            }
            return 0;
        }
        break;
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
            measure->itemHeight = DpiScale(h,
                (state->mode==ManagementMode::Devices||
                 (state->mode==ManagementMode::Settings&&state->settings_page==SettingsPage::Devices))?64:
                (IsSettingsProjectTypes(*state)?52:44));
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
            if(state->mode==ManagementMode::Devices)InvalidateRect(h,&state->panel,FALSE);
            if(state->mode==ManagementMode::Settings&&state->settings_page==SettingsPage::Devices)
                InvalidateRect(state->content,nullptr,FALSE);
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
            if (id == kCheckUpdate) {
                aicli::update::CheckForUpdates(state->window);
                return 0;
            }
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
            if(id==kSettingsDevices){
                UpdateSettingsPage(*state,SettingsPage::Devices);
                return 0;
            }
            if(id==kSettingsAbout){
                UpdateSettingsPage(*state,SettingsPage::About);
                return 0;
            }
            if(state->settings_page==SettingsPage::Devices&&
               (id==kSelect||id==kBindEmpty)){
                state->result.open_binding=true;
                DestroyWindow(h);
                return 0;
            }
            if(state->settings_page==SettingsPage::Devices&&id==kUnbind){
                size_t index{};
                if(!SelectedIndex(state->list,index)||index>=state->devices.size()){
                    ShowError(h,L"请选择要解绑的设备");
                }else if(!state->devices[index].active){
                    ShowError(h,L"该设备已经解绑");
                }else if(!state->agent->RevokeDevice(state->devices[index].phone_id)){
                    ShowError(h,L"解绑失败");
                }else{
                    RefreshList(*state);
                    ReadSelection(*state);
                    InvalidateRect(state->content,nullptr,FALSE);
                }
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
            if (id == kTaskbarWorking || id == kKeepAwake ||
                id == kTaskbarStyleProgress ||
                id == kTaskbarStyleOverlay || id == kTaskbarStyleIconDots ||
                id == kTaskbarScopeCurrent ||
                id == kTaskbarScopeAll) {
                if (id == kTaskbarWorking) {
                    const bool enabled = state->store->GetSetting(
                        L"taskbar_working_enabled", L"1") != L"0";
                    state->store->SetSetting(L"taskbar_working_enabled", enabled ? L"0" : L"1");
                    SetWindowTextW(state->taskbar_working,
                        enabled ? L"关闭" : L"开启");
                } else if (id == kKeepAwake) {
                    const bool enabled = state->store->GetSetting(
                        L"keep_awake_while_working", L"0") != L"0";
                    state->store->SetSetting(L"keep_awake_while_working",
                                             enabled ? L"0" : L"1");
                    SetWindowTextW(state->keep_awake,
                                   enabled ? L"关闭" : L"开启");
                } else if (id == kTaskbarStyleProgress ||
                           id == kTaskbarStyleOverlay ||
                           id == kTaskbarStyleIconDots) {
                    state->store->SetSetting(L"taskbar_working_style",
                        id == kTaskbarStyleOverlay ? L"overlay" :
                        id == kTaskbarStyleIconDots ? L"icon_dots" : L"progress");
                } else {
                    state->store->SetSetting(L"taskbar_working_scope",
                        id == kTaskbarScopeCurrent ? L"current" : L"all");
                }
                for (HWND control : {state->taskbar_working,
                                     state->keep_awake,
                                     state->taskbar_style_progress,
                                     state->taskbar_style_overlay,
                                     state->taskbar_style_icon_dots,
                                     state->taskbar_scope_current,
                                     state->taskbar_scope_all}) {
                    InvalidateRect(control, nullptr, FALSE);
                }
                if (state->appearance_changed) state->appearance_changed();
                return 0;
            }
            if (state->settings_page == SettingsPage::Connection && id == kMobileAccess) {
                state->mobile_access_enabled = !state->mobile_access_enabled;
                SetWindowTextW(state->mobile_access,
                               state->mobile_access_enabled ? L"开启" : L"关闭");
                InvalidateRect(state->mobile_access, nullptr, FALSE);
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
            if (state->settings_page == SettingsPage::Connection &&
                id == kMobileWorkspaceBrowse) {
                IFileDialog* dialog = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                               CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&dialog))) && dialog) {
                    DWORD options = 0;
                    dialog->GetOptions(&options);
                    dialog->SetOptions(options | FOS_PICKFOLDERS |
                                      FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                    if (SUCCEEDED(dialog->Show(h))) {
                        IShellItem* item = nullptr;
                        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                            PWSTR value = nullptr;
                            if (SUCCEEDED(item->GetDisplayName(
                                    SIGDN_FILESYSPATH, &value)) && value) {
                                SetField(state->mobile_workspace_root, value);
                                CoTaskMemFree(value);
                            }
                            item->Release();
                        }
                    }
                    dialog->Release();
                }
                return 0;
            }
            if (state->settings_page == SettingsPage::Connection && id == kSave) {
                const std::wstring computer_name = Field(state->computer_name);
                const std::wstring listen = Field(state->listen).empty() ? L"0.0.0.0" : Field(state->listen);
                const std::wstring port = Field(state->port).empty() ? L"4280" : Field(state->port);
                const std::wstring local = Field(state->local);
                const std::wstring remote = Field(state->remote);
                const std::wstring mobile_workspace_root =
                    Field(state->mobile_workspace_root);
                if (computer_name.empty()) {
                    ShowError(h, L"本机名称不能为空");
                    return 0;
                }
                if (computer_name.size() > 64) {
                    ShowError(h, L"本机名称不能超过 64 个字符");
                    return 0;
                }
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
                const std::filesystem::path mobile_root_path(mobile_workspace_root);
                if ((!mobile_workspace_root.empty() && !mobile_root_path.is_absolute()) ||
                    (state->mobile_access_enabled && mobile_workspace_root.empty())) {
                    ShowError(h, state->mobile_access_enabled && mobile_workspace_root.empty()
                                     ? L"开启手机访问前请选择手机工作目录"
                                     : L"手机工作目录必须是有效的绝对路径");
                    return 0;
                }
                state->store->SetSetting(L"computer_name", computer_name);
                state->store->SetSetting(L"listen_address", listen);
                state->store->SetSetting(L"listen_port", port);
                state->store->SetSetting(L"local_address", local);
                state->store->SetSetting(L"remote_address", remote);
                state->store->SetSetting(
                    L"mobile_workspace_root",
                    mobile_workspace_root.empty()
                        ? L"" : mobile_root_path.lexically_normal().wstring());
                agent::SetMobileAccessEnabled(*state->store,
                                              state->mobile_access_enabled);
                agent::AgentConfig config;
                config.computer_name = computer_name;
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
                    bool make_default=false;
                    if (!ShowProjectTypeEditorDialog(
                            h, L"新增项目类型",
                            L"为新的 AI CLI 配置名称和默认启动命令。",
                            L"", L"", false, state->light_theme,
                            name, command, make_default)) return 0;
                    if (!state->store->AddProjectType(name, command, &error)) {
                        ShowError(h, error);
                    } else {
                        if (make_default &&
                            !SetDefaultProjectTypeByName(*state->store,name,&error)) {
                            ShowError(h,error);
                        }
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
                        const bool was_default =
                            current.id == state->store->ConfiguredDefaultProjectTypeId();
                        bool make_default = was_default;
                        if (!ShowProjectTypeEditorDialog(
                                h, L"修改项目类型",
                                L"保存后，类型名称和默认启动命令会立即更新。",
                                current.name, current.default_command,
                                make_default, state->light_theme,
                                name, command, make_default)) return 0;
                        if (!state->store->UpdateProjectType(
                                current.id, name, command, &error)) {
                            ShowError(h, error);
                        } else {
                            if (make_default) {
                                if (!state->store->SetDefaultProjectType(current.id,&error)) {
                                    ShowError(h,error);
                                }
                            } else if (was_default &&
                                       !state->store->ClearDefaultProjectType(&error)) {
                                ShowError(h,error);
                            }
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
                bool make_default=false;
                if (!ShowProjectTypeEditorDialog(
                        h, L"新增项目类型",
                        L"为新的 AI CLI 配置名称和默认启动命令。",
                        L"", L"", false, state->light_theme,
                        name, command, make_default)) return 0;
                if (!state->store->AddProjectType(name, command, &error)) ShowError(h, error);
                else {
                    if (make_default&&
                        !SetDefaultProjectTypeByName(*state->store,name,&error)) ShowError(h,error);
                    RefreshList(*state); ReadSelection(*state);
                }
            } else if (id == kSave) {
                size_t index{};
                if (!SelectedIndex(state->list, index) || index >= state->types_data.size()) ShowError(h, L"请选择要修改的类型");
                else {
                    const auto current = state->types_data[index];
                    std::wstring name;
                    std::wstring command;
                    const bool was_default =
                        current.id == state->store->ConfiguredDefaultProjectTypeId();
                    bool make_default = was_default;
                    if (!ShowProjectTypeEditorDialog(
                            h, L"修改项目类型",
                            L"保存后，类型名称和默认启动命令会立即更新。",
                            current.name, current.default_command,
                            make_default, state->light_theme,
                            name, command, make_default)) return 0;
                    if (!state->store->UpdateProjectType(current.id, name, command, &error)) ShowError(h, error);
                    else {
                        if (make_default) {
                            if (!state->store->SetDefaultProjectType(current.id,&error)) {
                                ShowError(h,error);
                            }
                        } else if (was_default&&
                                   !state->store->ClearDefaultProjectType(&error)) {
                            ShowError(h,error);
                        }
                        RefreshList(*state); ReadSelection(*state);
                    }
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
        KillTimer(h,kDevicePresenceTimerId);
        state->list_scrollbar.Detach();
        state->content_scrollbar.Detach();
        return 0;
    }
    return DefWindowProcW(h, message, wparam, lparam);
}

bool ShowProjectTypeEditorDialog(HWND owner,
                                 const std::wstring& title,
                                 const std::wstring& subtitle,
                                 const std::wstring& initial_name,
                                 const std::wstring& initial_command,
                                 bool initial_default,
                                 bool light_theme,
                                 std::wstring& name,
                                 std::wstring& command,
                                 bool& make_default) {
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
        make_default = state.make_default;
    }
    aicli::ui::RestoreModalOwner(owner);
    return state.accepted;
}

} // namespace

ManagementDialogResult ShowManagementDialog(HWND owner, ManagementMode mode,
                                            DataStore& store, agent::RemoteAgent& agent,
                                            AgentRestartCallback restart_agent,
                                            ThemeChangedCallback theme_changed,
                                            AppearanceChangedCallback appearance_changed) {
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
    state.mode = mode==ManagementMode::Devices?ManagementMode::Settings:mode;
    state.settings_page = mode==ManagementMode::Devices
        ? SettingsPage::Devices : SettingsPage::Appearance;
    state.store = &store;
    state.agent = &agent;
    state.restart_agent = std::move(restart_agent);
    state.theme_changed = std::move(theme_changed);
    state.appearance_changed = std::move(appearance_changed);
    ApplyTheme(state, store.GetSetting(L"theme", L"dark") == L"light");
    state.font = CreateDialogFont(owner, 15, FW_NORMAL);
    state.title_font = CreateDialogFont(owner, 22, FW_SEMIBOLD);
    state.small_font = CreateDialogFont(owner, 13, FW_NORMAL);
    state.project_type_badge_font = CreateDialogFont(owner, 10, FW_NORMAL);
    // Settings uses a compact, explicit type scale independent of the other
    // management dialogs: 16 px navigation, 14 px content headings/controls,
    // and 12 px supporting text.
    if (state.mode == ManagementMode::Settings) {
        state.settings_title_font = CreateDialogFont(owner, 16, FW_SEMIBOLD);
        state.settings_nav_font = CreateDialogFont(owner, 16, FW_MEDIUM);
        state.settings_content_font = CreateDialogFont(owner, 14, FW_MEDIUM);
        state.settings_small_font = CreateDialogFont(owner, 12, FW_NORMAL);
        state.settings_icon_font = CreateDialogIconFont(owner, 14);
    }
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
    if (state.settings_title_font) DeleteObject(state.settings_title_font);
    if (state.settings_nav_font) DeleteObject(state.settings_nav_font);
    if (state.settings_content_font) DeleteObject(state.settings_content_font);
    if (state.settings_small_font) DeleteObject(state.settings_small_font);
    if (state.settings_icon_font) DeleteObject(state.settings_icon_font);
    if (state.project_type_badge_font) DeleteObject(state.project_type_badge_font);
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
    state.font = CreateDialogFont(owner, 15, FW_NORMAL);
    state.title_font = CreateDialogFont(owner, 16, FW_SEMIBOLD);
    state.small_font = CreateDialogFont(owner, 12, FW_NORMAL);
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
