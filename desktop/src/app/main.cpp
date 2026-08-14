#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <memory>
#include <sstream>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <optional>
#include <unordered_set>
#include <winrt/base.h>
#include <nlohmann/json.hpp>
#include <qrcodegen.hpp>
#include "aicli_build_identity.h"
#include "app/resource.h"
#include "agent/conversation_bridge.h"
#include "agent/remote_agent.h"
#include "agent/terminal_snapshot_publisher.h"
#include "agent/terminal_stream_publisher.h"
#include "app/application_activation.h"
#include "app/project_launcher.h"
#include "app/single_instance.h"
#include "app/runtime_components.h"
#include "core/models.h"
#include "core/process_metrics.h"
#include "core/text_utils.h"
#include "data/data_store.h"
#include "terminal/conpty_connection_bridge.h"
#include "terminal/terminal_working_detector.h"
#include "terminal/xaml_island_adapter.h"
#include "ui/styled_input.h"
#include "ui/taskbar_working_indicator.h"
#include "ui/drawing_utils.h"
#include "ui/footer_status.h"
#include "ui/main_window_renderer.h"
#include "ui/management_dialog.h"
#include "ui/theme.h"
#include "ui/layout_metrics.h"
#include "ui/modal_host.h"
#include "ui/window_chrome.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {
constexpr wchar_t kApplicationUserModelId[] = AICLI_APPLICATION_USER_MODEL_ID;
constexpr UINT WM_REMOTE_INPUT = WM_APP + 20;
constexpr UINT WM_REMOTE_CONTROL = WM_APP + 21;
constexpr UINT WM_PAIRING_PENDING = WM_APP + 22;
constexpr UINT WM_AGENT_STATUS = WM_APP + 23;
constexpr UINT WM_RESTORE_LAST_SESSION = WM_APP + 24;
constexpr UINT WM_TERMINAL_DIRTY = WM_APP + 25;
// Do not enter a nested modal loop from a native BUTTON's synchronous
// BN_CLICKED dispatch.  Posting this message lets WM_LBUTTONUP finish and
// the owner-draw button paint its released state before the modal is shown.
constexpr UINT WM_OPEN_SETTINGS = WM_APP + 26;
constexpr UINT WM_REMOTE_KEY = WM_APP + 27;
constexpr UINT WM_AGENT_WORKING_STATE = WM_APP + 28;
constexpr UINT WM_GIT_STATUS_READY = WM_APP + 29;
constexpr int ID_PROJECT_LIST = 1001;
constexpr int ID_NEW_PROJECT = 1004;
constexpr int ID_SETTINGS = 1005;
constexpr int ID_GROUP_DEFAULT = 1008;
constexpr int ID_GROUP_ADD = 1009;
constexpr int ID_BIND_PHONE = 1010;
constexpr int ID_START_PROJECT = 1013;
constexpr int ID_EDIT_PROJECT = 1014;
constexpr int ID_SEARCH_PROJECTS = 1015;
constexpr int ID_DEVICES = 1016;
constexpr int ID_OPEN_PROJECT_FOLDER = 1017;
constexpr int ID_CTX_PAUSE = 3001;
constexpr int ID_CTX_CONTINUE = 3002;
constexpr int ID_CTX_RESTART = 3004;
constexpr int ID_CTX_EDIT = 3005;
constexpr int ID_CTX_DELETE = 3006;
constexpr int ID_CTX_OPEN_FOLDER = 3007;
constexpr int ID_CTX_MOVE_DEFAULT = 3008;
constexpr int ID_CTX_OPEN_CMD = 3009;
constexpr int ID_CTX_MOVE_GROUP_BASE = 3200;
constexpr int ID_GROUP_SELECT_BASE = 3500;
constexpr int ID_MINIMIZE = 3010;
constexpr int ID_CLOSE = 3011;
constexpr int ID_MAXIMIZE = 3012;
constexpr wchar_t kClassName[] = L"AicliDesktopWindow";
constexpr wchar_t kAllGroupsName[] = L"全部";
constexpr wchar_t kStartedProjectsName[] = L"已启动";
constexpr UINT_PTR kXamlIslandSubclassId = 0xA1C11;
constexpr UINT_PTR kTerminalSnapshotTimerId = 2;
constexpr UINT_PTR kProjectSpinnerTimerId = 3;
constexpr UINT_PTR kGitSpinnerTimerId = 4;
constexpr UINT kProjectSpinnerIntervalMs = 75;
constexpr UINT kGitSpinnerIntervalMs = 80;
constexpr UINT kProjectListItemHeight = 40;


using aicli::DeviceRecord;
using aicli::Project;
using aicli::ProjectGroup;
using aicli::ProjectType;
using aicli::WorkingSetMegabytes;
using aicli::app::WinRtComponentRegistry;
using aicli::terminal::XamlIslandAdapter;
using aicli::text::Utf8Text;
using aicli::text::WideText;
using aicli::ui::AddRoundedPath;
using aicli::ui::ApplyRoundedRegion;
using aicli::ui::BlendBackdropColor;
using aicli::ui::DrawRoundedOutline;
using aicli::ui::DrawSmoothRoundedRect;
using aicli::ui::GdiplusRuntime;
using aicli::ui::SmoothColor;
struct RemoteInputMessage {
    std::wstring project_id,phone_id,input;
    HANDLE completed{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::atomic_bool cancelled{};
    bool accepted{};
    std::wstring error;
    RemoteInputMessage(std::wstring project,std::wstring phone,std::wstring value):
        project_id(std::move(project)),phone_id(std::move(phone)),input(std::move(value)){}
    ~RemoteInputMessage(){if(completed)CloseHandle(completed);}
};
struct RemoteControlMessage { std::wstring project_id, phone_id, action; };
struct RemoteKeyMessage {
    std::wstring project_id, phone_id, key;
    HANDLE completed{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::atomic_bool cancelled{};
    bool accepted{};
    std::wstring error;
    RemoteKeyMessage(std::wstring project,std::wstring phone,std::wstring value):
        project_id(std::move(project)),phone_id(std::move(phone)),key(std::move(value)){}
    ~RemoteKeyMessage(){if(completed)CloseHandle(completed);}
};
struct AgentStatusMessage { bool online{}; std::wstring text; };
struct AgentWorkingStateMessage { std::wstring project_id; bool working{}; };
struct GitStatusMessage {
    std::uint64_t request_id{};
    bool periodic{};
    std::wstring project_folder;
    aicli::git::RepositoryStatus repository;
};

// WinRT component registration and activation factories live in
// app/runtime_components.*.  The main window only owns the registry instance.

// Native TermControl/XAML Island session management lives in
// terminal/xaml_island_adapter.*.

HWND g_hwnd{}, g_list{}, g_status{}, g_new_project{}, g_settings{}, g_group{}, g_group_add{}, g_bind{}, g_devices{}, g_sidebar_title{}, g_search{};
HWND g_detail_title{}, g_detail_subtitle{}, g_detail_folder_label{}, g_detail_folder{},
     g_detail_command_label{}, g_detail_command{}, g_detail_note_label{}, g_detail_note{},
     g_detail_state{}, g_start_project{}, g_edit_project{}, g_open_project_folder{},
     g_detail_tooltip{};
HICON g_app_icon_large{},g_app_icon_small{};
WNDPROC g_button_proc{}; HWND g_hover_button{};
HFONT g_font{}, g_font_medium{}, g_font_small{}, g_font_git{}, g_font_sidebar_title{}, g_font_project_list{}, g_font_modal_title{}, g_font_modal_subtitle{}, g_icon_font{}; XamlIslandAdapter g_island; WinRtComponentRegistry g_components;
HFONT g_font_project_heading{},g_font_monospace{};
aicli::DataStore g_store;
aicli::agent::RemoteAgent g_remote_agent;
aicli::agent::ConversationBridge g_conversation_bridge(g_store,g_remote_agent);
std::optional<aicli::agent::PendingPairing> g_pending_pairing;
std::vector<Project> g_projects;
std::vector<ProjectType> g_project_types;
std::vector<ProjectGroup> g_groups;
std::vector<int> g_visible_project_indices;
std::wstring g_group_name=L"默认分组";
std::wstring g_last_project_id;
std::wstring g_startup_project_id;
aicli::ui::ThemeManager g_theme_manager;
auto& g_theme = g_theme_manager.palette();
aicli::ui::LayoutMetrics g_layout_metrics{};
// These aliases keep the rendering code readable while all values now come
// from one immutable palette transaction rather than per-window constants.
COLORREF& bg = g_theme.window;
COLORREF& panel = g_theme.panel;
COLORREF& elevated = g_theme.surface;
COLORREF& border = g_theme.border;
COLORREF& fg = g_theme.text;
COLORREF& muted = g_theme.muted;
COLORREF& accent = g_theme.accent;
COLORREF& accent2 = g_theme.accent_strong;
HBRUSH g_edit_brush{},g_focus_brush{},g_list_brush{},g_bg_brush{},g_detail_surface_brush{};bool g_light_theme{};bool g_theme_workspace_dirty{};
bool g_settings_open_posted{};
bool g_close_confirmation_open{};
HWND g_project_dialog{}, g_d_name{}, g_d_note{}, g_d_type{}, g_d_group{}, g_d_folder{}, g_d_folder_browse{}, g_d_command{}; bool g_project_dialog_ok{}; bool g_project_dialog_edit{}; Project g_project_draft{};
HWND g_binding_dialog{},g_binding_refresh{},g_binding_close{},g_binding_approve{},g_binding_reject{};
aicli::agent::PairingInfo g_binding_info{};
std::optional<qrcodegen::QrCode> g_binding_qr;
RECT g_binding_panel{};
RECT g_project_panel{};
RECT g_d_name_rect{}, g_d_note_rect{}, g_d_type_rect{}, g_d_group_rect{}, g_d_folder_rect{}, g_d_command_rect{};
RECT g_prestart_panel{},g_prestart_folder_surface{},g_prestart_command_surface{},g_prestart_note_surface{};
aicli::ui::WindowChrome g_window_chrome({ID_MINIMIZE,ID_MAXIMIZE,ID_CLOSE});
aicli::ui::FooterStatusController g_footer_status;
aicli::ui::TaskbarWorkingIndicator g_taskbar_working_indicator;
aicli::agent::TerminalSnapshotPublisher g_terminal_snapshots;
aicli::agent::TerminalStreamPublisher g_terminal_streams;
bool g_terminal_snapshot_timer_active{};
bool g_project_spinner_timer_active{};
float g_project_spinner_angle{-90.0f};
float g_project_idle_pulse_angle{-90.0f};
std::atomic_uint64_t g_git_status_request_id{};
std::atomic_bool g_git_status_query_active{};
std::unordered_set<std::wstring> g_working_projects;
int g_project_drag_source=-1;
int g_project_drag_insert=-1;
int g_project_drag_candidate=-1;
int g_project_hover_row=-1;
bool g_project_dragging{};
UINT g_project_drag_message{};
UINT g_taskbar_button_created_message{};
HWND g_project_drag_preview{};
POINT g_project_drag_hotspot{};

int ScaleUi(int value) noexcept {
    return g_layout_metrics.Scale(value);
}

float ScaleUiF(float value) noexcept {
    return value * static_cast<float>(std::max(1, g_layout_metrics.dpi)) / 96.0f;
}

enum { ID_D_NAME=2001, ID_D_TYPE=2002, ID_D_GROUP=2003, ID_D_FOLDER=2004, ID_D_COMMAND=2005, ID_D_OK=2006, ID_D_CANCEL=2007, ID_D_BROWSE=2008, ID_D_NOTE=2009 };
enum { ID_BIND_REFRESH=2101,ID_BIND_CLOSE=2102,ID_BIND_APPROVE=2103,ID_BIND_REJECT=2104 };
struct ProjectDropdownEntry{std::wstring label;bool selected{};};
int g_project_dropdown_width=220;
void DrawModernButton(const DRAWITEMSTRUCT* item);
LRESULT CALLBACK ButtonProc(HWND h,UINT m,WPARAM w,LPARAM l);
void Layout();
void UpdateDeviceHeader();
void UpdateHeaderTelemetry();
void RefreshConversationsCache();
bool ApplyStoredTheme(){
    const auto stored=g_store.GetSetting(L"theme",L"dark");
    const bool next_light=stored==L"light";
    const bool palette_changed=g_theme_manager.light()!=next_light||
                                !g_edit_brush||!g_focus_brush||
                                !g_list_brush||!g_bg_brush||!g_detail_surface_brush;
    if(!palette_changed){
        g_light_theme=next_light;
        return false;
    }
    g_theme_manager.LoadMode(next_light?aicli::ui::ThemeMode::Light:aicli::ui::ThemeMode::Dark);
    g_light_theme=g_theme_manager.light();
    if(g_edit_brush)DeleteObject(g_edit_brush);
    if(g_focus_brush)DeleteObject(g_focus_brush);
    if(g_list_brush)DeleteObject(g_list_brush);
    if(g_bg_brush)DeleteObject(g_bg_brush);
    if(g_detail_surface_brush)DeleteObject(g_detail_surface_brush);
    g_edit_brush=CreateSolidBrush(g_theme.field);
    g_focus_brush=CreateSolidBrush(g_theme.field_focus);
    g_list_brush=CreateSolidBrush(panel);
    g_bg_brush=CreateSolidBrush(bg);
    g_detail_surface_brush=CreateSolidBrush(
        g_light_theme?RGB(239,243,248):RGB(18,21,27));
    if(g_search) aicli::ui::SetStyledInputTheme(g_search,g_theme);
    return true;
}
COLORREF TitleBarBackground(){return g_window_chrome.Background(g_theme);}
COLORREF TitleBarText(){return g_window_chrome.Text(g_theme);}
void ApplyWindowChromeTheme(bool repaint=true){
    const COLORREF frame_border = g_light_theme ? border : RGB(18,21,27);
    g_window_chrome.ApplyTheme(g_hwnd,g_light_theme,frame_border,repaint);
}
void ApplyLiveTheme(bool flush_workspace=true){
    const bool changed=ApplyStoredTheme();
    if(changed){
        // Publish one palette to every surface before requesting a frame. The
        // modal remains open, but the workspace behind its translucent scrim
        // must switch immediately as well.
        g_island.SetTheme(g_light_theme);
        aicli::ui::RefreshModalThemes();
        g_theme_workspace_dirty=true;
        ApplyWindowChromeTheme(false);
        g_window_chrome.InvalidateTitleBar(g_hwnd);
        if(g_project_dialog)RedrawWindow(g_project_dialog,nullptr,nullptr,RDW_INVALIDATE|RDW_NOERASE|RDW_ALLCHILDREN);
        if(g_binding_dialog)RedrawWindow(g_binding_dialog,nullptr,nullptr,RDW_INVALIDATE|RDW_NOERASE|RDW_ALLCHILDREN);
    }
    if(!g_hwnd||!flush_workspace)return;
    if(!g_theme_workspace_dirty)return;
    // Redraw the owner without an erase pass. DWM presents the resulting
    // child paints together, so the live switch does not expose a blank frame
    // through the translucent modal scrim.
    ApplyWindowChromeTheme(false);
    RedrawWindow(g_hwnd,nullptr,nullptr,
                 RDW_INVALIDATE|RDW_FRAME|RDW_NOERASE|RDW_ALLCHILDREN|
                 RDW_UPDATENOW);
    g_theme_workspace_dirty=false;
}
void ExecuteTitleButton(HWND window,int id){
    if(id==ID_CLOSE)SendMessageW(window,WM_CLOSE,0,0);
    else if(id==ID_MINIMIZE)ShowWindow(window,SW_MINIMIZE);
    else if(id==ID_MAXIMIZE){ShowWindow(window,IsZoomed(window)?SW_RESTORE:SW_MAXIMIZE);Layout();}
}
void EnableButtonHover(HWND button){WNDPROC old=(WNDPROC)SetWindowLongPtrW(button,GWLP_WNDPROC,(LONG_PTR)ButtonProc);if(!g_button_proc)g_button_proc=old;}
constexpr UINT_PTR kProjectEditSubclassId=0xA1C12;
constexpr UINT_PTR kProjectListSubclassId=0xA1C13;
LRESULT CALLBACK ProjectEditSubclassProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR){if((m==WM_KEYDOWN||m==WM_CHAR)&&(w==VK_RETURN||w==L'\n'))return 0;if(m==WM_NCDESTROY)RemoveWindowSubclass(h,ProjectEditSubclassProc,kProjectEditSubclassId);return DefSubclassProc(h,m,w,l);}
void CenterProjectEditText(HWND control){
    if(!control)return;RECT client{};GetClientRect(control,&client);HDC dc=GetDC(control);if(!dc)return;
    HFONT font=(HFONT)SendMessageW(control,WM_GETFONT,0,0);auto oldFont=font?SelectObject(dc,font):nullptr;TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);if(oldFont)SelectObject(dc,oldFont);ReleaseDC(control,dc);
    const int height=client.bottom-client.top;const int textHeight=std::min(height,std::max(1,(int)metrics.tmHeight));const int top=std::max(0,(height-textHeight)/2);
    RECT format{10,top,std::max(11,(int)client.right-10),std::min(height,top+textHeight+1)};SendMessageW(control,EM_SETRECTNP,0,(LPARAM)&format);InvalidateRect(control,nullptr,FALSE);
}

HFONT CreateMainFont(int logical_height, int weight, const wchar_t* face,
                     DWORD pitch = DEFAULT_PITCH) {
    return CreateFontW(-ScaleUi(logical_height), 0, 0, 0, weight,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, pitch, face);
}

void ApplyMainFontsToControls() {
    for (HWND control : {g_group, g_group_add, g_bind, g_devices,
                         g_new_project, g_settings, g_detail_folder_label,
                         g_detail_command_label, g_detail_note_label,
                         g_detail_note, g_detail_state, g_start_project}) {
        if (control) SendMessageW(control, WM_SETFONT,
                                  reinterpret_cast<WPARAM>(g_font), TRUE);
    }
    for (HWND control : {g_edit_project, g_open_project_folder}) {
        if (control) SendMessageW(control, WM_SETFONT,
                                  reinterpret_cast<WPARAM>(g_icon_font), TRUE);
    }
    if (g_sidebar_title)
        SendMessageW(g_sidebar_title, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_font_sidebar_title), TRUE);
    if (g_search)
        SendMessageW(g_search, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_font), TRUE);
    if (g_list) {
        SendMessageW(g_list, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_font_project_list), TRUE);
        SendMessageW(g_list, LB_SETITEMHEIGHT, 0, ScaleUi(kProjectListItemHeight));
    }
    if (g_detail_title)
        SendMessageW(g_detail_title, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_font_project_heading), TRUE);
    if (g_detail_subtitle)
        SendMessageW(g_detail_subtitle, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_font_small), TRUE);
    for (HWND control : {g_detail_folder, g_detail_command}) {
        if (control) SendMessageW(control, WM_SETFONT,
                                  reinterpret_cast<WPARAM>(g_font_monospace), TRUE);
    }
    if (g_status)
        SendMessageW(g_status, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_font_git), TRUE);

    for (HWND control : {g_d_name, g_d_note, g_d_type, g_d_group,
                         g_d_folder, g_d_folder_browse, g_d_command,
                         g_binding_refresh, g_binding_close,
                         g_binding_approve, g_binding_reject}) {
        if (control) SendMessageW(control, WM_SETFONT,
                                  reinterpret_cast<WPARAM>(g_font), TRUE);
    }
}

void RecreateMainFontsForDpi() {
    const std::array<HFONT, 11> previous{
        g_font, g_font_medium, g_font_small, g_font_git,
        g_font_sidebar_title, g_font_project_list, g_font_modal_title,
        g_font_modal_subtitle, g_font_project_heading, g_font_monospace,
        g_icon_font};

    g_font = CreateMainFont(15, FW_NORMAL, L"Segoe UI");
    g_font_medium = CreateMainFont(17, FW_SEMIBOLD, L"Segoe UI");
    g_font_small = CreateMainFont(13, FW_NORMAL, L"Segoe UI");
    g_font_git = CreateMainFont(12, FW_NORMAL, L"Segoe UI");
    g_font_sidebar_title = CreateMainFont(16, FW_NORMAL, L"Segoe UI");
    g_font_project_list = CreateMainFont(14, FW_NORMAL, L"Segoe UI");
    g_font_modal_title = CreateMainFont(16, FW_SEMIBOLD, L"Segoe UI");
    g_font_modal_subtitle = CreateMainFont(14, FW_NORMAL, L"Segoe UI");
    g_font_project_heading = CreateMainFont(22, FW_SEMIBOLD, L"Segoe UI");
    g_font_monospace = CreateMainFont(14, FW_NORMAL, L"Cascadia Mono", FIXED_PITCH);
    g_icon_font = CreateMainFont(16, FW_NORMAL, L"Segoe MDL2 Assets");

    ApplyMainFontsToControls();
    for (HFONT font : previous) {
        if (font) DeleteObject(font);
    }
}

void SetText(HWND h,const std::wstring& s){
    if(!h||!IsWindow(h))return;
    const int length=GetWindowTextLengthW(h);
    std::wstring current(static_cast<size_t>(std::max(0,length)+1),L'\0');
    if(length>0){
        GetWindowTextW(h,current.data(),length+1);
        current.resize(static_cast<size_t>(length));
    }else current.clear();
    if(current!=s)SetWindowTextW(h,s.c_str());
}
bool MoveMainChild(HWND control,int x,int y,int width,int height){
    if(!control||!IsWindow(control))return false;
    RECT current{};GetWindowRect(control,&current);
    POINT points[2]={{current.left,current.top},{current.right,current.bottom}};
    MapWindowPoints(HWND_DESKTOP,g_hwnd,points,2);
    const RECT desired{x,y,x+std::max(1,width),y+std::max(1,height)};
    const RECT actual{points[0].x,points[0].y,points[1].x,points[1].y};
    if(EqualRect(&actual,&desired))return false;
    SetWindowPos(control,nullptr,desired.left,desired.top,
                 desired.right-desired.left,desired.bottom-desired.top,
                 SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER|
                 SWP_NOSENDCHANGING|SWP_NOCOPYBITS);
    return true;
}
bool SetMainChildVisible(HWND control,bool visible){
    if(!control||!IsWindow(control))return false;
    const bool current=IsWindowVisible(control)!=FALSE;
    if(current==visible)return false;
    ShowWindow(control,visible?SW_SHOW:SW_HIDE);
    return true;
}
void RecordAndBroadcastEvent(const std::wstring& project_id,const std::wstring& event_type,const nlohmann::json& payload){g_conversation_bridge.RecordAndBroadcast(project_id,event_type,payload);}
#include "ui/internal/project_dialog_section.inl"

#include "ui/internal/binding_dialog_section.inl"

bool IsProjectWorking(const Project& project){
    return g_island.HasSession(project.id)&&
           g_working_projects.find(project.id)!=g_working_projects.end();
}
void RefreshTaskbarWorkingIndicator(){
    g_taskbar_working_indicator.SetStyle(
        g_store.GetSetting(L"taskbar_working_style",L"progress")==L"overlay"
            ? aicli::ui::TaskbarWorkingStyle::OverlaySpinner
            : aicli::ui::TaskbarWorkingStyle::ProgressBar);
    if(g_store.GetSetting(L"taskbar_working_enabled",L"1")==L"0"){
        g_taskbar_working_indicator.SetWorking(false);
        return;
    }
    const bool currentOnly=
        g_store.GetSetting(L"taskbar_working_scope",L"all")==L"current";
    bool working=false;
    if(currentOnly&&!g_last_project_id.empty()){
        const auto found=std::find_if(g_projects.begin(),g_projects.end(),[](const Project& project){
            return project.id==g_last_project_id;
        });
        working=found!=g_projects.end()&&IsProjectWorking(*found);
    }else if(!currentOnly){
        working=std::any_of(g_projects.begin(),g_projects.end(),[](const Project& project){
            return IsProjectWorking(project);
        });
    }
    g_taskbar_working_indicator.SetWorking(working);
}
bool HasVisibleAnimatedProject(){
    return std::any_of(g_visible_project_indices.begin(),g_visible_project_indices.end(),[](int index){
        return index>=0&&index<static_cast<int>(g_projects.size())&&
               g_island.HasSession(g_projects[index].id);
    });
}
void StopProjectSpinnerTimer(){
    if(!g_project_spinner_timer_active)return;
    if(g_hwnd)KillTimer(g_hwnd,kProjectSpinnerTimerId);
    g_project_spinner_timer_active=false;
}
void SyncProjectSpinnerTimer(){
    const bool shouldRun=g_hwnd&&g_list&&HasVisibleAnimatedProject();
    if(shouldRun&&!g_project_spinner_timer_active){
        g_project_spinner_angle=-90.0f;
        g_project_idle_pulse_angle=-90.0f;
        g_project_spinner_timer_active=
            SetTimer(g_hwnd,kProjectSpinnerTimerId,kProjectSpinnerIntervalMs,nullptr)!=0;
    }else if(!shouldRun){
        StopProjectSpinnerTimer();
    }
}
void InvalidateProjectRow(const std::wstring& project_id){
    if(!g_list||!IsWindow(g_list))return;
    RECT client{};
    GetClientRect(g_list,&client);
    for(size_t row=0;row<g_visible_project_indices.size();++row){
        const int index=g_visible_project_indices[row];
        if(index<0||index>=static_cast<int>(g_projects.size())||
           g_projects[index].id!=project_id)continue;
        RECT itemRect{};
        if(SendMessageW(g_list,LB_GETITEMRECT,static_cast<WPARAM>(row),
                        reinterpret_cast<LPARAM>(&itemRect))!=LB_ERR&&
           itemRect.bottom>client.top&&itemRect.top<client.bottom){
            InvalidateRect(g_list,&itemRect,FALSE);
        }
        return;
    }
}
void SetProjectWorkingState(const std::wstring& project_id,bool working){
    if(project_id.empty())return;
    const bool changed=working
        ?g_working_projects.insert(project_id).second
        :g_working_projects.erase(project_id)>0;
    if(!changed)return;
    InvalidateProjectRow(project_id);
    SyncProjectSpinnerTimer();
    RefreshTaskbarWorkingIndicator();
}
bool InvalidateWorkingProjectRows(){
    if(!g_list||!IsWindow(g_list))return false;
    RECT client{};
    GetClientRect(g_list,&client);
    bool found=false;
    for(size_t row=0;row<g_visible_project_indices.size();++row){
        const int index=g_visible_project_indices[row];
        if(index<0||index>=static_cast<int>(g_projects.size())||
           !g_island.HasSession(g_projects[index].id))continue;
        found=true;
        RECT itemRect{};
        if(SendMessageW(g_list,LB_GETITEMRECT,static_cast<WPARAM>(row),
                        reinterpret_cast<LPARAM>(&itemRect))!=LB_ERR&&
           itemRect.bottom>client.top&&itemRect.top<client.bottom){
            InvalidateRect(g_list,&itemRect,FALSE);
        }
    }
    return found;
}

#include "ui/internal/project_list_renderer_section.inl"
bool PickFolder(HWND owner,std::wstring& path){IFileDialog* dialog=nullptr;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))||!dialog)return false;DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);if(FAILED(dialog->Show(owner))){dialog->Release();return false;}IShellItem* item=nullptr;bool ok=SUCCEEDED(dialog->GetResult(&item));if(ok){PWSTR value=nullptr;ok=SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&value));if(ok){path=value;CoTaskMemFree(value);}item->Release();}dialog->Release();return ok;}
int SelectedProjectIndex(){
    const LRESULT row=SendMessageW(g_list,LB_GETCURSEL,0,0);
    if(row==LB_ERR||row<0||row>=static_cast<LRESULT>(g_visible_project_indices.size()))return -1;
    return g_visible_project_indices[static_cast<size_t>(row)];
}
void RefreshGitStatus(bool force_redraw=false){
    const int index=SelectedProjectIndex();
    const std::wstring folder=index>=0&&index<(int)g_projects.size()
        ?g_projects[index].folder:std::wstring{};
    if(force_redraw)++g_git_status_request_id;
    if(!force_redraw&&g_git_status_query_active.exchange(true))return;
    {
        const std::uint64_t request_id=g_git_status_request_id.load();
        if(force_redraw){
        g_footer_status.BeginRepositoryRefresh(folder);
        if(g_hwnd)SetTimer(g_hwnd,kGitSpinnerTimerId,kGitSpinnerIntervalMs,nullptr);
        }
        const HWND target=g_hwnd;
        std::thread([target,request_id,folder,force_redraw]{
            auto message=std::make_unique<GitStatusMessage>();
            message->request_id=request_id;
            message->periodic=!force_redraw;
            message->project_folder=folder;
            message->repository=aicli::git::InspectRepository(folder);
            if(!target||!IsWindow(target)||
               !PostMessageW(target,WM_GIT_STATUS_READY,0,
                             reinterpret_cast<LPARAM>(message.get())))return;
            message.release();
        }).detach();
    }
}
void RefreshProjectDetails(bool force_git_refresh=false){
    const int index=SelectedProjectIndex();
    if(index<0 || index>=(int)g_projects.size()){
        g_last_project_id.clear();
        SetText(g_detail_title,L"未选择项目");
        SetText(g_detail_subtitle,L"");
        SetText(g_detail_folder,L"未设置");
        SetText(g_detail_command,L"未设置");
        SetText(g_detail_note,L"暂无备注");
        SetText(g_detail_state,L"● 尚未启动");
        RefreshGitStatus(force_git_refresh);
        RefreshTaskbarWorkingIndicator();
        return;
    }
    const auto& project=g_projects[index];
    g_last_project_id=project.id;
    g_store.SetSetting(L"last_project_id",project.id);
    SetText(g_detail_title,project.name);
    SetText(g_detail_subtitle,project.type+L"  ·  "+(project.group.empty()?L"默认分组":project.group));
    SetText(g_detail_folder,project.folder.empty()?L"未设置":project.folder);
    SetText(g_detail_command,project.command.empty()?L"未设置":project.command);
    SetText(g_detail_note,project.note.empty()?L"暂无备注":project.note);
    const wchar_t* state=g_island.HasSession(project.id)?L"● 运行中":(project.status==L"paused"?L"● 已暂停":L"● 尚未启动");
    SetText(g_detail_state,state);
    RefreshGitStatus(force_git_refresh);
    RefreshTaskbarWorkingIndicator();
}

bool InitializePersistentState() {
    std::wstring error;
    const std::filesystem::path database=std::filesystem::path(aicli::DefaultDataDirectory())/L"aicli.db";
    if(!g_store.Initialize(database.wstring(),&error)){
        MessageBoxW(nullptr,(L"SQLite 初始化失败："+error).c_str(),L"AI CLI",MB_OK|MB_ICONERROR);
        return false;
    }
    g_project_types=g_store.LoadProjectTypes();
    g_groups=g_store.LoadGroups();
    if(g_groups.empty()){
        g_store.AddGroup(L"默认分组");g_groups=g_store.LoadGroups();
    }
    g_projects=g_store.LoadProjects();
    for(auto& project:g_projects){
        if(project.status==L"running"){
            project.status=L"created";
            g_store.UpdateProjectStatus(project.id,project.status);
        }
    }
    // A fresh installation starts with an empty project/session list. Codex
    // and Claude remain built-in project types, but the user explicitly
    // creates the first project and chooses its folder, group and command.
    // Mark the legacy seed migration as handled so another front-end sharing
    // this database cannot recreate the removed sample sessions later.
    if(g_store.GetSetting(L"seeded_projects",L"0")!=L"1")
        g_store.SetSetting(L"seeded_projects",L"1");
    g_group_name=g_store.GetSetting(L"last_group",L"默认分组");
    if(g_group_name!=kAllGroupsName&&g_group_name!=kStartedProjectsName&&std::none_of(g_groups.begin(),g_groups.end(),[&](const ProjectGroup& group){return group.name==g_group_name;}))g_group_name=L"默认分组";
    g_last_project_id=g_store.GetSetting(L"last_project_id");
    g_startup_project_id=g_last_project_id;
    ApplyStoredTheme();
    return true;
}
void UpdateGroupButton(){
    if(!g_group)return;
    const auto count=g_group_name==kAllGroupsName?g_projects.size():std::count_if(g_projects.begin(),g_projects.end(),[](const Project& project){if(g_group_name==kStartedProjectsName)return g_island.HasSession(project.id);const std::wstring group=project.group.empty()?L"默认分组":project.group;return group==g_group_name;});
    SetText(g_group,g_group_name+L" ("+std::to_wstring(count)+L")  ▾");
}
void RefreshProjectList(){
    const int selected=SelectedProjectIndex();
    const std::wstring preferred_id=(selected>=0&&selected<(int)g_projects.size())?g_projects[selected].id:g_last_project_id;
    SendMessageW(g_list,WM_SETREDRAW,FALSE,0);
    SendMessageW(g_list,LB_RESETCONTENT,0,0);
    g_visible_project_indices.clear();
    const std::wstring query=aicli::ui::GetStyledInputText(g_search);
    for(size_t i=0;i<g_projects.size();++i){
        const auto& project=g_projects[i];
        const std::wstring group=project.group.empty()?L"默认分组":project.group;
        if(g_group_name==kStartedProjectsName){if(!g_island.HasSession(project.id))continue;}
        else if(g_group_name!=kAllGroupsName&&group!=g_group_name)continue;
        if(!query.empty()&&project.name.find(query)==std::wstring::npos&&
           project.note.find(query)==std::wstring::npos&&
           project.folder.find(query)==std::wstring::npos&&
           project.type.find(query)==std::wstring::npos)continue;
        const LRESULT row=SendMessageW(g_list,LB_ADDSTRING,0,(LPARAM)project.name.c_str());
        if(row!=LB_ERR){SendMessageW(g_list,LB_SETITEMDATA,row,static_cast<LPARAM>(i));g_visible_project_indices.push_back(static_cast<int>(i));}
    }
    int row=-1;for(size_t i=0;i<g_visible_project_indices.size();++i){const int project_index=g_visible_project_indices[i];if((!preferred_id.empty()&&project_index>=0&&project_index<(int)g_projects.size()&&g_projects[project_index].id==preferred_id)||project_index==selected){row=static_cast<int>(i);break;}}
    if(row<0&&!g_visible_project_indices.empty())row=0;
    SendMessageW(g_list,LB_SETCURSEL,row,0);
    SendMessageW(g_list,WM_SETREDRAW,TRUE,0);
    InvalidateRect(g_list,nullptr,FALSE);
    RefreshProjectDetails();
    RefreshConversationsCache();
    UpdateGroupButton();
    SyncProjectSpinnerTimer();
}
#include "ui/internal/project_drag_section.inl"
void Layout(){
    if(!g_hwnd || IsIconic(g_hwnd))return;
    g_layout_metrics=aicli::ui::LayoutMetrics::ForWindow(g_hwnd);
    RECT r{}; if(!GetClientRect(g_hwnd,&r))return;
    const int top=g_layout_metrics.title_bar_height;
    const int left=g_layout_metrics.sidebar_width;
    const int bottom=g_layout_metrics.footer_height;
    if(r.right<=left+ScaleUi(80) || r.bottom<=top+bottom+ScaleUi(120))return;
    const auto S=[](int value){return ScaleUi(value);};
    MoveMainChild(g_group,S(168),S(4),S(154),S(32));
    MoveMainChild(g_group_add,S(326),S(4),S(34),S(32));
    // Anchor the complete phone/settings cluster from the native caption
    // buttons and lay each control out sequentially. Clamping every button to
    // its own minimum X made the first two overlap on narrower/high-DPI
    // windows even though the cluster as a whole still had enough room.
    const int headerGap=S(8);
    const int bindWidth=S(104);
    const int devicesWidth=S(116);
    const int settingsWidth=S(72);
    const int captionReserve=g_layout_metrics.caption_button_width*3;
    const int clusterRight=(int)r.right-captionReserve-S(8);
    const int settingsX=clusterRight-settingsWidth;
    const int devicesX=settingsX-headerGap-devicesWidth;
    const int bindX=devicesX-headerGap-bindWidth;
    const bool headerControlsMoved =
        MoveMainChild(g_bind,bindX,S(4),bindWidth,S(32)) |
        MoveMainChild(g_devices,devicesX,S(4),devicesWidth,S(32)) |
        MoveMainChild(g_settings,settingsX,S(4),settingsWidth,S(32));
    if(headerControlsMoved){
        // During interactive horizontal resizing USER32 otherwise preserves
        // pixels from the controls' old positions.  The parent title bar is
        // custom-painted, so those copied pixels can survive as overlapping
        // button ghosts until another full repaint.
        RECT titleBar{0,0,r.right,top};
        RedrawWindow(g_hwnd,&titleBar,nullptr,
                     RDW_INVALIDATE|RDW_NOERASE|RDW_ALLCHILDREN);
    }

    const int sidebarInset=S(6);
    // Restore the search field geometry from before it was reduced to a
    // 35px outer box: 38px outer height with 2px inset = 34px control height.
    const int searchHeight=S(38);
    const int titleHeight=S(28);
    const int titleBottomGap=S(10);
    const int addButtonSize=S(28);
    const int searchTop=top+S(6);
    const int searchBottom=searchTop+searchHeight;
    RECT search_rect{sidebarInset+S(2),searchTop+S(2),
                     left-sidebarInset-S(2),searchBottom+S(2)};
    aicli::ui::LayoutStyledInput(g_search,search_rect,S(2),S(9));
    // Keep every control below the search field at its current position.
    const int titleTop=top+S(49);
    MoveMainChild(g_sidebar_title,S(9),titleTop,S(200),titleHeight);
    MoveMainChild(g_new_project,left-sidebarInset-addButtonSize,titleTop,addButtonSize,addButtonSize);
    SetWindowPos(g_new_project,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    const int footer_top=r.bottom-bottom;
    g_footer_status.Layout(r,bottom);
    const int listTop=titleTop+titleHeight+titleBottomGap;
    MoveMainChild(g_list,sidebarInset,listTop,left-sidebarInset*2,
                  std::max(S(80),footer_top-listTop-sidebarInset));

    // WM_NCCALCSIZE makes the complete custom frame part of the client area.
    // Keep the XAML Island off the outermost right-hand compositor rim so the
    // parent HWND always owns a real, directly hittable resize strip. The
    // island may otherwise visually cover the border and consume the initial
    // drag even when its outer host returns HTTRANSPARENT.
    const int terminalRightInset=IsZoomed(g_hwnd)?0:S(2);
    const LONG terminalRight=std::max<LONG>(left+1,r.right-terminalRightInset);
    RECT terminal{left,top,terminalRight,r.bottom-bottom};
    const int selectedIndex=SelectedProjectIndex();
    const bool selectedRunning=selectedIndex>=0&&selectedIndex<(int)g_projects.size()&&
        g_island.HasSession(g_projects[selectedIndex].id);
    if(selectedRunning){
        const auto& selectedProject=g_projects[selectedIndex];
        // Layout is called frequently for text/status updates.  Do not steal
        // focus from the native terminal or trigger a TUI repaint when the
        // selected session has not actually changed.
        g_island.Activate(selectedProject.id,false);
    }
    const bool showingTerminal=selectedRunning;
    const bool showingNative=showingTerminal&&g_island.Active();
    const bool emptyProjects=g_projects.empty();
    if(g_detail_title){
        LONG_PTR titleStyle=GetWindowLongPtrW(g_detail_title,GWL_STYLE);
        const LONG_PTR alignment=emptyProjects?SS_CENTER:SS_LEFT;
        const LONG_PTR nextStyle=(titleStyle&~SS_TYPEMASK)|alignment;
        if(nextStyle!=titleStyle){
            SetWindowLongPtrW(g_detail_title,GWL_STYLE,nextStyle);
            InvalidateRect(g_detail_title,nullptr,FALSE);
        }
    }
    SetMainChildVisible(g_detail_title,!showingTerminal);
    for(HWND control:{g_detail_subtitle,g_detail_folder_label,g_detail_folder,
                      g_detail_command_label,g_detail_command,g_detail_note_label,
                      g_detail_note,g_detail_state})
        SetMainChildVisible(control,!showingTerminal&&!emptyProjects);
    SetMainChildVisible(g_start_project,!showingTerminal&&!emptyProjects);
    SetMainChildVisible(g_edit_project,!showingTerminal&&!emptyProjects);
    SetMainChildVisible(g_open_project_folder,!showingTerminal&&!emptyProjects);
    if(emptyProjects){
        SetRectEmpty(&g_prestart_panel);SetRectEmpty(&g_prestart_folder_surface);
        SetRectEmpty(&g_prestart_command_surface);SetRectEmpty(&g_prestart_note_surface);
        SetText(g_detail_title,L"请先添加项目");
        MoveMainChild(g_detail_title,terminal.left+S(28),
                      terminal.top+(terminal.bottom-terminal.top-S(32))/2,
                      terminal.right-terminal.left-S(56),S(32));
    }else{
        const int workspaceWidth=terminal.right-terminal.left;
        const int panelWidth=std::min(S(620),std::max(S(480),workspaceWidth-S(72)));
        const int panelHeight=S(466);
        const int panelLeft=terminal.left+(workspaceWidth-panelWidth)/2;
        const int panelTop=static_cast<int>(terminal.top)+std::max(
            S(22),static_cast<int>((terminal.bottom-terminal.top-panelHeight)/2-S(12)));
        g_prestart_panel={panelLeft,panelTop,panelLeft+panelWidth,panelTop+panelHeight};
        MoveMainChild(g_detail_title,panelLeft,panelTop,panelWidth,S(32));
        MoveMainChild(g_detail_subtitle,panelLeft,panelTop+S(36),panelWidth,S(22));
        MoveMainChild(g_detail_state,panelLeft,panelTop+S(68),panelWidth,S(24));
        const int surfaceLeft=panelLeft;
        const int surfaceRight=panelLeft+panelWidth;
        MoveMainChild(g_detail_folder_label,surfaceLeft,panelTop+S(112),panelWidth,S(20));
        g_prestart_folder_surface={surfaceLeft,panelTop+S(136),surfaceRight,panelTop+S(178)};
        MoveMainChild(g_detail_folder,surfaceLeft+S(14),panelTop+S(136),panelWidth-S(28),S(42));
        MoveMainChild(g_detail_command_label,surfaceLeft,panelTop+S(198),panelWidth,S(20));
        g_prestart_command_surface={surfaceLeft,panelTop+S(222),surfaceRight,panelTop+S(264)};
        MoveMainChild(g_detail_command,surfaceLeft+S(14),panelTop+S(222),panelWidth-S(28),S(42));
        MoveMainChild(g_detail_note_label,surfaceLeft,panelTop+S(284),panelWidth,S(20));
        g_prestart_note_surface={surfaceLeft,panelTop+S(308),surfaceRight,panelTop+S(370)};
        MoveMainChild(g_detail_note,surfaceLeft+S(14),panelTop+S(308),panelWidth-S(28),S(62));
        const int actionsWidth=S(132+12+38+10+38);
        const int actionsLeft=panelLeft+(panelWidth-actionsWidth)/2;
        MoveMainChild(g_start_project,actionsLeft,panelTop+S(402),S(132),S(38));
        MoveMainChild(g_edit_project,actionsLeft+S(144),panelTop+S(403),S(38),S(36));
        MoveMainChild(g_open_project_folder,actionsLeft+S(192),panelTop+S(403),S(38),S(36));
    }
    // The central surface is always the real Windows Terminal control.  Do
    // not create a second EDIT-based command line or a text-output fallback:
    // keyboard focus, shortcuts, selection and scrolling must stay native.
    const int statusLeft=g_footer_status.git_bounds().right+S(12);
    MoveMainChild(g_status,statusLeft,r.bottom-bottom+S(6),
                  std::max(0,g_footer_status.telemetry_left()-statusLeft-S(8)),S(20));
    if(g_island.Window()){
        if(showingNative)g_island.Resize(terminal);
        // Modal windows are now separate owned popups.  Keep the real
        // TermControl alive and visible beneath the translucent scrim.
        g_island.SetVisible(showingNative);
    }
}
void CreateProject(){
    if(!ShowProjectDialog(g_hwnd))return;
    std::wstring error;
    if(!g_store.SaveProject(g_project_draft,&error)){MessageBoxW(g_hwnd,error.c_str(),L"保存项目",MB_OK|MB_ICONWARNING);return;}
    g_projects.push_back(g_project_draft);g_group_name=g_project_draft.group;g_store.SetSetting(L"last_group",g_group_name);RefreshProjectList();
    for(size_t i=0;i<g_projects.size();++i)if(g_projects[i].id==g_project_draft.id){const int row=static_cast<int>(std::find(g_visible_project_indices.begin(),g_visible_project_indices.end(),static_cast<int>(i))-g_visible_project_indices.begin());if(row<(int)g_visible_project_indices.size())SendMessageW(g_list,LB_SETCURSEL,row,0);break;}
    RefreshProjectDetails();Layout();SetText(g_status,L"项目已创建    |    选择右侧立即开始");
}
void EditProject(){
    const int index=SelectedProjectIndex();if(index<0||index>=(int)g_projects.size())return;const Project before=g_projects[index];
    if(!ShowProjectDialog(g_hwnd,&before))return;std::wstring error;
    if(!g_store.SaveProject(g_project_draft,&error)){MessageBoxW(g_hwnd,error.c_str(),L"保存项目",MB_OK|MB_ICONWARNING);return;}
    g_projects[index]=g_project_draft;RefreshProjectList();SetText(g_status,L"项目配置已更新");Layout();
}
void DeleteProject(){
    const int index=SelectedProjectIndex();
    if(index<0||index>=(int)g_projects.size())return;
    const Project project=g_projects[index];
    const bool deletingRunning=g_island.HasSession(project.id);
    std::wstring message=L"确定从 AI CLI 中删除项目“"+project.name+L"”吗？\n\n";
    if(deletingRunning)message+=L"该项目正在运行，删除后会同时结束当前终端会话。\n\n";
    message+=L"只会删除项目配置，不会删除项目文件夹或其中的任何文件。";
    if(MessageBoxW(g_hwnd,message.c_str(),L"删除项目",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
    if(deletingRunning){
        SetProjectWorkingState(project.id,false);
        g_island.Stop(project.id);
    }
    g_terminal_streams.RemoveSession(project.id);
    g_store.DeleteProject(project.id);
    g_projects.erase(g_projects.begin()+index);
    RefreshProjectList();
    RefreshProjectDetails();
    Layout();
    SetText(g_status,L"项目“"+project.name+L"”已删除，项目文件夹未受影响");
}
#include "ui/internal/project_context_menu_section.inl"
bool StartCommandSession(const std::wstring& cmd,const std::wstring& cwd,const std::wstring& title,const std::wstring& sessionAgent,const std::wstring& project_id){
    SetProjectWorkingState(project_id,false);
    g_terminal_streams.BeginSession(project_id);
    std::shared_ptr<aicli::terminal::TerminalWorkingDetector> workingDetector;
    const auto workingProtocol=
        aicli::terminal::WorkingProtocolForSession(sessionAgent,cmd);
    if(workingProtocol){
        const HWND owner=g_hwnd;
        workingDetector=std::make_shared<aicli::terminal::TerminalWorkingDetector>(
            *workingProtocol,
            [owner,project_id](bool working){
                auto* message=new(std::nothrow)
                    AgentWorkingStateMessage{project_id,working};
                if(!message||!owner||
                   !PostMessageW(owner,WM_AGENT_WORKING_STATE,0,
                                 reinterpret_cast<LPARAM>(message))){
                    delete message;
                }
            });
    }
    if(g_island.Start(
           project_id,cmd,cwd,title,g_hwnd,g_status,
           [project_id,workingDetector](std::wstring_view output){
               g_terminal_streams.PublishOutput(project_id,output);
               g_terminal_snapshots.Queue(project_id);
               if(workingDetector)workingDetector->Observe(output);
           },
           [project_id](std::uint32_t rows,std::uint32_t columns){
               g_terminal_streams.PublishResize(project_id,rows,columns);
           })){
        SetText(g_status,L"");
        Layout();
        return true;
    }
    const std::wstring error=g_island.LastError().empty()?L"无法创建原生终端控件":g_island.LastError();
    g_terminal_streams.RemoveSession(project_id);
    SetText(g_detail_state,L"状态: 启动失败");
    SetText(g_status,L"终端启动失败: "+error);
    Layout(); return false;
}
void StartSession(bool restart=false){
    const int i=SelectedProjectIndex();if(i<0||i>=(int)g_projects.size())return;
    auto& project=g_projects[i];
    if(g_island.HasSession(project.id)){
        if(!restart){g_island.Activate(project.id);Layout();return;}
        SetProjectWorkingState(project.id,false);
        g_island.Stop(project.id);
    }
    const std::wstring sessionAgent=project.type;
    const std::wstring cmd=project.command.empty()?DefaultProjectCommand(sessionAgent):project.command;
    if(StartCommandSession(cmd,project.folder,project.name,sessionAgent,project.id)){
        project.status=L"running";
        g_store.UpdateProjectStatus(project.id,L"running");
        RecordAndBroadcastEvent(project.id,L"conversation.running",nlohmann::json{{"status","running"}});
        RefreshProjectDetails();RefreshProjectList();Layout();
    }
}
int ProjectIndexById(const std::wstring& project_id){for(size_t i=0;i<g_projects.size();++i)if(g_projects[i].id==project_id)return static_cast<int>(i);return -1;}
bool SelectProjectById(const std::wstring& project_id){const int index=ProjectIndexById(project_id);if(index<0)return false;const bool visible_in_filter=g_group_name==kAllGroupsName||(g_group_name==kStartedProjectsName?g_island.HasSession(project_id):(g_projects[index].group.empty()?L"默认分组":g_projects[index].group)==g_group_name);if(!visible_in_filter){g_group_name=g_projects[index].group.empty()?L"默认分组":g_projects[index].group;SetText(g_group,g_group_name);g_store.SetSetting(L"last_group",g_group_name);}RefreshProjectList();for(size_t row=0;row<g_visible_project_indices.size();++row)if(g_visible_project_indices[row]==index){SendMessageW(g_list,LB_SETCURSEL,row,0);RefreshProjectDetails(true);Layout();return true;}return false;}
void RefreshConversationsCache(){g_conversation_bridge.RefreshConversations(g_projects);}
std::string ConversationsJson(){return g_conversation_bridge.ConversationsJson();}
std::string EventsJson(const std::wstring& project_id,std::int64_t after){return g_conversation_bridge.EventsJson(project_id,after);}
void UpdateDeviceHeader(){if(!g_devices)return;const auto count=g_store.LoadDevices().size();SetText(g_devices,L"已绑定 "+std::to_wstring(count)+L" 台");InvalidateRect(g_hwnd,nullptr,FALSE);}
void UpdateHeaderTelemetry(){
    g_footer_status.UpdateTelemetry(
        g_remote_agent.Online(),
        WorkingSetMegabytes(),
        !g_store.GetSetting(L"remote_address").empty());
}
void ConfigureRemoteAgent(){
    g_terminal_streams.SetBroadcastCallback([](const std::wstring& project,
                                                const std::string& event){
        g_remote_agent.BroadcastEvent(project,event);
    });
    g_remote_agent.SetTerminalReplayCallback(
        [](const std::wstring& project,const std::wstring& session,
           std::uint64_t after){
            return g_terminal_streams.Replay(project,session,after);
        });
    g_remote_agent.SetTerminalHistoryCallback(
        [](const std::wstring& project,const std::wstring& token,
           std::size_t before,std::size_t limit){
            return g_terminal_streams.History(project,token,before,limit);
        });
    g_remote_agent.SetConversationsCallback([]{return ConversationsJson();});
    g_remote_agent.SetEventsCallback([](const std::wstring& project,std::int64_t after){return EventsJson(project,after);});
    g_remote_agent.SetInputCallback([](const std::wstring& project,const std::wstring& phone,
                                       const std::wstring& input,std::wstring* error){
        auto request=std::make_shared<RemoteInputMessage>(project,phone,input);
        if(!request->completed){if(error)*error=L"无法创建输入同步事件";return false;}
        auto* envelope=new(std::nothrow) std::shared_ptr<RemoteInputMessage>(request);
        if(!envelope||!g_hwnd||
           !PostMessageW(g_hwnd,WM_REMOTE_INPUT,0,reinterpret_cast<LPARAM>(envelope))){
            delete envelope;
            if(error)*error=L"桌面窗口未就绪";
            return false;
        }
        if(WaitForSingleObject(request->completed,30000)!=WAIT_OBJECT_0){
            request->cancelled=true;
            if(error)*error=L"电脑端处理输入超时";
            return false;
        }
        if(!request->accepted&&error)*error=request->error.empty()?L"终端未接受输入":request->error;
        return request->accepted;
    });
    g_remote_agent.SetKeyCallback([](const std::wstring& project,const std::wstring& phone,
                                     const std::wstring& key,std::wstring* error){
        auto request=std::make_shared<RemoteKeyMessage>(project,phone,key);
        if(!request->completed){if(error)*error=L"无法创建按键同步事件";return false;}
        auto* envelope=new(std::nothrow) std::shared_ptr<RemoteKeyMessage>(request);
        if(!envelope||!g_hwnd||
           !PostMessageW(g_hwnd,WM_REMOTE_KEY,0,reinterpret_cast<LPARAM>(envelope))){
            delete envelope;
            if(error)*error=L"桌面窗口未就绪";
            return false;
        }
        if(WaitForSingleObject(request->completed,30000)!=WAIT_OBJECT_0){
            request->cancelled=true;
            if(error)*error=L"电脑端处理按键超时";
            return false;
        }
        if(!request->accepted&&error)*error=request->error.empty()?L"终端未接受按键":request->error;
        return request->accepted;
    });
    g_remote_agent.SetControlCallback([](const std::wstring& project,const std::wstring& phone,const std::wstring& action,std::wstring* error){auto* message=new(std::nothrow) RemoteControlMessage{project,phone,action};if(!message||!g_hwnd||!PostMessageW(g_hwnd,WM_REMOTE_CONTROL,0,reinterpret_cast<LPARAM>(message))){delete message;if(error)*error=L"桌面窗口未就绪";return false;}return true;});
    g_remote_agent.SetPairingCallback([](const aicli::agent::PendingPairing& pairing){auto* message=new(std::nothrow) aicli::agent::PendingPairing(pairing);if(!message||!g_hwnd||!PostMessageW(g_hwnd,WM_PAIRING_PENDING,0,reinterpret_cast<LPARAM>(message)))delete message;});
    g_remote_agent.SetStatusCallback([](bool online,const std::wstring& text){auto* message=new(std::nothrow) AgentStatusMessage{online,text};if(!message||!g_hwnd||!PostMessageW(g_hwnd,WM_AGENT_STATUS,0,reinterpret_cast<LPARAM>(message)))delete message;});
    aicli::agent::AgentConfig config;config.computer_name=g_store.GetSetting(L"computer_name",aicli::text::LocalComputerName());config.bind_address=g_store.GetSetting(L"listen_address",L"0.0.0.0");config.port=g_store.GetSetting(L"listen_port",L"4280");config.local_address=g_store.GetSetting(L"local_address");config.remote_address=g_store.GetSetting(L"remote_address");config.data_directory=aicli::DefaultDataDirectory();std::wstring error;if(!g_remote_agent.Start(&g_store,config,&error))SetText(g_status,L"Agent 启动失败: "+error);else SetText(g_status,L"就绪");UpdateDeviceHeader();
}

aicli::agent::AgentConfig CurrentAgentConfig() {
    aicli::agent::AgentConfig config;
    config.computer_name = g_store.GetSetting(
        L"computer_name", aicli::text::LocalComputerName());
    config.bind_address = g_store.GetSetting(L"listen_address", L"0.0.0.0");
    config.port = g_store.GetSetting(L"listen_port", L"4280");
    config.local_address = g_store.GetSetting(L"local_address");
    config.remote_address = g_store.GetSetting(L"remote_address");
    config.data_directory = aicli::DefaultDataDirectory();
    return config;
}

bool RestartAgent(const aicli::agent::AgentConfig& config, std::wstring* error) {
    g_remote_agent.Stop();
    if (!g_remote_agent.Start(&g_store, config, error)) {
        if (g_status) SetText(g_status, L"Agent 启动失败: " + (error ? *error : L"未知错误"));
        return false;
    }
    if (g_status) SetText(g_status, L"Agent 服务已重启");
    UpdateDeviceHeader();
    return true;
}

void ReloadPersistentModels() {
    g_project_types = g_store.LoadProjectTypes();
    g_groups = g_store.LoadGroups();
    g_projects = g_store.LoadProjects();
    if (g_groups.empty()) {
        g_store.AddGroup(L"默认分组");
        g_groups = g_store.LoadGroups();
    }
    if (g_group_name != kAllGroupsName && g_group_name != kStartedProjectsName && std::none_of(g_groups.begin(), g_groups.end(), [&](const ProjectGroup& value) {
            return value.name == g_group_name;
        })) {
        g_group_name = L"默认分组";
    }
    SetText(g_group, g_group_name);
    RefreshProjectList();
    RefreshProjectDetails();
    UpdateDeviceHeader();
    Layout();
}

void OpenSettingsDialog() {
    const auto result = aicli::ui::ShowManagementDialog(
        g_hwnd, aicli::ui::ManagementMode::Settings, g_store, g_remote_agent,
        [](const aicli::agent::AgentConfig& config, std::wstring* error) {
            return RestartAgent(config, error);
        },
        [](bool) { ApplyLiveTheme(); },
        [] { RefreshTaskbarWorkingIndicator(); });
    (void)result;
    // Theme selection is saved immediately, even when the dialog is closed
    // without pressing another save button. Repaint the complete workspace
    // only after the modal child has gone away.
    ApplyLiveTheme();
}

void OpenGroupsDialog() {
    const auto result = aicli::ui::ShowManagementDialog(
        g_hwnd, aicli::ui::ManagementMode::Groups, g_store, g_remote_agent);
    if (result.accepted && !result.selected_group.empty()) {
        g_group_name = result.selected_group;
        g_store.SetSetting(L"last_group", g_group_name);
    }
    ReloadPersistentModels();
}

void OpenDevicesDialog() {
    aicli::ui::ShowManagementDialog(g_hwnd, aicli::ui::ManagementMode::Devices,
                                    g_store, g_remote_agent);
    UpdateDeviceHeader();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void ExtendCustomTitleBar(HWND h){
    g_window_chrome.ExtendFrame(h);
}
void SetTitleBarActive(HWND h,bool active){
    if(!g_window_chrome.SetActive(active))return;
    ApplyWindowChromeTheme(false);
    g_window_chrome.InvalidateTitleBar(h);
}
LRESULT HitTestCustomFrame(HWND h,LPARAM l){
    return g_window_chrome.HitTest(h,l);
}
void ResizeModalHosts(HWND owner){
    // ModalWorkspaceScope subclasses the owner and repositions both popup
    // layers atomically.  This hook remains for the root lifecycle but must
    // never resize popup surfaces as child windows.
    (void)owner;
}
LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(g_project_drag_message&&m==g_project_drag_message)return HandleProjectDragListMessage(h,l);
    if(g_taskbar_button_created_message&&m==g_taskbar_button_created_message){g_taskbar_working_indicator.OnTaskbarButtonCreated();return 0;}
    switch(m){
    case WM_CREATE:{g_hwnd=h;if(g_taskbar_button_created_message)ChangeWindowMessageFilterEx(h,g_taskbar_button_created_message,MSGFLT_ALLOW,nullptr);g_taskbar_working_indicator.Attach(h);g_footer_status.Attach(h);g_terminal_snapshots.Attach(h,WM_TERMINAL_DIRTY);g_layout_metrics=aicli::ui::LayoutMetrics::ForWindow(h);RecreateMainFontsForDpi();
        g_group=CreateWindowW(L"BUTTON",g_group_name.c_str(),WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,168,8,154,32,h,(HMENU)(INT_PTR)ID_GROUP_DEFAULT,GetModuleHandleW(nullptr),nullptr);g_group_add=CreateWindowW(L"BUTTON",L"+",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,326,8,34,32,h,(HMENU)(INT_PTR)ID_GROUP_ADD,GetModuleHandleW(nullptr),nullptr);g_bind=CreateWindowW(L"BUTTON",L"绑定手机",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,365,8,100,32,h,(HMENU)(INT_PTR)ID_BIND_PHONE,GetModuleHandleW(nullptr),nullptr);g_devices=CreateWindowW(L"BUTTON",L"已绑定 0 台",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,470,8,112,32,h,(HMENU)(INT_PTR)ID_DEVICES,GetModuleHandleW(nullptr),nullptr);g_new_project=CreateWindowW(L"BUTTON",L"+",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,8,55,34,30,h,(HMENU)(INT_PTR)ID_NEW_PROJECT,GetModuleHandleW(nullptr),nullptr);g_settings=CreateWindowW(L"BUTTON",L"设置",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,1000,8,70,32,h,(HMENU)(INT_PTR)ID_SETTINGS,GetModuleHandleW(nullptr),nullptr);HWND controls[]={g_group,g_group_add,g_bind,g_devices,g_new_project,g_settings};for(HWND b:controls){SendMessageW(b,WM_SETFONT,(WPARAM)g_font,TRUE);EnableButtonHover(b);}
        g_sidebar_title=CreateWindowW(L"STATIC",L"项目列表",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_sidebar_title,WM_SETFONT,(WPARAM)g_font_sidebar_title,TRUE);g_search=aicli::ui::CreateStyledInput(h,ID_SEARCH_PROJECTS,g_font,L"搜索会话");aicli::ui::SetStyledInputTheme(g_search,g_theme);aicli::ui::SetStyledInputFocusBorderOnly(g_search);aicli::ui::SetStyledInputParentFrame(g_search,true,2);
        g_list=CreateWindowW(L"LISTBOX",nullptr,WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,0,0,0,0,h,(HMENU)(INT_PTR)ID_PROJECT_LIST,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_list,WM_SETFONT,(WPARAM)g_font_project_list,TRUE);SendMessageW(g_list,LB_SETITEMHEIGHT,0,ScaleUi(kProjectListItemHeight));g_project_drag_message=RegisterWindowMessageW(DRAGLISTMSGSTRING);MakeDragList(g_list);SetWindowSubclass(g_list,ProjectListSubclassProc,kProjectListSubclassId,0);RefreshProjectList();
        auto detailStatic=[&](const wchar_t* text){return CreateWindowW(L"STATIC",text,WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);};
        g_detail_title=detailStatic(L"选择一个项目");
        g_detail_subtitle=detailStatic(L"");
        g_detail_folder_label=detailStatic(L"项目目录");g_detail_folder=detailStatic(L"未设置");
        g_detail_command_label=detailStatic(L"启动命令");g_detail_command=detailStatic(L"未设置");
        g_detail_note_label=detailStatic(L"备注");g_detail_note=detailStatic(L"暂无备注");
        g_detail_state=detailStatic(L"● 尚未启动");
        g_start_project=CreateWindowW(L"BUTTON",L"▶  立即开始",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,0,0,0,0,h,(HMENU)(INT_PTR)ID_START_PROJECT,GetModuleHandleW(nullptr),nullptr);
        g_edit_project=CreateWindowW(L"BUTTON",L"\xE70F",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,0,0,0,0,h,(HMENU)(INT_PTR)ID_EDIT_PROJECT,GetModuleHandleW(nullptr),nullptr);
        g_open_project_folder=CreateWindowW(L"BUTTON",L"\xE8B7",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,0,0,0,0,h,(HMENU)(INT_PTR)ID_OPEN_PROJECT_FOLDER,GetModuleHandleW(nullptr),nullptr);
        for(HWND c:{g_detail_title,g_detail_subtitle,g_detail_folder_label,g_detail_folder,g_detail_command_label,g_detail_command,g_detail_note_label,g_detail_note,g_detail_state,g_start_project,g_edit_project,g_open_project_folder})SendMessageW(c,WM_SETFONT,(WPARAM)g_font,TRUE);
        SendMessageW(g_detail_title,WM_SETFONT,(WPARAM)g_font_project_heading,TRUE);
        SendMessageW(g_detail_subtitle,WM_SETFONT,(WPARAM)g_font_small,TRUE);
        SendMessageW(g_detail_folder,WM_SETFONT,(WPARAM)g_font_monospace,TRUE);
        SendMessageW(g_detail_command,WM_SETFONT,(WPARAM)g_font_monospace,TRUE);
        for(HWND b:{g_start_project,g_edit_project,g_open_project_folder})EnableButtonHover(b);
        g_detail_tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(g_detail_tooltip){for(auto entry:{std::pair<HWND,const wchar_t*>{g_edit_project,L"编辑项目"},{g_open_project_folder,L"打开项目文件夹"}}){TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=h;tool.uId=reinterpret_cast<UINT_PTR>(entry.first);tool.lpszText=const_cast<LPWSTR>(entry.second);SendMessageW(g_detail_tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));}}
        g_status=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,0,0,h,(HMENU)(INT_PTR)ID_SETTINGS,GetModuleHandleW(nullptr),nullptr);ApplyMainFontsToControls();
        g_island.Attach(h,RECT{250,60,500,400});g_island.SetTheme(g_light_theme);RefreshProjectDetails();ExtendCustomTitleBar(h);SetWindowPos(h,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);Layout();ConfigureRemoteAgent();UpdateHeaderTelemetry();SetTimer(h,1,2000,nullptr);PostMessageW(h,WM_RESTORE_LAST_SESSION,0,0);return 0;}
    case WM_ACTIVATE:
        // An owned modal surface becomes the thread's active window while it
        // is open.  That is an internal focus change, not an application
        // deactivation; repainting the custom title bar here used to make it
        // jump to the inactive palette and visibly flash.  WM_ACTIVATEAPP is
        // the authoritative signal for crossing the application boundary.
        if (!aicli::ui::HasAnyActiveModal()) {
            SetTitleBarActive(h,LOWORD(w)!=WA_INACTIVE);
        }
        break;
    case WM_ACTIVATEAPP:SetTitleBarActive(h,w!=FALSE);break;
    case WM_DWMCOMPOSITIONCHANGED:ExtendCustomTitleBar(h);return 0;
    case WM_DPICHANGED: {
        // Per-Monitor-V2 gives Windows ownership of the correct client/frame
        // geometry when this window moves between screens.  Applying the
        // suggested rectangle avoids a stale DPI-sized surface that Windows
        // would otherwise bitmap-scale for a frame.
        const auto* suggested = reinterpret_cast<const RECT*>(l);
        if (suggested) {
            SetWindowPos(h, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        g_layout_metrics=aicli::ui::LayoutMetrics::ForWindow(h);
        RecreateMainFontsForDpi();
        Layout();
        InvalidateRect(h,nullptr,FALSE);
        return 0;
    }
    case WM_NCCALCSIZE:if(w){auto* params=(NCCALCSIZE_PARAMS*)l;if(IsZoomed(h)){HMONITOR monitor=MonitorFromRect(&params->rgrc[0],MONITOR_DEFAULTTONEAREST);MONITORINFO info{sizeof(info)};if(GetMonitorInfoW(monitor,&info))params->rgrc[0]=info.rcWork;}return 0;}break;
    case WM_NCHITTEST:return HitTestCustomFrame(h,l);
    case WM_MOUSEMOVE:{POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};const bool over_git=g_footer_status.OnMouseMove(point,g_theme);const bool over_chrome=g_window_chrome.OnMouseMove(h,point);if(over_git||over_chrome)return 0;break;}
    case WM_MOUSELEAVE:g_footer_status.OnMouseLeave();g_window_chrome.OnMouseLeave(h);return 0;
    case WM_LBUTTONDOWN:{POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};if(g_window_chrome.OnLeftButtonDown(h,point))return 0;break;}
    case WM_LBUTTONUP:{POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};const auto action=g_window_chrome.OnLeftButtonUp(h,point);if(action.has_value()){if(*action)ExecuteTitleButton(h,*action);return 0;}break;}
    case WM_CAPTURECHANGED:g_window_chrome.OnCaptureChanged(h);break;
    case WM_MEASUREITEM:{auto* item=(MEASUREITEMSTRUCT*)l;if(item->CtlType==ODT_MENU){const auto* entry=reinterpret_cast<const ContextMenuEntry*>(item->itemData);item->itemWidth=ScaleUi(218);item->itemHeight=entry&&!entry->label[0]?ScaleUi(10):ScaleUi(38);return TRUE;}if(item->CtlID==ID_PROJECT_LIST){item->itemHeight=ScaleUi(kProjectListItemHeight);return TRUE;}}break;
    case WM_DRAWITEM:{auto* item=(DRAWITEMSTRUCT*)l;if(item->CtlType==ODT_MENU){DrawContextMenuItem(item);return TRUE;}if(item->CtlType==ODT_BUTTON){DrawModernButton(item);return TRUE;}if(item->CtlID==ID_PROJECT_LIST){DrawProjectItem(item);return TRUE;}}break;
    case WM_SIZE:
        if(w==SIZE_MINIMIZED){g_island.SetVisible(false);return 0;}
        // Remove the compositor rim while maximized, then restore it with the
        // normal shadow/border as soon as the window returns to a floating
        // size. This keeps the client surface flush with every monitor edge.
        g_window_chrome.UpdateSizeState(
            h,g_light_theme,g_light_theme?border:RGB(18,21,27));
        if(aicli::ui::HasActiveModal(h))ResizeModalHosts(h);
        Layout();
        // Moving the footer's child status HWND does not reliably invalidate
        // its previous position during the first maximize transition. Since
        // this window intentionally suppresses WM_ERASEBKGND, that stale
        // strip can survive until a later restore/resize. Repaint the whole
        // client composition now so the old footer is covered immediately.
        RedrawWindow(h, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
        return 0;
    case WM_TERMINAL_DIRTY:g_terminal_snapshots.MarkDirtyMessageHandled();if(!g_terminal_snapshot_timer_active){SetTimer(h,kTerminalSnapshotTimerId,200,nullptr);g_terminal_snapshot_timer_active=true;}return 0;
    case WM_TIMER:if(g_footer_status.HandleTimer(w))return 0;if(w==1){RefreshGitStatus();UpdateHeaderTelemetry();return 0;}if(w==kTerminalSnapshotTimerId){if(g_terminal_snapshots.TakeDirty())g_terminal_snapshots.Publish(g_island,g_conversation_bridge,g_terminal_streams);else{KillTimer(h,kTerminalSnapshotTimerId);g_terminal_snapshot_timer_active=false;}return 0;}if(w==kProjectSpinnerTimerId){g_project_spinner_angle=std::fmod(g_project_spinner_angle+18.0f,360.0f);g_project_idle_pulse_angle=std::fmod(g_project_idle_pulse_angle+9.0f,360.0f);if(!InvalidateWorkingProjectRows())StopProjectSpinnerTimer();return 0;}if(w==kGitSpinnerTimerId){if(!g_footer_status.AdvanceRepositorySpinner())KillTimer(h,kGitSpinnerTimerId);return 0;}break;
    case WM_GETMINMAXINFO:{auto* info=(MINMAXINFO*)l;const auto metrics=aicli::ui::LayoutMetrics::ForWindow(h);info->ptMinTrackSize.x=metrics.Scale(900);info->ptMinTrackSize.y=metrics.Scale(600);return 0;}
    case WM_ERASEBKGND: return 1;
    case WM_CONTEXTMENU: if((HWND)w==g_list){ShowProjectContextMenu(h,l);return 0;} break;
    case WM_CTLCOLORSTATIC: {HDC dc=(HDC)w;HWND control=(HWND)l;const bool onPanel=control==g_sidebar_title||control==g_status;const bool detailValue=control==g_detail_folder||control==g_detail_command||control==g_detail_note;const bool detailLabel=control==g_detail_subtitle||control==g_detail_folder_label||control==g_detail_command_label||control==g_detail_note_label;const bool detail=control==g_detail_title||detailLabel||detailValue||control==g_detail_state;if(detail){const COLORREF detailSurface=g_light_theme?RGB(239,243,248):RGB(18,21,27);SetBkMode(dc,OPAQUE);SetBkColor(dc,detailValue?detailSurface:bg);SetTextColor(dc,control==g_detail_state?(g_light_theme?RGB(177,113,16):RGB(245,178,72)):(detailLabel?muted:fg));return (LRESULT)(detailValue?g_detail_surface_brush:g_bg_brush);}SetBkMode(dc,OPAQUE);SetTextColor(dc,fg);SetBkColor(dc,onPanel?panel:bg);return (LRESULT)(onPanel?g_list_brush:g_bg_brush);}
    case WM_CTLCOLOREDIT: {
        HDC dc=(HDC)w;
        HWND control=(HWND)l;
        const bool focused=GetFocus()==control;
        const COLORREF field_color=focused?g_theme.field_focus:g_theme.field;
        SetTextColor(dc,fg);
        SetBkColor(dc,field_color);
        return (LRESULT)(focused?(g_focus_brush?g_focus_brush:GetStockObject(WHITE_BRUSH)):
                                   (g_edit_brush?g_edit_brush:GetStockObject(WHITE_BRUSH)));
    }
    case WM_CTLCOLORLISTBOX: {HDC dc=(HDC)w;SetTextColor(dc,fg);SetBkColor(dc,panel);return (LRESULT)(g_list_brush?g_list_brush:GetStockObject(NULL_BRUSH));}
    case WM_COMMAND:{
        const UINT id=LOWORD(w),notification=HIWORD(w);
        if(id==ID_CLOSE){SendMessageW(h,WM_CLOSE,0,0);return 0;}
        if(id==ID_MINIMIZE){ShowWindow(h,SW_MINIMIZE);return 0;}
        if(id==ID_MAXIMIZE){ShowWindow(h,IsZoomed(h)?SW_RESTORE:SW_MAXIMIZE);Layout();return 0;}
        // Root toolbar commands are blocked while an owned modal is active;
        // caption buttons continue to work through the custom frame path.
        if(aicli::ui::HasActiveModal(h))return 0;
        if(id==ID_SEARCH_PROJECTS&&notification==EN_CHANGE){RefreshProjectList();Layout();return 0;}
        if(id==ID_PROJECT_LIST&&notification==LBN_SELCHANGE){RefreshProjectDetails(true);Layout();return 0;}
        if(id==ID_PROJECT_LIST&&notification==LBN_DBLCLK){StartSession();return 0;}
        if(id==ID_NEW_PROJECT){CreateProject();return 0;}
        if(id==ID_START_PROJECT){StartSession();return 0;}
        if(id==ID_EDIT_PROJECT){EditProject();return 0;}
        if(id==ID_OPEN_PROJECT_FOLDER){OpenProjectFolder();return 0;}
        if(id==ID_CTX_OPEN_FOLDER){OpenProjectFolder();return 0;}
        if(id==ID_CTX_OPEN_CMD){OpenProjectCommandPrompt();return 0;}
        const UINT moveGroupEnd=static_cast<UINT>(ID_CTX_MOVE_GROUP_BASE)+static_cast<UINT>(g_groups.size());
        if(id>=static_cast<UINT>(ID_CTX_MOVE_GROUP_BASE)&&id<moveGroupEnd){MoveSelectedProjectToGroup(g_groups[id-ID_CTX_MOVE_GROUP_BASE].name);return 0;}
        if(id==ID_CTX_MOVE_DEFAULT){MoveSelectedProjectToDefaultGroup();return 0;}
        if(id==ID_CTX_RESTART){StartSession(true);return 0;}
        if(id==ID_CTX_PAUSE){
            const int selected=SelectedProjectIndex();
            if(selected>=0&&selected<(int)g_projects.size()){
                auto& project=g_projects[selected];
                SetProjectWorkingState(project.id,false);
                g_island.Stop(project.id);
                project.status=L"paused";
                g_store.UpdateProjectStatus(project.id,project.status);
            }
            SetText(g_status,L"会话已暂停");
            RefreshProjectDetails();RefreshProjectList();Layout();return 0;
        }
        if(id==ID_CTX_CONTINUE){StartSession();return 0;}
        if(id==ID_CTX_EDIT){EditProject();return 0;}
        if(id==ID_CTX_DELETE){DeleteProject();return 0;}
        if(id==ID_BIND_PHONE){ShowBindingDialog(h);return 0;}
        if(id==ID_DEVICES){OpenDevicesDialog();Layout();return 0;}
        if(id==ID_SETTINGS){
            if(!g_settings_open_posted){
                g_settings_open_posted=true;
                PostMessageW(h,WM_OPEN_SETTINGS,0,0);
            }
            return 0;
        }
        if(id==ID_GROUP_DEFAULT){ShowGroupDropdown();return 0;}
        if(id==ID_GROUP_ADD){CreateGroupFromHeader();Layout();return 0;}
        return 0;
    }
    case WM_CLOSE: {
        if(g_close_confirmation_open)return 0;
        g_close_confirmation_open=true;
        const bool hasRunningSession=std::any_of(
            g_projects.begin(),g_projects.end(),[](const Project& project){
                return g_island.HasSession(project.id);
            });
        const wchar_t* prompt=hasRunningSession
            ?L"确定要关闭 AiCli 吗？\n\n当前正在运行的终端会话将同时结束。"
            :L"确定要关闭 AiCli 吗？";
        const int answer=MessageBoxW(
            h,prompt,L"确认关闭",
            MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2|MB_APPLMODAL);
        g_close_confirmation_open=false;
        if(answer==IDYES)DestroyWindow(h);
        return 0;
    }
    case WM_OPEN_SETTINGS:
        g_settings_open_posted=false;
        if(!aicli::ui::HasActiveModal(h)){
            // BN_CLICKED is sent from inside the BUTTON's mouse-up handling.
            // Even after that message returns, DWM may still have the pressed
            // redirection surface queued.  Publish the released owner-draw
            // frame and wait for the compositor before creating the popup.
            if(g_settings&&IsWindow(g_settings)){
                SendMessageW(g_settings,BM_SETSTATE,FALSE,0);
                RedrawWindow(g_settings,nullptr,nullptr,
                             RDW_INVALIDATE|RDW_UPDATENOW|RDW_NOERASE);
            }
            DwmFlush();
            OpenSettingsDialog();
            ReloadPersistentModels();
        }
        return 0;
    case WM_RESTORE_LAST_SESSION:if(!g_startup_project_id.empty()&&SelectProjectById(g_startup_project_id)){g_startup_project_id.clear();StartSession();}return 0;
    case WM_REMOTE_INPUT:{
        std::unique_ptr<std::shared_ptr<RemoteInputMessage>> envelope(
            reinterpret_cast<std::shared_ptr<RemoteInputMessage>*>(l));
        if(!envelope||!*envelope)return 0;
        const auto request=*envelope;
        auto complete=[&](bool accepted,const std::wstring& error={}){
            request->accepted=accepted;
            request->error=error;
            SetEvent(request->completed);
        };
        if(request->cancelled){complete(false,L"输入请求已超时");return 0;}
        if(!SelectProjectById(request->project_id)){
            complete(false,L"项目或会话不存在");
            return 0;
        }
        if(!g_island.HasSession(request->project_id))StartSession();
        if(request->cancelled){complete(false,L"输入请求已超时");return 0;}
        if(!g_island.HasSession(request->project_id)){
            complete(false,L"终端会话未就绪");
            return 0;
        }
        std::wstring text=request->input;
        bool submit=false;
        while(!text.empty()&&(text.back()==L'\r'||text.back()==L'\n')){
            submit=true;
            text.pop_back();
        }
        const bool accepted=submit
            ? g_island.SubmitInput(request->project_id,text)
            : g_island.WriteInput(request->project_id,text);
        if(accepted){
            RecordAndBroadcastEvent(request->project_id,L"terminal.input",
                nlohmann::json{{"input",Utf8Text(text)},{"phoneId",Utf8Text(request->phone_id)}});
            complete(true);
        }else complete(false,L"终端输入失败");
        return 0;
    }
    case WM_REMOTE_KEY:{
        std::unique_ptr<std::shared_ptr<RemoteKeyMessage>> envelope(
            reinterpret_cast<std::shared_ptr<RemoteKeyMessage>*>(l));
        if(!envelope||!*envelope)return 0;
        const auto request=*envelope;
        auto complete=[&](bool accepted,const std::wstring& error={}){
            request->accepted=accepted;
            request->error=error;
            SetEvent(request->completed);
        };
        if(request->cancelled){complete(false,L"按键请求已超时");return 0;}
        if(!SelectProjectById(request->project_id)){
            complete(false,L"项目或会话不存在");
            return 0;
        }
        if(!g_island.HasSession(request->project_id))StartSession();
        if(request->cancelled){complete(false,L"按键请求已超时");return 0;}
        if(!g_island.HasSession(request->project_id)){
            complete(false,L"终端会话未就绪");
            return 0;
        }
        UINT virtualKey=0;
        if(request->key==L"left")virtualKey=VK_LEFT;
        else if(request->key==L"right")virtualKey=VK_RIGHT;
        else if(request->key==L"up")virtualKey=VK_UP;
        else if(request->key==L"down")virtualKey=VK_DOWN;
        else if(request->key==L"enter")virtualKey=VK_RETURN;
        if(!virtualKey){complete(false,L"不支持的终端按键");return 0;}
        if(g_island.SendKey(request->project_id,virtualKey))complete(true);
        else complete(false,L"终端按键发送失败");
        return 0;
    }
    case WM_REMOTE_CONTROL:{
        std::unique_ptr<RemoteControlMessage> message(reinterpret_cast<RemoteControlMessage*>(l));
        if(!message||!SelectProjectById(message->project_id))return 0;
        const int selected=SelectedProjectIndex();
        if(message->action==L"restart")StartSession(true);
        else if(message->action==L"continue"||message->action==L"start")StartSession();
        else if(message->action==L"pause"||message->action==L"cancel"){
            const bool pause=message->action==L"pause";
            SetProjectWorkingState(message->project_id,false);
            g_island.Stop(message->project_id);
            if(selected>=0&&selected<(int)g_projects.size()){
                g_projects[selected].status=pause?L"paused":L"cancelled";
                g_store.UpdateProjectStatus(g_projects[selected].id,g_projects[selected].status);
                RecordAndBroadcastEvent(g_projects[selected].id,pause?L"conversation.paused":L"conversation.cancelled",nlohmann::json{{"status",pause?"paused":"cancelled"}});
            }
            RefreshProjectList();RefreshProjectDetails();Layout();
        }else if(message->action==L"ctrl_c")g_island.WriteInput(message->project_id,std::wstring(1,L'\x03'));
        return 0;
    }
    case WM_PAIRING_PENDING:{std::unique_ptr<aicli::agent::PendingPairing> pairing(reinterpret_cast<aicli::agent::PendingPairing*>(l));if(!pairing)return 0;g_pending_pairing=*pairing;if(g_binding_dialog){LayoutBindingDialog(g_binding_dialog);InvalidateRect(g_binding_dialog,nullptr,FALSE);}else if(pairing->submitted&&!pairing->authorized&&!pairing->rejected){const std::wstring text=L"手机名称："+pairing->device.name+L"\n型号："+pairing->device.model+L"\n设备 ID："+pairing->device.phone_id+L"\n\n确认后该手机可远程操作管理员终端。";const bool approve=MessageBoxW(h,text.c_str(),L"确认绑定手机",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES;std::wstring error;if(!g_remote_agent.ConfirmPairing(pairing->info.pairing_id,approve,&error)&&!error.empty())MessageBoxW(h,error.c_str(),L"手机绑定",MB_OK|MB_ICONERROR);}UpdateDeviceHeader();return 0;}
    case WM_AGENT_STATUS:{std::unique_ptr<AgentStatusMessage> message(reinterpret_cast<AgentStatusMessage*>(l));if(message&&!message->online)SetText(g_status,message->text);UpdateHeaderTelemetry();return 0;}
    case WM_AGENT_WORKING_STATE:{std::unique_ptr<AgentWorkingStateMessage> message(reinterpret_cast<AgentWorkingStateMessage*>(l));if(message)SetProjectWorkingState(message->project_id,message->working);return 0;}
    case WM_GIT_STATUS_READY:{
        std::unique_ptr<GitStatusMessage> message(
            reinterpret_cast<GitStatusMessage*>(l));
        if(!message)return 0;
        if(message->periodic)g_git_status_query_active=false;
        if(message->request_id==g_git_status_request_id.load()){
            g_footer_status.CompleteRepositoryRefresh(
                message->project_folder,std::move(message->repository));
            KillTimer(h,kGitSpinnerTimerId);
        }
        return 0;
    }
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC target_dc=BeginPaint(h,&ps);
        RECT r{};
        GetClientRect(h,&r);
        const auto metrics=aicli::ui::LayoutMetrics::ForWindow(h);
        if(!g_font_git)RecreateMainFontsForDpi();
        aicli::ui::PaintMainWindow(
            target_dc,r,ps.rcPaint,h,g_search,g_app_icon_large,g_font_medium,g_font_small,g_font_git,
            metrics,g_theme,g_window_chrome,g_footer_status);
        if(!IsRectEmpty(&g_prestart_panel)){
            const COLORREF surface=g_light_theme?RGB(239,243,248):RGB(18,21,27);
            for(const RECT& rect:{g_prestart_folder_surface,g_prestart_command_surface,g_prestart_note_surface}){
                if(!IsRectEmpty(&rect))aicli::ui::DrawSmoothRoundedRect(target_dc,rect,5.0f,surface,border,1.0f);
            }
        }
        EndPaint(h,&ps);
        return 0;
    }
    case WM_DESTROY:++g_git_status_request_id;KillTimer(h,kGitSpinnerTimerId);StopProjectSpinnerTimer();g_taskbar_working_indicator.Detach();g_terminal_snapshots.Detach();g_footer_status.Detach();g_remote_agent.Stop();g_island.StopAll();EndProjectDragPreview();if(g_detail_tooltip)DestroyWindow(g_detail_tooltip);if(g_font)DeleteObject(g_font);if(g_font_medium)DeleteObject(g_font_medium);if(g_font_small)DeleteObject(g_font_small);if(g_font_sidebar_title)DeleteObject(g_font_sidebar_title);if(g_font_project_list)DeleteObject(g_font_project_list);if(g_font_modal_title)DeleteObject(g_font_modal_title);if(g_font_modal_subtitle)DeleteObject(g_font_modal_subtitle);if(g_font_project_heading)DeleteObject(g_font_project_heading);if(g_font_monospace)DeleteObject(g_font_monospace);if(g_icon_font)DeleteObject(g_icon_font);if(g_edit_brush){DeleteObject(g_edit_brush);g_edit_brush=nullptr;}if(g_focus_brush){DeleteObject(g_focus_brush);g_focus_brush=nullptr;}if(g_list_brush){DeleteObject(g_list_brush);g_list_brush=nullptr;}if(g_bg_brush){DeleteObject(g_bg_brush);g_bg_brush=nullptr;}if(g_detail_surface_brush){DeleteObject(g_detail_surface_brush);g_detail_surface_brush=nullptr;}PostQuitMessage(0);return 0; }
    if(m==WM_NCDESTROY&&g_font_git){
        DeleteObject(g_font_git);
        g_font_git=nullptr;
    }
    return DefWindowProcW(h,m,w,l);
}
}

int WINAPI wWinMain(HINSTANCE instance,
                    HINSTANCE,
                    PWSTR arguments,
                    int show_command) {
    // Must run before any HWND, GDI or XAML object is created. The manifest
    // is the authoritative declaration; this call makes the intent explicit
    // for loose/unpackaged launches as well. It can harmlessly return FALSE
    // when the manifest has already established this context.
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    GdiplusRuntime gdiplus;
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    int activation = 1;
#ifndef AICLI_SKIP_PACKAGED_ACTIVATION
    activation = aicli::app::EnsurePackagedActivation(
        kApplicationUserModelId, arguments);
#else
    (void)arguments;
#endif
    if (activation < 0) {
        MessageBoxW(nullptr,
                    L"无法启动已注册的 AI CLI 桌面实例，请先完成包注册。",
                    L"AI CLI", MB_OK | MB_ICONERROR);
        winrt::uninit_apartment();
        return 1;
    }
    if (activation == 0) {
        winrt::uninit_apartment();
        return 0;
    }

    aicli::app::SingleInstanceGuard single_instance;
    std::wstring single_instance_error;
    const auto instance_result = single_instance.Acquire(&single_instance_error);
    if (instance_result == aicli::app::SingleInstanceResult::activated_existing) {
        winrt::uninit_apartment();
        return 0;
    }
    if (instance_result == aicli::app::SingleInstanceResult::existing_starting) {
        MessageBoxW(nullptr, L"AiCli 已在启动，请稍候。", L"AiCli",
                    MB_OK | MB_ICONINFORMATION);
        winrt::uninit_apartment();
        return 0;
    }
    if (instance_result == aicli::app::SingleInstanceResult::error) {
        MessageBoxW(nullptr,
                    single_instance_error.empty() ? L"无法检查正在运行的 AiCli 实例。"
                                                  : single_instance_error.c_str(),
                    L"AiCli", MB_OK | MB_ICONERROR);
        winrt::uninit_apartment();
        return 1;
    }
    if (!InitializePersistentState()) {
        winrt::uninit_apartment();
        return 1;
    }

    g_components.Initialize();
    INITCOMMONCONTROLSEX common_controls{
        sizeof(common_controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&common_controls);

    g_taskbar_button_created_message=RegisterWindowMessageW(L"TaskbarButtonCreated");
    g_app_icon_large=static_cast<HICON>(LoadImageW(
        instance,MAKEINTRESOURCEW(IDI_AICLI_APP),IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR|LR_SHARED));
    g_app_icon_small=static_cast<HICON>(LoadImageW(
        instance,MAKEINTRESOURCEW(IDI_AICLI_APP),IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR|LR_SHARED));
    if(!g_app_icon_large)g_app_icon_large=LoadIconW(nullptr,IDI_APPLICATION);
    if(!g_app_icon_small)g_app_icon_small=g_app_icon_large;

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = WndProc;
    window_class.lpszClassName = kClassName;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hIcon = g_app_icon_large;
    window_class.hIconSm = g_app_icon_small;
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&window_class);

    constexpr DWORD window_style =
        WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
        WS_CLIPCHILDREN;
    const UINT initial_dpi = std::max<UINT>(96, GetDpiForSystem());
    g_hwnd = CreateWindowExW(0, kClassName, L"\x200B", window_style,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(1100, initial_dpi, 96),
                             MulDiv(700, initial_dpi, 96),
                             nullptr, nullptr, instance, nullptr);
    ApplyWindowChromeTheme();
    ShowWindow(g_hwnd, show_command);
    UpdateWindow(g_hwnd);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        if (g_island.PreTranslateMessage(message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_store.Close();
    g_island.Shutdown();
    g_components.Shutdown();
    winrt::uninit_apartment();
    return 0;
}
