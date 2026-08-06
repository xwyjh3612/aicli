#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <appmodel.h>
#include <roapi.h>
#include <psapi.h>
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <string_view>
#include <optional>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
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
#include "terminal/ansi_style_connection.h"
#include "terminal/conpty_connection_bridge.h"
#include "ui/styled_input.h"
#include "ui/management_dialog.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "psapi.lib")

namespace {
constexpr wchar_t kApplicationUserModelId[] = L"AiCli.Desktop.Dev_jw3zr00rmb0ym!App";
constexpr UINT WM_REMOTE_INPUT = WM_APP + 20;
constexpr UINT WM_REMOTE_CONTROL = WM_APP + 21;
constexpr UINT WM_PAIRING_PENDING = WM_APP + 22;
constexpr UINT WM_AGENT_STATUS = WM_APP + 23;
constexpr UINT WM_RESTORE_LAST_SESSION = WM_APP + 24;
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
constexpr int ID_MINIMIZE = 3010;
constexpr int ID_CLOSE = 3011;
constexpr wchar_t kClassName[] = L"AicliDesktopWindow";
constexpr UINT_PTR kXamlIslandSubclassId = 0xA1C11;
constexpr int kCustomTitleBarHeight = 52;

using aicli::DeviceRecord;
using aicli::Project;
using aicli::ProjectGroup;
using aicli::ProjectType;
struct RemoteInputMessage { std::wstring project_id, phone_id, input; };
struct RemoteControlMessage { std::wstring project_id, phone_id, action; };
struct AgentStatusMessage { bool online{}; std::wstring text; };

int EnsurePackagedActivation() {
    UINT32 length = 0;
    const LONG result = GetCurrentPackageFullName(&length, nullptr);
    if (result != APPMODEL_ERROR_NO_PACKAGE) return 1;
    IApplicationActivationManager* manager = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&manager))) || !manager) return -1;
    DWORD process_id = 0;
    const HRESULT activated = manager->ActivateApplication(kApplicationUserModelId, nullptr, AO_NONE, &process_id);
    manager->Release();
    return SUCCEEDED(activated) ? 0 : -1;
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
        if (!module) return false;
        auto callback = reinterpret_cast<PFNGETACTIVATIONFACTORY>(GetProcAddress(module, "DllGetActivationFactory"));
        if (!callback) return false;

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
        if (FAILED(RoRegisterActivationFactories(ids.data(), callbacks.data(),
                                                 static_cast<UINT32>(ids.size()), &cookie))) return false;
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
            grid_.Background(winrt::Windows::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{0xff,0x12,0x12,0x12}));
            try {
                application_.as<winrt::Windows::UI::Xaml::Application>()
                    .Resources().MergedDictionaries().Append(
                        winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources());
            } catch (...) {
                // The terminal still works with its compiled resources when WinUI theme resources are unavailable.
            }
            source_.Content(grid_); Resize(rc);
            ShowWindow(hwnd_,SW_HIDE); return true;
        } catch (...) { return false; }
    }
    bool Start(const std::wstring& command, const std::wstring& cwd, const std::wstring& title, HWND owner, HWND status,
               std::function<void(std::wstring_view)> output_observer = {}) {
        std::wstring stage = L"prepare";
        auto set_stage = [&](const wchar_t* value) {
            stage = value;
            if (status) SetWindowTextW(status, (L"正在初始化原生终端: " + stage).c_str());
        };
        try {
            Stop();
            using namespace winrt::Microsoft::Terminal;
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
            auto scheme_activation = LocalActivationFactory<winrt::Windows::Foundation::IActivationFactory>(
                L"Microsoft.Terminal.Settings.Model.dll",
                L"Microsoft.Terminal.Settings.Model.ColorScheme");
            auto scheme = scheme_activation.ActivateInstance<Settings::Model::ColorScheme>();
            using TerminalColor = Core::Color;
            scheme.Name(L"AI CLI Campbell");
            scheme.Foreground(TerminalColor{0xcc, 0xcc, 0xcc, 0xff});
            scheme.Background(TerminalColor{0x0c, 0x0c, 0x0c, 0xff});
            scheme.CursorColor(TerminalColor{0xff, 0xff, 0xff, 0xff});
            scheme.SelectionBackground(TerminalColor{0x26, 0x4f, 0x78, 0xff});
            constexpr std::array<TerminalColor, 16> campbell{
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
            for (uint8_t index = 0; index < campbell.size(); ++index) {
                scheme.SetColorTableEntry(index, campbell[index]);
            }
            default_settings.ApplyColorScheme(scheme);
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
            auto control_settings = settings_result.DefaultSettings();
            auto unfocused_settings = settings_result.UnfocusedSettings();

            // Do not activate TerminalConnection.dll here.  That DLL is a
            // private Windows Terminal implementation detail and its ABI and
            // dependency graph vary between installed builds.  The bridge
            // below implements the public ITerminalConnection contract and
            // owns a system ConPTY, while TermControl remains responsible for
            // native rendering, keyboard handling, selection and scrolling.
            set_stage(L"create ConPTY connection");
            auto bridge = winrt::make<aicli::terminal::ConPtyConnectionBridge>(cwd, command);
            connection_ = winrt::make<aicli::terminal::AnsiStyleConnection>(bridge, output_observer);
            official_connection_ = false;
            set_stage(L"create TermControl");
            auto control_factory = LocalActivationFactory<Control::ITermControlFactory>(
                L"Microsoft.Terminal.Control.dll",
                L"Microsoft.Terminal.Control.TermControl");
            control_ = control_factory.CreateInstance2(control_settings, unfocused_settings, connection_);
            set_stage(L"attach TermControl");
            control_.OwningHwnd(reinterpret_cast<uint64_t>(owner));
            control_.HorizontalAlignment(winrt::Windows::UI::Xaml::HorizontalAlignment::Stretch);
            control_.VerticalAlignment(winrt::Windows::UI::Xaml::VerticalAlignment::Stretch);
            grid_.Children().Append(control_);
            ShowWindow(hwnd_, SW_SHOW);
            SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            grid_.UpdateLayout();
            control_.UpdateLayout();
            visible_ = false;
            SetVisible(true);
            control_.Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic);
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
            Stop();
            return false;
        } catch (...) {
            last_error_ = stage + L": unknown error";
            Stop();
            return false;
        }
    }
    void Stop() {
        SetVisible(false);
        try {
            if (connection_) connection_.Close();
            if (grid_) grid_.Children().Clear();
        } catch (...) {}
        control_ = nullptr;
        connection_ = nullptr;
        official_connection_ = false;
        visible_ = false;
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    }
    void SetVisible(bool visible) {
        if (!hwnd_) return;
        try {
            if (control_ && visible_ != visible) {
                control_.WindowVisibilityChanged(visible);
                visible_ = visible;
            }
            ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
            if (visible) EnsureNativeScrollbar();
        } catch (...) {
            ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
        }
    }
    void Resize(RECT rc) {
        if (!hwnd_ || !parent_ || IsIconic(parent_)) return;
        const LONG raw_width = rc.right - rc.left;
        const LONG raw_height = rc.bottom - rc.top;
        if (raw_width <= 0 || raw_height <= 0) return;
        const int width = static_cast<int>(raw_width);
        const int height = static_cast<int>(raw_height);
        try {
            SetWindowPos(hwnd_, nullptr, rc.left, rc.top, width, height,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (grid_) {
                const UINT dpi = GetDpiForWindow(parent_);
                const double scale = dpi ? static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI : 1.0;
                grid_.Width(width / scale);
                grid_.Height(height / scale);
                if (control_) {
                    control_.Width(width / scale);
                    control_.Height(height / scale);
                    control_.UpdateLayout();
                }
                grid_.UpdateLayout();
                EnsureNativeScrollbar();
            }
        } catch (...) {
            OutputDebugStringW(L"AI CLI TermControl: ignored invalid resize during window transition\n");
        }
    }
    HWND Window() const { return hwnd_; }
    bool Active() const { return control_ != nullptr; }
    bool WriteInput(const std::wstring& value) {
        if(!connection_||value.empty())return false;
        try{
            const auto* first=reinterpret_cast<const char16_t*>(value.data());
            connection_.WriteInput(winrt::array_view<const char16_t>{first,static_cast<uint32_t>(value.size())});
            return true;
        }catch(...){return false;}
    }
    bool HandleMouseWheel(WPARAM wparam, LPARAM lparam, bool horizontal = false) {
        if (!control_ || !visible_ || !hwnd_) return false;
        const auto delta = static_cast<int32_t>(GET_WHEEL_DELTA_WPARAM(wparam));
        if (delta == 0) return false;
        try {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &point);
            const UINT dpi = parent_ ? GetDpiForWindow(parent_) : USER_DEFAULT_SCREEN_DPI;
            const float scale = dpi ? static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI : 1.0f;
            const auto keys = GET_KEYSTATE_WPARAM(wparam);
            control_.as<winrt::Microsoft::Terminal::Control::IMouseWheelListener>().OnMouseWheel(
                winrt::Windows::Foundation::Point{
                    static_cast<float>(point.x) / scale,
                    static_cast<float>(point.y) / scale},
                winrt::Microsoft::Terminal::Core::Point{
                    horizontal ? delta : 0,
                    horizontal ? 0 : delta},
                (keys & MK_LBUTTON) != 0,
                (keys & MK_MBUTTON) != 0,
                (keys & MK_RBUTTON) != 0);
            EnsureNativeScrollbar();
            return true;
        } catch (...) {
            return false;
        }
    }
    void OnIslandDestroyed(HWND window) noexcept {
        if (hwnd_ == window) hwnd_ = nullptr;
    }
    const std::wstring& LastError() const { return last_error_; }
private:
    void EnsureNativeScrollbar() {
        if (!control_) return;
        try {
            const auto element = control_.as<winrt::Windows::UI::Xaml::FrameworkElement>()
                                     .FindName(L"ScrollBar");
            if (const auto scrollbar = element.try_as<
                    winrt::Windows::UI::Xaml::Controls::Primitives::ScrollBar>()) {
                scrollbar.Visibility(winrt::Windows::UI::Xaml::Visibility::Visible);
                scrollbar.IndicatorMode(
                    winrt::Windows::UI::Xaml::Controls::Primitives::ScrollingIndicatorMode::MouseIndicator);
                scrollbar.IsHitTestVisible(true);
                scrollbar.Opacity(1.0);
            }
        } catch (...) {
        }
    }

    winrt::com_ptr<HostXamlApplication> application_;
    winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager manager_{nullptr};
    winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource source_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid grid_{nullptr};
    winrt::Microsoft::Terminal::Control::TermControl control_{nullptr};
    winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection connection_{nullptr};
    bool official_connection_{};
    bool visible_{};
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
        if (message == WM_MOUSEWHEEL && adapter->HandleMouseWheel(wparam, lparam)) return 0;
        if (message == WM_MOUSEHWHEEL && adapter->HandleMouseWheel(wparam, lparam, true)) return 0;
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
bool g_session_running=false; int g_running_project=-1; int g_paused_project=-1;
COLORREF bg=RGB(12,14,18), panel=RGB(18,21,27), elevated=RGB(25,29,37), border=RGB(45,51,62), fg=RGB(238,241,247), muted=RGB(139,148,162), accent=RGB(83,156,255), accent2=RGB(42,108,214);
HBRUSH g_edit_brush{},g_list_brush{},g_bg_brush{};bool g_light_theme{};
HWND g_project_dialog{}, g_d_name{}, g_d_type{}, g_d_group{}, g_d_folder{}, g_d_folder_browse{}, g_d_command{}; bool g_project_dialog_ok{}; bool g_project_dialog_edit{}; Project g_project_draft{};
HWND g_binding_dialog{},g_binding_refresh{},g_binding_close{},g_binding_approve{},g_binding_reject{};
aicli::agent::PairingInfo g_binding_info{};
std::optional<qrcodegen::QrCode> g_binding_qr;
RECT g_binding_panel{};
HBITMAP g_project_snapshot{}; SIZE g_project_snapshot_size{}; RECT g_project_panel{};
RECT g_d_name_rect{}, g_d_type_rect{}, g_d_group_rect{}, g_d_folder_rect{}, g_d_command_rect{};

enum { ID_D_NAME=2001, ID_D_TYPE=2002, ID_D_GROUP=2003, ID_D_FOLDER=2004, ID_D_COMMAND=2005, ID_D_OK=2006, ID_D_CANCEL=2007, ID_D_BROWSE=2008 };
enum { ID_BIND_REFRESH=2101,ID_BIND_CLOSE=2102,ID_BIND_APPROVE=2103,ID_BIND_REJECT=2104 };
struct ProjectDropdownEntry{std::wstring label;bool selected{};};
int g_project_dropdown_width=220;
void DrawModernButton(const DRAWITEMSTRUCT* item);
LRESULT CALLBACK ButtonProc(HWND h,UINT m,WPARAM w,LPARAM l);
void Layout();
void UpdateDeviceHeader();
void RefreshConversationsCache();
void ApplyStoredTheme(){const auto theme=g_store.GetSetting(L"theme",L"dark");g_light_theme=theme==L"light";if(g_light_theme){bg=RGB(246,248,251);panel=RGB(255,255,255);elevated=RGB(239,243,248);border=RGB(215,221,230);fg=RGB(28,34,43);muted=RGB(101,111,125);accent=RGB(46,118,211);accent2=RGB(39,102,190);}else{bg=RGB(12,14,18);panel=RGB(18,21,27);elevated=RGB(25,29,37);border=RGB(45,51,62);fg=RGB(238,241,247);muted=RGB(139,148,162);accent=RGB(83,156,255);accent2=RGB(42,108,214);}if(g_edit_brush)DeleteObject(g_edit_brush);if(g_list_brush)DeleteObject(g_list_brush);if(g_bg_brush)DeleteObject(g_bg_brush);g_edit_brush=CreateSolidBrush(elevated);g_list_brush=CreateSolidBrush(panel);g_bg_brush=CreateSolidBrush(bg);}
void EnableButtonHover(HWND button){WNDPROC old=(WNDPROC)SetWindowLongPtrW(button,GWLP_WNDPROC,(LONG_PTR)ButtonProc);if(!g_button_proc)g_button_proc=old;}
constexpr UINT_PTR kProjectEditSubclassId=0xA1C12;
LRESULT CALLBACK ProjectEditSubclassProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR){if((m==WM_KEYDOWN||m==WM_CHAR)&&(w==VK_RETURN||w==L'\n'))return 0;if(m==WM_NCDESTROY)RemoveWindowSubclass(h,ProjectEditSubclassProc,kProjectEditSubclassId);return DefSubclassProc(h,m,w,l);}
void CenterProjectEditText(HWND control){
    if(!control)return;RECT client{};GetClientRect(control,&client);HDC dc=GetDC(control);if(!dc)return;
    HFONT font=(HFONT)SendMessageW(control,WM_GETFONT,0,0);auto oldFont=font?SelectObject(dc,font):nullptr;TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);if(oldFont)SelectObject(dc,oldFont);ReleaseDC(control,dc);
    const int height=client.bottom-client.top;const int textHeight=std::min(height,std::max(1,(int)metrics.tmHeight));const int top=std::max(0,(height-textHeight)/2);
    RECT format{10,top,std::max(11,(int)client.right-10),std::min(height,top+textHeight+1)};SendMessageW(control,EM_SETRECTNP,0,(LPARAM)&format);InvalidateRect(control,nullptr,TRUE);
}

void SetText(HWND h,const std::wstring& s){if(h)SetWindowTextW(h,s.c_str());}
std::string Utf8Text(std::wstring_view value){if(value.empty())return{};const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);if(count<=0)return{};std::string result(static_cast<size_t>(count),'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count,nullptr,nullptr);return result;}
std::wstring WideText(std::string_view value){if(value.empty())return{};const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);if(count<=0)return{};std::wstring result(static_cast<size_t>(count),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count);return result;}
std::wstring StripTerminalSequences(std::wstring_view value){std::wstring result;result.reserve(value.size());enum class State{Text,Escape,Csi,Osc,OscEscape};State state=State::Text;for(const wchar_t ch:value){switch(state){case State::Text:if(ch==L'\x1b')state=State::Escape;else if(ch==L'\r'||ch==L'\n'||ch==L'\t'||ch>=L' ')result.push_back(ch);break;case State::Escape:if(ch==L'[')state=State::Csi;else if(ch==L']')state=State::Osc;else state=State::Text;break;case State::Csi:if(ch>=0x40&&ch<=0x7e)state=State::Text;break;case State::Osc:if(ch==L'\a')state=State::Text;else if(ch==L'\x1b')state=State::OscEscape;break;case State::OscEscape:state=ch==L'\\'?State::Text:State::Osc;break;}}return result;}
size_t WorkingSetMegabytes(){PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);return GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters))?static_cast<size_t>(counters.WorkingSetSize/(1024ull*1024ull)):0;}
void RecordAndBroadcastEvent(const std::wstring& project_id,const std::wstring& event_type,const nlohmann::json& payload){if(project_id.empty())return;const std::string payload_json=payload.dump(-1,' ',false,nlohmann::json::error_handler_t::replace);const auto id=g_store.AppendEvent(project_id,event_type,payload_json);nlohmann::json event{{"id",id},{"eventId",id},{"conversationId",Utf8Text(project_id)},{"event_type",Utf8Text(event_type)},{"type",Utf8Text(event_type)},{"payload",payload}};g_remote_agent.BroadcastEvent(project_id,event.dump(-1,' ',false,nlohmann::json::error_handler_t::replace));}
void RecordTerminalOutput(const std::wstring& project_id,std::wstring_view output){const auto clean=StripTerminalSequences(output);if(clean.empty())return;RecordAndBroadcastEvent(project_id,L"terminal.output",nlohmann::json{{"text",Utf8Text(clean)}});}
void ApplyRoundedRegion(HWND h,int width,int height,int radius){if(!h||width<=0||height<=0)return;SetWindowRgn(h,CreateRoundRectRgn(0,0,width+1,height+1,radius,radius),TRUE);}
void DrawRoundedOutline(HDC dc,RECT r,COLORREF color,int radius){HPEN p=CreatePen(PS_SOLID,1,color);auto oldp=SelectObject(dc,p);auto oldb=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));RoundRect(dc,r.left,r.top,r.right,r.bottom,radius,radius);SelectObject(dc,oldp);SelectObject(dc,oldb);DeleteObject(p);}
HWND MakeDialogControl(LPCWSTR cls,LPCWSTR text,DWORD style,int id,HWND parent,int x,int y,int w,int h){bool button=wcscmp(cls,L"BUTTON")==0;if(button)style|=BS_OWNERDRAW;HWND c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,parent,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);SendMessageW(c,WM_SETFONT,(WPARAM)g_font,TRUE);if(button)EnableButtonHover(c);return c;}
void ReadDialogText(HWND c,std::wstring& value){wchar_t b[1024]{};GetWindowTextW(c,b,1024);value=b;}
bool PickFolder(HWND owner,std::wstring& path);
std::wstring DefaultProjectCommand(const std::wstring& type){
    const auto found=std::find_if(g_project_types.begin(),g_project_types.end(),[&](const ProjectType& value){return _wcsicmp(value.name.c_str(),type.c_str())==0;});
    if(found!=g_project_types.end())return found->default_command;
    return type==L"Claude"?L"claude --dangerously-skip-permissions --continue":L"codex --yolo resume --last";
}
void ReleaseProjectSnapshot(){if(g_project_snapshot){DeleteObject(g_project_snapshot);g_project_snapshot=nullptr;}g_project_snapshot_size={};}
void CaptureProjectSnapshot(HWND owner){
    ReleaseProjectSnapshot();
    RECT client{}; if(!GetClientRect(owner,&client))return;
    const int width=client.right-client.left, height=client.bottom-client.top;
    if(width<=0||height<=0)return;
    HDC screen=GetDC(nullptr); if(!screen)return;
    HDC memory=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);
    if(!memory||!bitmap){if(memory)DeleteDC(memory);ReleaseDC(nullptr,screen);return;}
    auto old=(HBITMAP)SelectObject(memory,bitmap);
    POINT origin{0,0}; ClientToScreen(owner,&origin);
    BitBlt(memory,0,0,width,height,screen,origin.x,origin.y,SRCCOPY);
    SelectObject(memory,old); DeleteDC(memory); ReleaseDC(nullptr,screen);
    g_project_snapshot=bitmap; g_project_snapshot_size={width,height};
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
    InvalidateRect(h,nullptr,FALSE);
}
void PaintProjectDialog(HDC dc,RECT client){
    const int width=client.right-client.left, height=client.bottom-client.top;
    if(g_project_snapshot&&g_project_snapshot_size.cx==width&&g_project_snapshot_size.cy==height){HDC source=CreateCompatibleDC(dc);auto old=(HBITMAP)SelectObject(source,g_project_snapshot);BitBlt(dc,0,0,width,height,source,0,0,SRCCOPY);SelectObject(source,old);DeleteDC(source);}
    else{HBRUSH fallback=CreateSolidBrush(RGB(31,35,42));FillRect(dc,&client,fallback);DeleteObject(fallback);}
    HDC scrim=CreateCompatibleDC(dc);HBITMAP scrimBitmap=CreateCompatibleBitmap(dc,1,1);auto oldBitmap=(HBITMAP)SelectObject(scrim,scrimBitmap);HBRUSH scrimBrush=CreateSolidBrush(RGB(126,132,140));RECT pixel{0,0,1,1};FillRect(scrim,&pixel,scrimBrush);DeleteObject(scrimBrush);BLENDFUNCTION blend{AC_SRC_OVER,0,150,0};AlphaBlend(dc,0,0,width,height,scrim,0,0,1,1,blend);SelectObject(scrim,oldBitmap);DeleteObject(scrimBitmap);DeleteDC(scrim);
    HBRUSH panelBrush=CreateSolidBrush(RGB(29,31,35));HPEN panelPen=CreatePen(PS_SOLID,1,RGB(59,64,73));auto oldBrush=SelectObject(dc,panelBrush);auto oldPen=SelectObject(dc,panelPen);RoundRect(dc,g_project_panel.left,g_project_panel.top,g_project_panel.right,g_project_panel.bottom,14,14);SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(panelBrush);DeleteObject(panelPen);
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(246,248,252));SelectObject(dc,g_font_modal_title?g_font_modal_title:g_font_medium);RECT title{g_project_panel.left+34,g_project_panel.top+24,g_project_panel.right-34,g_project_panel.top+50};DrawTextW(dc,g_project_dialog_edit?L"编辑项目":L"新建项目",-1,&title,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    SelectObject(dc,g_font_small);SetTextColor(dc,RGB(137,146,160));RECT subtitle{g_project_panel.left+34,g_project_panel.top+53,g_project_panel.right-34,g_project_panel.top+75};DrawTextW(dc,L"配置项目信息与 AI CLI 启动环境",-1,&subtitle,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    SelectObject(dc,g_font_small);SetTextColor(dc,RGB(190,196,206));auto label=[&](const wchar_t* text,int y,int x=g_project_panel.left+34){RECT r{x,y,x+250,y+22};DrawTextW(dc,text,-1,&r,DT_LEFT|DT_SINGLELINE|DT_VCENTER);};label(L"项目名称 / Tab 标题",g_project_panel.top+81);label(L"项目类型",g_project_panel.top+155);label(L"分组",g_project_panel.top+155,g_project_panel.left+(g_project_panel.right-g_project_panel.left)/2+12);label(L"项目路径",g_project_panel.top+229);label(L"启动命令",g_project_panel.top+303);
    if(g_project_dialog_edit){SetTextColor(dc,RGB(116,126,140));RECT locked{g_d_type_rect.right-96,g_project_panel.top+155,g_d_type_rect.right,g_project_panel.top+177};DrawTextW(dc,L"不可修改",-1,&locked,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);}
    HPEN divider=CreatePen(PS_SOLID,1,RGB(48,52,59));auto oldDivider=SelectObject(dc,divider);MoveToEx(dc,g_project_panel.left+34,g_project_panel.top+394,nullptr);LineTo(dc,g_project_panel.right-34,g_project_panel.top+394);SelectObject(dc,oldDivider);DeleteObject(divider);
    auto outline=[&](RECT r,HWND control){
        const bool active=GetFocus()==control;
        DrawRoundedOutline(dc,r,RGB(67,71,80),6);
        const int lineHeight=active?2:1;RECT line{r.left+2,r.bottom-lineHeight-1,r.right-2,r.bottom-1};
        HBRUSH underline=CreateSolidBrush(active?RGB(54,163,255):RGB(94,100,112));FillRect(dc,&line,underline);DeleteObject(underline);
    };outline(g_d_name_rect,g_d_name);outline(g_d_type_rect,g_d_type);outline(g_d_group_rect,g_d_group);outline(g_d_folder_rect,g_d_folder);outline(g_d_command_rect,g_d_command);
}
void DrawProjectDropdownItem(const DRAWITEMSTRUCT* item){
    const auto* entry=reinterpret_cast<const ProjectDropdownEntry*>(item->itemData);if(!entry)return;
    RECT r=item->rcItem;const bool hover=(item->itemState&ODS_SELECTED)!=0;
    HBRUSH fill=CreateSolidBrush(hover?RGB(45,52,63):RGB(31,34,40));FillRect(item->hDC,&r,fill);DeleteObject(fill);
    SetBkMode(item->hDC,TRANSPARENT);SelectObject(item->hDC,g_font);
    SetTextColor(item->hDC,entry->selected?RGB(115,184,255):fg);
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
    HMENU menu=CreatePopupMenu();if(!menu)return;HBRUSH background=CreateSolidBrush(RGB(31,34,40));MENUINFO info{sizeof(info)};info.fMask=MIM_BACKGROUND|MIM_STYLE;info.hbrBack=background;info.dwStyle=MNS_NOCHECK;SetMenuInfo(menu,&info);
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
    DestroyMenu(menu);DeleteObject(background);InvalidateRect(dialog,nullptr,FALSE);InvalidateRect(anchor,nullptr,TRUE);
}
LRESULT CALLBACK ProjectDialogProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_CREATE:{
        const std::wstring initialType=g_project_dialog_edit?g_project_draft.type:(g_project_types.empty()?L"Codex":g_project_types.front().name);
        g_d_name=aicli::ui::CreateStyledInput(h,ID_D_NAME,g_font,L"输入项目名称");
        g_d_type=g_project_dialog_edit
            ?aicli::ui::CreateStyledInput(h,ID_D_TYPE,g_font,L"",true)
            :MakeDialogControl(L"BUTTON",initialType.c_str(),BS_NOTIFY|WS_TABSTOP,ID_D_TYPE,h,0,0,1,1);
        g_d_group=MakeDialogControl(L"BUTTON",(g_project_draft.group.empty()?g_group_name:g_project_draft.group).c_str(),BS_NOTIFY|WS_TABSTOP,ID_D_GROUP,h,0,0,1,1);
        g_d_folder=aicli::ui::CreateStyledInput(h,ID_D_FOLDER,g_font,L"请选择项目文件夹",true);
        g_d_folder_browse=MakeDialogControl(L"BUTTON",L"选择文件夹",BS_PUSHBUTTON|WS_TABSTOP,ID_D_BROWSE,h,0,0,1,1);
        g_d_command=aicli::ui::CreateStyledInput(h,ID_D_COMMAND,g_font,L"输入启动命令");
        MakeDialogControl(L"BUTTON",L"保存",BS_DEFPUSHBUTTON|WS_TABSTOP,ID_D_OK,h,0,0,1,1);
        MakeDialogControl(L"BUTTON",L"取消",BS_PUSHBUTTON|WS_TABSTOP,ID_D_CANCEL,h,0,0,1,1);
        if(g_project_dialog_edit){aicli::ui::SetStyledInputText(g_d_type,g_project_draft.type);aicli::ui::SetStyledInputText(g_d_name,g_project_draft.name);aicli::ui::SetStyledInputText(g_d_folder,g_project_draft.folder);aicli::ui::SetStyledInputText(g_d_command,g_project_draft.command);}else{aicli::ui::SetStyledInputText(g_d_command,DefaultProjectCommand(initialType));}
        LayoutProjectDialog(h);SetFocus(g_d_name);return 0; }
    case WM_SIZE: LayoutProjectDialog(h);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT client{};GetClientRect(h,&client);PaintProjectDialog(dc,client);EndPaint(h,&ps);return 0;}
    case WM_NCHITTEST:return HTCLIENT;
    case WM_CTLCOLORSTATIC:{
        HDC dc=(HDC)w;HWND control=(HWND)l;
        if(control==g_d_folder||(g_project_dialog_edit&&control==g_d_type)){
            const bool active=GetFocus()==control;const COLORREF color=active?RGB(37,43,51):RGB(38,40,44);
            SetTextColor(dc,active?fg:RGB(205,210,219));SetBkColor(dc,color);SetBkMode(dc,OPAQUE);
            static HBRUSH normal=CreateSolidBrush(RGB(38,40,44));static HBRUSH focused=CreateSolidBrush(RGB(37,43,51));return (LRESULT)(active?focused:normal);
        }
        SetTextColor(dc,fg);SetBkMode(dc,TRANSPARENT);return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_CTLCOLOREDIT:{HDC dc=(HDC)w;HWND control=(HWND)l;const bool active=GetFocus()==control;const COLORREF color=active?RGB(37,43,51):RGB(38,40,44);SetTextColor(dc,fg);SetBkColor(dc,color);static HBRUSH normal=CreateSolidBrush(RGB(38,40,44));static HBRUSH focused=CreateSolidBrush(RGB(37,43,51));return (LRESULT)(active?focused:normal);}
    case WM_CTLCOLORLISTBOX:{HDC dc=(HDC)w;SetTextColor(dc,fg);SetBkColor(dc,RGB(37,38,41));static HBRUSH brush=CreateSolidBrush(RGB(37,38,41));return (LRESULT)brush;}
    case WM_MEASUREITEM:{auto* item=(MEASUREITEMSTRUCT*)l;if(item->CtlType==ODT_MENU){item->itemWidth=std::max(160,g_project_dropdown_width-21);item->itemHeight=34;return TRUE;}}break;
    case WM_DRAWITEM: {auto* item=(DRAWITEMSTRUCT*)l;if(item->CtlType==ODT_MENU){DrawProjectDropdownItem(item);return TRUE;}if(item->CtlType==ODT_BUTTON){DrawModernButton(item);return TRUE;}} break;
    case WM_COMMAND:
        if((HIWORD(w)==EN_SETFOCUS||HIWORD(w)==EN_KILLFOCUS||HIWORD(w)==BN_SETFOCUS||HIWORD(w)==BN_KILLFOCUS)&&
           (LOWORD(w)==ID_D_NAME||LOWORD(w)==ID_D_TYPE||LOWORD(w)==ID_D_GROUP||LOWORD(w)==ID_D_FOLDER||LOWORD(w)==ID_D_COMMAND)){
            InvalidateRect(h,nullptr,FALSE);if((HWND)l)InvalidateRect((HWND)l,nullptr,TRUE);
        }
        if(LOWORD(w)==ID_D_TYPE&&HIWORD(w)==BN_CLICKED&&!g_project_dialog_edit){ShowProjectDropdown(h,g_d_type,true);return 0;}
        if(LOWORD(w)==ID_D_GROUP&&HIWORD(w)==BN_CLICKED){ShowProjectDropdown(h,g_d_group,false);return 0;}
        if(LOWORD(w)==ID_D_BROWSE){std::wstring selected;if(PickFolder(h,selected))aicli::ui::SetStyledInputText(g_d_folder,selected);InvalidateRect(h,nullptr,FALSE);return 0;}
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
bool ShowProjectDialog(HWND owner,const Project* initial=nullptr){static bool registered=false;if(!registered){WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=ProjectDialogProc;wc.lpszClassName=L"AicliProjectDialog";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=nullptr;wc.style=CS_DBLCLKS;RegisterClassW(&wc);registered=true;}g_project_dialog_ok=false;g_project_dialog_edit=initial!=nullptr;g_project_draft=initial?*initial:Project{};CaptureProjectSnapshot(owner);g_island.SetVisible(false);RECT client{};GetClientRect(owner,&client);g_project_dialog=CreateWindowExW(WS_EX_CONTROLPARENT,L"AicliProjectDialog",L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,client.right,client.bottom,owner,nullptr,GetModuleHandleW(nullptr),nullptr);if(!g_project_dialog){g_project_dialog=nullptr;ReleaseProjectSnapshot();Layout();return false;}SetWindowPos(g_project_dialog,HWND_TOP,0,0,client.right,client.bottom,SWP_SHOWWINDOW);SetFocus(g_d_name);MSG msg{};while(IsWindow(g_project_dialog)&&GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}g_project_dialog=nullptr;ReleaseProjectSnapshot();SetForegroundWindow(owner);SetFocus(owner);Layout();return g_project_dialog_ok;}

void RefreshBindingPairing(){
    g_binding_info=g_remote_agent.CreatePairing();g_pending_pairing.reset();g_binding_qr.reset();
    try{const auto value=Utf8Text(g_binding_info.qr_payload);g_binding_qr=qrcodegen::QrCode::encodeText(value.c_str(),qrcodegen::QrCode::Ecc::MEDIUM);}catch(...){g_binding_qr.reset();}
    if(g_binding_dialog)InvalidateRect(g_binding_dialog,nullptr,FALSE);
}
void LayoutBindingDialog(HWND h){RECT client{};GetClientRect(h,&client);const int width=std::min(820,std::max(700,(int)client.right-96));const int height=590;const int left=(client.right-width)/2,top=(client.bottom-height)/2;g_binding_panel={left,top,left+width,top+height};MoveWindow(g_binding_refresh,g_binding_panel.right-236,g_binding_panel.bottom-58,100,36,TRUE);MoveWindow(g_binding_close,g_binding_panel.right-124,g_binding_panel.bottom-58,90,36,TRUE);MoveWindow(g_binding_approve,g_binding_panel.right-236,g_binding_panel.bottom-106,100,36,TRUE);MoveWindow(g_binding_reject,g_binding_panel.right-124,g_binding_panel.bottom-106,90,36,TRUE);const bool pending=g_pending_pairing.has_value()&&g_pending_pairing->submitted&&!g_pending_pairing->authorized&&!g_pending_pairing->rejected;ShowWindow(g_binding_approve,pending?SW_SHOW:SW_HIDE);ShowWindow(g_binding_reject,pending?SW_SHOW:SW_HIDE);}
void PaintBindingDialog(HDC dc,RECT client){
    const int width=client.right,height=client.bottom;if(g_project_snapshot&&g_project_snapshot_size.cx==width&&g_project_snapshot_size.cy==height){HDC source=CreateCompatibleDC(dc);auto old=(HBITMAP)SelectObject(source,g_project_snapshot);BitBlt(dc,0,0,width,height,source,0,0,SRCCOPY);SelectObject(source,old);DeleteDC(source);}else{HBRUSH fallback=CreateSolidBrush(RGB(31,35,42));FillRect(dc,&client,fallback);DeleteObject(fallback);}HDC scrim=CreateCompatibleDC(dc);HBITMAP pixel=CreateCompatibleBitmap(dc,1,1);auto oldPixel=(HBITMAP)SelectObject(scrim,pixel);HBRUSH shade=CreateSolidBrush(RGB(126,132,140));RECT one{0,0,1,1};FillRect(scrim,&one,shade);DeleteObject(shade);BLENDFUNCTION blend{AC_SRC_OVER,0,150,0};AlphaBlend(dc,0,0,width,height,scrim,0,0,1,1,blend);SelectObject(scrim,oldPixel);DeleteObject(pixel);DeleteDC(scrim);
    HBRUSH panelBrush=CreateSolidBrush(RGB(29,31,35));HPEN panelPen=CreatePen(PS_SOLID,1,RGB(59,64,73));auto oldBrush=SelectObject(dc,panelBrush);auto oldPen=SelectObject(dc,panelPen);RoundRect(dc,g_binding_panel.left,g_binding_panel.top,g_binding_panel.right,g_binding_panel.bottom,14,14);SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(panelBrush);DeleteObject(panelPen);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,fg);SelectObject(dc,g_font_modal_title);RECT title{g_binding_panel.left+34,g_binding_panel.top+24,g_binding_panel.right-34,g_binding_panel.top+54};DrawTextW(dc,L"绑定 Android 手机",-1,&title,DT_LEFT|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,g_font_small);SetTextColor(dc,muted);RECT subtitle{g_binding_panel.left+34,g_binding_panel.top+56,g_binding_panel.right-34,g_binding_panel.top+82};DrawTextW(dc,L"必须使用 AI CLI App 内的扫码入口；电脑只需确认这一次",-1,&subtitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const int qrArea=360,qrLeft=g_binding_panel.left+34,qrTop=g_binding_panel.top+100;HBRUSH white=CreateSolidBrush(RGB(245,247,250));RECT qrBack{qrLeft,qrTop,qrLeft+qrArea,qrTop+qrArea};FillRect(dc,&qrBack,white);DeleteObject(white);if(g_binding_qr){const int modules=g_binding_qr->getSize();const int quiet=4;const int scale=std::max(1,qrArea/(modules+quiet*2));const int actual=scale*(modules+quiet*2);const int originX=qrLeft+(qrArea-actual)/2+quiet*scale,originY=qrTop+(qrArea-actual)/2+quiet*scale;HBRUSH black=CreateSolidBrush(RGB(18,20,24));for(int y=0;y<modules;++y)for(int x=0;x<modules;++x)if(g_binding_qr->getModule(x,y)){RECT module{originX+x*scale,originY+y*scale,originX+(x+1)*scale,originY+(y+1)*scale};FillRect(dc,&module,black);}DeleteObject(black);}else{SetTextColor(dc,RGB(40,43,50));SelectObject(dc,g_font);DrawTextW(dc,L"二维码生成失败",-1,&qrBack,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
    const int infoLeft=qrLeft+qrArea+34,infoRight=g_binding_panel.right-34;SelectObject(dc,g_font);SetTextColor(dc,RGB(219,224,232));RECT statusTitle{infoLeft,qrTop,infoRight,qrTop+30};DrawTextW(dc,L"等待手机扫码",-1,&statusTitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,g_font_small);SetTextColor(dc,muted);std::wstring localAddress=g_store.GetSetting(L"local_address");if(localAddress.empty()){try{const auto payload=nlohmann::json::parse(Utf8Text(g_binding_info.qr_payload));localAddress=WideText(payload.value("localAddress",std::string{}));}catch(...){}}std::wstring details=L"本地地址\n"+localAddress+L"\n\n证书指纹\n"+g_binding_info.certificate_fingerprint.substr(0,std::min<size_t>(24,g_binding_info.certificate_fingerprint.size()))+L"…\n\n二维码 5 分钟内有效。已绑定手机断网或电脑重启后不需要重新确认。若首次局域网连接超时，请在设置中执行一次“允许手机访问”。";RECT detailRect{infoLeft,qrTop+40,infoRight,qrTop+258};DrawTextW(dc,details.c_str(),-1,&detailRect,DT_LEFT|DT_TOP|DT_WORDBREAK);
    if(g_pending_pairing&&g_pending_pairing->submitted){SetTextColor(dc,g_pending_pairing->authorized?RGB(83,203,146):g_pending_pairing->rejected?RGB(225,101,112):RGB(255,196,92));SelectObject(dc,g_font_medium);RECT phoneTitle{infoLeft,qrTop+268,infoRight,qrTop+298};const wchar_t* state=g_pending_pairing->authorized?L"绑定完成":g_pending_pairing->rejected?L"已拒绝":L"等待电脑确认";DrawTextW(dc,state,-1,&phoneTitle,DT_LEFT|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,g_font_small);SetTextColor(dc,fg);std::wstring phone=L"设备："+g_pending_pairing->device.name+L"\n型号："+g_pending_pairing->device.model;RECT phoneRect{infoLeft,qrTop+302,infoRight,qrTop+360};DrawTextW(dc,phone.c_str(),-1,&phoneRect,DT_LEFT|DT_TOP|DT_WORDBREAK);}
}
LRESULT CALLBACK BindingDialogProc(HWND h,UINT m,WPARAM w,LPARAM l){switch(m){case WM_CREATE:g_binding_refresh=MakeDialogControl(L"BUTTON",L"刷新二维码",BS_PUSHBUTTON|WS_TABSTOP,ID_BIND_REFRESH,h,0,0,1,1);g_binding_close=MakeDialogControl(L"BUTTON",L"关闭",BS_PUSHBUTTON|WS_TABSTOP,ID_BIND_CLOSE,h,0,0,1,1);g_binding_approve=MakeDialogControl(L"BUTTON",L"确认绑定",BS_PUSHBUTTON|WS_TABSTOP,ID_BIND_APPROVE,h,0,0,1,1);g_binding_reject=MakeDialogControl(L"BUTTON",L"拒绝",BS_PUSHBUTTON|WS_TABSTOP,ID_BIND_REJECT,h,0,0,1,1);RefreshBindingPairing();LayoutBindingDialog(h);return 0;case WM_SIZE:LayoutBindingDialog(h);return 0;case WM_ERASEBKGND:return 1;case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT client{};GetClientRect(h,&client);PaintBindingDialog(dc,client);EndPaint(h,&ps);return 0;}case WM_DRAWITEM:DrawModernButton((DRAWITEMSTRUCT*)l);return TRUE;case WM_COMMAND:if(LOWORD(w)==ID_BIND_REFRESH){RefreshBindingPairing();return 0;}if(LOWORD(w)==ID_BIND_CLOSE){DestroyWindow(h);return 0;}if((LOWORD(w)==ID_BIND_APPROVE||LOWORD(w)==ID_BIND_REJECT)&&g_pending_pairing){std::wstring error;if(!g_remote_agent.ConfirmPairing(g_pending_pairing->info.pairing_id,LOWORD(w)==ID_BIND_APPROVE,&error)&&!error.empty())MessageBoxW(h,error.c_str(),L"手机绑定",MB_OK|MB_ICONERROR);UpdateDeviceHeader();InvalidateRect(h,nullptr,FALSE);return 0;}break;case WM_KEYDOWN:if(w==VK_ESCAPE){DestroyWindow(h);return 0;}break;case WM_DESTROY:g_binding_dialog=nullptr;g_binding_refresh=g_binding_close=g_binding_approve=g_binding_reject=nullptr;return 0;}return DefWindowProcW(h,m,w,l);}
void ShowBindingDialog(HWND owner){if(!g_remote_agent.Online()){MessageBoxW(owner,L"HTTPS/WSS Agent 未启动，请先检查设置中的端口。",L"绑定手机",MB_OK|MB_ICONERROR);return;}static bool registered=false;if(!registered){WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=BindingDialogProc;wc.lpszClassName=L"AicliBindingDialog";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=nullptr;RegisterClassW(&wc);registered=true;}CaptureProjectSnapshot(owner);g_island.SetVisible(false);RECT client{};GetClientRect(owner,&client);g_binding_dialog=CreateWindowExW(WS_EX_CONTROLPARENT,L"AicliBindingDialog",L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,client.right,client.bottom,owner,nullptr,GetModuleHandleW(nullptr),nullptr);if(!g_binding_dialog){ReleaseProjectSnapshot();Layout();return;}SetWindowPos(g_binding_dialog,HWND_TOP,0,0,client.right,client.bottom,SWP_SHOWWINDOW);MSG msg{};while(IsWindow(g_binding_dialog)&&GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}g_binding_dialog=nullptr;ReleaseProjectSnapshot();SetForegroundWindow(owner);SetFocus(owner);Layout();}

LRESULT CALLBACK ButtonProc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_MOUSEMOVE){if(g_hover_button!=h){HWND old=g_hover_button;g_hover_button=h;if(old)InvalidateRect(old,nullptr,TRUE);InvalidateRect(h,nullptr,TRUE);TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);}}else if(m==WM_MOUSELEAVE){if(g_hover_button==h)g_hover_button=nullptr;InvalidateRect(h,nullptr,TRUE);}else if(m==WM_LBUTTONDOWN||m==WM_LBUTTONUP||m==WM_SETFOCUS||m==WM_KILLFOCUS){InvalidateRect(h,nullptr,TRUE);}return CallWindowProcW(g_button_proc,h,m,w,l);}
void DrawModernButton(const DRAWITEMSTRUCT* item){
    RECT r=item->rcItem;const bool down=(item->itemState&ODS_SELECTED)!=0;const bool disabled=(item->itemState&ODS_DISABLED)!=0;const bool hover=item->hwndItem==g_hover_button;const bool focused=GetFocus()==item->hwndItem;HWND parentWindow=GetParent(item->hwndItem);
    const bool projectDropDown=parentWindow==g_project_dialog&&(item->CtlID==ID_D_TYPE||item->CtlID==ID_D_GROUP);
    const bool projectButton=parentWindow==g_project_dialog;
    COLORREF under=projectButton?RGB(29,31,35):panel;
    if(parentWindow==g_hwnd){POINT pos{};ClientToScreen(item->hwndItem,&pos);ScreenToClient(parentWindow,&pos);under=pos.y<52||pos.x<286?panel:bg;}
    HBRUSH clear=CreateSolidBrush(under);FillRect(item->hDC,&r,clear);DeleteObject(clear);
    wchar_t text[256]{};GetWindowTextW(item->hwndItem,text,256);SetBkMode(item->hDC,TRANSPARENT);
    if(projectDropDown){
        const COLORREF fill=down?RGB(32,38,47):(focused?RGB(37,43,51):(hover?RGB(43,46,52):RGB(38,40,44)));
        HBRUSH brush=CreateSolidBrush(fill);auto oldBrush=SelectObject(item->hDC,brush);auto oldPen=SelectObject(item->hDC,GetStockObject(NULL_PEN));RoundRect(item->hDC,r.left,r.top,r.right,r.bottom,7,7);SelectObject(item->hDC,oldBrush);SelectObject(item->hDC,oldPen);DeleteObject(brush);
        SetTextColor(item->hDC,RGB(235,238,244));SelectObject(item->hDC,g_font);RECT textRect{r.left+10,r.top,r.right-34,r.bottom};DrawTextW(item->hDC,text,-1,&textRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        SetTextColor(item->hDC,focused?RGB(107,181,255):RGB(165,173,185));SelectObject(item->hDC,g_icon_font?g_icon_font:g_font);RECT arrow{r.right-30,r.top,r.right-7,r.bottom};DrawTextW(item->hDC,L"\xE70D",-1,&arrow,DT_CENTER|DT_VCENTER|DT_SINGLELINE);return;
    }
    COLORREF fill=under,stroke=RGB(52,59,70);
    if(projectButton){
        const bool primary=item->CtlID==ID_D_OK;
        if(primary){fill=disabled?RGB(56,83,105):(down?RGB(52,126,177):(hover?RGB(87,174,231):RGB(72,155,211)));stroke=hover?RGB(116,194,244):RGB(84,167,222);}
        else{fill=disabled?RGB(34,36,40):(down?RGB(35,38,44):(hover?RGB(50,53,59):RGB(42,44,49)));stroke=hover?RGB(83,91,103):RGB(57,62,70);}
    }else{
        if(item->CtlID==ID_CLI_TAB||item->CtlID==ID_D_CANCEL||item->CtlID==ID_EDIT_PROJECT)fill=elevated;
        if(item->CtlID==ID_NEW_PROJECT||item->CtlID==ID_D_OK||item->CtlID==ID_START_PROJECT)fill=accent2;
        if(disabled)fill=g_light_theme?RGB(235,238,243):RGB(33,37,45);else if(down)fill=g_light_theme?RGB(205,220,241):RGB(37,91,174);else if(hover)fill=g_light_theme?RGB(229,235,243):RGB(34,40,51);
        if(item->CtlID==ID_CLOSE)fill=down?RGB(157,44,55):(hover?RGB(120,38,47):panel);if(item->CtlID==ID_MINIMIZE)fill=hover?RGB(37,42,51):panel;
        stroke=disabled?(g_light_theme?RGB(218,223,231):RGB(43,48,57)):(hover?(g_light_theme?RGB(164,181,205):RGB(78,104,142)):border);if(item->CtlID==ID_NEW_PROJECT||item->CtlID==ID_D_OK||item->CtlID==ID_START_PROJECT)stroke=hover?RGB(117,177,255):RGB(64,128,225);
    }
    HBRUSH brush=CreateSolidBrush(fill);HPEN pen=CreatePen(PS_SOLID,1,stroke);auto oldBrush=SelectObject(item->hDC,brush);auto oldPen=SelectObject(item->hDC,pen);RoundRect(item->hDC,r.left+1,r.top+1,r.right-1,r.bottom-1,9,9);SelectObject(item->hDC,oldBrush);SelectObject(item->hDC,oldPen);DeleteObject(brush);DeleteObject(pen);
    const bool primary_button=item->CtlID==ID_NEW_PROJECT||item->CtlID==ID_D_OK||item->CtlID==ID_START_PROJECT;SetTextColor(item->hDC,disabled?muted:((primary_button||(!g_light_theme&&hover))?RGB(255,255,255):fg));SelectObject(item->hDC,g_font);DrawTextW(item->hDC,text,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}
void DrawProjectItem(const DRAWITEMSTRUCT* item){
    if(item->itemID==(UINT)-1)return;
    const int project_index=static_cast<int>(SendMessageW(item->hwndItem,LB_GETITEMDATA,item->itemID,0));
    if(project_index<0||project_index>=static_cast<int>(g_projects.size()))return;
    RECT row=item->rcItem;HBRUSH back=CreateSolidBrush(panel);FillRect(item->hDC,&row,back);DeleteObject(back);bool selected=(item->itemState&ODS_SELECTED)!=0;RECT r{row.left+3,row.top+4,row.right-4,row.bottom-4};COLORREF fill=selected?(g_light_theme?RGB(226,237,251):RGB(29,43,63)):(g_light_theme?RGB(250,251,253):RGB(22,25,32));COLORREF stroke=selected?(g_light_theme?RGB(112,158,217):RGB(58,106,171)):(g_light_theme?RGB(222,227,234):RGB(38,43,52));HBRUSH b=CreateSolidBrush(fill);HPEN p=CreatePen(PS_SOLID,1,stroke);auto oldb=SelectObject(item->hDC,b);auto oldp=SelectObject(item->hDC,p);RoundRect(item->hDC,r.left,r.top,r.right,r.bottom,10,10);SelectObject(item->hDC,oldb);SelectObject(item->hDC,oldp);DeleteObject(b);DeleteObject(p);SetBkMode(item->hDC,TRANSPARENT);SelectObject(item->hDC,g_font);SetTextColor(item->hDC,fg);wchar_t name[256]{};SendMessageW(item->hwndItem,LB_GETTEXT,item->itemID,(LPARAM)name);RECT title{r.left+24,r.top+11,r.right-12,r.top+34};DrawTextW(item->hDC,name,-1,&title,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);const auto& project=g_projects[project_index];std::wstring sub=project.status==L"running"?L"运行中":(project.status==L"paused"?L"已暂停":L"就绪");sub+=L"   "+project.type+L"   /   "+project.group;SelectObject(item->hDC,g_font_small?g_font_small:g_font);SetTextColor(item->hDC,muted);RECT subRect{r.left+24,r.top+37,r.right-12,r.bottom-8};DrawTextW(item->hDC,sub.c_str(),-1,&subRect,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);HBRUSH dot=CreateSolidBrush(selected?accent:(project.status==L"running"?RGB(71,187,132):muted));Ellipse(item->hDC,r.left+11,r.top+18,r.left+17,r.top+24);DeleteObject(dot);
}
bool PickFolder(HWND owner,std::wstring& path){IFileDialog* dialog=nullptr;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))||!dialog)return false;DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);if(FAILED(dialog->Show(owner))){dialog->Release();return false;}IShellItem* item=nullptr;bool ok=SUCCEEDED(dialog->GetResult(&item));if(ok){PWSTR value=nullptr;ok=SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&value));if(ok){path=value;CoTaskMemFree(value);}item->Release();}dialog->Release();return ok;}
int SelectedProjectIndex(){
    const LRESULT row=SendMessageW(g_list,LB_GETCURSEL,0,0);
    if(row==LB_ERR||row<0||row>=static_cast<LRESULT>(g_visible_project_indices.size()))return -1;
    return g_visible_project_indices[static_cast<size_t>(row)];
}
void RefreshProjectDetails(){
    const int index=SelectedProjectIndex();
    if(index<0 || index>=(int)g_projects.size()){
        SetText(g_detail_title,L"未选择项目");
        SetText(g_detail_folder,L"文件夹: 未设置");
        SetText(g_detail_command,L"启动命令: 未设置");
        SetText(g_detail_state,L"状态: 未运行");
        return;
    }
    const auto& project=g_projects[index];
    g_last_project_id=project.id;
    g_store.SetSetting(L"last_project_id",project.id);
    SetText(g_detail_title,project.name+L"   /   "+project.type);
    SetText(g_detail_folder,L"文件夹: "+(project.folder.empty()?L"未设置":project.folder));
    SetText(g_detail_command,L"启动命令: "+(project.command.empty()?L"未设置":project.command));
    const wchar_t* state=(g_session_running&&index==g_running_project)?L"运行中":(index==g_paused_project?L"已暂停":L"未运行");
    SetText(g_detail_state,std::wstring(L"状态: ")+state);
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
    if(g_store.GetSetting(L"seeded_projects",L"0")!=L"1"&&g_projects.empty()){
        const auto cwd=std::filesystem::current_path().wstring();
        Project codex{};codex.name=L"默认 Codex 会话";codex.type=L"Codex";codex.group=L"默认分组";codex.folder=cwd;codex.command=DefaultProjectCommand(codex.type);g_store.SaveProject(codex);
        Project claude{};claude.name=L"Claude 工作区";claude.type=L"Claude";claude.group=L"默认分组";claude.folder=cwd;claude.command=DefaultProjectCommand(claude.type);g_store.SaveProject(claude);
        g_store.SetSetting(L"seeded_projects",L"1");g_projects=g_store.LoadProjects();
    }
    g_group_name=g_store.GetSetting(L"last_group",L"默认分组");
    if(std::none_of(g_groups.begin(),g_groups.end(),[&](const ProjectGroup& group){return group.name==g_group_name;}))g_group_name=L"默认分组";
    g_last_project_id=g_store.GetSetting(L"last_project_id");
    g_startup_project_id=g_last_project_id;
    ApplyStoredTheme();
    return true;
}
void RefreshProjectList(){
    const int selected=SelectedProjectIndex();
    const std::wstring preferred_id=(selected>=0&&selected<(int)g_projects.size())?g_projects[selected].id:g_last_project_id;
    SendMessageW(g_list,LB_RESETCONTENT,0,0);
    g_visible_project_indices.clear();
    const std::wstring query=aicli::ui::GetStyledInputText(g_search);
    for(size_t i=0;i<g_projects.size();++i){
        const auto& project=g_projects[i];
        const std::wstring group=project.group.empty()?L"默认分组":project.group;
        if(group!=g_group_name)continue;
        if(!query.empty()&&project.name.find(query)==std::wstring::npos&&project.type.find(query)==std::wstring::npos)continue;
        const LRESULT row=SendMessageW(g_list,LB_ADDSTRING,0,(LPARAM)project.name.c_str());
        if(row!=LB_ERR){SendMessageW(g_list,LB_SETITEMDATA,row,static_cast<LPARAM>(i));g_visible_project_indices.push_back(static_cast<int>(i));}
    }
    int row=-1;for(size_t i=0;i<g_visible_project_indices.size();++i){const int project_index=g_visible_project_indices[i];if((!preferred_id.empty()&&project_index>=0&&project_index<(int)g_projects.size()&&g_projects[project_index].id==preferred_id)||project_index==selected){row=static_cast<int>(i);break;}}
    if(row<0&&!g_visible_project_indices.empty())row=0;
    SendMessageW(g_list,LB_SETCURSEL,row,0);
    RefreshProjectDetails();
    RefreshConversationsCache();
}
void Layout(){
    if(!g_hwnd || IsIconic(g_hwnd))return;
    RECT r{}; if(!GetClientRect(g_hwnd,&r))return;
    constexpr int top=kCustomTitleBarHeight, left=286, bottom=30;
    if(r.right<=left+80 || r.bottom<=top+bottom+120)return;
    MoveWindow(g_group,168,10,106,32,TRUE);
    MoveWindow(g_group_add,278,10,34,32,TRUE);
    const int captionReserve=MulDiv(144,(int)GetDpiForWindow(g_hwnd),96);const int settingsX=std::max(860,(int)r.right-captionReserve-82);
    MoveWindow(g_bind,std::max(700,settingsX-220),10,96,32,TRUE);
    MoveWindow(g_devices,std::max(700,settingsX-116),10,106,32,TRUE);
    MoveWindow(g_settings,settingsX,10,72,32,TRUE);

    RECT search_rect{14,top+16,left-14,top+54};aicli::ui::LayoutStyledInput(g_search,search_rect,2,9);
    MoveWindow(g_sidebar_title,18,top+70,200,24,TRUE);
    MoveWindow(g_new_project,left-50,top+64,36,34,TRUE);
    SetWindowPos(g_new_project,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    MoveWindow(g_list,12,top+104,left-24,r.bottom-top-bottom-112,TRUE);

    MoveWindow(g_cli_tab,left+16,top+12,96,34,TRUE);
    MoveWindow(g_terminal_title,left+132,top+19,r.right-left-150,22,TRUE);
    RECT terminal{left+16,top+58,r.right-14,r.bottom-bottom-10};
    const bool selectedRunning=g_session_running&&SelectedProjectIndex()==g_running_project;
    const bool showingTerminal=selectedRunning;
    const bool showingNative=showingTerminal&&g_island.Active();
    ShowWindow(g_detail_title,showingTerminal?SW_HIDE:SW_SHOW);
    ShowWindow(g_detail_folder,showingTerminal?SW_HIDE:SW_SHOW);
    ShowWindow(g_detail_command,showingTerminal?SW_HIDE:SW_SHOW);
    ShowWindow(g_detail_state,showingTerminal?SW_HIDE:SW_SHOW);
    ShowWindow(g_start_project,showingTerminal?SW_HIDE:SW_SHOW);
    ShowWindow(g_edit_project,showingTerminal?SW_HIDE:SW_SHOW);
    MoveWindow(g_detail_title,terminal.left+28,terminal.top+28,terminal.right-terminal.left-56,32,TRUE);
    MoveWindow(g_detail_folder,terminal.left+28,terminal.top+100,terminal.right-terminal.left-56,30,TRUE);
    MoveWindow(g_detail_command,terminal.left+28,terminal.top+172,terminal.right-terminal.left-56,30,TRUE);
    MoveWindow(g_detail_state,terminal.left+28,terminal.top+244,terminal.right-terminal.left-56,30,TRUE);
    MoveWindow(g_start_project,terminal.left+28,terminal.bottom-66,112,36,TRUE);
    MoveWindow(g_edit_project,terminal.left+154,terminal.bottom-66,112,36,TRUE);
    // The central surface is always the real Windows Terminal control.  Do
    // not create a second EDIT-based command line or a text-output fallback:
    // keyboard focus, shortcuts, selection and scrolling must stay native.
    MoveWindow(g_status,14,r.bottom-bottom+6,r.right-28,20,TRUE);
    if(g_island.Window()){
        if(showingNative)g_island.Resize(terminal);
        const bool management_open=FindWindowExW(g_hwnd,nullptr,L"AicliManagementDialog",nullptr)!=nullptr;
        g_island.SetVisible(!g_project_dialog&&!g_binding_dialog&&!management_open&&showingNative);
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
    const bool deletingRunning=g_session_running&&g_running_project==index;
    std::wstring message=L"确定从 AI CLI 中删除项目“"+project.name+L"”吗？\n\n";
    if(deletingRunning)message+=L"该项目正在运行，删除后会同时结束当前终端会话。\n\n";
    message+=L"只会删除项目配置，不会删除项目文件夹或其中的任何文件。";
    if(MessageBoxW(g_hwnd,message.c_str(),L"删除项目",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
    if(deletingRunning){
        g_island.Stop();
        g_session_running=false;
        g_running_project=-1;
    }else if(g_running_project>index){
        --g_running_project;
    }
    if(g_paused_project==index)g_paused_project=-1;
    else if(g_paused_project>index)--g_paused_project;
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
    if(!entry->label[0]){HBRUSH background=CreateSolidBrush(RGB(34,36,40));FillRect(item->hDC,&r,background);DeleteObject(background);HPEN line=CreatePen(PS_SOLID,1,RGB(57,60,67));auto old=(HPEN)SelectObject(item->hDC,line);const int y=(r.top+r.bottom)/2;MoveToEx(item->hDC,r.left+8,y,nullptr);LineTo(item->hDC,r.right-8,y);SelectObject(item->hDC,old);DeleteObject(line);return;}
    const bool selected=(item->itemState&ODS_SELECTED)!=0;
    const bool disabled=(item->itemState&ODS_DISABLED)!=0;
    HBRUSH fill=CreateSolidBrush(selected?RGB(55,62,73):RGB(34,36,40));FillRect(item->hDC,&r,fill);DeleteObject(fill);
    SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,disabled?RGB(111,116,126):RGB(222,226,233));
    SelectObject(item->hDC,g_icon_font?g_icon_font:g_font);RECT icon{r.left+13,r.top+1,r.left+39,r.bottom-1};DrawTextW(item->hDC,entry->icon,-1,&icon,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(item->hDC,g_font);RECT text{r.left+49,r.top+1,r.right-34,r.bottom-1};DrawTextW(item->hDC,entry->label,-1,&text,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    if(entry->submenu){SelectObject(item->hDC,g_font_small);RECT arrow{r.right-28,r.top+1,r.right-10,r.bottom-1};DrawTextW(item->hDC,L">",-1,&arrow,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
}
void ConfigureContextMenu(HMENU menu,HBRUSH background){MENUINFO info{sizeof(info)};info.fMask=MIM_BACKGROUND;info.hbrBack=background;SetMenuInfo(menu,&info);}
void AppendContextEntry(HMENU menu,UINT flags,UINT_PTR id,const ContextMenuEntry& entry){AppendMenuW(menu,flags|MF_OWNERDRAW,id,reinterpret_cast<LPCWSTR>(&entry));}
void OpenProjectFolder(){const int index=SelectedProjectIndex();if(index<0||index>=(int)g_projects.size())return;const auto& folder=g_projects[index].folder;if(folder.empty()){MessageBoxW(g_hwnd,L"当前项目还没有设置项目文件夹。",L"打开项目文件夹",MB_OK|MB_ICONINFORMATION);return;}if((INT_PTR)ShellExecuteW(g_hwnd,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL)<=32)MessageBoxW(g_hwnd,L"无法打开项目文件夹。",L"打开项目文件夹",MB_OK|MB_ICONWARNING);}
void MoveSelectedProjectToGroup(const std::wstring& group){const int index=SelectedProjectIndex();if(index<0||index>=(int)g_projects.size())return;g_projects[index].group=group;g_store.SaveProject(g_projects[index]);g_group_name=group;g_store.SetSetting(L"last_group",group);RefreshProjectList();SetText(g_status,L"项目已移动到 "+group);Layout();}
void MoveSelectedProjectToDefaultGroup(){MoveSelectedProjectToGroup(L"默认分组");}
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
    const bool running=g_session_running&&index==g_running_project;
    const bool paused=index==g_paused_project;
    HMENU menu=CreatePopupMenu(),groups=CreatePopupMenu();
    if(!menu||!groups){if(menu)DestroyMenu(menu);if(groups)DestroyMenu(groups);return;}
    HBRUSH menuBrush=CreateSolidBrush(RGB(34,36,40));ConfigureContextMenu(menu,menuBrush);ConfigureContextMenu(groups,menuBrush);
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
    // Start() owns the current ConPTY through the TerminalControl bridge.
    // Stop the previous native terminal before creating the next one.
    g_island.Stop();
    if(g_island.Start(cmd,cwd,title,g_hwnd,g_status,[project_id](std::wstring_view output){RecordTerminalOutput(project_id,output);})){
        g_session_running=true;
        SetText(g_status,L"原生终端已连接    |    "+sessionAgent+L"    |    Agent 在线");
        Layout();
        return true;
    }
    g_session_running=false;
    const std::wstring error=g_island.LastError().empty()?L"无法创建原生终端控件":g_island.LastError();
    SetText(g_detail_state,L"状态: 启动失败");
    SetText(g_status,L"原生终端启动失败: "+error);
    Layout(); return false;
}
void StartSession(){const int i=SelectedProjectIndex();if(i<0||i>=(int)g_projects.size())return;const auto& project=g_projects[i];const std::wstring sessionAgent=project.type;const std::wstring cmd=project.command.empty()?DefaultProjectCommand(sessionAgent):project.command;if(StartCommandSession(cmd,project.folder,project.name,sessionAgent,project.id)){g_running_project=i;g_paused_project=-1;g_projects[i].status=L"running";g_store.UpdateProjectStatus(project.id,L"running");RecordAndBroadcastEvent(project.id,L"conversation.running",nlohmann::json{{"status","running"}});RefreshProjectDetails();RefreshProjectList();Layout();}}
int ProjectIndexById(const std::wstring& project_id){for(size_t i=0;i<g_projects.size();++i)if(g_projects[i].id==project_id)return static_cast<int>(i);return -1;}
bool SelectProjectById(const std::wstring& project_id){const int index=ProjectIndexById(project_id);if(index<0)return false;g_group_name=g_projects[index].group.empty()?L"默认分组":g_projects[index].group;SetText(g_group,g_group_name);g_store.SetSetting(L"last_group",g_group_name);RefreshProjectList();for(size_t row=0;row<g_visible_project_indices.size();++row)if(g_visible_project_indices[row]==index){SendMessageW(g_list,LB_SETCURSEL,row,0);RefreshProjectDetails();Layout();return true;}return false;}
void RefreshConversationsCache(){nlohmann::json items=nlohmann::json::array();for(const auto& project:g_projects){std::string provider=Utf8Text(project.type);std::transform(provider.begin(),provider.end(),provider.begin(),[](unsigned char ch){return static_cast<char>(std::tolower(ch));});items.push_back({{"id",Utf8Text(project.id)},{"name",Utf8Text(project.name)},{"provider",provider},{"type",Utf8Text(project.type)},{"group",Utf8Text(project.group)},{"folder",Utf8Text(project.folder)},{"command",Utf8Text(project.command)},{"status",Utf8Text(project.status)}});}std::scoped_lock lock(g_conversations_mutex);g_conversations_cache=nlohmann::json{{"items",items}}.dump();}
std::string ConversationsJson(){std::scoped_lock lock(g_conversations_mutex);return g_conversations_cache.empty()?"{\"items\":[]}":g_conversations_cache;}
std::string EventsJson(const std::wstring& project_id,std::int64_t after){nlohmann::json items=nlohmann::json::array();for(const auto& event:g_store.LoadEvents(project_id,after,1000)){nlohmann::json payload;try{payload=nlohmann::json::parse(event.payload_json);}catch(...){payload={{"text",event.payload_json}};}items.push_back({{"id",event.id},{"eventId",event.id},{"conversationId",Utf8Text(event.project_id)},{"event_type",Utf8Text(event.event_type)},{"type",Utf8Text(event.event_type)},{"payload",payload},{"createdAt",event.created_at}});}return nlohmann::json{{"items",items}}.dump();}
void UpdateDeviceHeader(){if(!g_devices)return;const auto count=g_store.LoadDevices().size();SetText(g_devices,L"已绑定 "+std::to_wstring(count)+L" 台");InvalidateRect(g_hwnd,nullptr,FALSE);}
void ConfigureRemoteAgent(){
    g_remote_agent.SetConversationsCallback([]{return ConversationsJson();});
    g_remote_agent.SetEventsCallback([](const std::wstring& project,std::int64_t after){return EventsJson(project,after);});
    g_remote_agent.SetInputCallback([](const std::wstring& project,const std::wstring& phone,const std::wstring& input,std::wstring* error){auto* message=new(std::nothrow) RemoteInputMessage{project,phone,input};if(!message||!g_hwnd||!PostMessageW(g_hwnd,WM_REMOTE_INPUT,0,reinterpret_cast<LPARAM>(message))){delete message;if(error)*error=L"桌面窗口未就绪";return false;}return true;});
    g_remote_agent.SetControlCallback([](const std::wstring& project,const std::wstring& phone,const std::wstring& action,std::wstring* error){auto* message=new(std::nothrow) RemoteControlMessage{project,phone,action};if(!message||!g_hwnd||!PostMessageW(g_hwnd,WM_REMOTE_CONTROL,0,reinterpret_cast<LPARAM>(message))){delete message;if(error)*error=L"桌面窗口未就绪";return false;}return true;});
    g_remote_agent.SetPairingCallback([](const aicli::agent::PendingPairing& pairing){auto* message=new(std::nothrow) aicli::agent::PendingPairing(pairing);if(!message||!g_hwnd||!PostMessageW(g_hwnd,WM_PAIRING_PENDING,0,reinterpret_cast<LPARAM>(message)))delete message;});
    g_remote_agent.SetStatusCallback([](bool online,const std::wstring& text){auto* message=new(std::nothrow) AgentStatusMessage{online,text};if(!message||!g_hwnd||!PostMessageW(g_hwnd,WM_AGENT_STATUS,0,reinterpret_cast<LPARAM>(message)))delete message;});
    aicli::agent::AgentConfig config;config.bind_address=g_store.GetSetting(L"listen_address",L"0.0.0.0");config.port=g_store.GetSetting(L"listen_port",L"4280");config.local_address=g_store.GetSetting(L"local_address");config.remote_address=g_store.GetSetting(L"remote_address");config.data_directory=aicli::DefaultDataDirectory();std::wstring error;if(!g_remote_agent.Start(&g_store,config,&error))SetText(g_status,L"Agent 启动失败: "+error);else SetText(g_status,L"就绪    |    HTTPS/WSS Agent 在线");UpdateDeviceHeader();
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
    if (g_status) SetText(g_status, L"HTTPS/WSS Agent 在线");
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
    if (std::none_of(g_groups.begin(), g_groups.end(), [&](const ProjectGroup& value) {
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
        });
    if (result.accepted) {
        ApplyStoredTheme();
        BOOL dark = g_light_theme ? FALSE : TRUE;
        DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        COLORREF caption_color = panel;
        COLORREF border_color = border;
        DwmSetWindowAttribute(g_hwnd, DWMWA_CAPTION_COLOR, &caption_color, sizeof(caption_color));
        DwmSetWindowAttribute(g_hwnd, DWMWA_BORDER_COLOR, &border_color, sizeof(border_color));
        InvalidateRect(g_hwnd, nullptr, TRUE);
        Layout();
    }
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
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

void ExtendCustomTitleBar(HWND h){MARGINS margins{0,0,kCustomTitleBarHeight,0};DwmExtendFrameIntoClientArea(h,&margins);}
LRESULT HitTestCustomFrame(HWND h,LPARAM l){
    POINT screen{GET_X_LPARAM(l),GET_Y_LPARAM(l)};RECT window{};GetWindowRect(h,&window);const UINT dpi=GetDpiForWindow(h);const int frame=GetSystemMetricsForDpi(SM_CXSIZEFRAME,dpi)+GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi);
    if(!IsZoomed(h)){
        const bool left=screen.x>=window.left&&screen.x<window.left+frame,right=screen.x<window.right&&screen.x>=window.right-frame,top=screen.y>=window.top&&screen.y<window.top+frame,bottom=screen.y<window.bottom&&screen.y>=window.bottom-frame;
        if(top&&left)return HTTOPLEFT;if(top&&right)return HTTOPRIGHT;if(bottom&&left)return HTBOTTOMLEFT;if(bottom&&right)return HTBOTTOMRIGHT;if(left)return HTLEFT;if(right)return HTRIGHT;if(top)return HTTOP;if(bottom)return HTBOTTOM;
    }
    POINT client=screen;ScreenToClient(h,&client);
    if(client.y>=0&&client.y<kCustomTitleBarHeight){HWND child=ChildWindowFromPointEx(h,client,CWP_SKIPINVISIBLE|CWP_SKIPDISABLED|CWP_SKIPTRANSPARENT);if(child&&child!=h)return HTCLIENT;return HTCAPTION;}
    return HTCLIENT;
}
LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    LRESULT dwmResult=0;if(m!=WM_NCCALCSIZE&&DwmDefWindowProc(h,m,w,l,&dwmResult))return dwmResult;
    switch(m){
    case WM_CREATE:{g_hwnd=h; g_font=CreateFontW(-15,0,0,0,400,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");g_font_medium=CreateFontW(-17,0,0,0,600,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");g_font_small=CreateFontW(-13,0,0,0,400,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");g_font_modal_title=CreateFontW(-21,0,0,0,600,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");g_icon_font=CreateFontW(-16,0,0,0,400,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe MDL2 Assets");
        g_group=CreateWindowW(L"BUTTON",g_group_name.c_str(),WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,168,8,106,32,h,(HMENU)(INT_PTR)ID_GROUP_DEFAULT,GetModuleHandleW(nullptr),nullptr);g_group_add=CreateWindowW(L"BUTTON",L"+",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,278,8,34,32,h,(HMENU)(INT_PTR)ID_GROUP_ADD,GetModuleHandleW(nullptr),nullptr);g_bind=CreateWindowW(L"BUTTON",L"绑定手机",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,365,8,100,32,h,(HMENU)(INT_PTR)ID_BIND_PHONE,GetModuleHandleW(nullptr),nullptr);g_devices=CreateWindowW(L"BUTTON",L"已绑定 0 台",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,470,8,112,32,h,(HMENU)(INT_PTR)ID_DEVICES,GetModuleHandleW(nullptr),nullptr);g_new_project=CreateWindowW(L"BUTTON",L"+",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,8,55,34,30,h,(HMENU)(INT_PTR)ID_NEW_PROJECT,GetModuleHandleW(nullptr),nullptr);g_settings=CreateWindowW(L"BUTTON",L"设置",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,1000,8,70,32,h,(HMENU)(INT_PTR)ID_SETTINGS,GetModuleHandleW(nullptr),nullptr);g_cli_tab=CreateWindowW(L"BUTTON",L"AI 终端",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,302,72,100,30,h,(HMENU)(INT_PTR)ID_CLI_TAB,GetModuleHandleW(nullptr),nullptr);HWND controls[]={g_group,g_group_add,g_bind,g_devices,g_new_project,g_settings,g_cli_tab};for(HWND b:controls){SendMessageW(b,WM_SETFONT,(WPARAM)g_font,TRUE);EnableButtonHover(b);}
        g_sidebar_title=CreateWindowW(L"STATIC",L"项目与会话",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_sidebar_title,WM_SETFONT,(WPARAM)g_font_medium,TRUE);g_search=aicli::ui::CreateStyledInput(h,ID_SEARCH_PROJECTS,g_font,L"搜索会话");
        g_list=CreateWindowW(L"LISTBOX",nullptr,WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,0,0,0,0,h,(HMENU)(INT_PTR)ID_PROJECT_LIST,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_list,WM_SETFONT,(WPARAM)g_font,TRUE);RefreshProjectList();
        g_detail_title=CreateWindowW(L"STATIC",L"选择一个项目",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        g_detail_folder=CreateWindowW(L"STATIC",L"文件夹: 未设置",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        g_detail_command=CreateWindowW(L"STATIC",L"启动命令: 未设置",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        g_detail_state=CreateWindowW(L"STATIC",L"状态: 未运行",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        g_start_project=CreateWindowW(L"BUTTON",L"立即开始",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,0,0,0,0,h,(HMENU)(INT_PTR)ID_START_PROJECT,GetModuleHandleW(nullptr),nullptr);
        g_edit_project=CreateWindowW(L"BUTTON",L"编辑项目",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,0,0,0,0,h,(HMENU)(INT_PTR)ID_EDIT_PROJECT,GetModuleHandleW(nullptr),nullptr);
        HWND detailControls[]={g_detail_title,g_detail_folder,g_detail_command,g_detail_state,g_start_project,g_edit_project};for(HWND c:detailControls){SendMessageW(c,WM_SETFONT,(WPARAM)g_font,TRUE);if(c==g_start_project||c==g_edit_project)EnableButtonHover(c);}
        g_terminal_title=CreateWindowW(L"STATIC",L"选择一个项目开始",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_terminal_title,WM_SETFONT,(WPARAM)g_font,TRUE);g_status=CreateWindowW(L"STATIC",L"就绪    |    选择项目后查看启动信息",WS_CHILD|WS_VISIBLE,0,0,0,0,h,(HMENU)(INT_PTR)ID_SETTINGS,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_status,WM_SETFONT,(WPARAM)g_font_small,TRUE);
        g_island.Attach(h,RECT{250,60,500,400});RefreshProjectDetails();ExtendCustomTitleBar(h);SetWindowPos(h,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);Layout();ConfigureRemoteAgent();SetTimer(h,1,2000,nullptr);PostMessageW(h,WM_RESTORE_LAST_SESSION,0,0);return 0;}
    case WM_ACTIVATE:ExtendCustomTitleBar(h);break;
    case WM_DWMCOMPOSITIONCHANGED:ExtendCustomTitleBar(h);return 0;
    case WM_NCCALCSIZE:if(w){auto* params=(NCCALCSIZE_PARAMS*)l;if(IsZoomed(h)){HMONITOR monitor=MonitorFromRect(&params->rgrc[0],MONITOR_DEFAULTTONEAREST);MONITORINFO info{sizeof(info)};if(GetMonitorInfoW(monitor,&info))params->rgrc[0]=info.rcWork;}return 0;}break;
    case WM_NCHITTEST:return HitTestCustomFrame(h,l);
    case WM_MEASUREITEM:{auto* item=(MEASUREITEMSTRUCT*)l;if(item->CtlType==ODT_MENU){const auto* entry=reinterpret_cast<const ContextMenuEntry*>(item->itemData);item->itemWidth=218;item->itemHeight=entry&&!entry->label[0]?10:38;return TRUE;}if(item->CtlID==ID_PROJECT_LIST){item->itemHeight=72;return TRUE;}}break;
    case WM_DRAWITEM:{auto* item=(DRAWITEMSTRUCT*)l;if(item->CtlType==ODT_MENU){DrawContextMenuItem(item);return TRUE;}if(item->CtlType==ODT_BUTTON){DrawModernButton(item);return TRUE;}if(item->CtlID==ID_PROJECT_LIST){DrawProjectItem(item);return TRUE;}}break;
    case WM_SIZE: if(w==SIZE_MINIMIZED){g_island.SetVisible(false);return 0;}if(g_project_dialog||g_binding_dialog||FindWindowExW(h,nullptr,L"AicliManagementDialog",nullptr)){RECT client{};GetClientRect(h,&client);if(g_project_dialog)MoveWindow(g_project_dialog,0,0,client.right,client.bottom,TRUE);if(g_binding_dialog)MoveWindow(g_binding_dialog,0,0,client.right,client.bottom,TRUE);if(HWND management=FindWindowExW(h,nullptr,L"AicliManagementDialog",nullptr))MoveWindow(management,0,0,client.right,client.bottom,TRUE);}Layout();return 0;
    case WM_TIMER:if(w==1){RECT header{0,0,900,kCustomTitleBarHeight};InvalidateRect(h,&header,FALSE);return 0;}break;
    case WM_GETMINMAXINFO:{auto* info=(MINMAXINFO*)l;info->ptMinTrackSize.x=900;info->ptMinTrackSize.y=600;return 0;}
    case WM_MOUSEWHEEL: if(g_island.HandleMouseWheel(w,l))return 0; break;
    case WM_MOUSEHWHEEL: if(g_island.HandleMouseWheel(w,l,true))return 0; break;
    case WM_ERASEBKGND: return 1;
    case WM_CONTEXTMENU: if((HWND)w==g_list){ShowProjectContextMenu(h,l);return 0;} break;
    case WM_CTLCOLORSTATIC: {HDC dc=(HDC)w;HWND control=(HWND)l;const bool onPanel=control==g_sidebar_title||control==g_status;SetBkMode(dc,OPAQUE);SetTextColor(dc,fg);SetBkColor(dc,onPanel?panel:bg);return (LRESULT)(onPanel?g_list_brush:g_bg_brush);}
    case WM_CTLCOLOREDIT: {HDC dc=(HDC)w;SetTextColor(dc,fg);SetBkColor(dc,elevated);return (LRESULT)(g_edit_brush?g_edit_brush:GetStockObject(NULL_BRUSH));}
    case WM_CTLCOLORLISTBOX: {HDC dc=(HDC)w;SetTextColor(dc,fg);SetBkColor(dc,panel);return (LRESULT)(g_list_brush?g_list_brush:GetStockObject(NULL_BRUSH));}
    case WM_COMMAND: if(LOWORD(w)==ID_CLOSE){DestroyWindow(h);return 0;}if(LOWORD(w)==ID_MINIMIZE){ShowWindow(h,SW_MINIMIZE);return 0;}if(LOWORD(w)==ID_SEARCH_PROJECTS&&HIWORD(w)==EN_CHANGE){RefreshProjectList();Layout();return 0;}if(LOWORD(w)==ID_PROJECT_LIST&&HIWORD(w)==LBN_SELCHANGE){RefreshProjectDetails();Layout();return 0;}if(LOWORD(w)==ID_PROJECT_LIST&&HIWORD(w)==LBN_DBLCLK){StartSession();return 0;} if(LOWORD(w)==ID_NEW_PROJECT){CreateProject();return 0;} if(LOWORD(w)==ID_START_PROJECT){StartSession();return 0;} if(LOWORD(w)==ID_EDIT_PROJECT){EditProject();return 0;} if(LOWORD(w)==ID_CTX_OPEN_FOLDER){OpenProjectFolder();return 0;} if(LOWORD(w)>=ID_CTX_MOVE_GROUP_BASE&&LOWORD(w)<ID_CTX_MOVE_GROUP_BASE+static_cast<int>(g_groups.size())){MoveSelectedProjectToGroup(g_groups[LOWORD(w)-ID_CTX_MOVE_GROUP_BASE].name);return 0;} if(LOWORD(w)==ID_CTX_MOVE_DEFAULT){MoveSelectedProjectToDefaultGroup();return 0;} if(LOWORD(w)==ID_CTX_RESTART){StartSession();return 0;} if(LOWORD(w)==ID_CTX_CANCEL||LOWORD(w)==ID_CTX_PAUSE){const int selected=SelectedProjectIndex();const bool pausing=LOWORD(w)==ID_CTX_PAUSE;g_island.Stop();g_session_running=false;g_running_project=-1;if(selected>=0&&selected<(int)g_projects.size()){g_projects[selected].status=pausing?L"paused":L"cancelled";g_store.UpdateProjectStatus(g_projects[selected].id,g_projects[selected].status);}g_paused_project=pausing?selected:-1;SetText(g_status,pausing?L"会话已暂停":L"会话已取消");RefreshProjectDetails();RefreshProjectList();Layout();return 0;} if(LOWORD(w)==ID_CTX_CONTINUE){StartSession();return 0;} if(LOWORD(w)==ID_CTX_EDIT){EditProject();return 0;} if(LOWORD(w)==ID_CTX_DELETE){DeleteProject();return 0;} if(LOWORD(w)==ID_BIND_PHONE){ShowBindingDialog(h);return 0;} if(LOWORD(w)==ID_DEVICES){g_island.SetVisible(false);OpenDevicesDialog();Layout();return 0;} if(LOWORD(w)==ID_SETTINGS){g_island.SetVisible(false);OpenSettingsDialog();ReloadPersistentModels();return 0;} if(LOWORD(w)==ID_GROUP_DEFAULT||LOWORD(w)==ID_GROUP_ADD){g_island.SetVisible(false);OpenGroupsDialog();return 0;} if(LOWORD(w)==ID_CLI_TAB){RefreshProjectDetails();Layout();return 0;} return 0;
    case WM_RESTORE_LAST_SESSION:if(!g_startup_project_id.empty()&&SelectProjectById(g_startup_project_id)){g_startup_project_id.clear();StartSession();}return 0;
    case WM_REMOTE_INPUT:{std::unique_ptr<RemoteInputMessage> message(reinterpret_cast<RemoteInputMessage*>(l));if(!message)return 0;if(!SelectProjectById(message->project_id))return 0;if(!g_session_running||g_running_project!=SelectedProjectIndex())StartSession();if(g_session_running&&g_running_project==SelectedProjectIndex()&&g_island.WriteInput(message->input)){RecordAndBroadcastEvent(message->project_id,L"terminal.input",nlohmann::json{{"input",Utf8Text(message->input)},{"phoneId",Utf8Text(message->phone_id)}});}return 0;}
    case WM_REMOTE_CONTROL:{std::unique_ptr<RemoteControlMessage> message(reinterpret_cast<RemoteControlMessage*>(l));if(!message||!SelectProjectById(message->project_id))return 0;const int selected=SelectedProjectIndex();if(message->action==L"restart"||message->action==L"continue"||message->action==L"start"){StartSession();}else if(message->action==L"pause"||message->action==L"cancel"){const bool pause=message->action==L"pause";g_island.Stop();g_session_running=false;g_running_project=-1;g_paused_project=pause?selected:-1;if(selected>=0&&selected<(int)g_projects.size()){g_projects[selected].status=pause?L"paused":L"cancelled";g_store.UpdateProjectStatus(g_projects[selected].id,g_projects[selected].status);RecordAndBroadcastEvent(g_projects[selected].id,pause?L"conversation.paused":L"conversation.cancelled",nlohmann::json{{"status",pause?"paused":"cancelled"}});}RefreshProjectList();RefreshProjectDetails();Layout();}else if(message->action==L"ctrl_c"){g_island.WriteInput(std::wstring(1,L'\x03'));}return 0;}
    case WM_PAIRING_PENDING:{std::unique_ptr<aicli::agent::PendingPairing> pairing(reinterpret_cast<aicli::agent::PendingPairing*>(l));if(!pairing)return 0;g_pending_pairing=*pairing;if(g_binding_dialog){InvalidateRect(g_binding_dialog,nullptr,FALSE);}else if(pairing->submitted&&!pairing->authorized&&!pairing->rejected){const std::wstring text=L"手机名称："+pairing->device.name+L"\n型号："+pairing->device.model+L"\n设备 ID："+pairing->device.phone_id+L"\n\n确认后该手机可远程操作管理员终端。";const bool approve=MessageBoxW(h,text.c_str(),L"确认绑定手机",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES;std::wstring error;if(!g_remote_agent.ConfirmPairing(pairing->info.pairing_id,approve,&error)&&!error.empty())MessageBoxW(h,error.c_str(),L"手机绑定",MB_OK|MB_ICONERROR);}UpdateDeviceHeader();return 0;}
    case WM_AGENT_STATUS:{std::unique_ptr<AgentStatusMessage> message(reinterpret_cast<AgentStatusMessage*>(l));if(message)SetText(g_status,message->text);InvalidateRect(h,nullptr,FALSE);return 0;}
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);HBRUSH b=CreateSolidBrush(bg);FillRect(dc,&r,b);DeleteObject(b);HBRUSH bar=CreateSolidBrush(panel);RECT top{0,0,r.right,52};FillRect(dc,&top,bar);RECT side{0,52,286,r.bottom-30};FillRect(dc,&side,bar);DeleteObject(bar);HPEN pen=CreatePen(PS_SOLID,1,border);auto old=(HPEN)SelectObject(dc,pen);MoveToEx(dc,285,52,nullptr);LineTo(dc,285,r.bottom-30);MoveToEx(dc,0,51,nullptr);LineTo(dc,r.right,51);MoveToEx(dc,0,r.bottom-30,nullptr);LineTo(dc,r.right,r.bottom-30);SelectObject(dc,old);DeleteObject(pen);const COLORREF searchBorder=GetFocus()==g_search?accent:border;DrawRoundedOutline(dc,RECT{14,68,272,108},searchBorder,10);RECT searchLine{22,105,264,GetFocus()==g_search?107:106};HBRUSH searchBrush=CreateSolidBrush(GetFocus()==g_search?accent:muted);FillRect(dc,&searchLine,searchBrush);DeleteObject(searchBrush);DrawRoundedOutline(dc,RECT{302,110,r.right-14,r.bottom-40},border,10);HBRUSH logo=CreateSolidBrush(accent2);HPEN logoPen=CreatePen(PS_SOLID,1,RGB(75,143,244));auto oldLogoB=SelectObject(dc,logo);auto oldLogoP=SelectObject(dc,logoPen);RoundRect(dc,14,10,46,42,9,9);SelectObject(dc,oldLogoB);SelectObject(dc,oldLogoP);DeleteObject(logo);DeleteObject(logoPen);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));SelectObject(dc,g_font_medium);TextOutW(dc,22,16,L">_",2);SetTextColor(dc,fg);TextOutW(dc,58,11,L"AI CLI",6);SelectObject(dc,g_font_small);SetTextColor(dc,g_remote_agent.Online()?RGB(83,203,146):RGB(225,101,112));const std::wstring agent_label=g_remote_agent.Online()?L"●  Agent 在线":L"○  Agent 离线";TextOutW(dc,334,20,agent_label.c_str(),static_cast<int>(agent_label.size()));SetTextColor(dc,muted);const std::wstring memory_label=L"内存 "+std::to_wstring(WorkingSetMegabytes())+L" MB";TextOutW(dc,448,20,memory_label.c_str(),static_cast<int>(memory_label.size()));const bool relay_configured=!g_store.GetSetting(L"remote_address").empty();const std::wstring relay_label=relay_configured?L"公网已配置":L"公网未配置";TextOutW(dc,558,20,relay_label.c_str(),static_cast<int>(relay_label.size()));EndPaint(h,&ps);return 0;}
    case WM_DESTROY:g_remote_agent.Stop();g_island.Stop();ReleaseProjectSnapshot();if(g_font)DeleteObject(g_font);if(g_font_medium)DeleteObject(g_font_medium);if(g_font_small)DeleteObject(g_font_small);if(g_font_modal_title)DeleteObject(g_font_modal_title);if(g_icon_font)DeleteObject(g_icon_font);if(g_edit_brush){DeleteObject(g_edit_brush);g_edit_brush=nullptr;}if(g_list_brush){DeleteObject(g_list_brush);g_list_brush=nullptr;}if(g_bg_brush){DeleteObject(g_bg_brush);g_bg_brush=nullptr;}PostQuitMessage(0);return 0; }
    return DefWindowProcW(h,m,w,l);
}
}

int WINAPI wWinMain(HINSTANCE inst,HINSTANCE, PWSTR, int show){winrt::init_apartment(winrt::apartment_type::single_threaded);const int activation=EnsurePackagedActivation();if(activation<0){MessageBoxW(nullptr,L"无法启动已注册的 AI CLI 桌面实例，请先完成包注册。",L"AI CLI",MB_OK|MB_ICONERROR);return 1;}if(activation==0)return 0;if(!InitializePersistentState()){winrt::uninit_apartment();return 1;}g_components.Initialize();INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_STANDARD_CLASSES};InitCommonControlsEx(&ic);WNDCLASSW wc{};wc.hInstance=inst;wc.lpfnWndProc=WndProc;wc.lpszClassName=kClassName;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&wc);constexpr DWORD windowStyle=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_CLIPCHILDREN;g_hwnd=CreateWindowExW(0,kClassName,L"\x200B",windowStyle,CW_USEDEFAULT,CW_USEDEFAULT,1280,800,nullptr,nullptr,inst,nullptr);BOOL dark=g_light_theme?FALSE:TRUE;DwmSetWindowAttribute(g_hwnd,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));COLORREF captionColor=panel,borderColor=border;DwmSetWindowAttribute(g_hwnd,DWMWA_CAPTION_COLOR,&captionColor,sizeof(captionColor));DwmSetWindowAttribute(g_hwnd,DWMWA_BORDER_COLOR,&borderColor,sizeof(borderColor));ShowWindow(g_hwnd,show);UpdateWindow(g_hwnd);MSG msg;while(GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}g_store.Close();winrt::uninit_apartment();return 0;}
