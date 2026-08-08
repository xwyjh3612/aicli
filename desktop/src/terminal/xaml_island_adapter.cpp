#include "terminal/xaml_island_adapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windowsx.h>
#include <commctrl.h>
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
#include <winrt/Microsoft.Terminal.Core.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

#include "app/runtime_components.h"
#include "core/text_utils.h"
#include "terminal/conpty_connection_bridge.h"

namespace aicli::terminal {
using aicli::app::LocalActivationFactory;
constexpr UINT kXamlIslandSubclassId = 0xA1C11;

namespace {

// Match Windows Terminal's paste filtering rules: normalize all line endings
// to CR and remove C0/C1 control characters except tab and line endings. This
// also prevents remote text from prematurely terminating bracketed paste.
std::wstring FilterRemotePasteText(std::wstring_view value) {
    std::wstring filtered;
    filtered.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        const wchar_t ch = value[index];
        if (ch == L'\r') {
            filtered.push_back(L'\r');
            if (index + 1 < value.size() && value[index + 1] == L'\n') ++index;
            continue;
        }
        if (ch == L'\n') {
            filtered.push_back(L'\r');
            continue;
        }
        const bool printable_ascii = ch >= L'\x20' && ch < L'\x7f';
        const bool non_control_unicode = ch > L'\x9f';
        if (printable_ascii || non_control_unicode || ch == L'\t') {
            filtered.push_back(ch);
        }
    }
    return filtered;
}

} // namespace

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

LRESULT CALLBACK XamlIslandSubclassProc(
    HWND hwnd,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR reference_data);

struct XamlIslandAdapter::Impl {
public:
    bool Attach(HWND parent, RECT rc) {
        std::wstring stage = L"prepare";
        try {
            parent_ = parent;
            stage = L"create XAML application";
            application_ = winrt::make_self<HostXamlApplication>();
            stage = L"initialize WindowsXamlManager";
            manager_=winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
            stage = L"create DesktopWindowXamlSource";
            source_=winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource();
            stage = L"attach XAML source to Qt host";
            auto interop=source_.as<::IDesktopWindowXamlSourceNative>();
            winrt::check_hresult(interop->AttachToWindow(parent));
            interop->get_WindowHandle(&hwnd_);
            stage = L"subclass XAML island";
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
            stage = L"size XAML island";
            Resize(rc);
            ShowWindow(hwnd_,SW_HIDE);
            last_error_.clear();
            return true;
        } catch (const winrt::hresult_error& error) {
            wchar_t code[16]{};
            swprintf_s(code, L"%08X", static_cast<unsigned>(error.code().value));
            last_error_ = stage + L": 0x" + code + L" " + error.message().c_str();
            OutputDebugStringW((L"AI CLI XAML attach: " + last_error_ + L"\n").c_str());
            return false;
        } catch (const std::exception& error) {
            last_error_ = stage + L": " + aicli::text::WideText(error.what());
            OutputDebugStringW((L"AI CLI XAML attach: " + last_error_ + L"\n").c_str());
            return false;
        } catch (...) {
            last_error_ = stage + L": unknown error";
            OutputDebugStringW((L"AI CLI XAML attach: " + last_error_ + L"\n").c_str());
            return false;
        }
    }
    bool Start(const std::wstring& project_id, const std::wstring& command,
               const std::wstring& cwd, const std::wstring& title, HWND owner, HWND status,
               std::function<void(std::wstring_view)> output_observer = {},
               std::function<void(std::uint32_t, std::uint32_t)> resize_observer = {}) {
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
                    cwd, command, std::move(output_observer),
                    std::move(resize_observer));
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
    bool SubmitInput(std::wstring_view project_id, const std::wstring& value) {
        auto* session = FindSession(project_id);
        if (!session || !session->control || value.empty()) return false;
        try {
            auto filtered = FilterRemotePasteText(value);
            if (filtered.empty()) return false;

            // A phone message is semantically a paste followed by a real Enter
            // key. Sending the whole string and a raw CR directly to ConPTY
            // makes Codex classify the stream as a paste burst and reinterpret
            // Enter as an embedded newline.
            const bool bracketed_paste = session->control.BracketedPasteEnabled();
            if (bracketed_paste) {
                filtered.insert(0, L"\x1b[200~");
                filtered.append(L"\x1b[201~");
            }
            session->control.RawWriteString(winrt::hstring{filtered});

            // This application nests TermControl over ConPTY. Even when the
            // outer terminal reports bracketed-paste support, the inner Windows
            // console can still surface the pasted payload to Codex as a rapid
            // KEY_EVENT burst. Codex deliberately treats Enter as an embedded
            // newline for 120 ms after such a burst. Cross that window before
            // generating the real Enter event, otherwise phone Send only adds a
            // line break to the composer.
            Sleep(150);

            const auto scan_code = static_cast<uint16_t>(
                MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
            const winrt::Microsoft::Terminal::Core::ControlKeyStates modifiers{0};

            // Mirror TermControl's native keyboard pipeline. A non-character
            // protocol (including enhanced keyboard reporting) handles keydown
            // directly; otherwise the following character event emits Enter.
            bool handled = session->control.RawWriteKeyEvent(
                VK_RETURN, scan_code, modifiers, true);
            if (!handled) {
                handled = session->control.RawWriteChar(
                    L'\r', scan_code, modifiers);
            }
            (void)session->control.RawWriteKeyEvent(
                VK_RETURN, scan_code, modifiers, false);
            return handled;
        } catch (...) {
            return false;
        }
    }
    bool SendKey(std::wstring_view project_id, UINT virtual_key) {
        auto* session = FindSession(project_id);
        if (!session || !session->control) return false;
        if (virtual_key != VK_LEFT && virtual_key != VK_RIGHT &&
            virtual_key != VK_UP && virtual_key != VK_DOWN &&
            virtual_key != VK_RETURN) return false;
        try {
            const auto scan_code = static_cast<uint16_t>(
                MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC));
            const bool enhanced = virtual_key == VK_LEFT || virtual_key == VK_RIGHT ||
                                  virtual_key == VK_UP || virtual_key == VK_DOWN;
            const winrt::Microsoft::Terminal::Core::ControlKeyStates modifiers{
                enhanced ? ENHANCED_KEY : 0u};
            bool handled = session->control.RawWriteKeyEvent(
                static_cast<uint16_t>(virtual_key), scan_code, modifiers, true);
            if (!handled && virtual_key == VK_RETURN) {
                handled = session->control.RawWriteChar(
                    L'\r', scan_code, modifiers);
            }
            (void)session->control.RawWriteKeyEvent(
                static_cast<uint16_t>(virtual_key), scan_code, modifiers, false);
            return handled;
        } catch (...) {
            return false;
        }
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
    auto* adapter = reinterpret_cast<XamlIslandAdapter::Impl*>(reference_data);
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




XamlIslandAdapter::XamlIslandAdapter()
    : impl_(std::make_unique<Impl>()) {}

XamlIslandAdapter::~XamlIslandAdapter() = default;

XamlIslandAdapter::XamlIslandAdapter(XamlIslandAdapter&&) noexcept = default;

XamlIslandAdapter& XamlIslandAdapter::operator=(XamlIslandAdapter&&) noexcept = default;

bool XamlIslandAdapter::Attach(HWND parent, RECT rc) {
    return impl_ && impl_->Attach(parent, rc);
}

bool XamlIslandAdapter::Start(const std::wstring& project_id,
                              const std::wstring& command,
                              const std::wstring& cwd,
                              const std::wstring& title,
                              HWND owner,
                              HWND status,
                              std::function<void(std::wstring_view)> output_observer,
                              std::function<void(std::uint32_t, std::uint32_t)> resize_observer) {
    return impl_ && impl_->Start(project_id, command, cwd, title, owner, status,
                                 std::move(output_observer),
                                 std::move(resize_observer));
}

bool XamlIslandAdapter::HasSession(std::wstring_view project_id) const {
    return impl_ && impl_->HasSession(project_id);
}

bool XamlIslandAdapter::Activate(std::wstring_view project_id, bool focus) {
    return impl_ && impl_->Activate(project_id, focus);
}

void XamlIslandAdapter::Stop(std::wstring_view project_id) {
    if (impl_) impl_->Stop(project_id);
}

void XamlIslandAdapter::StopAll() {
    if (impl_) impl_->StopAll();
}

void XamlIslandAdapter::SetVisible(bool visible) {
    if (impl_) impl_->SetVisible(visible);
}

void XamlIslandAdapter::SetTheme(bool light) {
    if (impl_) impl_->SetTheme(light);
}

void XamlIslandAdapter::Resize(RECT rc) {
    if (impl_) impl_->Resize(rc);
}

HWND XamlIslandAdapter::Window() const {
    return impl_ ? impl_->Window() : nullptr;
}

bool XamlIslandAdapter::Active() const {
    return impl_ && impl_->Active();
}

bool XamlIslandAdapter::Active(std::wstring_view project_id) const {
    return impl_ && impl_->Active(project_id);
}

std::wstring XamlIslandAdapter::ReadEntireBuffer(
    std::wstring_view project_id) const {
    return impl_ ? impl_->ReadEntireBuffer(project_id) : std::wstring{};
}

bool XamlIslandAdapter::WriteInput(const std::wstring& value) {
    return impl_ && impl_->WriteInput(value);
}

bool XamlIslandAdapter::WriteInput(std::wstring_view project_id,
                                    const std::wstring& value) {
    return impl_ && impl_->WriteInput(project_id, value);
}

bool XamlIslandAdapter::SubmitInput(std::wstring_view project_id,
                                    const std::wstring& value) {
    return impl_ && impl_->SubmitInput(project_id, value);
}
bool XamlIslandAdapter::SendKey(std::wstring_view project_id, UINT virtual_key) {
    return impl_ && impl_->SendKey(project_id, virtual_key);
}

void XamlIslandAdapter::UpdateNativeScrollbarHover() {
    if (impl_) impl_->UpdateNativeScrollbarHover();
}

void XamlIslandAdapter::CollapseNativeScrollbar() {
    if (impl_) impl_->CollapseNativeScrollbar();
}

const std::wstring& XamlIslandAdapter::LastError() const {
    static const std::wstring empty;
    return impl_ ? impl_->LastError() : empty;
}

} // namespace aicli::terminal
