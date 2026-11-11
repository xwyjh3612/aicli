#pragma once

#include "ui/management_dialog.h"
#include "ui/dialog_metrics.h"
#include "ui/overlay_scrollbar.h"
#include "ui/theme.h"
#include "app/auto_update.h"

// Private state shared by the management shell, pages, renderer and actions.
// Public callers depend only on management_dialog.h and the individual dialogs.
namespace aicli::ui::management
{
inline constexpr wchar_t kSettingsContentClassName[] = L"AicliSettingsContent";
inline constexpr UINT kPopulateMessage = WM_APP + 60;
enum : int
{
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
    kDownloadUpdate = 2338,
};

enum class SettingsPage
{
    Appearance,
    Connection,
    ProjectTypes,
    Groups,
    Devices,
    About,
};
struct State
{
    HWND window{};
    HWND owner{};
    ManagementMode mode{};
    DataStore *store{};
    agent::RemoteAgent *agent{};
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
    HWND download_update{};
    aicli::update::CheckStatus update_status;
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

State *GetState(HWND window);
State *FindState(HWND window);
bool IsSettingsProjectTypes(const State &state);
bool IsSettingsGroups(const State &state);
bool IsSettingsNavigationId(int id);
bool IsActiveSettingsNavigation(const State &state, int id);
HWND SettingsNavigationControl(const State &state, SettingsPage page);
COLORREF SettingsHeaderColor(const State &state);
COLORREF BlendBackdrop(COLORREF base, COLORREF overlay, BYTE alpha);
std::wstring FormatDeviceTime(std::int64_t timestamp);
const ThemePalette &DarkPalette();
void ApplyTheme(State &state, bool light);
HWND MakeButton(State &state, const wchar_t *text, int id, HWND parent = nullptr);
HWND MakeField(State &state, int id, const wchar_t *placeholder, HWND parent = nullptr);
void SetField(HWND field, const std::wstring &value);
std::wstring Field(HWND field);
void AddLine(HDC dc, const wchar_t *text, int x, int y, int width, HFONT font, COLORREF color);
void DrawButton(const DRAWITEMSTRUCT *item);
void DrawListItem(const DRAWITEMSTRUCT *item);
void Layout(State &state);
void Paint(State &state, HDC dc);
int SettingsContentExtent(SettingsPage page);
void SetSettingsScroll(State &state, int position);
void LayoutSettingsContent(State &state);
void PaintSettingsContent(State &state, HDC dc);
void RefreshUpdateStatus(State &state, bool force = false);
void UpdateSettingsPage(State &state, SettingsPage page);
LRESULT CALLBACK SettingsContentProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK SettingsWheelProc(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                   UINT_PTR subclass_id, DWORD_PTR reference);
void RefreshList(State &state);
void ReadSelection(State &state);
bool SelectedIndex(HWND list, size_t &index);
void ShowError(HWND owner, const std::wstring &message);
bool SetDefaultProjectTypeByName(DataStore &store, const std::wstring &name, std::wstring *error);
void CreateControls(State &state);
void PopulateControls(State &state);
LRESULT HandleCommand(State &state, WPARAM wparam);
constexpr UINT_PTR kSettingsWheelSubclassId = 0xA1C19;
constexpr UINT_PTR kDevicePresenceTimerId = 0xA1C32;
constexpr UINT_PTR kUpdateStatusTimerId = 0xA1C33;
constexpr int kSettingsHeaderHeight = 40;

} // namespace aicli::ui::management
