#include "ui/management_dialog.h"

#include "ui/styled_input.h"

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
};

constexpr COLORREF kBg = RGB(12, 14, 18);
constexpr COLORREF kPanel = RGB(28, 31, 37);
constexpr COLORREF kPanelEdge = RGB(60, 66, 76);
constexpr COLORREF kText = RGB(239, 242, 247);
constexpr COLORREF kMuted = RGB(143, 153, 168);
constexpr COLORREF kField = RGB(37, 40, 47);
constexpr COLORREF kFieldFocus = RGB(43, 48, 57);
constexpr COLORREF kAccent = RGB(67, 139, 224);

struct State {
    HWND window{};
    HWND owner{};
    ManagementMode mode{};
    DataStore* store{};
    agent::RemoteAgent* agent{};
    AgentRestartCallback restart_agent;
    ManagementDialogResult result;
    HBITMAP snapshot{};
    SIZE snapshot_size{};
    RECT panel{};
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
    HWND add{};
    HWND save{};
    HWND remove{};
    HWND select{};
    HWND close{};
    HWND types{};
    HWND unbind{};
    HWND firewall{};
    std::vector<ProjectGroup> groups;
    std::vector<ProjectType> types_data;
    std::vector<DeviceRecord> devices;
    bool light_theme{};
    COLORREF background_color{kBg};
    COLORREF panel_color{kPanel};
    COLORREF panel_edge{kPanelEdge};
    COLORREF text_color{kText};
    COLORREF muted_color{kMuted};
    COLORREF field_color{kField};
    COLORREF field_focus_color{kFieldFocus};
    COLORREF accent_color{kAccent};
};

State* GetState(HWND h) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

void ApplyTheme(State& state, bool light) {
    state.light_theme = light;
    state.background_color = light ? RGB(246, 248, 251) : kBg;
    state.panel_color = light ? RGB(255, 255, 255) : kPanel;
    state.panel_edge = light ? RGB(215, 221, 230) : kPanelEdge;
    state.text_color = light ? RGB(28, 34, 43) : kText;
    state.muted_color = light ? RGB(101, 111, 125) : kMuted;
    state.field_color = light ? RGB(239, 243, 248) : kField;
    state.field_focus_color = light ? RGB(231, 238, 249) : kFieldFocus;
    state.accent_color = light ? RGB(46, 118, 211) : kAccent;
}

void DeleteSnapshot(State& state) {
    if (state.snapshot) DeleteObject(state.snapshot);
    state.snapshot = nullptr;
    state.snapshot_size = {};
}

void CaptureSnapshot(State& state) {
    RECT client{};
    if (!GetClientRect(state.owner, &client)) return;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = (screen && memory) ? CreateCompatibleBitmap(screen, width, height) : nullptr;
    if (!screen || !memory || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        return;
    }
    HBITMAP old = static_cast<HBITMAP>(SelectObject(memory, bitmap));
    POINT origin{0, 0};
    ClientToScreen(state.owner, &origin);
    BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY);
    SelectObject(memory, old);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    state.snapshot = bitmap;
    state.snapshot_size = {width, height};
}

HWND MakeButton(State& state, const wchar_t* text, int id) {
    HWND button = CreateWindowExW(0, L"BUTTON", text,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  0, 0, 1, 1, state.window,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    if (button) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    return button;
}

HWND MakeField(State& state, int id, const wchar_t* placeholder) {
    return CreateStyledInput(state.window, id, state.font, placeholder);
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
    State* state = GetState(GetParent(item->hwndItem));
    const COLORREF text_color = state ? state->text_color : kText;
    const COLORREF muted_color = state ? state->muted_color : kMuted;
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    RECT rect = item->rcItem;
    COLORREF fill = selected ? RGB(42, 96, 165) : (state && state->light_theme ? RGB(239, 243, 248) : RGB(40, 44, 52));
    COLORREF edge = selected ? RGB(92, 163, 241) : (state && state->light_theme ? RGB(195, 204, 216) : RGB(67, 75, 88));
    if (item->CtlID == kDelete || item->CtlID == kUnbind) {
        fill = selected ? RGB(126, 48, 59) : RGB(56, 40, 45);
        edge = RGB(116, 65, 77);
    }
    if (disabled) {
        fill = state && state->light_theme ? RGB(230, 234, 240) : RGB(32, 35, 41);
        edge = state && state->light_theme ? RGB(205, 211, 220) : RGB(50, 54, 62);
    }
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ old_brush = SelectObject(item->hDC, brush);
    HGDIOBJ old_pen = SelectObject(item->hDC, pen);
    RoundRect(item->hDC, rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1, 9, 9);
    SelectObject(item->hDC, old_brush);
    SelectObject(item->hDC, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
    wchar_t text[256]{};
    GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, disabled ? muted_color : text_color);
    RECT text_rect = rect;
    DrawTextW(item->hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawListItem(const DRAWITEMSTRUCT* item) {
    if (!item || item->itemID == static_cast<UINT>(-1)) return;
    State* state = GetState(GetParent(item->hwndItem));
    if (!state) return;
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    HBRUSH fill = CreateSolidBrush(selected ? (state->light_theme ? RGB(226, 237, 251) : RGB(31, 57, 87))
                                            : (state->light_theme ? RGB(250, 251, 253) : RGB(31, 34, 41)));
    FillRect(item->hDC, &item->rcItem, fill);
    DeleteObject(fill);
    wchar_t text[512]{};
    SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, selected ? state->text_color : (state->light_theme ? RGB(67, 76, 89) : RGB(207, 214, 225)));
    SelectObject(item->hDC, state->font);
    RECT rect{item->rcItem.left + 12, item->rcItem.top, item->rcItem.right - 12, item->rcItem.bottom};
    DrawTextW(item->hDC, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void Layout(State& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int width = client.right;
    const int height = client.bottom;
    const int panel_width = std::min(760, std::max(600, width - 100));
    const int panel_height = state.mode == ManagementMode::Settings ? 560 :
                             state.mode == ManagementMode::ProjectTypes ? 590 : 520;
    const int left = (width - panel_width) / 2;
    const int top = std::max(30, (height - panel_height) / 2);
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
        place(state.listen, {x, top + 116, right, top + 152});
        place(state.port, {x, top + 190, right, top + 226});
        place(state.local, {x, top + 264, right, top + 300});
        place(state.remote, {x, top + 338, right, top + 374});
        if (state.theme) MoveWindow(state.theme, x, top + 412, 220, 34, TRUE);
        button(state.close, x, button_y, 108, 36);
        button(state.firewall, x + 120, button_y, 158, 36);
        button(state.types, right - 310, button_y, 142, 36);
        button(state.save, right - 156, button_y, 156, 36);
    } else if (state.mode == ManagementMode::Devices) {
        if (state.list) MoveWindow(state.list, x, top + 100, right - x, 300, TRUE);
        button(state.unbind, right - 270, button_y, 126, 36);
        button(state.close, right - 126, button_y, 126, 36);
    } else {
        if (state.list) MoveWindow(state.list, x, top + 100, right - x, 235, TRUE);
        place(state.name, {x, top + 362, right, top + 398});
        if (state.command) place(state.command, {x, top + 436, right, top + 472});
        const int y = button_y;
        button(state.add, x, y, 108, 36);
        button(state.save, x + 118, y, 108, 36);
        button(state.remove, x + 236, y, 108, 36);
        if (state.select) button(state.select, right - 236, y, 108, 36);
        button(state.close, right - 118, y, 118, 36);
    }
    InvalidateRect(state.window, nullptr, FALSE);
}

void Paint(State& state, HDC dc) {
    RECT client{};
    GetClientRect(state.window, &client);
    if (state.snapshot && state.snapshot_size.cx == client.right && state.snapshot_size.cy == client.bottom) {
        HDC source = CreateCompatibleDC(dc);
        HBITMAP old = static_cast<HBITMAP>(SelectObject(source, state.snapshot));
        BitBlt(dc, 0, 0, client.right, client.bottom, source, 0, 0, SRCCOPY);
        SelectObject(source, old);
        DeleteDC(source);
        HBRUSH scrim = CreateSolidBrush(state.light_theme ? RGB(210, 216, 225) : RGB(17, 20, 26));
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 170, 0};
        HDC overlay = CreateCompatibleDC(dc);
        HBITMAP pixel = CreateCompatibleBitmap(dc, 1, 1);
        HBITMAP old_pixel = static_cast<HBITMAP>(SelectObject(overlay, pixel));
        RECT one{0, 0, 1, 1};
        FillRect(overlay, &one, scrim);
        AlphaBlend(dc, 0, 0, client.right, client.bottom, overlay, 0, 0, 1, 1, blend);
        SelectObject(overlay, old_pixel);
        DeleteObject(pixel);
        DeleteDC(overlay);
        DeleteObject(scrim);
    } else {
        HBRUSH background = CreateSolidBrush(state.background_color);
        FillRect(dc, &client, background);
        DeleteObject(background);
    }
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
    if (state.mode == ManagementMode::Settings) { title = L"设置"; subtitle = L"网络、主题和项目类型管理"; }
    else if (state.mode == ManagementMode::Groups) { title = L"分组管理"; subtitle = L"选择当前分组，或添加、重命名和删除空分组"; }
    else if (state.mode == ManagementMode::ProjectTypes) { title = L"项目类型"; subtitle = L"Codex / Claude 为内置类型，自定义类型可编辑"; }
    else if (state.mode == ManagementMode::Devices) { title = L"已绑定设备"; subtitle = L"解绑后该手机的令牌和实时连接立即失效"; }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, state.text_color);
    SelectObject(dc, state.title_font);
    RECT title_rect{x, state.panel.top + 24, state.panel.right - 34, state.panel.top + 54};
    DrawTextW(dc, title, -1, &title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, state.muted_color);
    SelectObject(dc, state.small_font);
    RECT sub_rect{x, state.panel.top + 57, state.panel.right - 34, state.panel.top + 82};
    DrawTextW(dc, subtitle, -1, &sub_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    auto label = [&](const wchar_t* text, int y) {
        SetTextColor(dc, state.light_theme ? RGB(74, 84, 98) : RGB(194, 201, 212));
        SelectObject(dc, state.small_font);
        RECT rect{x, state.panel.top + y, state.panel.right - 34, state.panel.top + y + 22};
        DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    if (state.mode == ManagementMode::Settings) {
        label(L"监听地址（0.0.0.0 表示所有网卡）", 88);
        label(L"HTTPS 端口", 162);
        label(L"本机/内网访问地址（二维码优先使用）", 236);
        label(L"公网或 Relay 地址（可留空）", 310);
        label(L"主题", 384);
    } else if (state.mode == ManagementMode::Groups) {
        label(L"分组列表", 88);
        label(L"分组名称", 346);
    } else if (state.mode == ManagementMode::ProjectTypes) {
        label(L"项目类型列表", 88);
        label(L"类型名称", 346);
        label(L"默认启动命令", 420);
    } else {
        label(L"已绑定手机（双击或选择后点击解绑）", 88);
    }
}

void RefreshList(State& state) {
    if (!state.list) return;
    SendMessageW(state.list, LB_RESETCONTENT, 0, 0);
    if (state.mode == ManagementMode::Groups) {
        state.groups = state.store->LoadGroups();
        for (const auto& value : state.groups) {
            LRESULT row = SendMessageW(state.list, LB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(value.name.c_str()));
            if (row != LB_ERR) SendMessageW(state.list, LB_SETITEMDATA, row,
                                             static_cast<LPARAM>(&value - state.groups.data()));
        }
    } else if (state.mode == ManagementMode::ProjectTypes) {
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
    if (state.mode == ManagementMode::ProjectTypes && index < state.types_data.size()) {
        SetField(state.name, state.types_data[index].name);
        SetField(state.command, state.types_data[index].default_command);
    }
    if (state.mode == ManagementMode::Groups && index < state.groups.size()) {
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
        state.listen = MakeField(state, kListen, L"例如 0.0.0.0");
        state.port = MakeField(state, kPort, L"例如 4280");
        state.local = MakeField(state, kLocal, L"例如 https://192.168.1.10:4280");
        state.remote = MakeField(state, kRemote, L"例如 https://relay.example.com/aicli");
        const auto theme = state.store->GetSetting(L"theme", L"dark");
        state.light_theme = theme == L"light";
        state.theme = MakeButton(state, state.light_theme ? L"白天模式  ▾" : L"夜间模式  ▾", kTheme);
        state.save = MakeButton(state, L"保存并重启 Agent", kSave);
        state.types = MakeButton(state, L"管理项目类型", kTypes);
        state.firewall = MakeButton(state, L"允许手机访问", kFirewall);
        state.close = MakeButton(state, L"关闭", kClose);
    } else if (state.mode == ManagementMode::Groups) {
        state.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                         LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                     0, 0, 1, 1, state.window,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
                                     GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state.list, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        state.name = MakeField(state, kName, L"输入分组名称");
        state.add = MakeButton(state, L"新增", kAdd);
        state.save = MakeButton(state, L"重命名", kSave);
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
        state.name = MakeField(state, kName, L"输入类型名称");
        state.command = MakeField(state, kCommand, L"输入默认启动命令");
        state.add = MakeButton(state, L"新增", kAdd);
        state.save = MakeButton(state, L"保存修改", kSave);
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
        SetBkColor(dc, GetFocus() == control ? state->field_focus_color : state->field_color);
        static HBRUSH normal = CreateSolidBrush(kField);
        static HBRUSH focused = CreateSolidBrush(kFieldFocus);
        // The static brushes keep the control path allocation-free; the
        // light theme uses a freshly created brush for its different fill.
        if (state->light_theme) {
            static HBRUSH light_normal = CreateSolidBrush(RGB(239, 243, 248));
            static HBRUSH light_focused = CreateSolidBrush(RGB(231, 238, 249));
            return reinterpret_cast<LRESULT>(GetFocus() == control ? light_focused : light_normal);
        }
        return reinterpret_cast<LRESULT>(GetFocus() == control ? focused : normal);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, state->text_color);
        SetBkColor(dc, state->panel_color);
        static HBRUSH brush = CreateSolidBrush(RGB(31, 34, 41));
        if (state->light_theme) {
            static HBRUSH light_brush = CreateSolidBrush(RGB(250, 251, 253));
            return reinterpret_cast<LRESULT>(light_brush);
        }
        return reinterpret_cast<LRESULT>(brush);
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
            return 0;
        }
        if (notification != BN_CLICKED) return 0;
        std::wstring error;
        if (id == kClose) {
            DestroyWindow(h);
            return 0;
        }
        if (state->mode == ManagementMode::Settings) {
            if (id == kTheme) {
                ApplyTheme(*state, !state->light_theme);
                SetWindowTextW(state->theme,
                               state->light_theme ? L"白天模式  ▾" : L"夜间模式  ▾");
                InvalidateRect(state->theme, nullptr, TRUE);
                return 0;
            }
            if (id == kFirewall) {
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
            if (id == kTypes) {
                ShowManagementDialog(h, ManagementMode::ProjectTypes, *state->store,
                                     *state->agent, state->restart_agent);
                return 0;
            }
            if (id == kSave) {
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
                state->store->SetSetting(L"theme", state->light_theme ? L"light" : L"dark");
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
                if (!state->store->AddProjectType(Field(state->name), Field(state->command), &error)) ShowError(h, error);
                else { RefreshList(*state); SetField(state->name, L""); SetField(state->command, L""); }
            } else if (id == kSave) {
                size_t index{};
                if (!SelectedIndex(state->list, index) || index >= state->types_data.size()) ShowError(h, L"请选择要修改的类型");
                else if (!state->store->UpdateProjectType(state->types_data[index].id, Field(state->name), Field(state->command), &error)) ShowError(h, error);
                else { RefreshList(*state); ReadSelection(*state); }
            } else if (id == kDelete) {
                size_t index{};
                if (!SelectedIndex(state->list, index) || index >= state->types_data.size()) ShowError(h, L"请选择要删除的类型");
                else if (!state->store->DeleteProjectType(state->types_data[index].id, &error)) ShowError(h, error);
                else { RefreshList(*state); SetField(state->name, L""); SetField(state->command, L""); }
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
        DeleteSnapshot(*state);
        return 0;
    }
    return DefWindowProcW(h, message, wparam, lparam);
}

} // namespace

ManagementDialogResult ShowManagementDialog(HWND owner, ManagementMode mode,
                                            DataStore& store, agent::RemoteAgent& agent,
                                            AgentRestartCallback restart_agent) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = WindowProc;
        klass.lpszClassName = kClassName;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = nullptr;
        RegisterClassW(&klass);
        registered = true;
    }
    State state;
    state.owner = owner;
    state.mode = mode;
    state.store = &store;
    state.agent = &agent;
    state.restart_agent = std::move(restart_agent);
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
    CaptureSnapshot(state);
    RECT client{};
    GetClientRect(owner, &client);
    state.window = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, L"",
                                   WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                   0, 0, client.right, client.bottom, owner, nullptr,
                                   GetModuleHandleW(nullptr), &state);
    if (state.window) {
        SetWindowPos(state.window, HWND_TOP, 0, 0, client.right, client.bottom, SWP_SHOWWINDOW);
        SetFocus(state.window);
        PopulateControls(state);
        MSG message{};
        while (IsWindow(state.window) && GetMessageW(&message, nullptr, 0, 0)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (state.font) DeleteObject(state.font);
    if (state.title_font) DeleteObject(state.title_font);
    if (state.small_font) DeleteObject(state.small_font);
    DeleteSnapshot(state);
    SetForegroundWindow(owner);
    SetFocus(owner);
    return state.result;
}

} // namespace aicli::ui
