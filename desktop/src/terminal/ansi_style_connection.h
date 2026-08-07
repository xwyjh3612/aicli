#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.Terminal.TerminalConnection.h>

namespace aicli::terminal {

// Keeps the real Windows Terminal connection, ConPTY transport, keyboard input,
// and cursor behavior intact. It restores muted foreground styling that the
// Windows output path can drop from Codex labels and placeholders, and also
// normalizes SGR 2/90 without replacing any terminal interaction.
struct AnsiStyleConnection :
    winrt::implements<
        AnsiStyleConnection,
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection> {
    explicit AnsiStyleConnection(
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection inner,
        bool light_theme,
        std::function<void(std::wstring_view)> output_observer = {});
    ~AnsiStyleConnection();

    void Initialize(const winrt::Windows::Foundation::Collections::ValueSet& settings);
    void Start();
    void WriteInput(winrt::array_view<const char16_t> data);
    void Resize(uint32_t rows, uint32_t columns);
    void Close() noexcept;
    void SetLightTheme(bool light_theme) noexcept;

    winrt::event_token TerminalOutput(
        const winrt::Microsoft::Terminal::TerminalConnection::TerminalOutputHandler& handler);
    void TerminalOutput(const winrt::event_token& token) noexcept;

    winrt::event_token StateChanged(
        const winrt::Windows::Foundation::TypedEventHandler<
            winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection,
            winrt::Windows::Foundation::IInspectable>& handler);
    void StateChanged(const winrt::event_token& token) noexcept;

    winrt::guid SessionId() const noexcept;
    winrt::Microsoft::Terminal::TerminalConnection::ConnectionState State() const noexcept;

private:
    winrt::hstring TransformOutput(const winrt::hstring& output);

    winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection inner_{nullptr};
    winrt::event_token inner_output_token_{};
    winrt::event_token inner_state_token_{};
    std::mutex transform_mutex_;
    std::wstring pending_;
    bool dim_active_{false};
    bool codex_ui_seen_{false};
    std::atomic_bool user_input_active_{false};
    std::atomic_bool light_theme_{false};
    std::wstring placeholder_text_;
    std::function<void(std::wstring_view)> output_observer_;

    winrt::event<winrt::Microsoft::Terminal::TerminalConnection::TerminalOutputHandler>
        output_handlers_;
    winrt::event<winrt::Windows::Foundation::TypedEventHandler<
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection,
        winrt::Windows::Foundation::IInspectable>> state_handlers_;
};

}
