#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <appmodel.h>
#include <roapi.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <memory>
#include <atomic>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <array>
#include <filesystem>
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
#include "terminal/ansi_style_connection.h"
#include "terminal/conpty_connection_bridge.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {
constexpr wchar_t kApplicationUserModelId[] = L"AiCli.Desktop.Dev_jw3zr00rmb0ym!App";
constexpr UINT WM_PTY_OUTPUT = WM_APP + 10;
constexpr UINT WM_PTY_EXIT = WM_APP + 11;
constexpr int ID_PROJECT_LIST = 1001;
constexpr int ID_TERMINAL_OUTPUT = 1002;
constexpr int ID_TERMINAL_INPUT = 1003;
constexpr int ID_NEW_PROJECT = 1004;
constexpr int ID_SETTINGS = 1005;
constexpr int ID_AGENT_CODEX = 1006;
constexpr int ID_AGENT_CLAUDE = 1007;
constexpr int ID_GROUP_DEFAULT = 1008;
constexpr int ID_GROUP_ADD = 1009;
constexpr int ID_BIND_PHONE = 1010;
constexpr int ID_CLI_TAB = 1011;
constexpr int ID_PS_TAB = 1012;
constexpr int ID_START_PROJECT = 1013;
constexpr int ID_EDIT_PROJECT = 1014;
constexpr int ID_CTX_PAUSE = 3001;
constexpr int ID_CTX_CONTINUE = 3002;
constexpr int ID_CTX_CANCEL = 3003;
constexpr int ID_CTX_RESTART = 3004;
constexpr int ID_CTX_RENAME = 3005;
constexpr int ID_MINIMIZE = 3010;
constexpr int ID_CLOSE = 3011;
constexpr wchar_t kClassName[] = L"AicliDesktopWindow";
constexpr UINT_PTR kXamlIslandSubclassId = 0xA1C11;

struct Project { std::wstring name, type, group, folder, command; };
struct OutputMessage { std::wstring text; };

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

std::wstring Base64Encode(const std::wstring& text) {
    static constexpr wchar_t alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const size_t size = text.size() * sizeof(wchar_t);
    std::wstring out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        const unsigned value = (bytes[i] << 16) |
            ((i + 1 < size ? bytes[i + 1] : 0) << 8) |
            (i + 2 < size ? bytes[i + 2] : 0);
        out.push_back(alphabet[(value >> 18) & 0x3f]);
        out.push_back(alphabet[(value >> 12) & 0x3f]);
        out.push_back(i + 1 < size ? alphabet[(value >> 6) & 0x3f] : L'=');
        out.push_back(i + 2 < size ? alphabet[value & 0x3f] : L'=');
    }
    return out;
}

std::wstring PsLiteral(std::wstring value) {
    size_t pos = 0;
    while ((pos = value.find(L'\'', pos)) != std::wstring::npos) {
        value.insert(pos, 1, L'\'');
        pos += 2;
    }
    return L"'" + value + L"'";
}

std::wstring BuildTerminalCommand(const std::wstring& command, const std::wstring& cwd) {
    std::wstring script =
        L"$OutputEncoding=[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new();"
        L"[Console]::InputEncoding=[System.Text.UTF8Encoding]::new();";
    if (!cwd.empty()) {
        script += L"if(Test-Path -LiteralPath " + PsLiteral(cwd) + L"){Set-Location -LiteralPath " + PsLiteral(cwd) + L"};";
    }
    if (!command.empty()) {
        script += command;
    }
    return L"powershell.exe -NoLogo -NoExit -ExecutionPolicy Bypass -EncodedCommand " + Base64Encode(script);
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
               Register(L"TerminalConnection.dll", {
                    L"Microsoft.Terminal.TerminalConnection.ConptyConnection",
                    L"Microsoft.Terminal.TerminalConnection.ConnectionInformation"}) &&
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

class ConPtyProcess {
public:
    ~ConPtyProcess() { Stop(); }
    bool Start(const std::wstring& command, const std::wstring& cwd, HWND owner) {
        owner_ = owner;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE inRead=nullptr,inWrite=nullptr,outRead=nullptr,outWrite=nullptr;
        if (!CreatePipe(&inRead,&inWrite,&sa,0) || !CreatePipe(&outRead,&outWrite,&sa,0)) return false;
        SetHandleInformation(inWrite,HANDLE_FLAG_INHERIT,0); SetHandleInformation(outRead,HANDLE_FLAG_INHERIT,0);
        COORD size{120,32};
        if (FAILED(CreatePseudoConsole(size,inRead,outWrite,0,&pty_))) return false;
        CloseHandle(inRead); CloseHandle(outWrite); input_=inWrite; output_=outRead;
        SIZE_T bytes=0; InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
        auto* attrs=(LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(),0,bytes);
        if (!attrs || !InitializeProcThreadAttributeList(attrs,1,0,&bytes) ||
            !UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,pty_,sizeof(pty_),nullptr,nullptr)) return false;
        STARTUPINFOEXW si{}; si.StartupInfo.cb=sizeof(si); si.lpAttributeList=attrs;
        PROCESS_INFORMATION pi{}; std::wstring line=BuildTerminalCommand(command,cwd);
        std::vector<wchar_t> buf(line.begin(),line.end()); buf.push_back(0);
        BOOL ok=CreateProcessW(nullptr,buf.data(),nullptr,nullptr,FALSE,EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT,
            nullptr,cwd.empty()?nullptr:cwd.c_str(),&si.StartupInfo,&pi);
        DeleteProcThreadAttributeList(attrs); HeapFree(GetProcessHeap(),0,attrs);
        if (!ok) return false;
        process_=pi.hProcess; CloseHandle(pi.hThread); running_=true;
        reader_=std::thread([this]{ ReadLoop(); });
        return true;
    }
    void Write(const std::wstring& text) { if (!input_) return; int n=WideCharToMultiByte(CP_UTF8,0,text.data(),(int)text.size(),nullptr,0,nullptr,nullptr); std::string s(n,'\0'); WideCharToMultiByte(CP_UTF8,0,text.data(),(int)text.size(),s.data(),n,nullptr,nullptr); DWORD written=0; WriteFile(input_,s.data(),(DWORD)s.size(),&written,nullptr); }
    void Stop() { running_=false; if (pty_) { ClosePseudoConsole(pty_); pty_=nullptr; } if (input_) { CloseHandle(input_); input_=nullptr; } if (output_) { CloseHandle(output_); output_=nullptr; } if (process_) { TerminateProcess(process_,0); CloseHandle(process_); process_=nullptr; } if (reader_.joinable()) reader_.join(); }
private:
    void ReadLoop() { char data[4096]; DWORD got=0; std::string pending; while (running_ && output_ && ReadFile(output_,data,sizeof(data),&got,nullptr) && got) { pending.append(data,got); int chars=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,pending.data(),(int)pending.size(),nullptr,0); if (!chars) chars=MultiByteToWideChar(CP_ACP,0,pending.data(),(int)pending.size(),nullptr,0); if (chars) { std::wstring out(chars,L'\0'); MultiByteToWideChar(CP_UTF8,0,pending.data(),(int)pending.size(),out.data(),chars); auto* msg=new OutputMessage{std::move(out)}; PostMessageW(owner_,WM_PTY_OUTPUT,0,(LPARAM)msg); pending.clear(); } } PostMessageW(owner_,WM_PTY_EXIT,0,0); }
    HWND owner_{}; HPCON pty_{}; HANDLE input_{},output_{},process_{}; std::thread reader_; std::atomic<bool> running_{false};
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
    bool Start(const std::wstring& command, const std::wstring& cwd, const std::wstring& title, HWND owner, HWND status) {
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

            set_stage(L"create Windows Terminal connection");
            try {
                const auto command_line = BuildTerminalCommand(command, cwd);
                auto overrides = winrt::single_threaded_map<winrt::hstring, winrt::hstring>();
                // Advertise modern color support to terminal applications.
                overrides.Insert(L"TERM", L"xterm-256color");
                overrides.Insert(L"COLORTERM", L"truecolor");
                auto connection_settings = TerminalConnection::ConptyConnection::CreateSettings(
                    command_line, cwd, title, true, L"", overrides.GetView(),
                    32, 120, winrt::guid{}, winrt::guid{});
                auto connection_activation = LocalActivationFactory<winrt::Windows::Foundation::IActivationFactory>(
                    L"TerminalConnection.dll",
                    L"Microsoft.Terminal.TerminalConnection.ConptyConnection");
                auto official_connection = connection_activation.ActivateInstance<TerminalConnection::ConptyConnection>();
                official_connection.Initialize(connection_settings);
                connection_ = winrt::make<aicli::terminal::AnsiStyleConnection>(official_connection);
                official_connection_ = true;
            } catch (...) {
                auto bridge = winrt::make<aicli::terminal::ConPtyConnectionBridge>(cwd, command);
                connection_ = winrt::make<aicli::terminal::AnsiStyleConnection>(bridge);
                official_connection_ = false;
            }
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

HWND g_hwnd{}, g_list{}, g_output{}, g_input{}, g_status{}, g_codex{}, g_claude{}, g_new_project{}, g_settings{}, g_group{}, g_bind{}, g_cli_tab{}, g_ps_tab{}, g_sidebar_title{}, g_search{}, g_terminal_title{}, g_minimize{}, g_close{};
HWND g_detail_title{}, g_detail_folder{}, g_detail_command{}, g_detail_state{}, g_start_project{}, g_edit_project{};
WNDPROC g_input_proc{};
WNDPROC g_button_proc{}; HWND g_hover_button{};
HFONT g_font{}, g_font_medium{}, g_font_small{}, g_mono{}; std::unique_ptr<ConPtyProcess> g_pty; XamlIslandAdapter g_island; WinRtComponentRegistry g_components;
std::vector<Project> g_projects{{L"默认 Codex 会话",L"Codex",L"默认分组",L"",L""},{L"Claude 工作区",L"Claude",L"默认分组",L"",L""}};
std::wstring g_agent=L"Codex"; std::wstring g_group_name=L"默认分组";
bool g_session_running=false; int g_running_project=-1;
COLORREF bg=RGB(12,14,18), panel=RGB(18,21,27), elevated=RGB(25,29,37), border=RGB(45,51,62), fg=RGB(238,241,247), muted=RGB(139,148,162), accent=RGB(83,156,255), accent2=RGB(42,108,214);
HWND g_project_dialog{}, g_d_name{}, g_d_type{}, g_d_group{}, g_d_folder{}, g_d_folder_browse{}, g_d_command{}; bool g_project_dialog_ok{}; bool g_project_dialog_edit{}; Project g_project_draft{};

enum { ID_D_NAME=2001, ID_D_TYPE=2002, ID_D_GROUP=2003, ID_D_FOLDER=2004, ID_D_COMMAND=2005, ID_D_OK=2006, ID_D_CANCEL=2007, ID_D_BROWSE=2008 };
void DrawModernButton(const DRAWITEMSTRUCT* item);
LRESULT CALLBACK ButtonProc(HWND h,UINT m,WPARAM w,LPARAM l);
void EnableButtonHover(HWND button){WNDPROC old=(WNDPROC)SetWindowLongPtrW(button,GWLP_WNDPROC,(LONG_PTR)ButtonProc);if(!g_button_proc)g_button_proc=old;}

void SetText(HWND h,const std::wstring& s){SetWindowTextW(h,s.c_str());}
void AppendOutput(const std::wstring& s){int len=GetWindowTextLengthW(g_output); SendMessageW(g_output,EM_SETSEL,len,len); SendMessageW(g_output,EM_REPLACESEL,FALSE,(LPARAM)s.c_str()); SendMessageW(g_output,EM_SCROLL,SB_BOTTOM,0);}
void ApplyRoundedRegion(HWND h,int width,int height,int radius){if(!h||width<=0||height<=0)return;SetWindowRgn(h,CreateRoundRectRgn(0,0,width+1,height+1,radius,radius),TRUE);}
void DrawRoundedOutline(HDC dc,RECT r,COLORREF color,int radius){HPEN p=CreatePen(PS_SOLID,1,color);auto oldp=SelectObject(dc,p);auto oldb=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));RoundRect(dc,r.left,r.top,r.right,r.bottom,radius,radius);SelectObject(dc,oldp);SelectObject(dc,oldb);DeleteObject(p);}
HWND MakeDialogControl(LPCWSTR cls,LPCWSTR text,DWORD style,int id,HWND parent,int x,int y,int w,int h){bool button=wcscmp(cls,L"BUTTON")==0;if(button)style|=BS_OWNERDRAW;HWND c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,parent,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);SendMessageW(c,WM_SETFONT,(WPARAM)g_font,TRUE);if(button)EnableButtonHover(c);return c;}
void ReadDialogText(HWND c,std::wstring& value){wchar_t b[1024]{};GetWindowTextW(c,b,1024);value=b;}
bool PickFolder(HWND owner,std::wstring& path);
LRESULT CALLBACK ProjectDialogProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_CREATE:{
        MakeDialogControl(L"STATIC",L"项目名称",SS_LEFT,0,h,28,56,108,24);
        g_d_name=MakeDialogControl(L"EDIT",L"",ES_AUTOHSCROLL,ID_D_NAME,h,150,50,270,30);
        MakeDialogControl(L"STATIC",L"项目类型",SS_LEFT,0,h,28,98,108,24);
        g_d_type=MakeDialogControl(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,ID_D_TYPE,h,150,92,270,180); SendMessageW(g_d_type,CB_ADDSTRING,0,(LPARAM)L"Codex");SendMessageW(g_d_type,CB_ADDSTRING,0,(LPARAM)L"Claude");SendMessageW(g_d_type,CB_SETCURSEL,0,0);
        MakeDialogControl(L"STATIC",L"分组",SS_LEFT,0,h,28,140,108,24);
        g_d_group=MakeDialogControl(L"COMBOBOX",L"",CBS_DROPDOWNLIST,ID_D_GROUP,h,150,134,270,180); SendMessageW(g_d_group,CB_ADDSTRING,0,(LPARAM)L"默认分组");SendMessageW(g_d_group,CB_SETCURSEL,0,0);
        MakeDialogControl(L"STATIC",L"项目文件夹",SS_LEFT,0,h,28,182,108,24);
        g_d_folder=MakeDialogControl(L"EDIT",L"",ES_AUTOHSCROLL|ES_READONLY,ID_D_FOLDER,h,150,176,220,30);
        g_d_folder_browse=MakeDialogControl(L"BUTTON",L"...",BS_PUSHBUTTON,ID_D_BROWSE,h,378,176,42,30);
        MakeDialogControl(L"STATIC",L"启动命令",SS_LEFT,0,h,28,224,108,24);
        g_d_command=MakeDialogControl(L"EDIT",L"",ES_AUTOHSCROLL,ID_D_COMMAND,h,150,218,270,30);
        MakeDialogControl(L"BUTTON",g_project_dialog_edit?L"保存项目":L"创建项目",BS_DEFPUSHBUTTON,ID_D_OK,h,230,278,90,32); MakeDialogControl(L"BUTTON",L"取消",BS_PUSHBUTTON,ID_D_CANCEL,h,330,278,90,32);
        if(g_project_dialog_edit){SetWindowTextW(g_d_name,g_project_draft.name.c_str());SetWindowTextW(g_d_folder,g_project_draft.folder.c_str());SetWindowTextW(g_d_command,g_project_draft.command.c_str());}
        SetFocus(g_d_name); return 0; }
    case WM_CTLCOLORSTATIC: {HDC dc=(HDC)w;SetTextColor(dc,fg);SetBkColor(dc,panel);return (LRESULT)CreateSolidBrush(panel);}
    case WM_CTLCOLOREDIT: {HDC dc=(HDC)w;SetTextColor(dc,fg);SetBkColor(dc,RGB(36,36,36));return (LRESULT)CreateSolidBrush(RGB(36,36,36));}
    case WM_DRAWITEM: {auto* item=(DRAWITEMSTRUCT*)l;if(item->CtlType==ODT_BUTTON){DrawModernButton(item);return TRUE;}} break;
    case WM_COMMAND: if(LOWORD(w)==ID_D_BROWSE){std::wstring selected;if(PickFolder(h,selected))SetWindowTextW(g_d_folder,selected.c_str());return 0;} if(LOWORD(w)==ID_D_CANCEL){DestroyWindow(h);return 0;} if(LOWORD(w)==ID_D_OK){ReadDialogText(g_d_name,g_project_draft.name);ReadDialogText(g_d_folder,g_project_draft.folder);ReadDialogText(g_d_command,g_project_draft.command);wchar_t type[64]{},group[64]{};GetWindowTextW(g_d_type,type,64);GetWindowTextW(g_d_group,group,64);g_project_draft.type=type;g_project_draft.group=group;if(g_project_draft.name.empty()){MessageBoxW(h,L"项目名称不能为空。",L"创建项目",MB_OK|MB_ICONWARNING);return 0;}if(g_project_draft.folder.empty()){MessageBoxW(h,L"请选择项目文件夹。",L"创建项目",MB_OK|MB_ICONWARNING);return 0;}if(g_project_draft.command.empty())g_project_draft.command=g_project_draft.type==L"Claude"?L"claude":L"codex";g_project_dialog_ok=true;DestroyWindow(h);return 0;} break;
    case WM_CLOSE:DestroyWindow(h);return 0; }
    return DefWindowProcW(h,m,w,l);
}
bool ShowProjectDialog(HWND owner,const Project* initial=nullptr){static bool registered=false;if(!registered){WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=ProjectDialogProc;wc.lpszClassName=L"AicliProjectDialog";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=CreateSolidBrush(panel);RegisterClassW(&wc);registered=true;}g_project_dialog_ok=false;g_project_dialog_edit=initial!=nullptr;g_project_draft=initial?*initial:Project{};EnableWindow(owner,FALSE);g_project_dialog=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_TOPMOST,L"AicliProjectDialog",g_project_dialog_edit?L"编辑项目":L"新建项目",WS_POPUP|WS_CAPTION|WS_SYSMENU,0,0,460,350,owner,nullptr,GetModuleHandleW(nullptr),nullptr);RECT wr{},dr{};GetWindowRect(owner,&wr);GetWindowRect(g_project_dialog,&dr);int x=wr.left+(wr.right-wr.left-(dr.right-dr.left))/2;int y=wr.top+(wr.bottom-wr.top-(dr.bottom-dr.top))/2;SetWindowPos(g_project_dialog,HWND_TOP,x,y,0,0,SWP_NOSIZE|SWP_SHOWWINDOW);MSG msg{};while(IsWindow(g_project_dialog)&&GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}EnableWindow(owner,TRUE);SetForegroundWindow(owner);return g_project_dialog_ok;}

LRESULT CALLBACK ButtonProc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_MOUSEMOVE){if(g_hover_button!=h){HWND old=g_hover_button;g_hover_button=h;if(old)InvalidateRect(old,nullptr,TRUE);InvalidateRect(h,nullptr,TRUE);TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);}}else if(m==WM_MOUSELEAVE){if(g_hover_button==h)g_hover_button=nullptr;InvalidateRect(h,nullptr,TRUE);}else if(m==WM_LBUTTONDOWN||m==WM_LBUTTONUP||m==WM_SETFOCUS||m==WM_KILLFOCUS){InvalidateRect(h,nullptr,TRUE);}return CallWindowProcW(g_button_proc,h,m,w,l);}
void DrawModernButton(const DRAWITEMSTRUCT* item){RECT r=item->rcItem;bool down=(item->itemState&ODS_SELECTED)!=0;bool disabled=(item->itemState&ODS_DISABLED)!=0;bool hover=item->hwndItem==g_hover_button;POINT pos{};ClientToScreen(item->hwndItem,&pos);ScreenToClient(GetParent(item->hwndItem),&pos);COLORREF under=GetParent(item->hwndItem)==g_hwnd?(pos.y<52||pos.x<286?panel:bg):panel;HBRUSH clear=CreateSolidBrush(under);FillRect(item->hDC,&r,clear);DeleteObject(clear);COLORREF fill=under;if(item->CtlID==ID_CLI_TAB||item->CtlID==ID_PS_TAB||item->CtlID==ID_D_CANCEL||item->CtlID==ID_EDIT_PROJECT)fill=elevated;if(item->CtlID==ID_NEW_PROJECT||item->CtlID==ID_D_OK||item->CtlID==ID_START_PROJECT)fill=accent2;if(disabled)fill=RGB(33,37,45);else if(down)fill=RGB(37,91,174);else if(hover)fill=RGB(34,40,51);if(item->CtlID==ID_CLOSE)fill=down?RGB(157,44,55):(hover?RGB(120,38,47):panel);if(item->CtlID==ID_MINIMIZE)fill=hover?RGB(37,42,51):panel;COLORREF stroke=disabled?RGB(43,48,57):(hover?RGB(78,104,142):RGB(52,59,70));if(item->CtlID==ID_NEW_PROJECT||item->CtlID==ID_D_OK||item->CtlID==ID_START_PROJECT)stroke=hover?RGB(117,177,255):RGB(64,128,225);HBRUSH b=CreateSolidBrush(fill);HPEN p=CreatePen(PS_SOLID,1,stroke);auto oldb=SelectObject(item->hDC,b);auto oldp=SelectObject(item->hDC,p);RoundRect(item->hDC,r.left+1,r.top+1,r.right-1,r.bottom-1,9,9);SelectObject(item->hDC,oldb);SelectObject(item->hDC,oldp);DeleteObject(b);DeleteObject(p);wchar_t text[128]{};GetWindowTextW(item->hwndItem,text,128);SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,disabled?muted:(hover?RGB(255,255,255):fg));SelectObject(item->hDC,g_font);DrawTextW(item->hDC,text,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
void DrawProjectItem(const DRAWITEMSTRUCT* item){if(item->itemID==(UINT)-1)return;RECT row=item->rcItem;HBRUSH back=CreateSolidBrush(panel);FillRect(item->hDC,&row,back);DeleteObject(back);bool selected=(item->itemState&ODS_SELECTED)!=0;RECT r{row.left+3,row.top+4,row.right-4,row.bottom-4};COLORREF fill=selected?RGB(29,43,63):RGB(22,25,32);COLORREF stroke=selected?RGB(58,106,171):RGB(38,43,52);HBRUSH b=CreateSolidBrush(fill);HPEN p=CreatePen(PS_SOLID,1,stroke);auto oldb=SelectObject(item->hDC,b);auto oldp=SelectObject(item->hDC,p);RoundRect(item->hDC,r.left,r.top,r.right,r.bottom,10,10);SelectObject(item->hDC,oldb);SelectObject(item->hDC,oldp);DeleteObject(b);DeleteObject(p);SetBkMode(item->hDC,TRANSPARENT);SelectObject(item->hDC,g_font);SetTextColor(item->hDC,selected?fg:RGB(210,215,224));wchar_t name[256]{};SendMessageW(item->hwndItem,LB_GETTEXT,item->itemID,(LPARAM)name);RECT title{r.left+24,r.top+11,r.right-12,r.top+34};DrawTextW(item->hDC,name,-1,&title,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);std::wstring sub=L"就绪";if(item->itemID<g_projects.size())sub+=L"   "+g_projects[item->itemID].type+L"   /   "+g_projects[item->itemID].group;SelectObject(item->hDC,g_font_small?g_font_small:g_font);SetTextColor(item->hDC,muted);RECT subRect{r.left+24,r.top+37,r.right-12,r.bottom-8};DrawTextW(item->hDC,sub.c_str(),-1,&subRect,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);HBRUSH dot=CreateSolidBrush(selected?accent:RGB(71,187,132));Ellipse(item->hDC,r.left+11,r.top+18,r.left+17,r.top+24);DeleteObject(dot);}
bool PickFolder(HWND owner,std::wstring& path){IFileDialog* dialog=nullptr;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))||!dialog)return false;DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);if(FAILED(dialog->Show(owner))){dialog->Release();return false;}IShellItem* item=nullptr;bool ok=SUCCEEDED(dialog->GetResult(&item));if(ok){PWSTR value=nullptr;ok=SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&value));if(ok){path=value;CoTaskMemFree(value);}item->Release();}dialog->Release();return ok;}
int SelectedProjectIndex(){return (int)SendMessageW(g_list,LB_GETCURSEL,0,0);}
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
    SetText(g_detail_title,project.name+L"   /   "+project.type);
    SetText(g_detail_folder,L"文件夹: "+(project.folder.empty()?L"未设置":project.folder));
    SetText(g_detail_command,L"启动命令: "+(project.command.empty()?L"未设置":project.command));
    SetText(g_detail_state,std::wstring(L"状态: ")+((g_session_running&&index==g_running_project)?L"运行中":L"未运行"));
}
void RefreshProjectList(){
    const int selected=SelectedProjectIndex();
    SendMessageW(g_list,LB_RESETCONTENT,0,0);
    for(auto& project:g_projects)SendMessageW(g_list,LB_ADDSTRING,0,(LPARAM)project.name.c_str());
    if(!g_projects.empty())SendMessageW(g_list,LB_SETCURSEL,(WPARAM)std::min(selected,(int)g_projects.size()-1),0);
    RefreshProjectDetails();
}
void Layout(){
    if(!g_hwnd || IsIconic(g_hwnd))return;
    RECT r{}; if(!GetClientRect(g_hwnd,&r))return;
    constexpr int top=52, left=286, bottom=30;
    if(r.right<=left+80 || r.bottom<=top+bottom+120)return;
    ShowWindow(g_codex,SW_HIDE); ShowWindow(g_claude,SW_HIDE);
    MoveWindow(g_group,168,10,106,32,TRUE);
    MoveWindow(g_bind,std::max(760,(int)r.right-282),10,96,32,TRUE);
    MoveWindow(g_settings,std::max(860,(int)r.right-180),10,72,32,TRUE);
    if(g_minimize) ShowWindow(g_minimize,SW_HIDE);
    if(g_close) ShowWindow(g_close,SW_HIDE);

    MoveWindow(g_search,14,top+16,left-28,38,TRUE);
    ApplyRoundedRegion(g_search,left-28,38,10);
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
    if(showingNative){
        ShowWindow(g_output,SW_HIDE); ShowWindow(g_input,SW_HIDE);
    }else{
        ShowWindow(g_output,showingTerminal?SW_SHOW:SW_HIDE); ShowWindow(g_input,showingTerminal?SW_SHOW:SW_HIDE);
        MoveWindow(g_output,terminal.left,terminal.top,terminal.right-terminal.left,
                   terminal.bottom-terminal.top-52,TRUE);
        MoveWindow(g_input,terminal.left,terminal.bottom-42,terminal.right-terminal.left,40,TRUE);
        ApplyRoundedRegion(g_input,terminal.right-terminal.left,40,9);
    }
    MoveWindow(g_status,14,r.bottom-bottom+6,r.right-28,20,TRUE);
    if(g_island.Window()){
        if(showingNative)g_island.Resize(terminal);
        g_island.SetVisible(showingNative);
    }
}
void CreateProject(){ if(!ShowProjectDialog(g_hwnd))return; g_projects.push_back(g_project_draft); SendMessageW(g_list,LB_ADDSTRING,0,(LPARAM)g_project_draft.name.c_str()); SendMessageW(g_list,LB_SETCURSEL,(WPARAM)g_projects.size()-1,0); RefreshProjectDetails(); Layout(); SetText(g_status,L"项目已创建    |    选择右侧立即开始"); }
void EditProject(){const int index=SelectedProjectIndex();if(index<0||index>=(int)g_projects.size())return;const Project before=g_projects[index];if(!ShowProjectDialog(g_hwnd,&before))return;g_projects[index]=g_project_draft;RefreshProjectList();SetText(g_status,L"项目配置已更新");Layout();}
bool StartCommandSession(const std::wstring& cmd,const std::wstring& cwd,const std::wstring& title,const std::wstring& sessionAgent){
    SetText(g_terminal_title,title+L"   /   "+sessionAgent);
    if(g_pty){g_pty->Stop();g_pty.reset();}
    if(g_island.Start(cmd,cwd,title,g_hwnd,g_status)){
        g_session_running=true;
        SetText(g_status,L"原生终端已连接    |    "+sessionAgent+L"    |    Relay 在线");
        Layout();
        return true;
    }
    g_session_running=false;
    const std::wstring error=g_island.LastError().empty()?L"无法创建原生终端控件":g_island.LastError();
    SetText(g_detail_state,L"状态: 启动失败");
    SetText(g_status,L"原生终端启动失败: "+error);
    AppendOutput(L"\r\n[原生终端启动失败，未切换到文本回退窗口]\r\n");
    Layout(); return false;
}
void StartSession(){const int i=SelectedProjectIndex();if(i<0||i>=(int)g_projects.size())return;const auto& project=g_projects[i];const std::wstring sessionAgent=project.type;const std::wstring cmd=project.command.empty()?(sessionAgent==L"Claude"?L"claude":L"codex"):project.command;if(StartCommandSession(cmd,project.folder,project.name,sessionAgent)){g_running_project=i;RefreshProjectDetails();Layout();}}
LRESULT CALLBACK InputProc(HWND h,UINT m,WPARAM w,LPARAM l){ if(m==WM_KEYDOWN&&w==VK_RETURN){wchar_t b[4096];GetWindowTextW(h,b,4096);std::wstring s=b;if(!s.empty()){if(g_pty)g_pty->Write(s+L"\r");SetWindowTextW(h,L"");}return 0;} return CallWindowProcW(g_input_proc,h,m,w,l); }
LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_CREATE:{g_hwnd=h; g_font=CreateFontW(-15,0,0,0,400,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");g_font_medium=CreateFontW(-17,0,0,0,600,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");g_font_small=CreateFontW(-13,0,0,0,400,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");g_mono=CreateFontW(-16,0,0,0,400,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"Cascadia Mono");
        g_codex=CreateWindowW(L"BUTTON",L"Codex",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,100,8,70,30,h,(HMENU)(INT_PTR)ID_AGENT_CODEX,GetModuleHandleW(nullptr),nullptr);g_claude=CreateWindowW(L"BUTTON",L"Claude",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,174,8,76,30,h,(HMENU)(INT_PTR)ID_AGENT_CLAUDE,GetModuleHandleW(nullptr),nullptr);g_group=CreateWindowW(L"BUTTON",L"默认分组",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,260,8,100,30,h,(HMENU)(INT_PTR)ID_GROUP_DEFAULT,GetModuleHandleW(nullptr),nullptr);g_bind=CreateWindowW(L"BUTTON",L"绑定手机",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,365,8,100,30,h,(HMENU)(INT_PTR)ID_BIND_PHONE,GetModuleHandleW(nullptr),nullptr);g_new_project=CreateWindowW(L"BUTTON",L"+",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,8,55,34,30,h,(HMENU)(INT_PTR)ID_NEW_PROJECT,GetModuleHandleW(nullptr),nullptr);g_settings=CreateWindowW(L"BUTTON",L"设置",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,1000,8,70,30,h,(HMENU)(INT_PTR)ID_SETTINGS,GetModuleHandleW(nullptr),nullptr);g_cli_tab=CreateWindowW(L"BUTTON",L"AI 终端",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,250,52,100,30,h,(HMENU)(INT_PTR)ID_CLI_TAB,GetModuleHandleW(nullptr),nullptr);HWND controls[]={g_codex,g_claude,g_group,g_bind,g_new_project,g_settings,g_cli_tab};for(HWND b:controls){SendMessageW(b,WM_SETFONT,(WPARAM)g_font,TRUE);EnableButtonHover(b);}
        g_sidebar_title=CreateWindowW(L"STATIC",L"项目与会话",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_sidebar_title,WM_SETFONT,(WPARAM)g_font_medium,TRUE);g_search=CreateWindowW(L"EDIT",L"搜索项目",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_search,WM_SETFONT,(WPARAM)g_font,TRUE);SendMessageW(g_search,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(13,13));
        g_list=CreateWindowW(L"LISTBOX",nullptr,WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,0,0,0,0,h,(HMENU)(INT_PTR)ID_PROJECT_LIST,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_list,WM_SETFONT,(WPARAM)g_font,TRUE);for(auto&p:g_projects)SendMessageW(g_list,LB_ADDSTRING,0,(LPARAM)p.name.c_str());SendMessageW(g_list,LB_SETCURSEL,0,0);
        g_output=CreateWindowW(L"EDIT",L"AI CLI\r\n\r\n选择左侧项目后，终端会启动真实 Codex/Claude CLI。\r\n",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,0,0,0,0,h,(HMENU)(INT_PTR)ID_TERMINAL_OUTPUT,GetModuleHandleW(nullptr),nullptr); SendMessageW(g_output,WM_SETFONT,(WPARAM)g_mono,TRUE);
        g_input=CreateWindowW(L"EDIT",nullptr,WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,0,0,0,0,h,(HMENU)(INT_PTR)ID_TERMINAL_INPUT,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_input,WM_SETFONT,(WPARAM)g_mono,TRUE);SendMessageW(g_input,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(13,13));g_input_proc=(WNDPROC)SetWindowLongPtrW(g_input,GWLP_WNDPROC,(LONG_PTR)InputProc);
        g_detail_title=CreateWindowW(L"STATIC",L"选择一个项目",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        g_detail_folder=CreateWindowW(L"STATIC",L"文件夹: 未设置",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        g_detail_command=CreateWindowW(L"STATIC",L"启动命令: 未设置",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        g_detail_state=CreateWindowW(L"STATIC",L"状态: 未运行",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        g_start_project=CreateWindowW(L"BUTTON",L"立即开始",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,0,0,0,0,h,(HMENU)(INT_PTR)ID_START_PROJECT,GetModuleHandleW(nullptr),nullptr);
        g_edit_project=CreateWindowW(L"BUTTON",L"编辑项目",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW,0,0,0,0,h,(HMENU)(INT_PTR)ID_EDIT_PROJECT,GetModuleHandleW(nullptr),nullptr);
        HWND detailControls[]={g_detail_title,g_detail_folder,g_detail_command,g_detail_state,g_start_project,g_edit_project};for(HWND c:detailControls){SendMessageW(c,WM_SETFONT,(WPARAM)g_font,TRUE);if(c==g_start_project||c==g_edit_project)EnableButtonHover(c);}
        g_terminal_title=CreateWindowW(L"STATIC",L"选择一个项目开始",WS_CHILD|WS_VISIBLE,0,0,0,0,h,nullptr,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_terminal_title,WM_SETFONT,(WPARAM)g_font,TRUE);g_status=CreateWindowW(L"STATIC",L"就绪    |    选择项目后查看启动信息",WS_CHILD|WS_VISIBLE,0,0,0,0,h,(HMENU)(INT_PTR)ID_SETTINGS,GetModuleHandleW(nullptr),nullptr);SendMessageW(g_status,WM_SETFONT,(WPARAM)g_font_small,TRUE);
        g_island.Attach(h,RECT{250,60,500,400}); RefreshProjectDetails(); Layout(); return 0;}
    case WM_MEASUREITEM: if(((MEASUREITEMSTRUCT*)l)->CtlID==ID_PROJECT_LIST){((MEASUREITEMSTRUCT*)l)->itemHeight=72;return TRUE;} break;
    case WM_DRAWITEM: {auto* item=(DRAWITEMSTRUCT*)l;if(item->CtlType==ODT_BUTTON){DrawModernButton(item);return TRUE;}if(item->CtlID==ID_PROJECT_LIST){DrawProjectItem(item);return TRUE;}} break;
    case WM_SIZE: if(w==SIZE_MINIMIZED){g_island.SetVisible(false);return 0;}Layout();return 0;
    case WM_GETMINMAXINFO:{auto* info=(MINMAXINFO*)l;info->ptMinTrackSize.x=900;info->ptMinTrackSize.y=600;return 0;}
    case WM_MOUSEWHEEL: if(g_island.HandleMouseWheel(w,l))return 0; break;
    case WM_MOUSEHWHEEL: if(g_island.HandleMouseWheel(w,l,true))return 0; break;
    case WM_ERASEBKGND: return 1;
    case WM_CONTEXTMENU: if((HWND)w==g_list){HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,ID_CTX_PAUSE,L"暂停");AppendMenuW(menu,MF_STRING,ID_CTX_CONTINUE,L"继续");AppendMenuW(menu,MF_STRING,ID_CTX_CANCEL,L"取消");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,ID_CTX_RESTART,L"重新开始");AppendMenuW(menu,MF_STRING,ID_CTX_RENAME,L"重命名");TrackPopupMenu(menu,TPM_RIGHTBUTTON,GET_X_LPARAM(l),GET_Y_LPARAM(l),0,h,nullptr);DestroyMenu(menu);return 0;} break;
    case WM_CTLCOLORSTATIC: {HDC dc=(HDC)w;SetBkMode(dc,TRANSPARENT);SetTextColor(dc,fg);static HBRUSH brush=CreateSolidBrush(bg);return (LRESULT)brush;}
    case WM_CTLCOLOREDIT: {HDC dc=(HDC)w;SetTextColor(dc,fg);SetBkColor(dc,RGB(34,34,34));static HBRUSH brush=CreateSolidBrush(RGB(34,34,34));return (LRESULT)brush;}
    case WM_CTLCOLORLISTBOX: {HDC dc=(HDC)w;SetTextColor(dc,fg);SetBkColor(dc,RGB(25,25,25));static HBRUSH brush=CreateSolidBrush(RGB(25,25,25));return (LRESULT)brush;}
    case WM_COMMAND: if(LOWORD(w)==ID_CLOSE){DestroyWindow(h);return 0;}if(LOWORD(w)==ID_MINIMIZE){ShowWindow(h,SW_MINIMIZE);return 0;}if(LOWORD(w)==ID_PROJECT_LIST&&HIWORD(w)==LBN_SELCHANGE){RefreshProjectDetails();Layout();return 0;}if(LOWORD(w)==ID_PROJECT_LIST&&HIWORD(w)==LBN_DBLCLK){StartSession();return 0;} if(LOWORD(w)==ID_NEW_PROJECT){CreateProject();return 0;} if(LOWORD(w)==ID_START_PROJECT){StartSession();return 0;} if(LOWORD(w)==ID_EDIT_PROJECT){EditProject();return 0;} if(LOWORD(w)==ID_CTX_RESTART){StartSession();return 0;} if(LOWORD(w)==ID_CTX_CANCEL||LOWORD(w)==ID_CTX_PAUSE){g_island.Stop();if(g_pty)g_pty->Stop();g_session_running=false;g_running_project=-1;SetText(g_status,LOWORD(w)==ID_CTX_PAUSE?L"会话已暂停":L"会话已取消");RefreshProjectDetails();Layout();return 0;} if(LOWORD(w)==ID_CTX_CONTINUE){StartSession();return 0;} if(LOWORD(w)==ID_CTX_RENAME){EditProject();return 0;} if(LOWORD(w)==ID_BIND_PHONE){MessageBoxW(h,L"手机绑定入口已就绪。下一步接入二维码和一次性公钥授权。",L"绑定手机",MB_OK|MB_ICONINFORMATION);return 0;} if(LOWORD(w)==ID_SETTINGS){MessageBoxW(h,L"设置入口已就绪，可配置本地/公网地址、默认分组和主题。",L"设置",MB_OK|MB_ICONINFORMATION);return 0;} if(LOWORD(w)==ID_GROUP_DEFAULT){MessageBoxW(h,L"当前分组：默认分组\n新建项目时会继承当前分组。",L"分组",MB_OK|MB_ICONINFORMATION);return 0;} if(LOWORD(w)==ID_CLI_TAB){RefreshProjectDetails();Layout();return 0;} if(LOWORD(w)==ID_AGENT_CODEX||LOWORD(w)==ID_AGENT_CLAUDE){g_agent=LOWORD(w)==ID_AGENT_CODEX?L"Codex":L"Claude";SetText(g_status,g_agent+L"    |    Relay 在线    |    内存: -- MB");return 0;} return 0;
    case WM_PTY_OUTPUT:{auto* msg=(OutputMessage*)l;if(msg){AppendOutput(msg->text);delete msg;}return 0;} case WM_PTY_EXIT:AppendOutput(L"\r\n[会话已结束]\r\n");return 0;
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);HBRUSH b=CreateSolidBrush(bg);FillRect(dc,&r,b);DeleteObject(b);HBRUSH bar=CreateSolidBrush(panel);RECT top{0,0,r.right,52};FillRect(dc,&top,bar);RECT side{0,52,286,r.bottom-30};FillRect(dc,&side,bar);DeleteObject(bar);HPEN pen=CreatePen(PS_SOLID,1,border);auto old=(HPEN)SelectObject(dc,pen);MoveToEx(dc,285,52,nullptr);LineTo(dc,285,r.bottom-30);MoveToEx(dc,0,51,nullptr);LineTo(dc,r.right,51);MoveToEx(dc,0,r.bottom-30,nullptr);LineTo(dc,r.right,r.bottom-30);SelectObject(dc,old);DeleteObject(pen);DrawRoundedOutline(dc,RECT{14,68,272,108},RGB(49,56,68),10);DrawRoundedOutline(dc,RECT{302,110,r.right-14,r.bottom-40},RGB(40,46,56),10);HBRUSH logo=CreateSolidBrush(accent2);HPEN logoPen=CreatePen(PS_SOLID,1,RGB(75,143,244));auto oldLogoB=SelectObject(dc,logo);auto oldLogoP=SelectObject(dc,logoPen);RoundRect(dc,14,10,46,42,9,9);SelectObject(dc,oldLogoB);SelectObject(dc,oldLogoP);DeleteObject(logo);DeleteObject(logoPen);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));SelectObject(dc,g_font_medium);TextOutW(dc,22,16,L">_",2);SetTextColor(dc,fg);TextOutW(dc,58,11,L"AI CLI",6);SetTextColor(dc,muted);SelectObject(dc,g_font_small);TextOutW(dc,58,29,L"LOCAL WORKSPACE",15);SetTextColor(dc,RGB(83,203,146));TextOutW(dc,304,20,L"●  Relay 在线",11);SetTextColor(dc,muted);TextOutW(dc,420,20,L"0 台设备",5);TextOutW(dc,498,20,L"内存 -- MB",8);EndPaint(h,&ps);return 0;}
    case WM_DESTROY:g_island.Stop();if(g_pty)g_pty->Stop();if(g_font)DeleteObject(g_font);if(g_font_medium)DeleteObject(g_font_medium);if(g_font_small)DeleteObject(g_font_small);if(g_mono)DeleteObject(g_mono);PostQuitMessage(0);return 0; }
    return DefWindowProcW(h,m,w,l);
}
}

int WINAPI wWinMain(HINSTANCE inst,HINSTANCE, PWSTR, int show){winrt::init_apartment(winrt::apartment_type::single_threaded);const int activation=EnsurePackagedActivation();if(activation<0){MessageBoxW(nullptr,L"无法启动已注册的 AI CLI 桌面实例，请先完成包注册。",L"AI CLI",MB_OK|MB_ICONERROR);return 1;}if(activation==0)return 0;g_components.Initialize();INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_STANDARD_CLASSES};InitCommonControlsEx(&ic);WNDCLASSW wc{};wc.hInstance=inst;wc.lpfnWndProc=WndProc;wc.lpszClassName=kClassName;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&wc);constexpr DWORD windowStyle=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_CLIPCHILDREN;g_hwnd=CreateWindowExW(0,kClassName,L"AI CLI - Codex / Claude",windowStyle,CW_USEDEFAULT,CW_USEDEFAULT,1280,800,nullptr,nullptr,inst,nullptr);BOOL dark=TRUE;DwmSetWindowAttribute(g_hwnd,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));ShowWindow(g_hwnd,show);UpdateWindow(g_hwnd);MSG msg;while(GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}winrt::uninit_apartment();return 0;}
