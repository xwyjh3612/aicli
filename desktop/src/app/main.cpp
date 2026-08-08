#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <appmodel.h>
#include <roapi.h>
#include <psapi.h>
#include <wincrypt.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <cwchar>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.Terminal.Control.h>
#include <winrt/Microsoft.Terminal.TerminalConnection.h>
#include <winrt/Microsoft.Terminal.Settings.Model.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <winrt/Microsoft.Terminal.Core.h>
#include <nlohmann/json.hpp>
#include <qrcodegen.hpp>
#include "agent/remote_agent.h"
#include "core/models.h"
#include "data/data_store.h"
#include "terminal/conpty_connection_bridge.h"
#include "ui/styled_input.h"
#include "ui/management_dialog.h"
#include "ui/theme.h"
#include "ui/layout_metrics.h"
#include "ui/modal_host.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {
constexpr wchar_t kApplicationUserModelId[] = L"AiCli.Desktop.Dev_jw3zr00rmb0ym!App";
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
constexpr int ID_PROJECT_LIST = 1001;
constexpr int ID_NEW_PROJECT = 1004;
constexpr int ID_SETTINGS = 1005;
constexpr int ID_GROUP_DEFAULT = 1008;
constexpr int ID_GROUP_ADD = 1009;
constexpr int ID_BIND_PHONE = 1010;
constexpr int ID_CLI_TAB = 1011;
constexpr int ID_START_PROJECT = 1013;
constexpr int ID_EDIT_PROJECT = 1014;
constexpr int ID_SEARCH_PROJECTS = 1015;
constexpr int ID_DEVICES = 1016;
constexpr int ID_CTX_PAUSE = 3001;
constexpr int ID_CTX_CONTINUE = 3002;
constexpr int ID_CTX_CANCEL = 3003;
constexpr int ID_CTX_RESTART = 3004;
constexpr int ID_CTX_EDIT = 3005;
constexpr int ID_CTX_DELETE = 3006;
constexpr int ID_CTX_OPEN_FOLDER = 3007;
constexpr int ID_CTX_MOVE_DEFAULT = 3008;
constexpr int ID_CTX_MOVE_GROUP_BASE = 3200;
constexpr int ID_GROUP_SELECT_BASE = 3500;
constexpr int ID_MINIMIZE = 3010;
constexpr int ID_CLOSE = 3011;
constexpr int ID_MAXIMIZE = 3012;
constexpr wchar_t kClassName[] = L"AicliDesktopWindow";
constexpr wchar_t kAllGroupsName[] = L"全部";
constexpr UINT_PTR kXamlIslandSubclassId = 0xA1C11;
constexpr UINT_PTR kTerminalSnapshotTimerId = 2;
constexpr int kFooterTelemetryWidth = 344;

struct GdiplusRuntime {
    ULONG_PTR token{};
    GdiplusRuntime(){
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token,&input,nullptr);
    }
    ~GdiplusRuntime(){if(token)Gdiplus::GdiplusShutdown(token);}
};

Gdiplus::Color SmoothColor(COLORREF color,BYTE alpha=255){
    return Gdiplus::Color(alpha,GetRValue(color),GetGValue(color),GetBValue(color));
}

COLORREF BlendBackdropColor(COLORREF base,COLORREF overlay,BYTE alpha){
    const auto blend=[alpha](BYTE from,BYTE to)->BYTE{
        return static_cast<BYTE>((static_cast<unsigned>(from)*(255u-alpha)+
                                  static_cast<unsigned>(to)*alpha)/255u);
    };
    return RGB(blend(GetRValue(base),GetRValue(overlay)),
               blend(GetGValue(base),GetGValue(overlay)),
               blend(GetBValue(base),GetBValue(overlay)));
}

void AddRoundedPath(Gdiplus::GraphicsPath& path,const Gdiplus::RectF& rect,float radius){
    const float diameter=std::min(radius*2.0f,std::min(rect.Width,rect.Height));
    if(diameter<=1.0f){path.AddRectangle(rect);return;}
    path.AddArc(rect.X,rect.Y,diameter,diameter,180.0f,90.0f);
    path.AddArc(rect.GetRight()-diameter,rect.Y,diameter,diameter,270.0f,90.0f);
    path.AddArc(rect.GetRight()-diameter,rect.GetBottom()-diameter,diameter,diameter,0.0f,90.0f);
    path.AddArc(rect.X,rect.GetBottom()-diameter,diameter,diameter,90.0f,90.0f);
    path.CloseFigure();
}

void DrawSmoothRoundedRect(HDC dc,const RECT& rect,float radius,
                           COLORREF fill,COLORREF stroke,float stroke_width=1.0f,
                           bool dashed=false){
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::RectF bounds{
        static_cast<float>(rect.left)+stroke_width/2.0f,
        static_cast<float>(rect.top)+stroke_width/2.0f,
        std::max(1.0f,static_cast<float>(rect.right-rect.left)-stroke_width),
        std::max(1.0f,static_cast<float>(rect.bottom-rect.top)-stroke_width)};
    Gdiplus::GraphicsPath path;
    AddRoundedPath(path,bounds,radius);
    Gdiplus::SolidBrush brush(SmoothColor(fill));
    graphics.FillPath(&brush,&path);
    Gdiplus::Pen pen(SmoothColor(stroke),stroke_width);
    if(dashed)pen.SetDashStyle(Gdiplus::DashStyleDash);
    graphics.DrawPath(&pen,&path);
}

using aicli::DeviceRecord;
using aicli::Project;
using aicli::ProjectGroup;
using aicli::ProjectType;
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
struct AgentStatusMessage { bool online{}; std::wstring text; };

int EnsurePackagedActivation(const wchar_t* arguments) {
    UINT32 length = 0;
    const LONG result = GetCurrentPackageFullName(&length, nullptr);
    if (result != APPMODEL_ERROR_NO_PACKAGE) return 1;
    IApplicationActivationManager* manager = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&manager))) || !manager) return -1;
    DWORD process_id = 0;
    const HRESULT activated = manager->ActivateApplication(
        kApplicationUserModelId,
        arguments && *arguments ? arguments : nullptr,
        AO_NONE,
        &process_id);
    manager->Release();
    return SUCCEEDED(activated) ? 0 : -1;
}

std::wstring Base64EncodeUtf16(const std::wstring& text) {
    static constexpr wchar_t alphabet[] =
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const size_t size = text.size() * sizeof(wchar_t);
    std::wstring output;
    output.reserve(((size + 2) / 3) * 4);
    for (size_t offset = 0; offset < size; offset += 3) {
        const unsigned value =
            (static_cast<unsigned>(bytes[offset]) << 16) |
            ((offset + 1 < size ? static_cast<unsigned>(bytes[offset + 1]) : 0) << 8) |
            (offset + 2 < size ? static_cast<unsigned>(bytes[offset + 2]) : 0);
        output.push_back(alphabet[(value >> 18) & 0x3f]);
        output.push_back(alphabet[(value >> 12) & 0x3f]);
        output.push_back(offset + 1 < size ? alphabet[(value >> 6) & 0x3f] : L'=');
        output.push_back(offset + 2 < size ? alphabet[value & 0x3f] : L'=');
    }
    return output;
}

std::wstring PowerShellLiteral(std::wstring value) {
    size_t position = 0;
    while ((position = value.find(L'\'', position)) != std::wstring::npos) {
        value.insert(position, 1, L'\'');
        position += 2;
    }
    return L"'" + value + L"'";
}

std::wstring BuildTerminalCommand(const std::wstring& command, const std::wstring& cwd) {
    std::wstring script =
        L"$OutputEncoding=[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new();"
        L"[Console]::InputEncoding=[System.Text.UTF8Encoding]::new();";
    if (!cwd.empty()) {
        const auto folder = PowerShellLiteral(cwd);
        script += L"if(Test-Path -LiteralPath " + folder +
                  L"){Set-Location -LiteralPath " + folder + L"};";
    }
    script += command;
    return L"powershell.exe -NoLogo -NoExit -ExecutionPolicy Bypass -EncodedCommand " +
           Base64EncodeUtf16(script);
}

class WinRtComponentRegistry {
public:
    bool Initialize() {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
        directory_ = std::filesystem::path(path).parent_path();
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        dll_directory_ = AddDllDirectory(directory_.c_str());

        return Register(L"Microsoft.UI.Xaml.dll", {
                    L"Microsoft.UI.Xaml.Controls.CommandBarFlyout",
                    L"Microsoft.UI.Xaml.Controls.XamlControlsResources",
                    L"Microsoft.UI.Xaml.XamlTypeInfo.XamlControlsXamlMetaDataProvider"}) &&
               Register(L"Microsoft.Terminal.Settings.Model.dll", {
                    L"Microsoft.Terminal.Settings.Model.CascadiaSettings",
                    L"Microsoft.Terminal.Settings.Model.Profile",
                    L"Microsoft.Terminal.Settings.Model.TerminalSettings",
                    L"Microsoft.Terminal.Settings.Model.TerminalSettingsCreateResult"}) &&
               Register(L"Microsoft.Terminal.UI.dll", {
                    L"Microsoft.Terminal.UI.TextMenuFlyout",
                    L"Microsoft.Terminal.UI.ResourceString",
                    L"Microsoft.Terminal.UI.Converters",
                    L"Microsoft.Terminal.UI.IconPathConverter"}) &&
                Register(L"Microsoft.Terminal.Control.dll", {
                    L"Microsoft.Terminal.Control.ControlCore",
                    L"Microsoft.Terminal.Control.ControlInteractivity",
                    L"Microsoft.Terminal.Control.KeyChord",
                    L"Microsoft.Terminal.Control.OpenHyperlinkEventArgs",
                    L"Microsoft.Terminal.Control.ScrollBarVisualStateManager",
                    L"Microsoft.Terminal.Control.SearchBoxControl",
                    L"Microsoft.Terminal.Control.SelectionColor",
                    L"Microsoft.Terminal.Control.TSFInputControl",
                    L"Microsoft.Terminal.Control.TermControl",
                     L"Microsoft.Terminal.Control.VisualBellLight",
                     L"Microsoft.Terminal.Control.XamlMetaDataProvider"});
    }

    ~WinRtComponentRegistry() {
        for (auto cookie : cookies_) RoRevokeActivationFactories(cookie);
        if (dll_directory_) RemoveDllDirectory(dll_directory_);
    }

private:
    bool Register(const wchar_t* file, std::initializer_list<const wchar_t*> class_names) {
        const auto full_path = directory_ / file;
        HMODULE module = LoadLibraryExW(full_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        if (!module) {
            std::ofstream log(directory_ / L"component-load-error.txt", std::ios::trunc);
            log << "LoadLibraryEx failed for " << std::filesystem::path(file).string()
                << ": " << GetLastError() << "\n";
            return false;
        }
        auto callback = reinterpret_cast<PFNGETACTIVATIONFACTORY>(GetProcAddress(module, "DllGetActivationFactory"));
        if (!callback) {
            std::ofstream log(directory_ / L"component-load-error.txt", std::ios::trunc);
            log << "DllGetActivationFactory missing for "
                << std::filesystem::path(file).string() << "\n";
            return false;
        }

        modules_.push_back(module);
        std::vector<winrt::hstring> names;
        std::vector<HSTRING> ids;
        std::vector<PFNGETACTIVATIONFACTORY> callbacks;
        names.reserve(class_names.size());
        ids.reserve(class_names.size());
        callbacks.reserve(class_names.size());
        for (const auto* name : class_names) {
            names.emplace_back(name);
            ids.push_back(reinterpret_cast<HSTRING>(winrt::get_abi(names.back())));
            callbacks.push_back(callback);
        }
        RO_REGISTRATION_COOKIE cookie{};
        const HRESULT registration = RoRegisterActivationFactories(
            ids.data(), callbacks.data(), static_cast<UINT32>(ids.size()), &cookie);
        if (FAILED(registration)) {
            std::ofstream log(directory_ / L"component-load-error.txt", std::ios::trunc);
            log << "RoRegisterActivationFactories failed for "
                << std::filesystem::path(file).string() << ": 0x"
                << std::hex << static_cast<unsigned long>(registration) << "\n";
            return false;
        }
        cookies_.push_back(cookie);
        return true;
    }

    std::filesystem::path directory_;
    DLL_DIRECTORY_COOKIE dll_directory_{};
    std::vector<HMODULE> modules_;
    std::vector<RO_REGISTRATION_COOKIE> cookies_;
};

template <typename Interface>
Interface LocalActivationFactory(const wchar_t* module_name, const wchar_t* class_name) {
    HMODULE module = GetModuleHandleW(module_name);
    if (!module) module = LoadLibraryW(module_name);
    winrt::check_pointer(module);
    auto callback = reinterpret_cast<PFNGETACTIVATIONFACTORY>(
        GetProcAddress(module, "DllGetActivationFactory"));
    winrt::check_pointer(callback);
    winrt::hstring id{class_name};
    winrt::Windows::Foundation::IActivationFactory activation{nullptr};
    winrt::check_hresult(callback(
        reinterpret_cast<HSTRING>(winrt::get_abi(id)),
        reinterpret_cast<::IActivationFactory**>(winrt::put_abi(activation))));
    return activation.as<Interface>();
}

struct HostXamlApplication :
    winrt::Windows::UI::Xaml::ApplicationT<
        HostXamlApplication,
        winrt::Windows::UI::Xaml::Markup::IXamlMetadataProvider> {
    HostXamlApplication() {
        auto terminal_activation = LocalActivationFactory<winrt::Windows::Foundation::IActivationFactory>(
            L"Microsoft.Terminal.Control.dll",
            L"Microsoft.Terminal.Control.XamlMetaDataProvider");
        providers_.push_back(
            terminal_activation.ActivateInstance<winrt::Microsoft::Terminal::Control::XamlMetaDataProvider>());

        auto mux_activation = LocalActivationFactory<winrt::Windows::Foundation::IActivationFactory>(
            L"Microsoft.UI.Xaml.dll",
            L"Microsoft.UI.Xaml.XamlTypeInfo.XamlControlsXamlMetaDataProvider");
        providers_.push_back(
            mux_activation.ActivateInstance<winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider>());
    }

    winrt::Windows::UI::Xaml::Markup::IXamlType GetXamlType(
        const winrt::Windows::UI::Xaml::Interop::TypeName& type) {
        for (const auto& provider : providers_) {
            if (auto result = provider.GetXamlType(type)) return result;
        }
        return nullptr;
    }

    winrt::Windows::UI::Xaml::Markup::IXamlType GetXamlType(const winrt::hstring& name) {
        for (const auto& provider : providers_) {
            if (auto result = provider.GetXamlType(name)) return result;
        }
        return nullptr;
    }

    winrt::com_array<winrt::Windows::UI::Xaml::Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        std::vector<winrt::Windows::UI::Xaml::Markup::XmlnsDefinition> values;
        for (const auto& provider : providers_) {
            const auto definitions = provider.GetXmlnsDefinitions();
            values.insert(values.end(), definitions.begin(), definitions.end());
        }
        return {values.begin(), values.end()};
    }

    void OnActivated(const winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs&) {}
    void OnLaunched(const winrt::Windows::ApplicationModel::Activation::LaunchActivatedEventArgs&) {}
    void OnFileActivated(const winrt::Windows::ApplicationModel::Activation::FileActivatedEventArgs&) {}
    void OnSearchActivated(const winrt::Windows::ApplicationModel::Activation::SearchActivatedEventArgs&) {}
    void OnShareTargetActivated(const winrt::Windows::ApplicationModel::Activation::ShareTargetActivatedEventArgs&) {}
    void OnFileOpenPickerActivated(const winrt::Windows::ApplicationModel::Activation::FileOpenPickerActivatedEventArgs&) {}
    void OnFileSavePickerActivated(const winrt::Windows::ApplicationModel::Activation::FileSavePickerActivatedEventArgs&) {}
    void OnCachedFileUpdaterActivated(const winrt::Windows::ApplicationModel::Activation::CachedFileUpdaterActivatedEventArgs&) {}
    void OnWindowCreated(const winrt::Windows::UI::Xaml::WindowCreatedEventArgs&) {}
    void OnBackgroundActivated(const winrt::Windows::ApplicationModel::Activation::BackgroundActivatedEventArgs&) {}

private:
    std::vector<winrt::Windows::UI::Xaml::Markup::IXamlMetadataProvider> providers_;
};

class XamlIslandAdapter;
LRESULT CALLBACK XamlIslandSubclassProc(
    HWND hwnd,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR reference_data);

class XamlIslandAdapter {
public:
    bool Attach(HWND parent, RECT rc) {
        try {
            parent_ = parent;
            application_ = winrt::make_self<HostXamlApplication>();
            manager_=winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
            source_=winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource();
            auto interop=source_.as<::IDesktopWindowXamlSourceNative>();
            winrt::check_hresult(interop->AttachToWindow(parent));
            interop->get_WindowHandle(&hwnd_);
            if (!SetWindowSubclass(
                    hwnd_,
                    XamlIslandSubclassProc,
                    kXamlIslandSubclassId,
                    reinterpret_cast<DWORD_PTR>(this))) {
                winrt::throw_last_error();
            }
            grid_=winrt::Windows::UI::Xaml::Controls::Grid();
            const auto background = light_theme_
                ? winrt::Windows::UI::Color{0xff, 0xf7, 0xf8, 0xfa}
                : winrt::Windows::UI::Color{0xff, 0x0c, 0x0c, 0x0c};
            grid_.Background(winrt::Windows::UI::Xaml::Media::SolidColorBrush(background));
            try {
                application_.as<winrt::Windows::UI::Xaml::Application>()
                    .Resources().MergedDictionaries().Append(
                        winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources());
            } catch (...) {
                // The terminal still works with its compiled resources when WinUI theme resources are unavailable.
            }
            source_.Content(grid_);
            Resize(rc);
            ShowWindow(hwnd_,SW_HIDE); return true;
        } catch (...) { return false; }
    }
    bool Start(const std::wstring& project_id, const std::wstring& command,
               const std::wstring& cwd, const std::wstring& title, HWND owner, HWND status,
               std::function<void(std::wstring_view)> output_observer = {}) {
        if (project_id.empty()) {
            last_error_ = L"project id is empty";
            return false;
        }
        if (HasSession(project_id)) return Activate(project_id);
        std::wstring stage = L"prepare";
        auto set_stage = [&](const wchar_t* value) {
            stage = value;
            if (status) SetWindowTextW(status, (L"正在初始化原生终端: " + stage).c_str());
        };
        try {
            using namespace winrt::Microsoft::Terminal;
            auto session = std::make_unique<TerminalSession>();
            set_stage(L"activate TerminalSettings");
            auto settings_activation = LocalActivationFactory<winrt::Windows::Foundation::IActivationFactory>(
                L"Microsoft.Terminal.Settings.Model.dll",
                L"Microsoft.Terminal.Settings.Model.TerminalSettings");
            auto default_settings = settings_activation.ActivateInstance<Settings::Model::TerminalSettings>();
            const std::wstring command_line = L"powershell.exe -NoLogo -NoExit";
            set_stage(L"configure TerminalSettings");
            default_settings.Commandline(command_line);
            default_settings.StartingDirectory(cwd);
            default_settings.StartingTitle(title);
            default_settings.InitialRows(32);
            default_settings.InitialCols(120);
            default_settings.HistorySize(10000);

            // A directly activated TerminalSettings instance uses white as its
            // default foreground. Apply Windows Terminal's Campbell palette so
            // Codex's normal, dim, and bright text retain their intended visual
            // hierarchy instead of most text being rendered as bright white.
            ApplyPalette(default_settings, light_theme_);
            // Preserve SGR 2 exactly. Automatic contrast adjustment can lift
            // faint text back toward the normal foreground in some hosts.
            default_settings.AdjustIndistinguishableColors(Core::AdjustTextMode::Never);

            // TermControl expects the pair returned by TerminalSettingsCreateResult.
            // Passing the same raw settings object as both focused and unfocused
            // appearance bypasses the host's normal settings construction path.
            auto settings_result_factory = LocalActivationFactory<Settings::Model::ITerminalSettingsCreateResultFactory>(
                L"Microsoft.Terminal.Settings.Model.dll",
                L"Microsoft.Terminal.Settings.Model.TerminalSettingsCreateResult");
            auto settings_result = settings_result_factory.CreateInstance(default_settings);
            session->control_settings = settings_result.DefaultSettings();
            session->unfocused_settings = settings_result.UnfocusedSettings();

            // TerminalConnection.dll has repeatedly crashed inside its private
            // WinRT implementation on this Windows build. Keep TermControl for
            // native rendering/input, but use our ITerminalConnection bridge
            // with the matching official side-by-side conpty/OpenConsole host.
            set_stage(L"create ConPTY connection");
            // Feed the ConPTY stream directly into TermControl. The bridge
            // preserves every VT sequence except Codex's one fixed composer
            // background, which is mapped to the active app theme without
            // changing cursor, layout, input or scrollback sequences.
            session->conpty_connection =
                winrt::make_self<aicli::terminal::ConPtyConnectionBridge>(
                    cwd, command, std::move(output_observer));
            session->conpty_connection->SetLightTheme(light_theme_);
            session->connection = session->conpty_connection.as<
                winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection>();
            set_stage(L"create TermControl");
            auto control_factory = LocalActivationFactory<Control::ITermControlFactory>(
                L"Microsoft.Terminal.Control.dll",
                L"Microsoft.Terminal.Control.TermControl");
            session->control = control_factory.CreateInstance2(
                session->control_settings, session->unfocused_settings, session->connection);
            set_stage(L"attach TermControl");
            // TermControl derives the TSF/IME candidate-window position from
            // OwningHwnd's screen rectangle plus the cursor's XAML-local
            // coordinates.  Our XAML root begins at the terminal island (not
            // at the main window origin), so using the main HWND loses the
            // island's left/top offset and places IME UI over the sidebar.
            session->control.OwningHwnd(reinterpret_cast<uint64_t>(hwnd_ ? hwnd_ : owner));
            session->control.HorizontalAlignment(winrt::Windows::UI::Xaml::HorizontalAlignment::Stretch);
            session->control.VerticalAlignment(winrt::Windows::UI::Xaml::VerticalAlignment::Stretch);
            session->control.Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
            grid_.Children().Append(session->control);
            sessions_.emplace(project_id, std::move(session));
            Activate(project_id);
            ShowWindow(hwnd_, SW_SHOW);
            SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            grid_.UpdateLayout();
            if (auto* active = ActiveSession()) active->control.UpdateLayout();
            SetVisible(true);
            last_error_.clear();
            return true;
        } catch (const winrt::hresult_error& error) {
            last_error_ = stage + L": 0x";
            wchar_t code[16]{};
            swprintf_s(code, L"%08X", static_cast<unsigned>(error.code().value));
            last_error_ += code;
            last_error_ += L" ";
            last_error_ += error.message().c_str();
            OutputDebugStringW((L"AI CLI TermControl: " + last_error_ + L"\n").c_str());
            Stop(project_id);
            return false;
        } catch (...) {
            last_error_ = stage + L": unknown error";
            Stop(project_id);
            return false;
        }
    }
    bool HasSession(std::wstring_view project_id) const {
        return sessions_.find(std::wstring(project_id)) != sessions_.end();
    }
    bool Activate(std::wstring_view project_id, bool focus = true) {
        auto* next = FindSession(project_id);
        if (!next) return false;
        const std::wstring next_id{project_id};
        const bool changed = active_project_id_ != next_id;
        if (changed) {
            if (auto* current = ActiveSession()) {
                try {
                    if (visible_) current->control.WindowVisibilityChanged(false);
                    current->control.Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
                } catch (...) {}
            }
            active_project_id_ = next_id;
            try {
                next->control.Visibility(winrt::Windows::UI::Xaml::Visibility::Visible);
                if (visible_) next->control.WindowVisibilityChanged(true);
            } catch (...) {}
        }
        if (focus || changed) {
            try {
                next->control.Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic);
                EnsureNativeScrollbar(next);
            } catch (...) {}
        }
        return true;
    }
    void Stop(std::wstring_view project_id) {
        const std::wstring id{project_id};
        auto found = sessions_.find(id);
        if (found == sessions_.end()) return;
        const bool was_active = active_project_id_ == id;
        auto& session = *found->second;
        try {
            if (was_active && visible_) session.control.WindowVisibilityChanged(false);
            if (grid_ && session.control) {
                uint32_t index{};
                auto children = grid_.Children();
                if (children.IndexOf(session.control, index)) children.RemoveAt(index);
            }
            if (session.connection) session.connection.Close();
        } catch (...) {}
        sessions_.erase(found);
        if (was_active) {
            active_project_id_.clear();
            SetVisible(false);
        }
    }
    void StopAll() {
        SetVisible(false);
        for (auto& [_, session] : sessions_) {
            try { if (session->connection) session->connection.Close(); } catch (...) {}
        }
        try { if (grid_) grid_.Children().Clear(); } catch (...) {}
        sessions_.clear();
        active_project_id_.clear();
        visible_ = false;
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    }
    void SetVisible(bool visible) {
        if (!hwnd_) return;
        if (visible_ == visible) {
            if (visible) EnsureNativeScrollbar();
            return;
        }
        try {
            if (auto* active = ActiveSession(); active && visible_ != visible) {
                active->control.WindowVisibilityChanged(visible);
            }
            visible_ = visible;
            ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
            if (auto* active = ActiveSession()) {
                if (visible) {
                    EnsureNativeScrollbar(active);
                }
            }
        } catch (...) {
            ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
        }
    }
    void SetTheme(bool light) {
        if (light_theme_ == light) return;
        light_theme_ = light;
        try {
            if (grid_) {
                const auto background = light
                    ? winrt::Windows::UI::Color{0xff, 0xf7, 0xf8, 0xfa}
                    : winrt::Windows::UI::Color{0xff, 0x0c, 0x0c, 0x0c};
                grid_.Background(winrt::Windows::UI::Xaml::Media::SolidColorBrush(background));
            }
            for (auto& [project_id, session] : sessions_) {
                if (!session->control || !session->control_settings) continue;
                if (session->conpty_connection)
                    session->conpty_connection->SetLightTheme(light);
                ApplyPalette(session->control_settings, light);
                if (session->unfocused_settings) ApplyPalette(session->unfocused_settings, light);
                session->control.UpdateControlSettings(
                    session->control_settings, session->unfocused_settings);
                session->control.UpdateLayout();
                if (session->conpty_connection && active_project_id_ == project_id && visible_) {
                    session->conpty_connection->RefreshViewport();
                }
            }
            if (grid_) grid_.UpdateLayout();
        } catch (...) {
            OutputDebugStringW(L"AI CLI TermControl: live theme update failed\n");
        }
    }
    void Resize(RECT rc) {
        if (!hwnd_ || !parent_ || IsIconic(parent_)) return;
        const LONG raw_width = rc.right - rc.left;
        const LONG raw_height = rc.bottom - rc.top;
        if (raw_width <= 0 || raw_height <= 0) return;
        const int width = static_cast<int>(raw_width);
        const int height = static_cast<int>(raw_height);
        const UINT dpi = GetDpiForWindow(parent_);
        if (has_resize_rect_ && EqualRect(&last_resize_rect_, &rc) &&
            last_resize_dpi_ == dpi) return;
        try {
            SetWindowPos(hwnd_, nullptr, rc.left, rc.top, width, height,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (grid_) {
                const double scale = dpi ? static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI : 1.0;
                grid_.Width(width / scale);
                grid_.Height(height / scale);
                for (auto& [_, session] : sessions_) {
                    if (!session->control) continue;
                    session->control.Width(width / scale);
                    session->control.Height(height / scale);
                    session->control.UpdateLayout();
                }
                grid_.UpdateLayout();
                EnsureNativeScrollbar();
            }
            last_resize_rect_ = rc;
            last_resize_dpi_ = dpi;
            has_resize_rect_ = true;
        } catch (...) {
            OutputDebugStringW(L"AI CLI TermControl: ignored invalid resize during window transition\n");
        }
    }
    HWND Window() const { return hwnd_; }
    bool Active() const { return ActiveSession() != nullptr; }
    bool Active(std::wstring_view project_id) const { return HasSession(project_id); }
    std::wstring ReadEntireBuffer(std::wstring_view project_id = {}) const {
        const auto* session = project_id.empty() ? ActiveSession() : FindSession(project_id);
        if (!session || !session->control) return {};
        try {
            const auto value = session->control.ReadEntireBuffer();
            return {value.c_str(), value.size()};
        } catch (...) {
            return {};
        }
    }
    bool WriteInput(const std::wstring& value) {
        return WriteInput(active_project_id_, value);
    }
    bool WriteInput(std::wstring_view project_id, const std::wstring& value) {
        auto* session = FindSession(project_id);
        if(!session||!session->connection||value.empty())return false;
        try{
            const auto* first=reinterpret_cast<const char16_t*>(value.data());
            session->connection.WriteInput(winrt::array_view<const char16_t>{first,static_cast<uint32_t>(value.size())});
            return true;
        }catch(...){return false;}
    }
    void OnIslandDestroyed(HWND window) noexcept {
        if (hwnd_ == window) hwnd_ = nullptr;
    }
    void UpdateNativeScrollbarHover() {
        auto* session = ActiveSession();
        if (!session || !hwnd_) return;
        EnsureNativeScrollbar(session);
        if (!session->native_scrollbar) return;
        POINT cursor{};
        RECT terminal_rect{};
        if (!GetCursorPos(&cursor) || !GetWindowRect(hwnd_, &terminal_rect)) return;
        const UINT dpi = parent_ ? GetDpiForWindow(parent_) : USER_DEFAULT_SCREEN_DPI;
        const double scale = dpi ? static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI : 1.0;
        const int scrollbar_width = std::max(
            16, static_cast<int>(std::ceil(session->native_scrollbar.ActualWidth() * scale)));
        const bool expanded = PtInRect(&terminal_rect, cursor) &&
                              cursor.x >= terminal_rect.right - scrollbar_width;
        SetNativeScrollbarVisual(*session, expanded);
    }
    void CollapseNativeScrollbar() {
        if (auto* session = ActiveSession()) SetNativeScrollbarVisual(*session, false);
    }
    const std::wstring& LastError() const { return last_error_; }
private:
    struct TerminalSession {
        winrt::Microsoft::Terminal::Control::TermControl control{nullptr};
        winrt::Microsoft::Terminal::Settings::Model::TerminalSettings control_settings{nullptr};
        winrt::Microsoft::Terminal::Settings::Model::TerminalSettings unfocused_settings{nullptr};
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection{nullptr};
        winrt::com_ptr<aicli::terminal::ConPtyConnectionBridge> conpty_connection;
        winrt::Windows::UI::Xaml::Controls::Primitives::ScrollBar native_scrollbar{nullptr};
        bool scrollbar_visual_initialized{};
        bool scrollbar_expanded{};
        bool scrollbar_loaded_handler_attached{};
    };

    TerminalSession* FindSession(std::wstring_view project_id) {
        const auto found = sessions_.find(std::wstring(project_id));
        return found == sessions_.end() ? nullptr : found->second.get();
    }
    const TerminalSession* FindSession(std::wstring_view project_id) const {
        const auto found = sessions_.find(std::wstring(project_id));
        return found == sessions_.end() ? nullptr : found->second.get();
    }
    TerminalSession* ActiveSession() { return FindSession(active_project_id_); }
    const TerminalSession* ActiveSession() const { return FindSession(active_project_id_); }

    static void ApplyPalette(
        const winrt::Microsoft::Terminal::Settings::Model::TerminalSettings& settings,
        bool light) {
        using namespace winrt::Microsoft::Terminal;
        auto scheme_activation = LocalActivationFactory<winrt::Windows::Foundation::IActivationFactory>(
            L"Microsoft.Terminal.Settings.Model.dll",
            L"Microsoft.Terminal.Settings.Model.ColorScheme");
        auto scheme = scheme_activation.ActivateInstance<Settings::Model::ColorScheme>();
        using TerminalColor = Core::Color;
        scheme.Name(light ? L"AI CLI Light" : L"AI CLI Campbell");
        scheme.Foreground(light ? TerminalColor{0x24, 0x2a, 0x33, 0xff}
                                : TerminalColor{0xcc, 0xcc, 0xcc, 0xff});
        scheme.Background(light ? TerminalColor{0xf7, 0xf8, 0xfa, 0xff}
                                : TerminalColor{0x0c, 0x0c, 0x0c, 0xff});
        scheme.CursorColor(light ? TerminalColor{0x1c, 0x22, 0x2b, 0xff}
                                 : TerminalColor{0xff, 0xff, 0xff, 0xff});
        scheme.SelectionBackground(light ? TerminalColor{0xbf, 0xd8, 0xf4, 0xff}
                                         : TerminalColor{0x26, 0x4f, 0x78, 0xff});
        std::array<TerminalColor, 16> campbell{
            TerminalColor{0x0c, 0x0c, 0x0c, 0xff},
            TerminalColor{0xc5, 0x0f, 0x1f, 0xff},
            TerminalColor{0x13, 0xa1, 0x0e, 0xff},
            TerminalColor{0xc1, 0x9c, 0x00, 0xff},
            TerminalColor{0x00, 0x37, 0xda, 0xff},
            TerminalColor{0x88, 0x17, 0x98, 0xff},
            TerminalColor{0x3a, 0x96, 0xdd, 0xff},
            TerminalColor{0xcc, 0xcc, 0xcc, 0xff},
            TerminalColor{0x76, 0x76, 0x76, 0xff},
            TerminalColor{0xe7, 0x48, 0x56, 0xff},
            TerminalColor{0x16, 0xc6, 0x0c, 0xff},
            TerminalColor{0xf9, 0xf1, 0xa5, 0xff},
            TerminalColor{0x3b, 0x78, 0xff, 0xff},
            TerminalColor{0xb4, 0x00, 0x9e, 0xff},
            TerminalColor{0x61, 0xd6, 0xd6, 0xff},
            TerminalColor{0xf2, 0xf2, 0xf2, 0xff},
        };
        if (light) {
            // Codex uses ANSI black as the composer/background surface.  A
            // literal Campbell black leaves a black input block inside the
            // light application theme, so map that surface to a restrained
            // light gray while keeping bright-black available for muted text.
            campbell[0] = TerminalColor{0xe8, 0xeb, 0xef, 0xff};
            campbell[7] = TerminalColor{0x35, 0x3b, 0x45, 0xff};
            campbell[8] = TerminalColor{0x6d, 0x76, 0x83, 0xff};
            campbell[15] = TerminalColor{0x18, 0x1d, 0x25, 0xff};
        }
        for (uint8_t index = 0; index < campbell.size(); ++index) {
            scheme.SetColorTableEntry(index, campbell[index]);
        }
        settings.ApplyColorScheme(scheme);
        settings.AdjustIndistinguishableColors(Core::AdjustTextMode::Never);
    }

    static void SetNativeScrollbarVisual(TerminalSession& session, bool expanded) noexcept {
        if (!session.native_scrollbar ||
            (session.scrollbar_visual_initialized && session.scrollbar_expanded == expanded)) {
            return;
        }
        try {
            // TermControl ships the exact Windows Terminal scrollbar template.
            // The XAML Island does not reliably drive its pointer visual state,
            // so select the same built-in states ourselves: a thin idle thumb
            // and the full native track/thumb while the pointer is over it.
            session.native_scrollbar.ApplyTemplate();
            const bool mouse_indicator =
                winrt::Windows::UI::Xaml::VisualStateManager::GoToState(
                    session.native_scrollbar, L"MouseIndicator", false);
            const bool conscious_state =
                winrt::Windows::UI::Xaml::VisualStateManager::GoToState(
                    session.native_scrollbar,
                    expanded ? L"Expanded" : L"CollapsedWithoutAnimation",
                    expanded);
            if (mouse_indicator && conscious_state) {
                session.scrollbar_visual_initialized = true;
                session.scrollbar_expanded = expanded;
            }
        } catch (...) {
        }
    }

    static void ApplyLoadedNativeScrollbarVisual(
        const winrt::Windows::UI::Xaml::Controls::Primitives::ScrollBar& scrollbar) noexcept {
        if (!scrollbar) return;
        try {
            scrollbar.Visibility(winrt::Windows::UI::Xaml::Visibility::Visible);
            scrollbar.IndicatorMode(
                winrt::Windows::UI::Xaml::Controls::Primitives::ScrollingIndicatorMode::MouseIndicator);
            scrollbar.IsHitTestVisible(true);
            scrollbar.Opacity(1.0);
            scrollbar.ApplyTemplate();
            winrt::Windows::UI::Xaml::VisualStateManager::GoToState(
                scrollbar, L"MouseIndicator", false);
            winrt::Windows::UI::Xaml::VisualStateManager::GoToState(
                scrollbar, L"CollapsedWithoutAnimation", false);
        } catch (...) {
        }
    }

    void EnsureNativeScrollbar(TerminalSession* session = nullptr) {
        if (!session) session = ActiveSession();
        if (!session || !session->control) return;
        try {
            session->control.ApplyTemplate();
            if (!session->native_scrollbar) {
                const auto element = session->control.as<winrt::Windows::UI::Xaml::FrameworkElement>()
                                         .FindName(L"ScrollBar");
                session->native_scrollbar = element.try_as<
                    winrt::Windows::UI::Xaml::Controls::Primitives::ScrollBar>();
            }
            const auto scrollbar = session->native_scrollbar;
            if (scrollbar) {
                // Keep Windows Terminal's own visual-state manager intact:
                // a thin indicator while idle and the wider thumb/track on
                // pointer hover, exactly like the standalone terminal.
                scrollbar.Visibility(winrt::Windows::UI::Xaml::Visibility::Visible);
                scrollbar.IndicatorMode(
                    winrt::Windows::UI::Xaml::Controls::Primitives::ScrollingIndicatorMode::MouseIndicator);
                scrollbar.IsHitTestVisible(true);
                scrollbar.Opacity(1.0);
                if (!session->scrollbar_loaded_handler_attached) {
                    session->scrollbar_loaded_handler_attached = true;
                    const auto weak_scrollbar = winrt::make_weak(scrollbar);
                    scrollbar.Loaded([weak_scrollbar](
                        const auto&, const winrt::Windows::UI::Xaml::RoutedEventArgs&) noexcept {
                        if (const auto loaded = weak_scrollbar.get()) {
                            ApplyLoadedNativeScrollbarVisual(loaded);
                        }
                    });
                    (void)scrollbar.Dispatcher().RunAsync(
                        winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
                        [weak_scrollbar]() noexcept {
                            if (const auto loaded = weak_scrollbar.get()) {
                                ApplyLoadedNativeScrollbarVisual(loaded);
                            }
                        });
                }
                if (!session->scrollbar_visual_initialized) {
                    SetNativeScrollbarVisual(*session, false);
                }
            }
        } catch (...) {
        }
    }

    winrt::com_ptr<HostXamlApplication> application_;
    winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager manager_{nullptr};
    winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource source_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid grid_{nullptr};
    std::unordered_map<std::wstring, std::unique_ptr<TerminalSession>> sessions_;
    std::wstring active_project_id_;
    bool visible_{};
    bool light_theme_{};
    RECT last_resize_rect_{};
    UINT last_resize_dpi_{};
    bool has_resize_rect_{};
    std::wstring last_error_;
    HWND parent_{};
    HWND hwnd_{};
};

LRESULT CALLBACK XamlIslandSubclassProc(
    HWND hwnd,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR reference_data) {
    auto* adapter = reinterpret_cast<XamlIslandAdapter*>(reference_data);
    if (adapter) {
        if (message == WM_MOUSEMOVE) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
            adapter->UpdateNativeScrollbarHover();
        }
        if (message == WM_MOUSELEAVE) adapter->CollapseNativeScrollbar();
        if (message == WM_NCDESTROY) {
            adapter->OnIslandDestroyed(hwnd);
            RemoveWindowSubclass(hwnd, XamlIslandSubclassProc, subclass_id);
        }
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

HWND g_hwnd{}, g_list{}, g_status{}, g_new_project{}, g_settings{}, g_group{}, g_group_add{}, g_bind{}, g_devices{}, g_cli_tab{}, g_sidebar_title{}, g_search{}, g_terminal_title{};
HWND g_detail_title{}, g_detail_folder{}, g_detail_command{}, g_detail_state{}, g_start_project{}, g_edit_project{};
WNDPROC g_button_proc{}; HWND g_hover_button{};
HFONT g_font{}, g_font_medium{}, g_font_small{}, g_font_modal_title{}, g_icon_font{}; XamlIslandAdapter g_island; WinRtComponentRegistry g_components;
aicli::DataStore g_store;
aicli::agent::RemoteAgent g_remote_agent;
std::optional<aicli::agent::PendingPairing> g_pending_pairing;
std::vector<Project> g_projects;
std::vector<ProjectType> g_project_types;
std::vector<ProjectGroup> g_groups;
std::vector<int> g_visible_project_indices;
std::mutex g_conversations_mutex;
std::string g_conversations_cache;
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
HBRUSH g_edit_brush{},g_focus_brush{},g_list_brush{},g_bg_brush{};bool g_light_theme{};bool g_window_active=true;bool g_theme_workspace_dirty{};
bool g_settings_open_posted{};
HWND g_project_dialog{}, g_d_name{}, g_d_type{}, g_d_group{}, g_d_folder{}, g_d_folder_browse{}, g_d_command{}; bool g_project_dialog_ok{}; bool g_project_dialog_edit{}; Project g_project_draft{};
HWND g_binding_dialog{},g_binding_refresh{},g_binding_close{},g_binding_approve{},g_binding_reject{};
aicli::agent::PairingInfo g_binding_info{};
std::optional<qrcodegen::QrCode> g_binding_qr;
RECT g_binding_panel{};
RECT g_project_panel{};
RECT g_git_footer_rect{};
HWND g_git_tooltip{};
std::wstring g_git_tooltip_text;
std::wstring g_git_last_push;
std::wstring g_git_last_pull;
RECT g_d_name_rect{}, g_d_type_rect{}, g_d_group_rect{}, g_d_folder_rect{}, g_d_command_rect{};
enum class GitRepositoryState { Unavailable, NotInitialized, NoCommits, HasCommits };
GitRepositoryState g_git_repository_state=GitRepositoryState::Unavailable;
std::wstring g_git_repository_root;
std::mutex g_terminal_snapshot_mutex;
std::vector<std::wstring> g_pending_terminal_snapshot_projects;
std::unordered_map<std::wstring,std::wstring> g_last_terminal_snapshots;
std::atomic_bool g_terminal_snapshot_dirty{};
std::atomic_bool g_terminal_snapshot_message_pending{};
bool g_terminal_snapshot_timer_active{};
int g_project_drag_source=-1;
int g_project_drag_insert=-1;
int g_project_hover_row=-1;
int g_title_hover_id{};
int g_title_pressed_id{};
bool g_project_dragging{};
UINT g_project_drag_message{};
HWND g_project_drag_preview{};
POINT g_project_drag_hotspot{};

enum { ID_D_NAME=2001, ID_D_TYPE=2002, ID_D_GROUP=2003, ID_D_FOLDER=2004, ID_D_COMMAND=2005, ID_D_OK=2006, ID_D_CANCEL=2007, ID_D_BROWSE=2008 };
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
                                !g_list_brush||!g_bg_brush;
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
    g_edit_brush=CreateSolidBrush(g_theme.field);
    g_focus_brush=CreateSolidBrush(g_theme.field_focus);
    g_list_brush=CreateSolidBrush(panel);
    g_bg_brush=CreateSolidBrush(bg);
    if(g_search) aicli::ui::SetStyledInputTheme(g_search,g_theme);
    return true;
}
COLORREF TitleBarBackground(){return g_window_active?g_theme.titlebar:g_theme.titlebar_inactive;}
COLORREF TitleBarText(){return g_window_active?g_theme.titlebar_text:g_theme.titlebar_inactive_text;}
COLORREF TitleBarMuted(){return g_window_active?g_theme.titlebar_muted:g_theme.titlebar_inactive_text;}
void ApplyWindowChromeTheme(bool repaint=true){
    if(!g_hwnd)return;
    BOOL dark=g_light_theme?FALSE:TRUE;
    COLORREF border_color=border;
    DwmSetWindowAttribute(g_hwnd,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    DwmSetWindowAttribute(g_hwnd,DWMWA_BORDER_COLOR,&border_color,sizeof(border_color));
    // The frame is entirely client-owned.  DWM is used only for resize
    // borders and system composition; it must never paint a second caption
    // strip or caption buttons over our title bar.
    SetWindowPos(g_hwnd,nullptr,0,0,0,0,
                 SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    if(repaint){
        RedrawWindow(g_hwnd,nullptr,nullptr,
                     RDW_INVALIDATE|RDW_FRAME|RDW_NOERASE|RDW_ALLCHILDREN);
    }
}
void InvalidateTitleBar(HWND window){
    if(!window)return;
    const int height=aicli::ui::LayoutMetrics::ForWindow(window).title_bar_height;
    RECT rect{};GetClientRect(window,&rect);rect.bottom=std::min<LONG>(rect.bottom,height);
    RedrawWindow(window,&rect,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_NOERASE);
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
        InvalidateTitleBar(g_hwnd);
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
RECT TitleButtonRect(HWND window,int id){
    RECT client{};GetClientRect(window,&client);
    const int width=std::max(1,aicli::ui::LayoutMetrics::ForWindow(window).caption_button_width);
    const int offset=id==ID_CLOSE?1:(id==ID_MAXIMIZE?2:3);
    return RECT{std::max(0,static_cast<int>(client.right)-width*offset),0,
                std::max(0,static_cast<int>(client.right)-width*(offset-1)),
                aicli::ui::LayoutMetrics::ForWindow(window).title_bar_height};
}
int TitleButtonAtPoint(HWND window,POINT client_point){
    for(const int id:{ID_CLOSE,ID_MAXIMIZE,ID_MINIMIZE}){
        const RECT rect=TitleButtonRect(window,id);
        if(PtInRect(&rect,client_point))return id;
    }
    return 0;
}
void InvalidateTitleButtons(HWND window){
    RECT client{};GetClientRect(window,&client);
    const auto metrics=aicli::ui::LayoutMetrics::ForWindow(window);
    RECT controls{std::max(0,static_cast<int>(client.right)-metrics.caption_button_width*3),0,
                  client.right,metrics.title_bar_height};
    InvalidateRect(window,&controls,FALSE);
}
void DrawTitleButtons(HDC dc,HWND window){
    Gdiplus::Graphics graphics(dc);graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    for(const int id:{ID_MINIMIZE,ID_MAXIMIZE,ID_CLOSE}){
        const RECT rect=TitleButtonRect(window,id);
        COLORREF fill=TitleBarBackground();
        const bool hover=g_title_hover_id==id,pressed=g_title_pressed_id==id;
        if(hover){fill=id==ID_CLOSE?g_theme.titlebar_close_hover:g_theme.titlebar_hover;}
        if(pressed){fill=id==ID_CLOSE?g_theme.titlebar_close_pressed:g_theme.titlebar_pressed;}
        Gdiplus::SolidBrush background(SmoothColor(fill));
        graphics.FillRectangle(&background,static_cast<Gdiplus::REAL>(rect.left),static_cast<Gdiplus::REAL>(rect.top),
                               static_cast<Gdiplus::REAL>(rect.right-rect.left),static_cast<Gdiplus::REAL>(rect.bottom-rect.top));
        const COLORREF glyph=id==ID_CLOSE&&hover?RGB(255,255,255):TitleBarText();
        Gdiplus::Pen pen(SmoothColor(glyph),1.25f);pen.SetStartCap(Gdiplus::LineCapSquare);pen.SetEndCap(Gdiplus::LineCapSquare);
        const float cx=(rect.left+rect.right)/2.0f,cy=(rect.top+rect.bottom)/2.0f;
        if(id==ID_MINIMIZE)graphics.DrawLine(&pen,cx-7.0f,cy+1.0f,cx+7.0f,cy+1.0f);
        else if(id==ID_CLOSE){graphics.DrawLine(&pen,cx-6.0f,cy-6.0f,cx+6.0f,cy+6.0f);graphics.DrawLine(&pen,cx+6.0f,cy-6.0f,cx-6.0f,cy+6.0f);}
        else if(IsZoomed(window)){
            graphics.DrawRectangle(&pen,cx-4.0f,cy-6.0f,11.0f,11.0f);
            Gdiplus::SolidBrush cover(SmoothColor(fill));graphics.FillRectangle(&cover,cx-7.0f,cy-3.0f,11.0f,11.0f);
            graphics.DrawRectangle(&pen,cx-7.0f,cy-3.0f,11.0f,11.0f);
        }else graphics.DrawRectangle(&pen,cx-7.0f,cy-6.0f,14.0f,13.0f);
    }
}
void ExecuteTitleButton(HWND window,int id){
    if(id==ID_CLOSE)DestroyWindow(window);
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
                 SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_NOSENDCHANGING);
    return true;
}
bool SetMainChildVisible(HWND control,bool visible){
    if(!control||!IsWindow(control))return false;
    const bool current=IsWindowVisible(control)!=FALSE;
    if(current==visible)return false;
    ShowWindow(control,visible?SW_SHOW:SW_HIDE);
    return true;
}
std::string Utf8Text(std::wstring_view value){if(value.empty())return{};const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);if(count<=0)return{};std::string result(static_cast<size_t>(count),'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count,nullptr,nullptr);return result;}
std::wstring WideText(std::string_view value){if(value.empty())return{};const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);if(count<=0)return{};std::wstring result(static_cast<size_t>(count),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count);return result;}
std::wstring StripTerminalSequences(std::wstring_view value){std::wstring result;result.reserve(value.size());enum class State{Text,Escape,Csi,Osc,OscEscape};State state=State::Text;for(const wchar_t ch:value){switch(state){case State::Text:if(ch==L'\x1b')state=State::Escape;else if(ch==L'\r'||ch==L'\n'||ch==L'\t'||ch>=L' ')result.push_back(ch);break;case State::Escape:if(ch==L'[')state=State::Csi;else if(ch==L']')state=State::Osc;else state=State::Text;break;case State::Csi:if(ch>=0x40&&ch<=0x7e)state=State::Text;break;case State::Osc:if(ch==L'\a')state=State::Text;else if(ch==L'\x1b')state=State::OscEscape;break;case State::OscEscape:state=ch==L'\\'?State::Text:State::Osc;break;}}return result;}
size_t WorkingSetMegabytes(){PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);return GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters))?static_cast<size_t>(counters.WorkingSetSize/(1024ull*1024ull)):0;}
void RecordAndBroadcastEvent(const std::wstring& project_id,const std::wstring& event_type,const nlohmann::json& payload){if(project_id.empty())return;const std::string payload_json=payload.dump(-1,' ',false,nlohmann::json::error_handler_t::replace);const auto id=g_store.AppendEvent(project_id,event_type,payload_json);nlohmann::json event{{"id",id},{"eventId",id},{"conversationId",Utf8Text(project_id)},{"event_type",Utf8Text(event_type)},{"type",Utf8Text(event_type)},{"payload",payload}};g_remote_agent.BroadcastEvent(project_id,event.dump(-1,' ',false,nlohmann::json::error_handler_t::replace));}
std::wstring NormalizeTerminalSnapshot(std::wstring value){
    std::wstring normalized;normalized.reserve(value.size());
    for(size_t i=0;i<value.size();++i){
        const wchar_t ch=value[i];
        if(ch==L'\r'){
            if(i+1<value.size()&&value[i+1]==L'\n')++i;
            normalized.push_back(L'\n');
        }else normalized.push_back(ch);
    }
    size_t line_start=0;
    for(size_t i=0;i<=normalized.size();++i){
        if(i==normalized.size()||normalized[i]==L'\n'){
            size_t end=i;
            while(end>line_start&&(normalized[end-1]==L' '||normalized[end-1]==L'\t'))--end;
            if(end<i)normalized.erase(end,i-end);
            i=end;line_start=i+1;
        }
    }
    while(!normalized.empty()&&(normalized.back()==L'\n'||normalized.back()==L' '||normalized.back()==L'\t'))normalized.pop_back();
    constexpr size_t max_chars=32*1024;
    constexpr size_t max_lines=300;
    size_t start=normalized.size()>max_chars?normalized.size()-max_chars:0;
    size_t lines=0;
    for(size_t i=normalized.size();i>start;--i){
        if(normalized[i-1]==L'\n'&&++lines>=max_lines){start=i;break;}
    }
    if(start>0){
        const auto next=normalized.find(L'\n',start);
        start=next==std::wstring::npos?normalized.size():next+1;
        normalized.erase(0,start);
    }
    return normalized;
}
void QueueTerminalSnapshot(const std::wstring& project_id){
    if(project_id.empty()||!g_hwnd)return;
    {
        std::scoped_lock lock(g_terminal_snapshot_mutex);
        if(std::find(g_pending_terminal_snapshot_projects.begin(),g_pending_terminal_snapshot_projects.end(),project_id)==g_pending_terminal_snapshot_projects.end())
            g_pending_terminal_snapshot_projects.push_back(project_id);
    }
    g_terminal_snapshot_dirty=true;
    if(!g_terminal_snapshot_message_pending.exchange(true))PostMessageW(g_hwnd,WM_TERMINAL_DIRTY,0,0);
}
void PublishTerminalSnapshots(){
    std::vector<std::wstring> project_ids;
    {std::scoped_lock lock(g_terminal_snapshot_mutex);project_ids.swap(g_pending_terminal_snapshot_projects);}
    for(const auto& project_id:project_ids){
        auto snapshot=NormalizeTerminalSnapshot(g_island.ReadEntireBuffer(project_id));
        if(snapshot.empty())continue;
        const auto previous=g_last_terminal_snapshots.find(project_id);
        if(previous!=g_last_terminal_snapshots.end()&&previous->second==snapshot)continue;
        g_last_terminal_snapshots[project_id]=snapshot;
        g_store.SaveTerminalSnapshot(project_id,snapshot);
        nlohmann::json event{{"conversationId",Utf8Text(project_id)},
                             {"event_type","terminal.snapshot"},
                             {"type","terminal.snapshot"},
                             {"payload",{{"text",Utf8Text(snapshot)},{"replace",true}}}};
        g_remote_agent.BroadcastEvent(project_id,event.dump(-1,' ',false,nlohmann::json::error_handler_t::replace));
    }
}
void ApplyRoundedRegion(HWND h,int width,int height,int radius){if(!h||width<=0||height<=0)return;SetWindowRgn(h,CreateRoundRectRgn(0,0,width+1,height+1,radius,radius),TRUE);}
void DrawRoundedOutline(HDC dc,RECT r,COLORREF color,int radius){HPEN p=CreatePen(PS_SOLID,1,color);auto oldp=SelectObject(dc,p);auto oldb=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));RoundRect(dc,r.left,r.top,r.right,r.bottom,radius,radius);SelectObject(dc,oldp);SelectObject(dc,oldb);DeleteObject(p);}
HWND MakeDialogControl(LPCWSTR cls,LPCWSTR text,DWORD style,int id,HWND parent,int x,int y,int w,int h){bool button=wcscmp(cls,L"BUTTON")==0;if(button)style|=BS_OWNERDRAW;HWND c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,parent,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);SendMessageW(c,WM_SETFONT,(WPARAM)g_font,TRUE);if(button)EnableButtonHover(c);return c;}
void ReadDialogText(HWND c,std::wstring& value){wchar_t b[1024]{};GetWindowTextW(c,b,1024);value=b;}
bool PickFolder(HWND owner,std::wstring& path);
std::wstring DefaultProjectCommand(const std::wstring& type){
    const auto found=std::find_if(g_project_types.begin(),g_project_types.end(),[&](const ProjectType& value){return _wcsicmp(value.name.c_str(),type.c_str())==0;});
    if(found!=g_project_types.end())return found->default_command;
    return type==L"Claude"?L"claude --dangerously-skip-permissions --continue":L"codex --yolo --no-alt-screen resume --last";
}
void LayoutProjectDialog(HWND h){
    RECT client{}; GetClientRect(h,&client);
    const int availableWidth=static_cast<int>(client.right-client.left);
    const int width=std::min(620,std::max(540,availableWidth-96));
    const int height=476;
    const int left=(client.right-width)/2, top=(client.bottom-height)/2;
    g_project_panel={left,top,left+width,top+height};
    constexpr int padding=34;
    g_d_name_rect={left+padding,top+107,left+width-padding,top+143};
    g_d_type_rect={left+padding,top+181,left+width/2-12,top+217};
    g_d_group_rect={left+width/2+12,top+181,left+width-padding,top+217};
    g_d_folder_rect={left+padding,top+255,left+width-padding-122,top+291};
    g_d_command_rect={left+padding,top+329,left+width-padding,top+365};
    auto placeEdit=[&](HWND control,RECT outer){if(!control)return;aicli::ui::LayoutStyledInput(control,outer,2,5);};
    auto placeDropDown=[&](HWND control,RECT outer){if(!control)return;const int w=outer.right-outer.left-4,h=outer.bottom-outer.top-8;MoveWindow(control,outer.left+2,outer.top+4,w,h,TRUE);ApplyRoundedRegion(control,w,h,5);};
    auto placeButton=[&](HWND control,RECT outer){if(!control)return;MoveWindow(control,outer.left,outer.top,outer.right-outer.left,outer.bottom-outer.top,TRUE);ApplyRoundedRegion(control,outer.right-outer.left,outer.bottom-outer.top,7);};
    placeEdit(g_d_name,g_d_name_rect);if(g_project_dialog_edit)placeEdit(g_d_type,g_d_type_rect);else placeDropDown(g_d_type,g_d_type_rect);placeDropDown(g_d_group,g_d_group_rect);placeEdit(g_d_folder,g_d_folder_rect);placeEdit(g_d_command,g_d_command_rect);
    RECT browse{g_d_folder_rect.right+12,g_d_folder_rect.top,g_project_panel.right-padding,g_d_folder_rect.bottom}; placeButton(g_d_folder_browse,browse);
    RECT save{left+padding,top+416,left+width/2-7,top+452}, cancel{left+width/2+7,top+416,left+width-padding,top+452};
    HWND child=GetWindow(h,GW_CHILD); while(child){int id=GetDlgCtrlID(child);if(id==ID_D_OK){MoveWindow(child,save.left,save.top,save.right-save.left,save.bottom-save.top,TRUE);ApplyRoundedRegion(child,save.right-save.left,save.bottom-save.top,8);}if(id==ID_D_CANCEL){MoveWindow(child,cancel.left,cancel.top,cancel.right-cancel.left,cancel.bottom-cancel.top,TRUE);ApplyRoundedRegion(child,cancel.right-cancel.left,cancel.bottom-cancel.top,8);}child=GetWindow(child,GW_HWNDNEXT);}
    aicli::ui::SetModalSurfaceRegion(h,g_project_panel,28);
    InvalidateRect(h,nullptr,FALSE);
}

void InvalidateProjectControlFrame(HWND dialog, HWND control) {
    if (!dialog || !control) return;
    RECT frame{};
    if (control == g_d_name) frame = g_d_name_rect;
    else if (control == g_d_type) frame = g_d_type_rect;
    else if (control == g_d_group) frame = g_d_group_rect;
    else if (control == g_d_folder) frame = g_d_folder_rect;
    else if (control == g_d_command) frame = g_d_command_rect;
    else {
        GetWindowRect(control, &frame);
        MapWindowPoints(HWND_DESKTOP, dialog,
                        reinterpret_cast<POINT*>(&frame), 2);
    }
    InflateRect(&frame, 2, 2);
    InvalidateRect(dialog, &frame, FALSE);
}

// The shared ModalHost surface.  Keeping this renderer palette-driven means
// a live theme switch changes the dialog without rebuilding its controls.
void PaintProjectDialogSurface(HDC dc,RECT client){
    // The layered ModalWorkspaceScope scrim is behind this content window.
    // Do not paint an opaque backdrop here; doing so would hide the live
    // terminal/workspace and defeat modal translucency.
    (void)client;
    DrawSmoothRoundedRect(dc,g_project_panel,14,panel,border,1.0f);
    SetBkMode(dc,TRANSPARENT);
    SelectObject(dc,g_font_modal_title?g_font_modal_title:g_font_medium);
    SetTextColor(dc,fg);
    RECT title{g_project_panel.left+34,g_project_panel.top+24,g_project_panel.right-34,g_project_panel.top+50};
    DrawTextW(dc,g_project_dialog_edit?L"编辑项目":L"新建项目",-1,&title,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    SelectObject(dc,g_font_small);SetTextColor(dc,muted);
    RECT subtitle{g_project_panel.left+34,g_project_panel.top+53,g_project_panel.right-34,g_project_panel.top+75};
    DrawTextW(dc,L"配置项目信息与 AI CLI 启动环境",-1,&subtitle,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    SetTextColor(dc,muted);
    auto label=[&](const wchar_t* value,int y,int x=g_project_panel.left+34){
        RECT rect{x,y,x+250,y+22};DrawTextW(dc,value,-1,&rect,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    };
    label(L"项目名称 / Tab 标题",g_project_panel.top+81);
    label(L"项目类型",g_project_panel.top+155);
    label(L"分组",g_project_panel.top+155,g_project_panel.left+(g_project_panel.right-g_project_panel.left)/2+12);
    label(L"项目路径",g_project_panel.top+229);label(L"启动命令",g_project_panel.top+303);
    if(g_project_dialog_edit){
        SetTextColor(dc,g_theme.titlebar_muted);
        RECT locked{g_d_type_rect.right-96,g_project_panel.top+155,g_d_type_rect.right,g_project_panel.top+177};
        DrawTextW(dc,L"不可修改",-1,&locked,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    }
    HPEN divider=CreatePen(PS_SOLID,1,border);auto old=SelectObject(dc,divider);
    MoveToEx(dc,g_project_panel.left+34,g_project_panel.top+394,nullptr);
    LineTo(dc,g_project_panel.right-34,g_project_panel.top+394);
    SelectObject(dc,old);DeleteObject(divider);
    auto outline=[&](RECT rect,HWND control){
        const bool active=GetFocus()==control;
        DrawRoundedOutline(dc,rect,active?accent:border,6);
        RECT underline{rect.left+2,rect.bottom-(active?3:2),rect.right-2,rect.bottom-1};
        HBRUSH line=CreateSolidBrush(active?accent:muted);FillRect(dc,&underline,line);DeleteObject(line);
    };
    outline(g_d_name_rect,g_d_name);outline(g_d_type_rect,g_d_type);
    outline(g_d_group_rect,g_d_group);outline(g_d_folder_rect,g_d_folder);outline(g_d_command_rect,g_d_command);
}
void DrawProjectDropdownItem(const DRAWITEMSTRUCT* item){
    const auto* entry=reinterpret_cast<const ProjectDropdownEntry*>(item->itemData);if(!entry)return;
    RECT r=item->rcItem;const bool hover=(item->itemState&ODS_SELECTED)!=0;
    HBRUSH fill=CreateSolidBrush(hover?g_theme.selection:g_theme.list_surface);FillRect(item->hDC,&r,fill);DeleteObject(fill);
    SetBkMode(item->hDC,TRANSPARENT);SelectObject(item->hDC,g_font);
    SetTextColor(item->hDC,entry->selected?accent:fg);
    RECT mark{r.left+10,r.top,r.left+34,r.bottom};DrawTextW(item->hDC,entry->selected?L"✓":L"",-1,&mark,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    RECT textRect{r.left+38,r.top,r.right-12,r.bottom};DrawTextW(item->hDC,entry->label.c_str(),-1,&textRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
}
void ShowProjectDropdown(HWND dialog,HWND anchor,bool projectType){
    std::vector<std::wstring> options;
    if(projectType){for(const auto& type:g_project_types)options.push_back(type.name);}
    else{
        for(const auto& group:g_groups)options.push_back(group.name);
        if(!g_project_draft.group.empty()&&std::find(options.begin(),options.end(),g_project_draft.group)==options.end())options.push_back(g_project_draft.group);
    }
    wchar_t currentText[256]{};GetWindowTextW(anchor,currentText,256);const std::wstring current=currentText;
    std::vector<ProjectDropdownEntry> entries;entries.reserve(options.size());for(const auto& option:options)entries.push_back({option,option==current});
    HMENU menu=CreatePopupMenu();if(!menu)return;HBRUSH background=CreateSolidBrush(g_theme.list_surface);MENUINFO info{sizeof(info)};info.fMask=MIM_BACKGROUND|MIM_STYLE;info.hbrBack=background;info.dwStyle=MNS_NOCHECK;SetMenuInfo(menu,&info);
    constexpr UINT base=2400;for(size_t i=0;i<entries.size();++i)AppendMenuW(menu,MF_OWNERDRAW|MF_ENABLED,base+(UINT)i,reinterpret_cast<LPCWSTR>(&entries[i]));
    RECT anchorRect{};GetWindowRect(anchor,&anchorRect);g_project_dropdown_width=anchorRect.right-anchorRect.left;
    SetFocus(anchor);const UINT selected=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_LEFTALIGN|TPM_TOPALIGN|TPM_RIGHTBUTTON,anchorRect.left,anchorRect.bottom+2,dialog,nullptr);
    if(selected>=base&&selected<base+entries.size()){
        const std::wstring value=entries[selected-base].label;
        wchar_t previousType[256]{}; GetWindowTextW(anchor, previousType, 256);
        SetWindowTextW(anchor,value.c_str());
        if(projectType){
            wchar_t command[1024]{}; GetWindowTextW(g_d_command,command,1024);
            const std::wstring currentCommand=command;
            const std::wstring previousDefault=DefaultProjectCommand(previousType);
            const bool useDefault=currentCommand.empty()||currentCommand==previousDefault;
            if(useDefault) SetWindowTextW(g_d_command,DefaultProjectCommand(value).c_str());
        }
    }
    DestroyMenu(menu);DeleteObject(background);
    InvalidateRect(anchor,nullptr,FALSE);
}
LRESULT CALLBACK ProjectDialogProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_CREATE:{
        const std::wstring initialType=g_project_dialog_edit?g_project_draft.type:(g_project_types.empty()?L"Codex":g_project_types.front().name);
        g_d_name=aicli::ui::CreateStyledInput(h,ID_D_NAME,g_font,L"输入项目名称");
        g_d_type=g_project_dialog_edit
            ?aicli::ui::CreateStyledInput(h,ID_D_TYPE,g_font,L"",true)
            :MakeDialogControl(L"BUTTON",initialType.c_str(),BS_NOTIFY|WS_TABSTOP,ID_D_TYPE,h,0,0,1,1);
        const std::wstring initialGroup=g_project_draft.group.empty()?(g_group_name==kAllGroupsName?L"默认分组":g_group_name):g_project_draft.group;
        g_d_group=MakeDialogControl(L"BUTTON",initialGroup.c_str(),BS_NOTIFY|WS_TABSTOP,ID_D_GROUP,h,0,0,1,1);
        g_d_folder=aicli::ui::CreateStyledInput(h,ID_D_FOLDER,g_font,L"请选择项目文件夹",true);
        g_d_folder_browse=MakeDialogControl(L"BUTTON",L"选择文件夹",BS_PUSHBUTTON|WS_TABSTOP,ID_D_BROWSE,h,0,0,1,1);
        g_d_command=aicli::ui::CreateStyledInput(h,ID_D_COMMAND,g_font,L"输入启动命令");
        for(HWND field:{g_d_name,g_d_type,g_d_folder,g_d_command})
            aicli::ui::SetStyledInputTheme(field,g_theme);
        MakeDialogControl(L"BUTTON",L"保存",BS_DEFPUSHBUTTON|WS_TABSTOP,ID_D_OK,h,0,0,1,1);
        MakeDialogControl(L"BUTTON",L"取消",BS_PUSHBUTTON|WS_TABSTOP,ID_D_CANCEL,h,0,0,1,1);
        if(g_project_dialog_edit){aicli::ui::SetStyledInputText(g_d_type,g_project_draft.type);aicli::ui::SetStyledInputText(g_d_name,g_project_draft.name);aicli::ui::SetStyledInputText(g_d_folder,g_project_draft.folder);aicli::ui::SetStyledInputText(g_d_command,g_project_draft.command);}else{aicli::ui::SetStyledInputText(g_d_command,DefaultProjectCommand(initialType));}
        LayoutProjectDialog(h);SetFocus(g_d_name);return 0; }
    case WM_SIZE: LayoutProjectDialog(h);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT client{};GetClientRect(h,&client);PaintProjectDialogSurface(dc,client);EndPaint(h,&ps);return 0;}
    case WM_NCHITTEST:return HTCLIENT;
    case WM_CTLCOLORSTATIC:{
        HDC dc=(HDC)w;HWND control=(HWND)l;
        if(control==g_d_folder||(g_project_dialog_edit&&control==g_d_type)){
            const bool active=GetFocus()==control;const COLORREF color=active?g_theme.field_focus:g_theme.field;
            SetTextColor(dc,active?fg:g_theme.list_text);SetBkColor(dc,color);SetBkMode(dc,OPAQUE);
            return (LRESULT)(active?g_focus_brush:g_edit_brush);
        }
        SetTextColor(dc,fg);SetBkMode(dc,TRANSPARENT);return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_CTLCOLOREDIT:{HDC dc=(HDC)w;HWND control=(HWND)l;const bool active=GetFocus()==control;const COLORREF color=active?g_theme.field_focus:g_theme.field;SetTextColor(dc,fg);SetBkColor(dc,color);return (LRESULT)(active?g_focus_brush:g_edit_brush);}
    case WM_CTLCOLORLISTBOX:{HDC dc=(HDC)w;SetTextColor(dc,fg);SetBkColor(dc,g_theme.list_surface);return (LRESULT)g_list_brush;}
    case WM_MEASUREITEM:{auto* item=(MEASUREITEMSTRUCT*)l;if(item->CtlType==ODT_MENU){item->itemWidth=std::max(160,g_project_dropdown_width-21);item->itemHeight=34;return TRUE;}}break;
    case WM_DRAWITEM: {auto* item=(DRAWITEMSTRUCT*)l;if(item->CtlType==ODT_MENU){DrawProjectDropdownItem(item);return TRUE;}if(item->CtlType==ODT_BUTTON){DrawModernButton(item);return TRUE;}} break;
    case WM_COMMAND:
        if((HIWORD(w)==EN_SETFOCUS||HIWORD(w)==EN_KILLFOCUS||HIWORD(w)==BN_SETFOCUS||HIWORD(w)==BN_KILLFOCUS)&&
           (LOWORD(w)==ID_D_NAME||LOWORD(w)==ID_D_TYPE||LOWORD(w)==ID_D_GROUP||LOWORD(w)==ID_D_FOLDER||LOWORD(w)==ID_D_COMMAND)){
            InvalidateProjectControlFrame(h,(HWND)l);
        }
        if(LOWORD(w)==ID_D_TYPE&&HIWORD(w)==BN_CLICKED&&!g_project_dialog_edit){ShowProjectDropdown(h,g_d_type,true);return 0;}
        if(LOWORD(w)==ID_D_GROUP&&HIWORD(w)==BN_CLICKED){ShowProjectDropdown(h,g_d_group,false);return 0;}
        if(LOWORD(w)==ID_D_BROWSE){std::wstring selected;if(PickFolder(h,selected))aicli::ui::SetStyledInputText(g_d_folder,selected);InvalidateProjectControlFrame(h,g_d_folder);return 0;}
        if(LOWORD(w)==ID_D_CANCEL){DestroyWindow(h);return 0;}
        if(LOWORD(w)==ID_D_OK){
            g_project_draft.name=aicli::ui::GetStyledInputText(g_d_name);
            g_project_draft.folder=aicli::ui::GetStyledInputText(g_d_folder);
            g_project_draft.command=aicli::ui::GetStyledInputText(g_d_command);
            if(g_project_dialog_edit) g_project_draft.type=aicli::ui::GetStyledInputText(g_d_type);
            else { wchar_t type[256]{}; GetWindowTextW(g_d_type,type,256); g_project_draft.type=type; }
            wchar_t group[256]{}; GetWindowTextW(g_d_group,group,256); g_project_draft.group=group;
            if(g_project_draft.name.empty()){MessageBoxW(h,L"项目名称不能为空。",L"项目详情",MB_OK|MB_ICONWARNING);return 0;}
            std::error_code folderError;
            if(g_project_draft.folder.empty()||!std::filesystem::is_directory(g_project_draft.folder,folderError)){MessageBoxW(h,L"请选择一个存在的项目文件夹。",L"项目详情",MB_OK|MB_ICONWARNING);return 0;}
            if(g_project_draft.command.empty())g_project_draft.command=DefaultProjectCommand(g_project_draft.type);
            const bool knownType=std::any_of(g_project_types.begin(),g_project_types.end(),[&](const ProjectType& value){return _wcsicmp(value.name.c_str(),g_project_draft.type.c_str())==0;});
            const bool knownGroup=std::any_of(g_groups.begin(),g_groups.end(),[&](const ProjectGroup& value){return _wcsicmp(value.name.c_str(),g_project_draft.group.c_str())==0;});
            if(g_project_draft.type.empty()||!knownType||g_project_draft.group.empty()||!knownGroup){MessageBoxW(h,L"请选择有效的项目类型和分组。",L"项目详情",MB_OK|MB_ICONWARNING);return 0;}
            g_project_dialog_ok=true;DestroyWindow(h);return 0;
        } break;
    case WM_KEYDOWN:if(w==VK_ESCAPE){DestroyWindow(h);return 0;}break;
    case WM_DESTROY:g_project_dialog=nullptr;g_d_name=g_d_type=g_d_group=g_d_folder=g_d_folder_browse=g_d_command=nullptr;return 0;
    case WM_CLOSE:DestroyWindow(h);return 0; }
    return DefWindowProcW(h,m,w,l);
}
bool ShowProjectDialog(HWND owner,const Project* initial=nullptr){
    static bool registered=false;
    if(!registered){
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=ProjectDialogProc;
        wc.lpszClassName=L"AicliProjectDialog";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        wc.hbrBackground=nullptr;wc.style=CS_DBLCLKS;RegisterClassW(&wc);registered=true;
    }
    g_project_dialog_ok=false;g_project_dialog_edit=initial!=nullptr;
    g_project_draft=initial?*initial:Project{};
    aicli::ui::ModalWorkspaceScope workspace(owner,aicli::ui::ModalTopForOwner(owner),&g_theme);
    g_project_dialog=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_TOOLWINDOW,L"AicliProjectDialog",L"",
        WS_POPUP|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,1,1,
        owner,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!g_project_dialog){g_project_dialog=nullptr;Layout();return false;}
    workspace.AttachSurface(g_project_dialog);
    aicli::ui::ActivateModalSurface(g_project_dialog, g_d_name);
    MSG msg{};
    while(IsWindow(g_project_dialog)&&GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    g_project_dialog=nullptr;
    aicli::ui::RestoreModalOwner(owner);
    return g_project_dialog_ok;
}

void RefreshBindingPairing(){
    g_binding_info=g_remote_agent.CreatePairing();g_pending_pairing.reset();g_binding_qr.reset();
    try{const auto value=Utf8Text(g_binding_info.qr_payload);g_binding_qr=qrcodegen::QrCode::encodeText(value.c_str(),qrcodegen::QrCode::Ecc::MEDIUM);}catch(...){g_binding_qr.reset();}
    if(g_binding_dialog)InvalidateRect(g_binding_dialog,nullptr,FALSE);
}
void LayoutBindingDialog(HWND h){RECT client{};GetClientRect(h,&client);const int width=std::min(820,std::max(700,(int)client.right-96));const int height=590;const int left=(client.right-width)/2,top=(client.bottom-height)/2;g_binding_panel={left,top,left+width,top+height};MoveWindow(g_binding_refresh,g_binding_panel.right-236,g_binding_panel.bottom-58,100,36,TRUE);MoveWindow(g_binding_close,g_binding_panel.right-124,g_binding_panel.bottom-58,90,36,TRUE);MoveWindow(g_binding_approve,g_binding_panel.right-236,g_binding_panel.bottom-106,100,36,TRUE);MoveWindow(g_binding_reject,g_binding_panel.right-124,g_binding_panel.bottom-106,90,36,TRUE);const bool pending=g_pending_pairing.has_value()&&g_pending_pairing->submitted&&!g_pending_pairing->authorized&&!g_pending_pairing->rejected;ShowWindow(g_binding_approve,pending?SW_SHOW:SW_HIDE);ShowWindow(g_binding_reject,pending?SW_SHOW:SW_HIDE);aicli::ui::SetModalSurfaceRegion(h,g_binding_panel,28);}
void PaintBindingDialog(HDC dc,RECT client){
    HBRUSH fallback=CreateSolidBrush(BlendBackdropColor(bg,g_theme.scrim,150));FillRect(dc,&client,fallback);DeleteObject(fallback);
    HBRUSH panelBrush=CreateSolidBrush(RGB(29,31,35));HPEN panelPen=CreatePen(PS_SOLID,1,RGB(59,64,73));auto oldBrush=SelectObject(dc,panelBrush);auto oldPen=SelectObject(dc,panelPen);RoundRect(dc,g_binding_panel.left,g_binding_panel.top,g_binding_panel.right,g_binding_panel.bottom,14,14);SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(panelBrush);DeleteObject(panelPen);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,fg);SelectObject(dc,g_font_modal_title);RECT title{g_binding_panel.left+34,g_binding_panel.top+24,g_binding_panel.right-34,g_binding_panel.top+54};DrawTextW(dc,L"绑定 Android 手机",-1,&title,DT_LEFT|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,g_font_small);SetTextColor(dc,muted);RECT subtitle{g_binding_panel.left+34,g_binding_panel.top+56,g_binding_panel.right-34,g_binding_panel.top+82};DrawTextW(dc,L"必须使用 AI CLI App 内的扫码入口；电脑只需确认这一次",-1,&subtitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const int qrArea=360,qrLeft=g_binding_panel.left+34,qrTop=g_binding_panel.top+100;HBRUSH white=CreateSolidBrush(RGB(245,247,250));RECT qrBack{qrLeft,qrTop,qrLeft+qrArea,qrTop+qrArea};FillRect(dc,&qrBack,white);DeleteObject(white);if(g_binding_qr){const int modules=g_binding_qr->getSize();const int quiet=4;const int scale=std::max(1,qrArea/(modules+quiet*2));const int actual=scale*(modules+quiet*2);const int originX=qrLeft+(qrArea-actual)/2+quiet*scale,originY=qrTop+(qrArea-actual)/2+quiet*scale;HBRUSH black=CreateSolidBrush(RGB(18,20,24));for(int y=0;y<modules;++y)for(int x=0;x<modules;++x)if(g_binding_qr->getModule(x,y)){RECT module{originX+x*scale,originY+y*scale,originX+(x+1)*scale,originY+(y+1)*scale};FillRect(dc,&module,black);}DeleteObject(black);}else{SetTextColor(dc,RGB(40,43,50));SelectObject(dc,g_font);DrawTextW(dc,L"二维码生成失败",-1,&qrBack,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
    const int infoLeft=qrLeft+qrArea+34,infoRight=g_binding_panel.right-34;SelectObject(dc,g_font);SetTextColor(dc,RGB(219,224,232));RECT statusTitle{infoLeft,qrTop,infoRight,qrTop+30};DrawTextW(dc,L"等待手机扫码",-1,&statusTitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,g_font_small);SetTextColor(dc,muted);std::wstring localAddress=g_store.GetSetting(L"local_address");if(localAddress.empty()){try{const auto payload=nlohmann::json::parse(Utf8Text(g_binding_info.qr_payload));localAddress=WideText(payload.value("localAddress",std::string{}));}catch(...){}}std::wstring details=L"本地地址\n"+localAddress+L"\n\n证书指纹\n"+g_binding_info.certificate_fingerprint.substr(0,std::min<size_t>(24,g_binding_info.certificate_fingerprint.size()))+L"…\n\n二维码 5 分钟内有效。已绑定手机断网或电脑重启后不需要重新确认。若首次局域网连接超时，请在设置中执行一次“允许手机访问”。";RECT detailRect{infoLeft,qrTop+40,infoRight,qrTop+258};DrawTextW(dc,details.c_str(),-1,&detailRect,DT_LEFT|DT_TOP|DT_WORDBREAK);
    if(g_pending_pairing&&g_pending_pairing->submitted){SetTextColor(dc,g_pending_pairing->authorized?RGB(83,203,146):g_pending_pairing->rejected?RGB(225,101,112):RGB(255,196,92));SelectObject(dc,g_font_medium);RECT phoneTitle{infoLeft,qrTop+268,infoRight,qrTop+298};const wchar_t* state=g_pending_pairing->authorized?L"绑定完成":g_pending_pairing->rejected?L"已拒绝":L"等待电脑确认";DrawTextW(dc,state,-1,&phoneTitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,g_font_small);SetTextColor(dc,fg);std::wstring phone=L"设备："+g_pending_pairing->device.name+L"\n型号："+g_pending_pairing->device.model;RECT phoneRect{infoLeft,qrTop+302,infoRight,qrTop+360};DrawTextW(dc,phone.c_str(),-1,&phoneRect,DT_LEFT|DT_TOP|DT_WORDBREAK);}
}

void PaintBindingDialogSurface(HDC dc,RECT client){
    (void)client;
    DrawSmoothRoundedRect(dc,g_binding_panel,14,panel,border,1.0f);
    SetBkMode(dc,TRANSPARENT);SelectObject(dc,g_font_modal_title);SetTextColor(dc,fg);
    RECT title{g_binding_panel.left+34,g_binding_panel.top+24,g_binding_panel.right-34,g_binding_panel.top+54};
    DrawTextW(dc,L"绑定 Android 手机",-1,&title,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,g_font_small);SetTextColor(dc,muted);
    RECT subtitle{g_binding_panel.left+34,g_binding_panel.top+56,g_binding_panel.right-34,g_binding_panel.top+82};
    DrawTextW(dc,L"必须使用 AI CLI App 内的扫码入口；电脑只需确认这一次",-1,&subtitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const int qrArea=360,qrLeft=g_binding_panel.left+34,qrTop=g_binding_panel.top+100;
    HBRUSH white=CreateSolidBrush(RGB(245,247,250));RECT qrBack{qrLeft,qrTop,qrLeft+qrArea,qrTop+qrArea};
    FillRect(dc,&qrBack,white);DeleteObject(white);
    if(g_binding_qr){
        const int modules=g_binding_qr->getSize(),quiet=4,scale=std::max(1,qrArea/(modules+quiet*2));
        const int actual=scale*(modules+quiet*2);
        const int originX=qrLeft+(qrArea-actual)/2+quiet*scale,originY=qrTop+(qrArea-actual)/2+quiet*scale;
        HBRUSH black=CreateSolidBrush(RGB(18,20,24));
        for(int y=0;y<modules;++y)for(int x=0;x<modules;++x)if(g_binding_qr->getModule(x,y)){
            RECT module{originX+x*scale,originY+y*scale,originX+(x+1)*scale,originY+(y+1)*scale};FillRect(dc,&module,black);
        }
        DeleteObject(black);
    }else{SetTextColor(dc,fg);SelectObject(dc,g_font);DrawTextW(dc,L"二维码生成失败",-1,&qrBack,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
    const int infoLeft=qrLeft+qrArea+34,infoRight=g_binding_panel.right-34;
    SelectObject(dc,g_font);SetTextColor(dc,fg);RECT statusTitle{infoLeft,qrTop,infoRight,qrTop+30};
    DrawTextW(dc,L"等待手机扫码",-1,&statusTitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,g_font_small);SetTextColor(dc,muted);
    std::wstring localAddress=g_store.GetSetting(L"local_address");
    if(localAddress.empty()){try{const auto payload=nlohmann::json::parse(Utf8Text(g_binding_info.qr_payload));localAddress=WideText(payload.value("localAddress",std::string{}));}catch(...){}}
    std::wstring details=L"本地地址\n"+localAddress+L"\n\n证书指纹\n"+g_binding_info.certificate_fingerprint.substr(0,std::min<size_t>(24,g_binding_info.certificate_fingerprint.size()))+L"…\n\n二维码 5 分钟内有效。已绑定手机断网或电脑重启后不需要重新确认。";
    RECT detailRect{infoLeft,qrTop+40,infoRight,qrTop+258};DrawTextW(dc,details.c_str(),-1,&detailRect,DT_LEFT|DT_TOP|DT_WORDBREAK);
    if(g_pending_pairing&&g_pending_pairing->submitted){
        SetTextColor(dc,g_pending_pairing->authorized?g_theme.success:g_pending_pairing->rejected?g_theme.danger:g_theme.warning);
        SelectObject(dc,g_font_medium);RECT phoneTitle{infoLeft,qrTop+268,infoRight,qrTop+298};
        const wchar_t* state=g_pending_pairing->authorized?L"绑定完成":g_pending_pairing->rejected?L"已拒绝":L"等待电脑确认";
        DrawTextW(dc,state,-1,&phoneTitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        SelectObject(dc,g_font_small);SetTextColor(dc,fg);
        std::wstring phone=L"设备："+g_pending_pairing->device.name+L"\n型号："+g_pending_pairing->device.model;
        RECT phoneRect{infoLeft,qrTop+302,infoRight,qrTop+360};DrawTextW(dc,phone.c_str(),-1,&phoneRect,DT_LEFT|DT_TOP|DT_WORDBREAK);
    }
}
LRESULT CALLBACK BindingDialogProc(HWND h,UINT m,WPARAM w,LPARAM l){switch(m){case WM_CREATE:g_binding_refresh=MakeDialogControl(L"BUTTON",L"刷新二维码",BS_PUSHBUTTON|WS_TABSTOP,ID_BIND_REFRESH,h,0,0,1,1);g_binding_close=MakeDialogControl(L"BUTTON",L"关闭",BS_PUSHBUTTON|WS_TABSTOP,ID_BIND_CLOSE,h,0,0,1,1);g_binding_approve=MakeDialogControl(L"BUTTON",L"确认绑定",BS_PUSHBUTTON|WS_TABSTOP,ID_BIND_APPROVE,h,0,0,1,1);g_binding_reject=MakeDialogControl(L"BUTTON",L"拒绝",BS_PUSHBUTTON|WS_TABSTOP,ID_BIND_REJECT,h,0,0,1,1);RefreshBindingPairing();LayoutBindingDialog(h);return 0;case WM_SIZE:LayoutBindingDialog(h);return 0;case WM_ERASEBKGND:return 1;case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT client{};GetClientRect(h,&client);PaintBindingDialogSurface(dc,client);EndPaint(h,&ps);return 0;}case WM_DRAWITEM:DrawModernButton((DRAWITEMSTRUCT*)l);return TRUE;case WM_COMMAND:if(LOWORD(w)==ID_BIND_REFRESH){RefreshBindingPairing();return 0;}if(LOWORD(w)==ID_BIND_CLOSE){DestroyWindow(h);return 0;}if((LOWORD(w)==ID_BIND_APPROVE||LOWORD(w)==ID_BIND_REJECT)&&g_pending_pairing){std::wstring error;if(!g_remote_agent.ConfirmPairing(g_pending_pairing->info.pairing_id,LOWORD(w)==ID_BIND_APPROVE,&error)&&!error.empty())MessageBoxW(h,error.c_str(),L"手机绑定",MB_OK|MB_ICONERROR);UpdateDeviceHeader();InvalidateRect(h,nullptr,FALSE);return 0;}break;case WM_KEYDOWN:if(w==VK_ESCAPE){DestroyWindow(h);return 0;}break;case WM_DESTROY:g_binding_dialog=nullptr;g_binding_refresh=g_binding_close=g_binding_approve=g_binding_reject=nullptr;return 0;}return DefWindowProcW(h,m,w,l);}
void ShowBindingDialog(HWND owner){
    if(!g_remote_agent.Online()){
        MessageBoxW(owner,L"HTTPS/WSS Agent 未启动，请先检查设置中的端口。",L"绑定手机",MB_OK|MB_ICONERROR);return;
    }
    static bool registered=false;
    if(!registered){
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=BindingDialogProc;
        wc.lpszClassName=L"AicliBindingDialog";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        wc.hbrBackground=nullptr;RegisterClassW(&wc);registered=true;
    }
    aicli::ui::ModalWorkspaceScope workspace(owner,aicli::ui::ModalTopForOwner(owner),&g_theme);
    g_binding_dialog=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_TOOLWINDOW,L"AicliBindingDialog",L"",
        WS_POPUP|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,1,1,
        owner,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!g_binding_dialog){Layout();return;}
    workspace.AttachSurface(g_binding_dialog);
    aicli::ui::ActivateModalSurface(g_binding_dialog);
    MSG msg{};
    while(IsWindow(g_binding_dialog)&&GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    g_binding_dialog=nullptr;
    aicli::ui::RestoreModalOwner(owner);
}

LRESULT CALLBACK ButtonProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_ERASEBKGND)return 1;
    if(m==WM_MOUSEMOVE){if(g_hover_button!=h){HWND old=g_hover_button;g_hover_button=h;if(old)InvalidateRect(old,nullptr,FALSE);InvalidateRect(h,nullptr,FALSE);TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);}}
    else if(m==WM_MOUSELEAVE){if(g_hover_button==h)g_hover_button=nullptr;InvalidateRect(h,nullptr,FALSE);}
    else if(m==WM_LBUTTONDOWN||m==WM_LBUTTONUP||m==WM_SETFOCUS||m==WM_KILLFOCUS){InvalidateRect(h,nullptr,FALSE);}
    return CallWindowProcW(g_button_proc,h,m,w,l);
}
void InvalidateProjectRow(HWND list,int row){
    if(!list||row<0)return;
    RECT item{};
    if(SendMessageW(list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))!=LB_ERR)
        InvalidateRect(list,&item,FALSE);
}
LRESULT CALLBACK ProjectListSubclassProc(HWND h,UINT m,WPARAM w,LPARAM l,
                                         UINT_PTR,DWORD_PTR){
    if(m==WM_ERASEBKGND){
        // Owner-drawn rows paint their complete cards.  Hover invalidations
        // use erase=FALSE, so they never pass through a blank frame.  A real
        // background erase (initial show/resize) still has to fill the empty
        // area below the last project, otherwise the LISTBOX appears
        // transparent and exposes windows behind the application.
        HDC dc=reinterpret_cast<HDC>(w);
        if(dc){
            RECT client{};GetClientRect(h,&client);
            HBRUSH brush=g_list_brush?g_list_brush:CreateSolidBrush(panel);
            FillRect(dc,&client,brush);
            if(!g_list_brush)DeleteObject(brush);
        }
        return 1;
    }else if(m==WM_MOUSEMOVE){
        const LRESULT hit=SendMessageW(h,LB_ITEMFROMPOINT,0,l);
        const int next=HIWORD(hit)?-1:static_cast<int>(LOWORD(hit));
        if(next!=g_project_hover_row){
            const int previous=g_project_hover_row;
            g_project_hover_row=next;
            InvalidateProjectRow(h,previous);
            InvalidateProjectRow(h,next);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,h,0};
        TrackMouseEvent(&tracking);
    }else if(m==WM_MOUSELEAVE){
        const int previous=g_project_hover_row;
        g_project_hover_row=-1;
        InvalidateProjectRow(h,previous);
    }else if(m==WM_SETCURSOR&&LOWORD(l)==HTCLIENT&&
             aicli::ui::GetStyledInputText(g_search).empty()){
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(h,&point);
        const LRESULT hit=SendMessageW(h,LB_ITEMFROMPOINT,0,MAKELPARAM(point.x,point.y));
        if(!HIWORD(hit)){
            RECT item{};
            const int row=static_cast<int>(LOWORD(hit));
            if(SendMessageW(h,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))!=LB_ERR){
                const RECT handle{item.left+3,item.top+3,item.left+31,item.bottom-3};
                if(PtInRect(&handle,point)){
                    SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));
                    return TRUE;
                }
            }
        }
    }else if(m==WM_SETFOCUS||m==WM_KILLFOCUS){
        const LRESULT result=DefSubclassProc(h,m,w,l);
        const LRESULT selected=SendMessageW(h,LB_GETCURSEL,0,0);
        if(selected!=LB_ERR)InvalidateProjectRow(h,static_cast<int>(selected));
        return result;
    }else if(m==WM_NCDESTROY){
        g_project_hover_row=-1;
        RemoveWindowSubclass(h,ProjectListSubclassProc,kProjectListSubclassId);
    }
    return DefSubclassProc(h,m,w,l);
}
void DrawModernButton(const DRAWITEMSTRUCT* item){
    RECT r=item->rcItem;const bool down=(item->itemState&ODS_SELECTED)!=0;const bool disabled=(item->itemState&ODS_DISABLED)!=0;const bool hover=item->hwndItem==g_hover_button;const bool focused=GetFocus()==item->hwndItem;HWND parentWindow=GetParent(item->hwndItem);
    const bool projectDropDown=parentWindow==g_project_dialog&&(item->CtlID==ID_D_TYPE||item->CtlID==ID_D_GROUP);
    const bool projectButton=parentWindow==g_project_dialog;
    COLORREF under=panel;
    bool onMainTitleBar=false;
    if(parentWindow==g_hwnd){POINT pos{};ClientToScreen(item->hwndItem,&pos);ScreenToClient(parentWindow,&pos);onMainTitleBar=pos.y<g_layout_metrics.title_bar_height;under=onMainTitleBar?TitleBarBackground():(pos.x<g_layout_metrics.sidebar_width?panel:bg);}
    HBRUSH clear=CreateSolidBrush(under);FillRect(item->hDC,&r,clear);DeleteObject(clear);
    wchar_t text[256]{};GetWindowTextW(item->hwndItem,text,256);SetBkMode(item->hDC,TRANSPARENT);
    const bool title_button=parentWindow==g_hwnd&&
        (item->CtlID==ID_MINIMIZE||item->CtlID==ID_MAXIMIZE||item->CtlID==ID_CLOSE);
    if(title_button){
        COLORREF fill=TitleBarBackground();
        if(hover){
            if(item->CtlID==ID_CLOSE)fill=g_theme.titlebar_close_hover;
            else fill=g_theme.titlebar_hover;
        }
        if(down){
            if(item->CtlID==ID_CLOSE)fill=g_theme.titlebar_close_pressed;
            else fill=g_theme.titlebar_pressed;
        }
        HBRUSH button_brush=CreateSolidBrush(fill);FillRect(item->hDC,&r,button_brush);DeleteObject(button_brush);
        const COLORREF glyph=(item->CtlID==ID_CLOSE&&hover)?RGB(255,255,255):TitleBarText();
        HPEN glyph_pen=CreatePen(PS_SOLID,1,glyph);auto old_pen=SelectObject(item->hDC,glyph_pen);
        const int cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;
        if(item->CtlID==ID_MINIMIZE){MoveToEx(item->hDC,cx-7,cy+1,nullptr);LineTo(item->hDC,cx+7,cy+1);}
        else if(item->CtlID==ID_MAXIMIZE){
            if(IsZoomed(g_hwnd)){MoveToEx(item->hDC,cx-4,cy-6,nullptr);LineTo(item->hDC,cx+7,cy-6);LineTo(item->hDC,cx+7,cy+5);MoveToEx(item->hDC,cx-7,cy-3,nullptr);LineTo(item->hDC,cx+4,cy-3);LineTo(item->hDC,cx+4,cy+8);LineTo(item->hDC,cx-7,cy+8);LineTo(item->hDC,cx-7,cy-3);}
            else{Rectangle(item->hDC,cx-7,cy-6,cx+7,cy+7);}
        }else{MoveToEx(item->hDC,cx-6,cy-6,nullptr);LineTo(item->hDC,cx+6,cy+6);MoveToEx(item->hDC,cx+6,cy-6,nullptr);LineTo(item->hDC,cx-6,cy+6);}
        SelectObject(item->hDC,old_pen);DeleteObject(glyph_pen);return;
    }
    if(projectDropDown){
        const COLORREF fill=down?g_theme.pressed:
            (focused?g_theme.field_focus:(hover?g_theme.hover:g_theme.field));
        HBRUSH brush=CreateSolidBrush(fill);auto oldBrush=SelectObject(item->hDC,brush);auto oldPen=SelectObject(item->hDC,GetStockObject(NULL_PEN));RoundRect(item->hDC,r.left,r.top,r.right,r.bottom,7,7);SelectObject(item->hDC,oldBrush);SelectObject(item->hDC,oldPen);DeleteObject(brush);
        SetTextColor(item->hDC,fg);SelectObject(item->hDC,g_font);RECT textRect{r.left+10,r.top,r.right-34,r.bottom};DrawTextW(item->hDC,text,-1,&textRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        SetTextColor(item->hDC,focused?accent:muted);SelectObject(item->hDC,g_icon_font?g_icon_font:g_font);RECT arrow{r.right-30,r.top,r.right-7,r.bottom};DrawTextW(item->hDC,L"\xE70D",-1,&arrow,DT_CENTER|DT_VCENTER|DT_SINGLELINE);return;
    }
    COLORREF fill=under,stroke=RGB(52,59,70);
    if(projectButton){
        const bool primary=item->CtlID==ID_D_OK;
        if(primary){fill=disabled?g_theme.surface:(down?g_theme.accent_strong:(hover?g_theme.accent:g_theme.accent_strong));stroke=hover?g_theme.accent:g_theme.accent_strong;}
        else{fill=disabled?g_theme.surface:(down?g_theme.pressed:(hover?g_theme.hover:g_theme.field));stroke=hover?g_theme.accent:g_theme.border;}
    }else{
        if(item->CtlID==ID_CLI_TAB||item->CtlID==ID_D_CANCEL||item->CtlID==ID_EDIT_PROJECT)fill=elevated;
        if(item->CtlID==ID_NEW_PROJECT||item->CtlID==ID_D_OK||item->CtlID==ID_START_PROJECT)fill=accent2;
        if(disabled)fill=g_light_theme?RGB(235,238,243):RGB(33,37,45);
        else if(down){
            // Header actions already provide a hover cue.  A bright blue
            // pressed frame is perceived as a flash when a modal opens on
            // the next compositor frame, especially on the fast second open.
            // Keep the pressed surface identical to hover for these actions.
            fill=onMainTitleBar
                ?(g_light_theme?RGB(229,235,243):RGB(34,40,51))
                :(g_light_theme?RGB(205,220,241):RGB(37,91,174));
        }else if(hover)fill=g_light_theme?RGB(229,235,243):RGB(34,40,51);
        if(item->CtlID==ID_CLOSE)fill=down?RGB(157,44,55):(hover?RGB(120,38,47):panel);if(item->CtlID==ID_MINIMIZE)fill=hover?RGB(37,42,51):panel;
        stroke=disabled?(g_light_theme?RGB(218,223,231):RGB(43,48,57)):(hover?(g_light_theme?RGB(164,181,205):RGB(78,104,142)):border);if(item->CtlID==ID_NEW_PROJECT||item->CtlID==ID_D_OK||item->CtlID==ID_START_PROJECT)stroke=hover?RGB(117,177,255):RGB(64,128,225);
    }
    HBRUSH brush=CreateSolidBrush(fill);HPEN pen=CreatePen(PS_SOLID,1,stroke);auto oldBrush=SelectObject(item->hDC,brush);auto oldPen=SelectObject(item->hDC,pen);RoundRect(item->hDC,r.left+1,r.top+1,r.right-1,r.bottom-1,9,9);SelectObject(item->hDC,oldBrush);SelectObject(item->hDC,oldPen);DeleteObject(brush);DeleteObject(pen);
    const bool primary_button=item->CtlID==ID_NEW_PROJECT||item->CtlID==ID_D_OK||item->CtlID==ID_START_PROJECT;COLORREF text_color=disabled?muted:((primary_button||(!g_light_theme&&hover))?RGB(255,255,255):fg);HFONT text_font=g_font;UINT text_format=DT_CENTER|DT_VCENTER|DT_SINGLELINE;if(onMainTitleBar&&!g_window_active&&!hover&&!down){text_color=TitleBarText();}SetTextColor(item->hDC,text_color);SelectObject(item->hDC,text_font);DrawTextW(item->hDC,text,-1,&r,text_format);
}
void DrawProjectItemContent(const DRAWITEMSTRUCT* item){
    if(item->itemID==(UINT)-1)return;
    const int projectIndex=static_cast<int>(
        SendMessageW(item->hwndItem,LB_GETITEMDATA,item->itemID,0));
    if(projectIndex<0||projectIndex>=static_cast<int>(g_projects.size()))return;

    const auto& project=g_projects[projectIndex];
    const bool selected=(item->itemState&ODS_SELECTED)!=0;
    const bool hovered=static_cast<int>(item->itemID)==g_project_hover_row;
    const bool sourceDragging=g_project_dragging&&
        static_cast<int>(item->itemID)==g_project_drag_source;
    RECT row=item->rcItem;
    HBRUSH rowBackground=CreateSolidBrush(panel);
    FillRect(item->hDC,&row,rowBackground);
    DeleteObject(rowBackground);

    RECT card{row.left+3,row.top+3,row.right-4,row.bottom-3};
    COLORREF cardFill=g_light_theme?RGB(250,251,253):RGB(22,25,32);
    COLORREF cardStroke=g_light_theme?RGB(222,227,234):RGB(38,43,52);
    if(sourceDragging){
        cardFill=panel;
        cardStroke=g_light_theme?RGB(169,181,197):RGB(72,84,102);
    }else if(selected){
        cardFill=g_light_theme?RGB(238,245,254):RGB(25,37,54);
        cardStroke=g_light_theme?RGB(119,163,218):RGB(61,102,157);
    }else if(hovered){
        cardFill=g_light_theme?RGB(243,247,252):RGB(29,34,43);
        cardStroke=g_light_theme?RGB(193,207,224):RGB(55,65,79);
    }
    DrawSmoothRoundedRect(item->hDC,card,9.0f,cardFill,cardStroke,1.0f,sourceDragging);

    COLORREF stateColor=g_light_theme?RGB(100,113,131):RGB(139,157,183);
    enum class StateIcon{Idle,Running,Paused,Warning};
    StateIcon stateIcon=StateIcon::Idle;
    if(g_island.HasSession(project.id)){
        stateIcon=StateIcon::Running;
        stateColor=g_light_theme?RGB(28,139,91):RGB(73,203,145);
    }else if(project.status==L"paused"){
        stateIcon=StateIcon::Paused;
        stateColor=g_light_theme?RGB(177,113,16):RGB(245,178,72);
    }else if(project.status==L"cancelled"||project.status==L"error"){
        // A stopped project is a recoverable state, so use a warm warning
        // triangle instead of a red close/cancel glyph.
        stateIcon=StateIcon::Warning;
        stateColor=g_light_theme?RGB(190,132,26):RGB(244,187,72);
    }
    if(sourceDragging)stateColor=muted;

    {
        Gdiplus::Graphics graphics(item->hDC);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        Gdiplus::Pen grip(SmoothColor(
            hovered&&!sourceDragging
                ?(g_light_theme?RGB(111,128,150):RGB(131,145,165))
                :(g_light_theme?RGB(157,166,178):RGB(91,101,116))),1.25f);
        grip.SetStartCap(Gdiplus::LineCapRound);
        grip.SetEndCap(Gdiplus::LineCapRound);
        const float gripCenter=(card.top+card.bottom)/2.0f;
        for(float offset=-4.0f;offset<=4.0f;offset+=4.0f)
            graphics.DrawLine(&grip,static_cast<float>(card.left+11),gripCenter+offset,
                              static_cast<float>(card.left+20),gripCenter+offset);

        const float iconSize=20.0f;
        const float iconX=static_cast<float>(card.right)-iconSize-11.0f;
        const float iconY=(card.top+card.bottom-iconSize)/2.0f;
        Gdiplus::RectF iconBounds{iconX,iconY,iconSize,iconSize};
        Gdiplus::SolidBrush halo(SmoothColor(stateColor,30));
        graphics.FillEllipse(&halo,iconBounds);
        Gdiplus::Pen iconPen(SmoothColor(stateColor),1.65f);
        iconPen.SetStartCap(Gdiplus::LineCapRound);
        iconPen.SetEndCap(Gdiplus::LineCapRound);
        Gdiplus::SolidBrush iconBrush(SmoothColor(stateColor));
        const float cx=iconX+iconSize/2.0f,cy=iconY+iconSize/2.0f;
        if(stateIcon==StateIcon::Running){
            graphics.DrawEllipse(&iconPen,iconX+2.0f,iconY+2.0f,iconSize-4.0f,iconSize-4.0f);
            graphics.FillEllipse(&iconBrush,cx-3.0f,cy-3.0f,6.0f,6.0f);
        }else if(stateIcon==StateIcon::Paused){
            graphics.DrawLine(&iconPen,cx-2.5f,cy-4.0f,cx-2.5f,cy+4.0f);
            graphics.DrawLine(&iconPen,cx+2.5f,cy-4.0f,cx+2.5f,cy+4.0f);
        }else if(stateIcon==StateIcon::Warning){
            Gdiplus::PointF triangle[3]{
                {cx,iconY+3.0f},
                {iconX+3.0f,iconY+iconSize-3.0f},
                {iconX+iconSize-3.0f,iconY+iconSize-3.0f}};
            graphics.DrawPolygon(&iconPen,triangle,3);
            graphics.DrawLine(&iconPen,cx,cy-2.5f,cx,cy+1.5f);
            graphics.FillEllipse(&iconBrush,cx-0.9f,cy+3.0f,1.8f,1.8f);
        }else{
            graphics.DrawEllipse(&iconPen,cx-4.0f,cy-4.0f,8.0f,8.0f);
        }
    }

    SetBkMode(item->hDC,TRANSPARENT);
    auto oldFont=SelectObject(item->hDC,g_font);
    wchar_t name[256]{};
    SendMessageW(item->hwndItem,LB_GETTEXT,item->itemID,(LPARAM)name);
    SetTextColor(item->hDC,sourceDragging?muted:fg);
    RECT title{card.left+29,card.top,card.right-39,card.bottom};
    DrawTextW(item->hDC,name,-1,&title,
              DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    SelectObject(item->hDC,oldFont);

    if(g_project_dragging){
        const int count=static_cast<int>(SendMessageW(item->hwndItem,LB_GETCOUNT,0,0));
        int markerY=-1;
        if(g_project_drag_insert==static_cast<int>(item->itemID))markerY=row.top+2;
        else if(g_project_drag_insert==count&&static_cast<int>(item->itemID)==count-1)
            markerY=row.bottom-3;
        if(markerY>=0){
            Gdiplus::Graphics graphics(item->hDC);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen glow(SmoothColor(accent,70),4.0f);
            Gdiplus::Pen marker(SmoothColor(accent),2.0f);
            glow.SetStartCap(Gdiplus::LineCapRound);glow.SetEndCap(Gdiplus::LineCapRound);
            marker.SetStartCap(Gdiplus::LineCapRound);marker.SetEndCap(Gdiplus::LineCapRound);
            graphics.DrawLine(&glow,static_cast<float>(row.left+10),static_cast<float>(markerY),
                              static_cast<float>(row.right-8),static_cast<float>(markerY));
            graphics.DrawLine(&marker,static_cast<float>(row.left+10),static_cast<float>(markerY),
                              static_cast<float>(row.right-8),static_cast<float>(markerY));
            Gdiplus::SolidBrush markerDot(SmoothColor(accent));
            graphics.FillEllipse(&markerDot,static_cast<float>(row.left+6),
                                  static_cast<float>(markerY-3),6.0f,6.0f);
        }
    }
}
void DrawProjectItem(const DRAWITEMSTRUCT* item){
    if(!item||!item->hDC||item->itemID==(UINT)-1)return;
    const RECT row=item->rcItem;
    const int width=std::max(1L,row.right-row.left);
    const int height=std::max(1L,row.bottom-row.top);
    HDC buffer=CreateCompatibleDC(item->hDC);
    HBITMAP bitmap=buffer?CreateCompatibleBitmap(item->hDC,width,height):nullptr;
    if(!buffer||!bitmap){
        if(bitmap)DeleteObject(bitmap);
        if(buffer)DeleteDC(buffer);
        DrawProjectItemContent(item);
        return;
    }
    const HGDIOBJ previous=SelectObject(buffer,bitmap);
    // Keep the original list coordinates so all card/icon geometry remains
    // identical, but render into an off-screen row-sized surface first.
    SetWindowOrgEx(buffer,row.left,row.top,nullptr);
    DRAWITEMSTRUCT buffered=*item;
    buffered.hDC=buffer;
    DrawProjectItemContent(&buffered);
    SetWindowOrgEx(buffer,0,0,nullptr);
    BitBlt(item->hDC,row.left,row.top,width,height,buffer,0,0,SRCCOPY);
    SelectObject(buffer,previous);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}
bool PickFolder(HWND owner,std::wstring& path){IFileDialog* dialog=nullptr;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))||!dialog)return false;DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);if(FAILED(dialog->Show(owner))){dialog->Release();return false;}IShellItem* item=nullptr;bool ok=SUCCEEDED(dialog->GetResult(&item));if(ok){PWSTR value=nullptr;ok=SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&value));if(ok){path=value;CoTaskMemFree(value);}item->Release();}dialog->Release();return ok;}
int SelectedProjectIndex(){
    const LRESULT row=SendMessageW(g_list,LB_GETCURSEL,0,0);
    if(row==LB_ERR||row<0||row>=static_cast<LRESULT>(g_visible_project_indices.size()))return -1;
    return g_visible_project_indices[static_cast<size_t>(row)];
}
std::string ReadGitLine(const std::filesystem::path& path){
    std::ifstream stream(path,std::ios::binary);std::string line;if(stream)std::getline(stream,line);if(!line.empty()&&line.back()=='\r')line.pop_back();return line;
}
std::wstring FormatGitTime(const std::filesystem::path& path){
    std::error_code error;
    if(!std::filesystem::is_regular_file(path,error))return{};
    const auto file_time=std::filesystem::last_write_time(path,error);
    if(error)return{};
    const auto system_time=std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        file_time-decltype(file_time)::clock::now()+std::chrono::system_clock::now());
    const std::time_t value=std::chrono::system_clock::to_time_t(system_time);
    tm local{};
    if(localtime_s(&local,&value)!=0)return{};
    wchar_t buffer[64]{};
    if(!std::wcsftime(buffer,std::size(buffer),L"%Y-%m-%d %H:%M",&local))return{};
    return buffer;
}
std::wstring LatestGitLogTime(const std::filesystem::path& directory){
    std::error_code error;
    if(!std::filesystem::is_directory(directory,error))return{};
    std::filesystem::file_time_type latest{};
    bool found=false;
    std::filesystem::recursive_directory_iterator iterator(directory,error),end;
    for(;!error&&iterator!=end;iterator.increment(error)){
        if(!iterator->is_regular_file(error))continue;
        const auto value=iterator->last_write_time(error);
        if(error)continue;
        if(!found||value>latest){latest=value;found=true;}
    }
    if(!found)return{};
    const auto system_time=std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        latest-decltype(latest)::clock::now()+std::chrono::system_clock::now());
    const std::time_t value=std::chrono::system_clock::to_time_t(system_time);
    tm local{};
    if(localtime_s(&local,&value)!=0)return{};
    wchar_t buffer[64]{};
    if(!std::wcsftime(buffer,std::size(buffer),L"%Y-%m-%d %H:%M",&local))return{};
    return buffer;
}
std::filesystem::path ResolveGitDirectory(const std::filesystem::path& project_folder){
    std::error_code error;auto current=std::filesystem::weakly_canonical(project_folder,error);if(error||!std::filesystem::is_directory(current,error))return{};
    for(;;){
        const auto marker=current/L".git";
        if(std::filesystem::is_directory(marker,error))return marker;
        error.clear();
        if(std::filesystem::is_regular_file(marker,error)){
            const auto line=ReadGitLine(marker);constexpr std::string_view prefix="gitdir:";
            if(line.rfind(prefix,0)==0){std::string value=line.substr(prefix.size());while(!value.empty()&&(value.front()==' '||value.front()=='\t'))value.erase(value.begin());std::filesystem::path gitdir(WideText(value));if(gitdir.is_relative())gitdir=current/gitdir;auto resolved=std::filesystem::weakly_canonical(gitdir,error);if(!error&&std::filesystem::is_directory(resolved,error))return resolved;}
        }
        const auto parent=current.parent_path();if(parent.empty()||parent==current)break;current=parent;error.clear();
    }
    return{};
}
std::filesystem::path ResolveGitCommonDirectory(const std::filesystem::path& git_directory){
    const auto value=ReadGitLine(git_directory/L"commondir");if(value.empty())return git_directory;std::error_code error;std::filesystem::path common(WideText(value));if(common.is_relative())common=git_directory/common;auto resolved=std::filesystem::weakly_canonical(common,error);return error?git_directory:resolved;
}
bool IsGitObjectId(std::string_view value){
    if(value.size()<7)return false;bool nonzero=false;for(char ch:value){const bool hex=(ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')||(ch>='A'&&ch<='F');if(!hex)return false;if(ch!='0')nonzero=true;}return nonzero;
}
bool GitHasCommit(const std::filesystem::path& git_directory){
    const auto head=ReadGitLine(git_directory/L"HEAD");if(head.empty())return false;constexpr std::string_view ref_prefix="ref: ";if(head.rfind(ref_prefix,0)!=0)return IsGitObjectId(head);
    const std::string reference=head.substr(ref_prefix.size());const auto common=ResolveGitCommonDirectory(git_directory);for(const auto& root:{git_directory,common}){const auto value=ReadGitLine(root/std::filesystem::path(WideText(reference)));if(IsGitObjectId(value))return true;}
    std::ifstream packed(common/L"packed-refs",std::ios::binary);std::string line;while(std::getline(packed,line)){if(line.empty()||line.front()=='#'||line.front()=='^')continue;if(!line.empty()&&line.back()=='\r')line.pop_back();const auto separator=line.find(' ');if(separator!=std::string::npos&&line.substr(separator+1)==reference&&IsGitObjectId(std::string_view(line).substr(0,separator)))return true;}return false;
}
std::wstring GitStatusLabel(){
    switch(g_git_repository_state){
    case GitRepositoryState::HasCommits:return L"GIT  ● 已提交";
    case GitRepositoryState::NoCommits:return L"GIT  ● 尚无提交";
    case GitRepositoryState::NotInitialized:return L"GIT  ○ 未初始化";
    case GitRepositoryState::Unavailable:break;
    }
    return L"GIT  ○ 未选择项目";
}
COLORREF GitStatusColor(){
    switch(g_git_repository_state){
    case GitRepositoryState::HasCommits:return RGB(63,194,130);
    case GitRepositoryState::NoCommits:return RGB(241,174,65);
    case GitRepositoryState::NotInitialized:return RGB(226,91,105);
    case GitRepositoryState::Unavailable:break;
    }
    return muted;
}
void UpdateGitTooltip(){
    if(!g_hwnd)return;
    const std::wstring root=g_git_repository_root.empty()?L"未选择有效项目目录":g_git_repository_root;
    const std::wstring push=g_git_last_push.empty()?L"未记录":g_git_last_push;
    const std::wstring pull=g_git_last_pull.empty()?L"未记录":g_git_last_pull;
    g_git_tooltip_text=L"GIT\n目录："+root+L"\n状态："+GitStatusLabel().substr(5)+
                       L"\n最后推送："+push+L"\n最后拉取："+pull;
    if(!g_git_tooltip){
        g_git_tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,
            WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,
            CW_USEDEFAULT,CW_USEDEFAULT,g_hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(g_git_tooltip){
            SendMessageW(g_git_tooltip,TTM_SETMAXTIPWIDTH,0,static_cast<LPARAM>(420));
            TOOLINFOW tool{sizeof(tool)};
            tool.uFlags=TTF_IDISHWND;
            tool.hwnd=g_hwnd;
            tool.uId=reinterpret_cast<UINT_PTR>(g_hwnd);
            tool.rect=g_git_footer_rect;
            tool.lpszText=const_cast<LPWSTR>(g_git_tooltip_text.c_str());
            SendMessageW(g_git_tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
        }
    }else{
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags=TTF_IDISHWND;
        tool.hwnd=g_hwnd;
        tool.uId=reinterpret_cast<UINT_PTR>(g_hwnd);
        tool.rect=g_git_footer_rect;
        tool.lpszText=const_cast<LPWSTR>(g_git_tooltip_text.c_str());
        SendMessageW(g_git_tooltip,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&tool));
        SendMessageW(g_git_tooltip,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&tool));
    }
}
void RefreshGitStatus(){
    GitRepositoryState next=GitRepositoryState::Unavailable;std::wstring root;std::wstring last_push;std::wstring last_pull;
    const int index=SelectedProjectIndex();
    if(index>=0&&index<(int)g_projects.size()&&!g_projects[index].folder.empty()){
        std::error_code error;const std::filesystem::path folder(g_projects[index].folder);
        if(std::filesystem::is_directory(folder,error)){
            const auto git=ResolveGitDirectory(folder);
            if(git.empty())next=GitRepositoryState::NotInitialized;
            else{
                next=GitHasCommit(git)?GitRepositoryState::HasCommits:GitRepositoryState::NoCommits;
                const auto common=ResolveGitCommonDirectory(git);
                root=(common.filename()==L".git"?common.parent_path():git.parent_path()).wstring();
                // FETCH_HEAD is the local, reliable marker for the last fetch/
                // pull. Git does not persist a push timestamp locally, so do
                // not invent one; a future in-app push can fill this field.
                last_pull=FormatGitTime(common/L"FETCH_HEAD");
                if(last_pull.empty())last_pull=LatestGitLogTime(common/L"logs"/L"refs"/L"remotes");
                last_push=FormatGitTime(common/L"aicli-last-push");
            }
        }
    }
    const bool changed=next!=g_git_repository_state||root!=g_git_repository_root||
                       last_push!=g_git_last_push||last_pull!=g_git_last_pull;
    g_git_repository_state=next;g_git_repository_root=std::move(root);
    g_git_last_push=std::move(last_push);g_git_last_pull=std::move(last_pull);
    if(changed&&g_hwnd){
        RECT client{};GetClientRect(g_hwnd,&client);
        const auto metrics=aicli::ui::LayoutMetrics::ForWindow(g_hwnd);
        RECT footer{0,std::max(0,static_cast<int>(client.bottom)-metrics.footer_height),client.right,client.bottom};
        InvalidateRect(g_hwnd,&footer,FALSE);
        UpdateGitTooltip();
    }
}
void RefreshProjectDetails(){
    const int index=SelectedProjectIndex();
    if(index<0 || index>=(int)g_projects.size()){
        SetText(g_detail_title,L"未选择项目");
        SetText(g_detail_folder,L"文件夹: 未设置");
        SetText(g_detail_command,L"启动命令: 未设置");
        SetText(g_detail_state,L"状态: 未运行");
        RefreshGitStatus();
        return;
    }
    const auto& project=g_projects[index];
    g_last_project_id=project.id;
    g_store.SetSetting(L"last_project_id",project.id);
    SetText(g_detail_title,project.name+L"   /   "+project.type);
    SetText(g_detail_folder,L"文件夹: "+(project.folder.empty()?L"未设置":project.folder));
    SetText(g_detail_command,L"启动命令: "+(project.command.empty()?L"未设置":project.command));
    const wchar_t* state=g_island.HasSession(project.id)?L"运行中":(project.status==L"paused"?L"已暂停":L"未运行");
    SetText(g_detail_state,std::wstring(L"状态: ")+state);
    RefreshGitStatus();
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
    if(g_store.GetSetting(L"seeded_projects",L"0")!=L"1"&&g_projects.empty()){
        const auto cwd=std::filesystem::current_path().wstring();
        Project codex{};codex.name=L"默认 Codex 会话";codex.type=L"Codex";codex.group=L"默认分组";codex.folder=cwd;codex.command=DefaultProjectCommand(codex.type);g_store.SaveProject(codex);
        Project claude{};claude.name=L"Claude 工作区";claude.type=L"Claude";claude.group=L"默认分组";claude.folder=cwd;claude.command=DefaultProjectCommand(claude.type);g_store.SaveProject(claude);
        g_store.SetSetting(L"seeded_projects",L"1");g_projects=g_store.LoadProjects();
    }
    g_group_name=g_store.GetSetting(L"last_group",L"默认分组");
    if(g_group_name!=kAllGroupsName&&std::none_of(g_groups.begin(),g_groups.end(),[&](const ProjectGroup& group){return group.name==g_group_name;}))g_group_name=L"默认分组";
    g_last_project_id=g_store.GetSetting(L"last_project_id");
    g_startup_project_id=g_last_project_id;
    ApplyStoredTheme();
    return true;
}
void UpdateGroupButton(){
    if(!g_group)return;
    const auto count=g_group_name==kAllGroupsName?g_projects.size():std::count_if(g_projects.begin(),g_projects.end(),[](const Project& project){const std::wstring group=project.group.empty()?L"默认分组":project.group;return group==g_group_name;});
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
        if(g_group_name!=kAllGroupsName&&group!=g_group_name)continue;
        if(!query.empty()&&project.name.find(query)==std::wstring::npos&&project.type.find(query)==std::wstring::npos)continue;
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
}
void EndProjectDragPreview(){
    if(g_project_drag_preview)DestroyWindow(g_project_drag_preview);
    g_project_drag_preview=nullptr;
    g_project_drag_hotspot={};
}
bool BeginProjectDragPreview(HWND list,int row,POINT cursor_screen){
    RECT item{};
    if(SendMessageW(list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))==LB_ERR)return false;
    const int project_index=static_cast<int>(SendMessageW(list,LB_GETITEMDATA,row,0));
    if(project_index<0||project_index>=static_cast<int>(g_projects.size()))return false;

    constexpr int shadow_margin=7;
    const int card_width=std::max(1,static_cast<int>(item.right-item.left)-7);
    const int card_height=std::max(1,static_cast<int>(item.bottom-item.top)-6);
    const int width=card_width+shadow_margin*2;
    const int height=card_height+shadow_margin*2+3;

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth=width;
    bitmap_info.bmiHeader.biHeight=-height;
    bitmap_info.bmiHeader.biPlanes=1;
    bitmap_info.bmiHeader.biBitCount=32;
    bitmap_info.bmiHeader.biCompression=BI_RGB;
    void* pixels{};
    HDC screen=GetDC(nullptr);
    HDC memory=screen?CreateCompatibleDC(screen):nullptr;
    HBITMAP bitmap=memory?CreateDIBSection(memory,&bitmap_info,DIB_RGB_COLORS,&pixels,nullptr,0):nullptr;
    if(!screen||!memory||!bitmap||!pixels){
        if(bitmap)DeleteObject(bitmap);
        if(memory)DeleteDC(memory);
        if(screen)ReleaseDC(nullptr,screen);
        return false;
    }
    std::memset(pixels,0,static_cast<size_t>(width)*height*4);
    auto old_bitmap=SelectObject(memory,bitmap);

    {
        Gdiplus::Bitmap surface(width,height,width*4,PixelFormat32bppPARGB,
                                static_cast<BYTE*>(pixels));
        Gdiplus::Graphics graphics(&surface);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

        Gdiplus::RectF shadow_bounds{
            static_cast<float>(shadow_margin),static_cast<float>(shadow_margin+4),
            static_cast<float>(card_width),static_cast<float>(card_height)};
        Gdiplus::GraphicsPath shadow_path;
        AddRoundedPath(shadow_path,shadow_bounds,10.0f);
        Gdiplus::SolidBrush shadow_brush(Gdiplus::Color(72,0,0,0));
        graphics.FillPath(&shadow_brush,&shadow_path);

        Gdiplus::RectF card_bounds{
            static_cast<float>(shadow_margin)+0.5f,static_cast<float>(shadow_margin)+0.5f,
            static_cast<float>(card_width)-1.0f,static_cast<float>(card_height)-1.0f};
        Gdiplus::GraphicsPath card_path;
        AddRoundedPath(card_path,card_bounds,9.5f);
        const COLORREF card_fill=g_light_theme?RGB(246,249,253):RGB(28,34,43);
        const COLORREF card_stroke=g_light_theme?RGB(124,166,220):RGB(72,116,177);
        Gdiplus::SolidBrush card_brush(SmoothColor(card_fill,244));
        Gdiplus::Pen card_pen(SmoothColor(card_stroke,245),1.25f);
        graphics.FillPath(&card_brush,&card_path);
        graphics.DrawPath(&card_pen,&card_path);

        Gdiplus::Pen grip(SmoothColor(g_light_theme?RGB(107,123,145):RGB(151,164,184),230),1.4f);
        grip.SetStartCap(Gdiplus::LineCapRound);
        grip.SetEndCap(Gdiplus::LineCapRound);
        const float grip_center=shadow_margin+card_height/2.0f;
        for(float offset=-4.0f;offset<=4.0f;offset+=4.0f)
            graphics.DrawLine(&grip,static_cast<float>(shadow_margin+11),grip_center+offset,
                              static_cast<float>(shadow_margin+20),grip_center+offset);

        const auto& project=g_projects[project_index];
        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font preview_font(&family,15.0f,Gdiplus::FontStyleRegular,Gdiplus::UnitPixel);
        Gdiplus::SolidBrush text_brush(SmoothColor(fg,245));
        Gdiplus::StringFormat text_format;
        text_format.SetAlignment(Gdiplus::StringAlignmentNear);
        text_format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        text_format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        text_format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::RectF text_bounds{
            static_cast<float>(shadow_margin+29),static_cast<float>(shadow_margin),
            static_cast<float>(std::max(1,card_width-69)),static_cast<float>(card_height)};
        graphics.DrawString(project.name.c_str(),static_cast<INT>(project.name.size()),
                            &preview_font,text_bounds,&text_format,&text_brush);

        COLORREF state_color=g_light_theme?RGB(100,113,131):RGB(139,157,183);
        enum class PreviewState{Idle,Running,Paused,Warning};
        PreviewState state=PreviewState::Idle;
        if(g_island.HasSession(project.id)){
            state=PreviewState::Running;
            state_color=g_light_theme?RGB(28,139,91):RGB(73,203,145);
        }else if(project.status==L"paused"){
            state=PreviewState::Paused;
            state_color=g_light_theme?RGB(177,113,16):RGB(245,178,72);
        }else if(project.status==L"cancelled"||project.status==L"error"){
            state=PreviewState::Warning;
            state_color=g_light_theme?RGB(190,132,26):RGB(244,187,72);
        }
        const float icon_size=20.0f;
        const float icon_x=shadow_margin+card_width-icon_size-11.0f;
        const float icon_y=shadow_margin+(card_height-icon_size)/2.0f;
        const float cx=icon_x+icon_size/2.0f,cy=icon_y+icon_size/2.0f;
        Gdiplus::SolidBrush halo(SmoothColor(state_color,34));
        Gdiplus::SolidBrush icon_brush(SmoothColor(state_color,245));
        Gdiplus::Pen icon_pen(SmoothColor(state_color,245),1.65f);
        icon_pen.SetStartCap(Gdiplus::LineCapRound);
        icon_pen.SetEndCap(Gdiplus::LineCapRound);
        graphics.FillEllipse(&halo,icon_x,icon_y,icon_size,icon_size);
        if(state==PreviewState::Running){
            graphics.DrawEllipse(&icon_pen,icon_x+2.0f,icon_y+2.0f,icon_size-4.0f,icon_size-4.0f);
            graphics.FillEllipse(&icon_brush,cx-3.0f,cy-3.0f,6.0f,6.0f);
        }else if(state==PreviewState::Paused){
            graphics.DrawLine(&icon_pen,cx-2.5f,cy-4.0f,cx-2.5f,cy+4.0f);
            graphics.DrawLine(&icon_pen,cx+2.5f,cy-4.0f,cx+2.5f,cy+4.0f);
        }else if(state==PreviewState::Warning){
            Gdiplus::PointF triangle[3]{{cx,icon_y+3.0f},
                {icon_x+3.0f,icon_y+icon_size-3.0f},
                {icon_x+icon_size-3.0f,icon_y+icon_size-3.0f}};
            graphics.DrawPolygon(&icon_pen,triangle,3);
            graphics.DrawLine(&icon_pen,cx,cy-2.5f,cx,cy+1.5f);
            graphics.FillEllipse(&icon_brush,cx-0.9f,cy+3.0f,1.8f,1.8f);
        }else graphics.DrawEllipse(&icon_pen,cx-4.0f,cy-4.0f,8.0f,8.0f);
    }

    POINT card_origin{item.left+3,item.top+3};
    ClientToScreen(list,&card_origin);
    g_project_drag_hotspot={
        std::clamp<int>(static_cast<int>(cursor_screen.x-card_origin.x)+shadow_margin,0,width-1),
        std::clamp<int>(static_cast<int>(cursor_screen.y-card_origin.y)+shadow_margin,0,height-1)};
    POINT destination{cursor_screen.x-g_project_drag_hotspot.x,
                      cursor_screen.y-g_project_drag_hotspot.y};
    SIZE size{width,height};
    POINT source_origin{};
    BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};

    EndProjectDragPreview();
    g_project_drag_preview=CreateWindowExW(
        WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,
        L"STATIC",L"",WS_POPUP,destination.x,destination.y,width,height,
        g_hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
    bool updated=g_project_drag_preview&&UpdateLayeredWindow(
        g_project_drag_preview,screen,&destination,&size,memory,&source_origin,0,&blend,ULW_ALPHA);
    if(updated)ShowWindow(g_project_drag_preview,SW_SHOWNOACTIVATE);

    SelectObject(memory,old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr,screen);
    if(!updated){EndProjectDragPreview();return false;}
    return true;
}
void MoveProjectDragPreview(POINT cursor_screen){
    if(!g_project_drag_preview)return;
    SetWindowPos(g_project_drag_preview,HWND_TOP,
                 cursor_screen.x-g_project_drag_hotspot.x,
                 cursor_screen.y-g_project_drag_hotspot.y,
                 0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
void RepaintProjectDragList(HWND list){
    if(!list)return;
    InvalidateRect(list,nullptr,FALSE);
    UpdateWindow(list);
}
void ResetProjectDrag(HWND list){
    const bool repaint=g_project_dragging||g_project_drag_insert>=0;
    EndProjectDragPreview();
    g_project_drag_source=-1;g_project_drag_insert=-1;g_project_dragging=false;
    if(repaint&&list)InvalidateRect(list,nullptr,FALSE);
}
int ProjectDropSlot(HWND list,POINT point){
    const int count=static_cast<int>(SendMessageW(list,LB_GETCOUNT,0,0));
    if(count<=0)return 0;
    RECT client{};GetClientRect(list,&client);
    if(point.y<=client.top)return 0;
    if(point.y>=client.bottom)return count;
    const LRESULT hit=SendMessageW(list,LB_ITEMFROMPOINT,0,MAKELPARAM(point.x,point.y));
    int row=static_cast<int>(LOWORD(hit));
    if(row<0)return 0;if(row>=count)return count;
    RECT item{};if(SendMessageW(list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))==LB_ERR)return row;
    return point.y<(item.top+item.bottom)/2?row:row+1;
}
void ReorderVisibleProjects(int source_row,int insert_slot){
    if(g_group_name==kAllGroupsName)return;
    const int count=static_cast<int>(g_visible_project_indices.size());
    if(source_row<0||source_row>=count||insert_slot<0||insert_slot>count)return;
    int destination=insert_slot;if(destination>source_row)--destination;
    if(destination==source_row)return;

    std::vector<std::wstring> ordered_ids;ordered_ids.reserve(g_visible_project_indices.size());
    for(const int index:g_visible_project_indices){if(index>=0&&index<(int)g_projects.size())ordered_ids.push_back(g_projects[index].id);}
    if(source_row>=static_cast<int>(ordered_ids.size()))return;
    const std::wstring moving_id=ordered_ids[source_row];
    ordered_ids.erase(ordered_ids.begin()+source_row);
    destination=std::clamp(destination,0,static_cast<int>(ordered_ids.size()));
    ordered_ids.insert(ordered_ids.begin()+destination,moving_id);

    std::wstring error;
    if(!g_store.ReorderProjects(g_group_name,ordered_ids,&error)){
        MessageBoxW(g_hwnd,(L"无法保存项目顺序："+error).c_str(),L"项目排序",MB_OK|MB_ICONWARNING);
        return;
    }
    g_projects=g_store.LoadProjects();
    g_last_project_id=moving_id;SendMessageW(g_list,LB_SETCURSEL,static_cast<WPARAM>(LB_ERR),0);
    RefreshProjectList();RefreshProjectDetails();Layout();SetText(g_status,L"项目顺序已保存");
}
LRESULT HandleProjectDragListMessage(HWND,LPARAM lparam){
    auto* info=reinterpret_cast<DRAGLISTINFO*>(lparam);
    if(!info||info->hWnd!=g_list)return 0;
    switch(info->uNotification){
    case DL_BEGINDRAG:{
        ResetProjectDrag(g_list);
        if(g_group_name==kAllGroupsName)return FALSE;
        if(!aicli::ui::GetStyledInputText(g_search).empty())return FALSE;
        const int row=LBItemFromPt(g_list,info->ptCursor,FALSE);
        const int count=static_cast<int>(SendMessageW(g_list,LB_GETCOUNT,0,0));
        if(row<0||row>=count)return FALSE;
        POINT client_point=info->ptCursor;
        ScreenToClient(g_list,&client_point);
        RECT item{};
        if(SendMessageW(g_list,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))==LB_ERR)
            return FALSE;
        const RECT drag_handle{item.left+3,item.top+3,item.left+31,item.bottom-3};
        if(!PtInRect(&drag_handle,client_point))return FALSE;
        BeginProjectDragPreview(g_list,row,info->ptCursor);
        g_project_dragging=true;g_project_drag_source=row;g_project_drag_insert=row;
        RepaintProjectDragList(g_list);return TRUE;
    }
    case DL_DRAGGING:{
        if(!g_project_dragging)return DL_STOPCURSOR;
        LBItemFromPt(g_list,info->ptCursor,TRUE);
        MoveProjectDragPreview(info->ptCursor);
        POINT client_point=info->ptCursor;ScreenToClient(g_list,&client_point);
        const int slot=ProjectDropSlot(g_list,client_point);
        if(slot!=g_project_drag_insert){g_project_drag_insert=slot;RepaintProjectDragList(g_list);}
        return DL_MOVECURSOR;
    }
    case DL_DROPPED:{
        if(!g_project_dragging)return 0;
        POINT client_point=info->ptCursor;ScreenToClient(g_list,&client_point);
        const int source=g_project_drag_source,slot=ProjectDropSlot(g_list,client_point);
        ResetProjectDrag(g_list);ReorderVisibleProjects(source,slot);return 0;
    }
    case DL_CANCELDRAG:
        ResetProjectDrag(g_list);return 0;
    }
    return 0;
}
void Layout(){
    if(!g_hwnd || IsIconic(g_hwnd))return;
    g_layout_metrics=aicli::ui::LayoutMetrics::ForWindow(g_hwnd);
    RECT r{}; if(!GetClientRect(g_hwnd,&r))return;
    const int top=g_layout_metrics.title_bar_height;
    const int left=g_layout_metrics.sidebar_width;
    const int bottom=g_layout_metrics.footer_height;
    if(r.right<=left+80 || r.bottom<=top+bottom+120)return;
    MoveMainChild(g_group,168,10,154,32);
    MoveMainChild(g_group_add,326,10,34,32);
    const int captionReserve=MulDiv(144,(int)GetDpiForWindow(g_hwnd),96);const int settingsX=std::max(860,(int)r.right-captionReserve-82);
    MoveMainChild(g_bind,std::max(700,settingsX-220),10,96,32);
    MoveMainChild(g_devices,std::max(700,settingsX-116),10,106,32);
    MoveMainChild(g_settings,settingsX,10,72,32);

    RECT search_rect{14,top+16,left-14,top+54};aicli::ui::LayoutStyledInput(g_search,search_rect,2,9);
    MoveMainChild(g_sidebar_title,18,top+70,200,24);
    MoveMainChild(g_new_project,left-50,top+64,36,34);
    SetWindowPos(g_new_project,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    const int footer_top=r.bottom-bottom;
    const int telemetry_left=std::max(14,static_cast<int>(r.right)-kFooterTelemetryWidth+8);
    const int git_right=std::max(14,telemetry_left-8);
    const int git_left=std::max(14,git_right-214);
    g_git_footer_rect={git_left,footer_top,git_right,r.bottom};
    MoveMainChild(g_list,12,top+104,left-24,std::max(80,footer_top-(top+104)-8));

    MoveMainChild(g_cli_tab,left+16,top+12,96,34);
    MoveMainChild(g_terminal_title,left+132,top+19,r.right-left-150,22);
    RECT terminal{left+16,top+58,r.right-14,r.bottom-bottom-10};
    const int selectedIndex=SelectedProjectIndex();
    const bool selectedRunning=selectedIndex>=0&&selectedIndex<(int)g_projects.size()&&
        g_island.HasSession(g_projects[selectedIndex].id);
    if(selectedRunning){
        const auto& selectedProject=g_projects[selectedIndex];
        // Layout is called frequently for text/status updates.  Do not steal
        // focus from the native terminal or trigger a TUI repaint when the
        // selected session has not actually changed.
        g_island.Activate(selectedProject.id,false);
        SetText(g_terminal_title,selectedProject.name+L"   /   "+selectedProject.type);
        SetText(g_status,L"原生终端已连接    |    "+selectedProject.type);
    }
    const bool showingTerminal=selectedRunning;
    const bool showingNative=showingTerminal&&g_island.Active();
    SetMainChildVisible(g_detail_title,!showingTerminal);
    SetMainChildVisible(g_detail_folder,!showingTerminal);
    SetMainChildVisible(g_detail_command,!showingTerminal);
    SetMainChildVisible(g_detail_state,!showingTerminal);
    SetMainChildVisible(g_start_project,!showingTerminal);
    SetMainChildVisible(g_edit_project,!showingTerminal);
    MoveMainChild(g_detail_title,terminal.left+28,terminal.top+28,terminal.right-terminal.left-56,32);
    MoveMainChild(g_detail_folder,terminal.left+28,terminal.top+100,terminal.right-terminal.left-56,30);
    MoveMainChild(g_detail_command,terminal.left+28,terminal.top+172,terminal.right-terminal.left-56,30);
    MoveMainChild(g_detail_state,terminal.left+28,terminal.top+244,terminal.right-terminal.left-56,30);
    MoveMainChild(g_start_project,terminal.left+28,terminal.bottom-66,112,36);
    MoveMainChild(g_edit_project,terminal.left+154,terminal.bottom-66,112,36);
    // The central surface is always the real Windows Terminal control.  Do
    // not create a second EDIT-based command line or a text-output fallback:
    // keyboard focus, shortcuts, selection and scrolling must stay native.
    MoveMainChild(g_status,14,r.bottom-bottom+6,
                  std::max(160,git_left-22),20);
    UpdateGitTooltip();
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
        g_island.Stop(project.id);
    }
    g_store.DeleteProject(project.id);
    g_projects.erase(g_projects.begin()+index);
    RefreshProjectList();
    RefreshProjectDetails();
    Layout();
    SetText(g_status,L"项目“"+project.name+L"”已删除，项目文件夹未受影响");
}
struct ContextMenuEntry{const wchar_t* icon;const wchar_t* label;bool submenu;};
void DrawContextMenuItem(const DRAWITEMSTRUCT* item){
    const auto* entry=reinterpret_cast<const ContextMenuEntry*>(item->itemData);
    if(!entry)return;
    RECT r=item->rcItem;
    if(!entry->label[0]){HBRUSH background=CreateSolidBrush(g_light_theme?RGB(250,251,253):RGB(34,36,40));FillRect(item->hDC,&r,background);DeleteObject(background);HPEN line=CreatePen(PS_SOLID,1,g_light_theme?RGB(218,223,231):RGB(57,60,67));auto old=(HPEN)SelectObject(item->hDC,line);const int y=(r.top+r.bottom)/2;MoveToEx(item->hDC,r.left+8,y,nullptr);LineTo(item->hDC,r.right-8,y);SelectObject(item->hDC,old);DeleteObject(line);return;}
    const bool selected=(item->itemState&ODS_SELECTED)!=0;
    const bool disabled=(item->itemState&ODS_DISABLED)!=0;
    HBRUSH fill=CreateSolidBrush(selected?(g_light_theme?RGB(226,237,251):RGB(55,62,73)):(g_light_theme?RGB(250,251,253):RGB(34,36,40)));FillRect(item->hDC,&r,fill);DeleteObject(fill);
    SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,disabled?(g_light_theme?RGB(153,160,170):RGB(111,116,126)):(g_light_theme?RGB(38,44,54):RGB(222,226,233)));
    SelectObject(item->hDC,g_icon_font?g_icon_font:g_font);RECT icon{r.left+13,r.top+1,r.left+39,r.bottom-1};DrawTextW(item->hDC,entry->icon,-1,&icon,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(item->hDC,g_font);RECT text{r.left+49,r.top+1,r.right-34,r.bottom-1};DrawTextW(item->hDC,entry->label,-1,&text,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    if(entry->submenu){SelectObject(item->hDC,g_font_small);RECT arrow{r.right-28,r.top+1,r.right-10,r.bottom-1};DrawTextW(item->hDC,L">",-1,&arrow,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
}
void ConfigureContextMenu(HMENU menu,HBRUSH background){MENUINFO info{sizeof(info)};info.fMask=MIM_BACKGROUND;info.hbrBack=background;SetMenuInfo(menu,&info);}
void AppendContextEntry(HMENU menu,UINT flags,UINT_PTR id,const ContextMenuEntry& entry){AppendMenuW(menu,flags|MF_OWNERDRAW,id,reinterpret_cast<LPCWSTR>(&entry));}
void OpenProjectFolder(){const int index=SelectedProjectIndex();if(index<0||index>=(int)g_projects.size())return;const auto& folder=g_projects[index].folder;if(folder.empty()){MessageBoxW(g_hwnd,L"当前项目还没有设置项目文件夹。",L"打开项目文件夹",MB_OK|MB_ICONINFORMATION);return;}if((INT_PTR)ShellExecuteW(g_hwnd,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL)<=32)MessageBoxW(g_hwnd,L"无法打开项目文件夹。",L"打开项目文件夹",MB_OK|MB_ICONWARNING);}
void MoveSelectedProjectToGroup(const std::wstring& group){const int index=SelectedProjectIndex();if(index<0||index>=(int)g_projects.size())return;g_projects[index].group=group;g_store.SaveProject(g_projects[index]);g_group_name=group;g_store.SetSetting(L"last_group",group);RefreshProjectList();SetText(g_status,L"项目已移动到 "+group);Layout();}
void MoveSelectedProjectToDefaultGroup(){MoveSelectedProjectToGroup(L"默认分组");}
void ShowGroupDropdown(){
    g_groups=g_store.LoadGroups();g_projects=g_store.LoadProjects();if(g_groups.empty())return;
    HMENU menu=CreatePopupMenu();if(!menu)return;HBRUSH brush=CreateSolidBrush(g_light_theme?RGB(250,251,253):RGB(34,36,40));ConfigureContextMenu(menu,brush);
    std::vector<std::wstring> labels;labels.reserve(g_groups.size()+1);
    labels.push_back(std::wstring(kAllGroupsName)+L" ("+std::to_wstring(g_projects.size())+L")");
    for(const auto& group:g_groups){const auto count=std::count_if(g_projects.begin(),g_projects.end(),[&](const Project& project){const std::wstring value=project.group.empty()?L"默认分组":project.group;return value==group.name;});labels.push_back(group.name+L" ("+std::to_wstring(count)+L")");}
    std::vector<ContextMenuEntry> entries;entries.reserve(labels.size());
    entries.push_back({g_group_name==kAllGroupsName?L"\xE73E":L"\xE8B7",labels[0].c_str(),false});
    AppendContextEntry(menu,MF_ENABLED,ID_GROUP_SELECT_BASE,entries.back());
    for(size_t i=0;i<g_groups.size();++i){entries.push_back({g_groups[i].name==g_group_name?L"\xE73E":L"\xE8B7",labels[i+1].c_str(),false});AppendContextEntry(menu,MF_ENABLED,ID_GROUP_SELECT_BASE+1+static_cast<UINT>(i),entries.back());}
    RECT button{};GetWindowRect(g_group,&button);const UINT command=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_LEFTALIGN|TPM_TOPALIGN|TPM_NONOTIFY,button.left,button.bottom,g_hwnd,nullptr);DestroyMenu(menu);DeleteObject(brush);
    if(command==ID_GROUP_SELECT_BASE){g_group_name=kAllGroupsName;g_store.SetSetting(L"last_group",g_group_name);RefreshProjectList();RefreshProjectDetails();Layout();}
    else if(command>ID_GROUP_SELECT_BASE&&command<=ID_GROUP_SELECT_BASE+g_groups.size()){g_group_name=g_groups[command-ID_GROUP_SELECT_BASE-1].name;g_store.SetSetting(L"last_group",g_group_name);RefreshProjectList();RefreshProjectDetails();Layout();}
}
void CreateGroupFromHeader(){
    std::wstring name;if(!aicli::ui::ShowGroupNameDialog(g_hwnd,L"添加分组",L"创建后会立即切换到这个分组。",L"",g_light_theme,name))return;std::wstring error;if(!g_store.AddGroup(name,&error)){MessageBoxW(g_hwnd,error.c_str(),L"添加分组",MB_OK|MB_ICONWARNING);return;}g_groups=g_store.LoadGroups();g_group_name=name;g_store.SetSetting(L"last_group",name);RefreshProjectList();RefreshProjectDetails();Layout();SetText(g_status,L"已创建分组 "+name);
}
void ShowProjectContextMenu(HWND owner,LPARAM position){
    int index=SelectedProjectIndex();
    POINT screen{};
    if(position==static_cast<LPARAM>(-1)){
        if(index<0||index>=(int)g_projects.size())return;
        RECT item{};
        if(SendMessageW(g_list,LB_GETITEMRECT,index,(LPARAM)&item)==LB_ERR)return;
        screen={item.left+28,item.bottom};
        ClientToScreen(g_list,&screen);
    }else{
        screen={GET_X_LPARAM(position),GET_Y_LPARAM(position)};
        POINT client=screen;
        ScreenToClient(g_list,&client);
        const LRESULT hit=SendMessageW(g_list,LB_ITEMFROMPOINT,0,MAKELPARAM(client.x,client.y));
        if(HIWORD(static_cast<DWORD_PTR>(hit))!=0)return;
        index=LOWORD(static_cast<DWORD_PTR>(hit));
        if(index<0||index>=(int)g_visible_project_indices.size())return;
        SendMessageW(g_list,LB_SETCURSEL,index,0);
        index=SelectedProjectIndex();
        RefreshProjectDetails();
        Layout();
    }
    const bool running=g_island.HasSession(g_projects[index].id);
    const bool paused=g_projects[index].status==L"paused";
    HMENU menu=CreatePopupMenu(),groups=CreatePopupMenu();
    if(!menu||!groups){if(menu)DestroyMenu(menu);if(groups)DestroyMenu(groups);return;}
    HBRUSH menuBrush=CreateSolidBrush(g_light_theme?RGB(250,251,253):RGB(34,36,40));ConfigureContextMenu(menu,menuBrush);ConfigureContextMenu(groups,menuBrush);
    ContextMenuEntry open{L"\xE8B7",L"打开项目文件夹",false};
    ContextMenuEntry details{L"\xE946",L"项目详情",false};
    ContextMenuEntry move{L"\xE8AB",L"移动到分组",true};
    ContextMenuEntry restart{L"\xE72C",L"重新启动项目",false};
    ContextMenuEntry session{L"\xE769",paused?L"继续会话":L"暂停会话",false};
    ContextMenuEntry cancel{L"\xE711",L"取消会话",false};
    ContextMenuEntry close{L"\xE74D",L"关闭项目",false};
    ContextMenuEntry separator{L"",L"",false};
    AppendContextEntry(menu,MF_ENABLED,ID_CTX_OPEN_FOLDER,open);
    AppendContextEntry(menu,MF_ENABLED,ID_CTX_EDIT,details);
    std::vector<ContextMenuEntry> group_entries;group_entries.reserve(g_groups.size());
    for(size_t group_index=0;group_index<g_groups.size();++group_index){
        group_entries.push_back({L"\xE8AB",g_groups[group_index].name.c_str(),false});
        AppendMenuW(groups,MF_OWNERDRAW|MF_ENABLED,ID_CTX_MOVE_GROUP_BASE+static_cast<UINT>(group_index),reinterpret_cast<LPCWSTR>(&group_entries.back()));
    }
    AppendMenuW(menu,MF_OWNERDRAW|MF_POPUP,(UINT_PTR)groups,reinterpret_cast<LPCWSTR>(&move));
    AppendContextEntry(menu,MF_ENABLED,ID_CTX_RESTART,restart);
    AppendContextEntry(menu,(running||paused)?MF_ENABLED:MF_GRAYED,paused?ID_CTX_CONTINUE:ID_CTX_PAUSE,session);
    AppendContextEntry(menu,(running||paused)?MF_ENABLED:MF_GRAYED,ID_CTX_CANCEL,cancel);
    AppendContextEntry(menu,MF_DISABLED,0,separator);
    AppendContextEntry(menu,MF_ENABLED,ID_CTX_DELETE,close);
    SetForegroundWindow(owner);
    TrackPopupMenuEx(menu,TPM_RIGHTBUTTON|TPM_LEFTALIGN,screen.x,screen.y,owner,nullptr);
    DestroyMenu(menu);
    DeleteObject(menuBrush);
    PostMessageW(owner,WM_NULL,0,0);
}
bool StartCommandSession(const std::wstring& cmd,const std::wstring& cwd,const std::wstring& title,const std::wstring& sessionAgent,const std::wstring& project_id){
    SetText(g_terminal_title,title+L"   /   "+sessionAgent);
    if(g_island.Start(project_id,cmd,cwd,title,g_hwnd,g_status,
                      [project_id](std::wstring_view){QueueTerminalSnapshot(project_id);})){
        SetText(g_status,L"原生终端已连接    |    "+sessionAgent);
        Layout();
        return true;
    }
    const std::wstring error=g_island.LastError().empty()?L"无法创建原生终端控件":g_island.LastError();
    SetText(g_detail_state,L"状态: 启动失败");
    SetText(g_status,L"原生终端启动失败: "+error);
    Layout(); return false;
}
void StartSession(bool restart=false){
    const int i=SelectedProjectIndex();if(i<0||i>=(int)g_projects.size())return;
    auto& project=g_projects[i];
    if(g_island.HasSession(project.id)){
        if(!restart){g_island.Activate(project.id);SetText(g_terminal_title,project.name+L"   /   "+project.type);Layout();return;}
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
bool SelectProjectById(const std::wstring& project_id){const int index=ProjectIndexById(project_id);if(index<0)return false;if(g_group_name!=kAllGroupsName){g_group_name=g_projects[index].group.empty()?L"默认分组":g_projects[index].group;SetText(g_group,g_group_name);g_store.SetSetting(L"last_group",g_group_name);}RefreshProjectList();for(size_t row=0;row<g_visible_project_indices.size();++row)if(g_visible_project_indices[row]==index){SendMessageW(g_list,LB_SETCURSEL,row,0);RefreshProjectDetails();Layout();return true;}return false;}
void RefreshConversationsCache(){nlohmann::json items=nlohmann::json::array();for(const auto& project:g_projects){std::string provider=Utf8Text(project.type);std::transform(provider.begin(),provider.end(),provider.begin(),[](unsigned char ch){return static_cast<char>(std::tolower(ch));});items.push_back({{"id",Utf8Text(project.id)},{"name",Utf8Text(project.name)},{"provider",provider},{"type",Utf8Text(project.type)},{"group",Utf8Text(project.group)},{"folder",Utf8Text(project.folder)},{"command",Utf8Text(project.command)},{"status",Utf8Text(project.status)}});}std::scoped_lock lock(g_conversations_mutex);g_conversations_cache=nlohmann::json{{"items",items}}.dump();}
std::string ConversationsJson(){std::scoped_lock lock(g_conversations_mutex);return g_conversations_cache.empty()?"{\"items\":[]}":g_conversations_cache;}
std::string EventsJson(const std::wstring& project_id,std::int64_t after){nlohmann::json items=nlohmann::json::array();const auto snapshot=g_store.LoadTerminalSnapshot(project_id);if(snapshot){items.push_back({{"conversationId",Utf8Text(project_id)},{"event_type","terminal.snapshot"},{"type","terminal.snapshot"},{"payload",{{"text",Utf8Text(*snapshot)},{"replace",true}}}});}for(const auto& event:g_store.LoadEvents(project_id,after,1000,!snapshot.has_value())){nlohmann::json payload;try{payload=nlohmann::json::parse(event.payload_json);}catch(...){payload={{"text",event.payload_json}};}items.push_back({{"id",event.id},{"eventId",event.id},{"conversationId",Utf8Text(event.project_id)},{"event_type",Utf8Text(event.event_type)},{"type",Utf8Text(event.event_type)},{"payload",payload},{"createdAt",event.created_at}});}return nlohmann::json{{"items",items}}.dump();}
void UpdateDeviceHeader(){if(!g_devices)return;const auto count=g_store.LoadDevices().size();SetText(g_devices,L"已绑定 "+std::to_wstring(count)+L" 台");InvalidateRect(g_hwnd,nullptr,FALSE);}
void UpdateHeaderTelemetry(){
    if(!g_hwnd)return;
    RECT client{};
    GetClientRect(g_hwnd,&client);
    const auto metrics=aicli::ui::LayoutMetrics::ForWindow(g_hwnd);
    RECT footer{0,std::max(0,static_cast<int>(client.bottom)-metrics.footer_height),client.right,client.bottom};
    InvalidateRect(g_hwnd,&footer,FALSE);
}
void ConfigureRemoteAgent(){
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
    g_remote_agent.SetControlCallback([](const std::wstring& project,const std::wstring& phone,const std::wstring& action,std::wstring* error){auto* message=new(std::nothrow) RemoteControlMessage{project,phone,action};if(!message||!g_hwnd||!PostMessageW(g_hwnd,WM_REMOTE_CONTROL,0,reinterpret_cast<LPARAM>(message))){delete message;if(error)*error=L"桌面窗口未就绪";return false;}return true;});
    g_remote_agent.SetPairingCallback([](const aicli::agent::PendingPairing& pairing){auto* message=new(std::nothrow) aicli::agent::PendingPairing(pairing);if(!message||!g_hwnd||!PostMessageW(g_hwnd,WM_PAIRING_PENDING,0,reinterpret_cast<LPARAM>(message)))delete message;});
    g_remote_agent.SetStatusCallback([](bool online,const std::wstring& text){auto* message=new(std::nothrow) AgentStatusMessage{online,text};if(!message||!g_hwnd||!PostMessageW(g_hwnd,WM_AGENT_STATUS,0,reinterpret_cast<LPARAM>(message)))delete message;});
    aicli::agent::AgentConfig config;config.bind_address=g_store.GetSetting(L"listen_address",L"0.0.0.0");config.port=g_store.GetSetting(L"listen_port",L"4280");config.local_address=g_store.GetSetting(L"local_address");config.remote_address=g_store.GetSetting(L"remote_address");config.data_directory=aicli::DefaultDataDirectory();std::wstring error;if(!g_remote_agent.Start(&g_store,config,&error))SetText(g_status,L"Agent 启动失败: "+error);else SetText(g_status,L"就绪");UpdateDeviceHeader();
}

aicli::agent::AgentConfig CurrentAgentConfig() {
    aicli::agent::AgentConfig config;
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
    if (g_group_name != kAllGroupsName && std::none_of(g_groups.begin(), g_groups.end(), [&](const ProjectGroup& value) {
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
        [](bool) { ApplyLiveTheme(); });
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
    if(!h)return;
    // Keep the title bar client-owned, but give DWM a one-pixel compositor
    // rim.  This is enough for the native window shadow and Windows 11 corner
    // treatment without asking DWM to paint a second caption or caption
    // buttons over the custom header.
    DWMNCRENDERINGPOLICY rendering=DWMNCRP_ENABLED;
    DwmSetWindowAttribute(h,DWMWA_NCRENDERING_POLICY,
                          &rendering,sizeof(rendering));
    DWM_WINDOW_CORNER_PREFERENCE corners=DWMWCP_ROUND;
    DwmSetWindowAttribute(h,DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corners,sizeof(corners));
    MARGINS compositor_rim{1,1,1,1};
    DwmExtendFrameIntoClientArea(h,&compositor_rim);
    SetWindowPos(h,nullptr,0,0,0,0,
                 SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
}
void SetTitleBarActive(HWND h,bool active){
    if(g_window_active==active)return;
    g_window_active=active;
    ApplyWindowChromeTheme(false);
    InvalidateTitleBar(h);
}
LRESULT HitTestCustomFrame(HWND h,LPARAM l){
    const auto metrics=aicli::ui::LayoutMetrics::ForWindow(h);
    POINT screen{GET_X_LPARAM(l),GET_Y_LPARAM(l)};RECT window{};GetWindowRect(h,&window);const UINT dpi=GetDpiForWindow(h);const int frame=GetSystemMetricsForDpi(SM_CXSIZEFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi);
    if(!IsZoomed(h)){
        const bool left=screen.x>=window.left&&screen.x<window.left+frame,right=screen.x<window.right&&screen.x>=window.right-frame,top=screen.y>=window.top&&screen.y<window.top+frame,bottom=screen.y<window.bottom&&screen.y>=window.bottom-frame;
        if(top&&left)return HTTOPLEFT;if(top&&right)return HTTOPRIGHT;if(bottom&&left)return HTBOTTOMLEFT;if(bottom&&right)return HTBOTTOMRIGHT;if(left)return HTLEFT;if(right)return HTRIGHT;if(top)return HTTOP;if(bottom)return HTBOTTOM;
    }
    POINT client=screen;ScreenToClient(h,&client);
    if(TitleButtonAtPoint(h,client))return HTCLIENT;
    if(client.y>=0&&client.y<metrics.title_bar_height){HWND child=ChildWindowFromPointEx(h,client,CWP_SKIPINVISIBLE|CWP_SKIPDISABLED|CWP_SKIPTRANSPARENT);if(child&&child!=h)return HTCLIENT;return HTCAPTION;}
    return HTCLIENT;
}
void ResizeModalHosts(HWND owner){
    // ModalWorkspaceScope subclasses the owner and repositions both popup
    // layers atomically.  This hook remains for the root lifecycle but must
    // never resize popup surfaces as child windows.
    (void)owner;
}
LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(g_project_drag_message&&m==g_project_drag_message)return HandleProjectDragListMessage(h,l);
    switch(m){
    case WM_CREATE:{g_hwnd=h; g_font=CreateFontW(-15,0,0,0,400,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");g_font_medium=CreateFontW(-17,0,0,0,600,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");g_font_small=CreateFontW(-13,0,0,0,400,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");g_font_modal_title=CreateFontW(-21,0,0,0,600,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");g_icon_font=CreateFontW(-16,0,0,0,400,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe MDL2 Assets");
        g_group=CreateWindowW(L"BUTTON",g_group_name.c_str(),WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,168,8,154,32,h,(HMENU)(INT_PTR)ID_GROUP_DEFAULT,GetModuleHandleW(nullptr),nullptr);g_group_add=CreateWindowW(L"BUTTON",L"+",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,326,8,34,32,h,(HMENU)(INT_PTR)ID_GROUP_ADD,GetModuleHandleW(nullptr),nullptr);g_bind=CreateWindowW(L"BUTTON",L"绑定手机",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,365,8,100,32,h,(HMENU)(INT_PTR)ID_BIND_PHONE,GetModuleHandleW(nullptr),nullptr);g_devices=CreateWindowW(L"BUTTON",L"已绑定 0 台",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,470,8,112,32,h,(HMENU)(INT_PTR)ID_DEVICES,GetModuleHandleW(nullptr),nullptr);g_new_project=CreateWindowW(L"BUTTON",L"+",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,8,55,34,30,h,(HMENU)(INT_PTR)ID_NEW_PROJECT,GetModuleHandleW(nullptr),nullptr);g_settings=CreateWindowW(L"BUTTON",L"设置",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,1000,8,70,32,h,(HMENU)(INT_PTR)ID_SETTINGS,GetModuleHandleW(nullptr),nullptr);g_cli_tab=CreateWindowW(L"BUTTON",L"AI 终端",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,302,72,100,30,h,(HMENU)(INT_PTR)ID_CLI_TAB,GetModuleHandleW(nullptr),nullptr);HWND controls[]={g_group,g_group_add,g_bind,g_devices,g_new_project,g_settings,g_cli_tab};for(HWND b:controls){SendMessageW(b,WM_SETFONT,(WPARAM)g_font,TRUE);EnableButtonHover(b);}
        g_sidebar_title=CreateWindowW(L"STATIC",L"项目与会话",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_sidebar_title,WM_SETFONT,(WPARAM)g_font_medium,TRUE);g_search=aicli::ui::CreateStyledInput(h,ID_SEARCH_PROJECTS,g_font,L"搜索会话");
        g_list=CreateWindowW(L"LISTBOX",nullptr,WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,0,0,0,0,h,(HMENU)(INT_PTR)ID_PROJECT_LIST,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_list,WM_SETFONT,(WPARAM)g_font,TRUE);SetWindowSubclass(g_list,ProjectListSubclassProc,kProjectListSubclassId,0);g_project_drag_message=RegisterWindowMessageW(DRAGLISTMSGSTRING);MakeDragList(g_list);RefreshProjectList();
        g_detail_title=CreateWindowW(L"STATIC",L"选择一个项目",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        g_detail_folder=CreateWindowW(L"STATIC",L"文件夹: 未设置",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        g_detail_command=CreateWindowW(L"STATIC",L"启动命令: 未设置",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        g_detail_state=CreateWindowW(L"STATIC",L"状态: 未运行",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        g_start_project=CreateWindowW(L"BUTTON",L"立即开始",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,0,0,0,0,h,(HMENU)(INT_PTR)ID_START_PROJECT,GetModuleHandleW(nullptr),nullptr);
        g_edit_project=CreateWindowW(L"BUTTON",L"编辑项目",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,0,0,0,0,h,(HMENU)(INT_PTR)ID_EDIT_PROJECT,GetModuleHandleW(nullptr),nullptr);
        HWND detailControls[]={g_detail_title,g_detail_folder,g_detail_command,g_detail_state,g_start_project,g_edit_project};for(HWND c:detailControls){SendMessageW(c,WM_SETFONT,(WPARAM)g_font,TRUE);if(c==g_start_project||c==g_edit_project)EnableButtonHover(c);}
        g_terminal_title=CreateWindowW(L"STATIC",L"选择一个项目开始",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_terminal_title,WM_SETFONT,(WPARAM)g_font,TRUE);g_status=CreateWindowW(L"STATIC",L"就绪    |    选择项目后查看启动信息",WS_CHILD|WS_VISIBLE,0,0,0,0,h,(HMENU)(INT_PTR)ID_SETTINGS,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_status,WM_SETFONT,(WPARAM)g_font_small,TRUE);
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
    case WM_DPICHANGED:
        g_layout_metrics=aicli::ui::LayoutMetrics::ForWindow(h);
        Layout();
        InvalidateRect(h,nullptr,FALSE);
        return 0;
    case WM_NCCALCSIZE:if(w){auto* params=(NCCALCSIZE_PARAMS*)l;if(IsZoomed(h)){HMONITOR monitor=MonitorFromRect(&params->rgrc[0],MONITOR_DEFAULTTONEAREST);MONITORINFO info{sizeof(info)};if(GetMonitorInfoW(monitor,&info))params->rgrc[0]=info.rcWork;}return 0;}break;
    case WM_NCHITTEST:return HitTestCustomFrame(h,l);
    case WM_MOUSEMOVE:{POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};const int hover=TitleButtonAtPoint(h,point);if(hover!=g_title_hover_id){g_title_hover_id=hover;InvalidateTitleButtons(h);}if(hover||g_title_pressed_id){TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,h,0};TrackMouseEvent(&tracking);if(hover)return 0;}break;}
    case WM_MOUSELEAVE:if(g_title_hover_id){g_title_hover_id=0;InvalidateTitleButtons(h);}return 0;
    case WM_LBUTTONDOWN:{POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};const int button=TitleButtonAtPoint(h,point);if(button){g_title_pressed_id=button;SetCapture(h);InvalidateTitleButtons(h);return 0;}break;}
    case WM_LBUTTONUP:{if(g_title_pressed_id){POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};const int pressed=g_title_pressed_id;g_title_pressed_id=0;if(GetCapture()==h)ReleaseCapture();InvalidateTitleButtons(h);if(TitleButtonAtPoint(h,point)==pressed)ExecuteTitleButton(h,pressed);return 0;}break;}
    case WM_CAPTURECHANGED:if(g_title_pressed_id){g_title_pressed_id=0;InvalidateTitleButtons(h);}break;
    case WM_MEASUREITEM:{auto* item=(MEASUREITEMSTRUCT*)l;if(item->CtlType==ODT_MENU){const auto* entry=reinterpret_cast<const ContextMenuEntry*>(item->itemData);item->itemWidth=218;item->itemHeight=entry&&!entry->label[0]?10:38;return TRUE;}if(item->CtlID==ID_PROJECT_LIST){item->itemHeight=50;return TRUE;}}break;
    case WM_DRAWITEM:{auto* item=(DRAWITEMSTRUCT*)l;if(item->CtlType==ODT_MENU){DrawContextMenuItem(item);return TRUE;}if(item->CtlType==ODT_BUTTON){DrawModernButton(item);return TRUE;}if(item->CtlID==ID_PROJECT_LIST){DrawProjectItem(item);return TRUE;}}break;
    case WM_SIZE:
        if(w==SIZE_MINIMIZED){g_island.SetVisible(false);return 0;}
        if(aicli::ui::HasActiveModal(h))ResizeModalHosts(h);
        Layout();return 0;
    case WM_TERMINAL_DIRTY:g_terminal_snapshot_message_pending=false;if(!g_terminal_snapshot_timer_active){SetTimer(h,kTerminalSnapshotTimerId,200,nullptr);g_terminal_snapshot_timer_active=true;}return 0;
    case WM_TIMER:if(w==1){RefreshGitStatus();UpdateHeaderTelemetry();return 0;}if(w==kTerminalSnapshotTimerId){if(g_terminal_snapshot_dirty.exchange(false))PublishTerminalSnapshots();else{KillTimer(h,kTerminalSnapshotTimerId);g_terminal_snapshot_timer_active=false;}return 0;}break;
    case WM_GETMINMAXINFO:{auto* info=(MINMAXINFO*)l;info->ptMinTrackSize.x=900;info->ptMinTrackSize.y=600;return 0;}
    case WM_ERASEBKGND: return 1;
    case WM_CONTEXTMENU: if((HWND)w==g_list){ShowProjectContextMenu(h,l);return 0;} break;
    case WM_CTLCOLORSTATIC: {HDC dc=(HDC)w;HWND control=(HWND)l;const bool onPanel=control==g_sidebar_title||control==g_status;SetBkMode(dc,OPAQUE);SetTextColor(dc,fg);SetBkColor(dc,onPanel?panel:bg);return (LRESULT)(onPanel?g_list_brush:g_bg_brush);}
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
        if(id==ID_CLOSE){DestroyWindow(h);return 0;}
        if(id==ID_MINIMIZE){ShowWindow(h,SW_MINIMIZE);return 0;}
        if(id==ID_MAXIMIZE){ShowWindow(h,IsZoomed(h)?SW_RESTORE:SW_MAXIMIZE);Layout();return 0;}
        // Root toolbar commands are blocked while an owned modal is active;
        // caption buttons continue to work through the custom frame path.
        if(aicli::ui::HasActiveModal(h))return 0;
        if(id==ID_SEARCH_PROJECTS&&notification==EN_CHANGE){RefreshProjectList();Layout();return 0;}
        if(id==ID_PROJECT_LIST&&notification==LBN_SELCHANGE){RefreshProjectDetails();Layout();return 0;}
        if(id==ID_PROJECT_LIST&&notification==LBN_DBLCLK){StartSession();return 0;}
        if(id==ID_NEW_PROJECT){CreateProject();return 0;}
        if(id==ID_START_PROJECT){StartSession();return 0;}
        if(id==ID_EDIT_PROJECT){EditProject();return 0;}
        if(id==ID_CTX_OPEN_FOLDER){OpenProjectFolder();return 0;}
        const UINT moveGroupEnd=static_cast<UINT>(ID_CTX_MOVE_GROUP_BASE)+static_cast<UINT>(g_groups.size());
        if(id>=static_cast<UINT>(ID_CTX_MOVE_GROUP_BASE)&&id<moveGroupEnd){MoveSelectedProjectToGroup(g_groups[id-ID_CTX_MOVE_GROUP_BASE].name);return 0;}
        if(id==ID_CTX_MOVE_DEFAULT){MoveSelectedProjectToDefaultGroup();return 0;}
        if(id==ID_CTX_RESTART){StartSession(true);return 0;}
        if(id==ID_CTX_CANCEL||id==ID_CTX_PAUSE){
            const int selected=SelectedProjectIndex();const bool pausing=id==ID_CTX_PAUSE;
            if(selected>=0&&selected<(int)g_projects.size()){
                auto& project=g_projects[selected];
                g_island.Stop(project.id);
                project.status=pausing?L"paused":L"cancelled";
                g_store.UpdateProjectStatus(project.id,project.status);
            }
            SetText(g_status,pausing?L"会话已暂停":L"会话已取消");
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
        if(id==ID_CLI_TAB){RefreshProjectDetails();Layout();return 0;}
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
        while(!text.empty()&&(text.back()==L'\r'||text.back()==L'\n'))text.pop_back();
        const bool typed=text.empty()||g_island.WriteInput(request->project_id,text);
        const bool submitted=typed&&g_island.WriteInput(request->project_id,L"\r");
        if(submitted){
            RecordAndBroadcastEvent(request->project_id,L"terminal.input",
                nlohmann::json{{"input",Utf8Text(text)},{"phoneId",Utf8Text(request->phone_id)}});
            complete(true);
        }else complete(false,L"终端输入失败");
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
    case WM_PAIRING_PENDING:{std::unique_ptr<aicli::agent::PendingPairing> pairing(reinterpret_cast<aicli::agent::PendingPairing*>(l));if(!pairing)return 0;g_pending_pairing=*pairing;if(g_binding_dialog){InvalidateRect(g_binding_dialog,nullptr,FALSE);}else if(pairing->submitted&&!pairing->authorized&&!pairing->rejected){const std::wstring text=L"手机名称："+pairing->device.name+L"\n型号："+pairing->device.model+L"\n设备 ID："+pairing->device.phone_id+L"\n\n确认后该手机可远程操作管理员终端。";const bool approve=MessageBoxW(h,text.c_str(),L"确认绑定手机",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES;std::wstring error;if(!g_remote_agent.ConfirmPairing(pairing->info.pairing_id,approve,&error)&&!error.empty())MessageBoxW(h,error.c_str(),L"手机绑定",MB_OK|MB_ICONERROR);}UpdateDeviceHeader();return 0;}
    case WM_AGENT_STATUS:{std::unique_ptr<AgentStatusMessage> message(reinterpret_cast<AgentStatusMessage*>(l));if(message&&!message->online)SetText(g_status,message->text);UpdateHeaderTelemetry();return 0;}
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(h,&ps);
        RECT r{};
        GetClientRect(h,&r);
        const auto metrics=aicli::ui::LayoutMetrics::ForWindow(h);

        HBRUSH background=CreateSolidBrush(bg);
        FillRect(dc,&r,background);
        DeleteObject(background);

        HBRUSH titleBar=CreateSolidBrush(TitleBarBackground());
        RECT top{0,0,r.right,metrics.title_bar_height};
        FillRect(dc,&top,titleBar);
        DeleteObject(titleBar);
        DrawTitleButtons(dc,h);

        HBRUSH panelBrush=CreateSolidBrush(panel);
        RECT side{0,metrics.title_bar_height,metrics.sidebar_width,r.bottom-metrics.footer_height};
        RECT footer{0,r.bottom-metrics.footer_height,r.right,r.bottom};
        FillRect(dc,&side,panelBrush);
        FillRect(dc,&footer,panelBrush);
        DeleteObject(panelBrush);

        HPEN pen=CreatePen(PS_SOLID,1,border);
        auto oldPen=(HPEN)SelectObject(dc,pen);
        MoveToEx(dc,metrics.sidebar_width-1,metrics.title_bar_height,nullptr);
        LineTo(dc,metrics.sidebar_width-1,r.bottom-metrics.footer_height);
        MoveToEx(dc,0,metrics.title_bar_height-1,nullptr);
        LineTo(dc,r.right,metrics.title_bar_height-1);
        MoveToEx(dc,0,r.bottom-metrics.footer_height,nullptr);
        LineTo(dc,r.right,r.bottom-metrics.footer_height);
        SelectObject(dc,oldPen);
        DeleteObject(pen);

        const COLORREF searchBorder=GetFocus()==g_search?accent:border;
        DrawRoundedOutline(dc,RECT{14,68,272,108},searchBorder,10);
        RECT searchLine{22,105,264,GetFocus()==g_search?107:106};
        HBRUSH searchBrush=CreateSolidBrush(GetFocus()==g_search?accent:muted);
        FillRect(dc,&searchLine,searchBrush);
        DeleteObject(searchBrush);
        DrawRoundedOutline(dc,RECT{302,110,r.right-14,r.bottom-40},border,10);

        HBRUSH logo=CreateSolidBrush(accent2);
        HPEN logoPen=CreatePen(PS_SOLID,1,RGB(75,143,244));
        auto oldLogoBrush=SelectObject(dc,logo);
        auto oldLogoPen=SelectObject(dc,logoPen);
        RoundRect(dc,14,10,46,42,9,9);
        SelectObject(dc,oldLogoBrush);
        SelectObject(dc,oldLogoPen);
        DeleteObject(logo);
        DeleteObject(logoPen);

        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,RGB(255,255,255));
        SelectObject(dc,g_font_medium);
        TextOutW(dc,22,16,L">_",2);
        SetTextColor(dc,TitleBarText());
        TextOutW(dc,58,11,L"AI CLI",6);

        SelectObject(dc,g_font_small);
        if(g_git_footer_rect.right>g_git_footer_rect.left){
            SetBkMode(dc,TRANSPARENT);
            SetTextColor(dc,GitStatusColor());
            RECT git_rect=g_git_footer_rect;
            git_rect.left+=4;git_rect.right-=4;
            DrawTextW(dc,GitStatusLabel().c_str(),-1,&git_rect,
                      DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        }

        const int telemetryLeft=std::max(14,static_cast<int>(r.right)-kFooterTelemetryWidth+8);
        const int telemetryTop=r.bottom-metrics.footer_height;
        const std::wstring agentLabel=g_remote_agent.Online()?L"● Agent 在线":L"○ Agent 离线";
        const std::wstring memoryLabel=L"内存 "+std::to_wstring(WorkingSetMegabytes())+L" MB";
        const bool relayConfigured=!g_store.GetSetting(L"remote_address").empty();
        const std::wstring relayLabel=relayConfigured?L"公网已配置":L"公网未配置";
        RECT agentRect{telemetryLeft,telemetryTop,telemetryLeft+116,r.bottom};
        RECT memoryRect{agentRect.right,telemetryTop,agentRect.right+108,r.bottom};
        RECT relayRect{memoryRect.right,telemetryTop,r.right-10,r.bottom};
        SetTextColor(dc,g_remote_agent.Online()?RGB(83,203,146):RGB(225,101,112));
        DrawTextW(dc,agentLabel.c_str(),-1,&agentRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        SetTextColor(dc,muted);
        DrawTextW(dc,memoryLabel.c_str(),-1,&memoryRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        DrawTextW(dc,relayLabel.c_str(),-1,&relayRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE);

        EndPaint(h,&ps);
        return 0;
    }
    case WM_DESTROY:g_remote_agent.Stop();g_island.StopAll();EndProjectDragPreview();if(g_git_tooltip){DestroyWindow(g_git_tooltip);g_git_tooltip=nullptr;}if(g_font)DeleteObject(g_font);if(g_font_medium)DeleteObject(g_font_medium);if(g_font_small)DeleteObject(g_font_small);if(g_font_modal_title)DeleteObject(g_font_modal_title);if(g_icon_font)DeleteObject(g_icon_font);if(g_edit_brush){DeleteObject(g_edit_brush);g_edit_brush=nullptr;}if(g_focus_brush){DeleteObject(g_focus_brush);g_focus_brush=nullptr;}if(g_list_brush){DeleteObject(g_list_brush);g_list_brush=nullptr;}if(g_bg_brush){DeleteObject(g_bg_brush);g_bg_brush=nullptr;}PostQuitMessage(0);return 0; }
    return DefWindowProcW(h,m,w,l);
}
}

int WINAPI wWinMain(HINSTANCE inst,HINSTANCE, PWSTR arguments, int show){GdiplusRuntime gdiplus;winrt::init_apartment(winrt::apartment_type::single_threaded);const int activation=EnsurePackagedActivation(arguments);if(activation<0){MessageBoxW(nullptr,L"无法启动已注册的 AI CLI 桌面实例，请先完成包注册。",L"AI CLI",MB_OK|MB_ICONERROR);return 1;}if(activation==0)return 0;if(!InitializePersistentState()){winrt::uninit_apartment();return 1;}g_components.Initialize();INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_STANDARD_CLASSES|ICC_WIN95_CLASSES};InitCommonControlsEx(&ic);WNDCLASSW wc{};wc.hInstance=inst;wc.lpfnWndProc=WndProc;wc.lpszClassName=kClassName;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&wc);constexpr DWORD windowStyle=WS_SYSMENU|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_CLIPCHILDREN;g_hwnd=CreateWindowExW(0,kClassName,L"\x200B",windowStyle,CW_USEDEFAULT,CW_USEDEFAULT,1280,800,nullptr,nullptr,inst,nullptr);ApplyWindowChromeTheme();ShowWindow(g_hwnd,show);UpdateWindow(g_hwnd);MSG msg;while(GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}g_store.Close();winrt::uninit_apartment();return 0;}
