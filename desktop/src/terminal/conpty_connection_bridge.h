#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.Terminal.TerminalConnection.h>

namespace aicli::terminal {

struct ConPtyConnectionBridge :
    winrt::implements<
        ConPtyConnectionBridge,
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection> {
    ConPtyConnectionBridge(
        std::wstring working_directory,
        std::wstring startup_command,
        std::function<void(std::wstring_view)> output_observer = {});
    ~ConPtyConnectionBridge();

    void Initialize(const winrt::Windows::Foundation::Collections::ValueSet& settings);
    void Start();
    void WriteInput(winrt::array_view<const char16_t> data);
    void Resize(uint32_t rows, uint32_t columns);
    void SetLightTheme(bool light_theme) noexcept;
    void RefreshViewport();
    void Close() noexcept;

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
    bool CreateProcessAttachedToConPty();
    void ReadLoop();
    void EmitOutput(std::wstring_view output) noexcept;
    std::wstring TransformDisplayOutput(std::wstring_view output);
    void SetState(winrt::Microsoft::Terminal::TerminalConnection::ConnectionState state) noexcept;
    std::wstring BuildStartupInput() const;

    std::wstring working_directory_;
    std::wstring startup_command_;
    std::wstring shell_command_line_;
    std::function<void(std::wstring_view)> output_observer_;
    uint32_t rows_{32};
    uint32_t columns_{120};
    DWORD pseudo_console_flags_{};
    winrt::guid session_id_{};

    HPCON pseudo_console_{};
    // Windows Terminal 1.24 uses one full-duplex pipe for both directions.
    // The server end stays here; the client end is handed to ConPTY as both
    // its input and output handle.
    HANDLE pipe_{};
    HANDLE process_{};
    std::thread reader_;
    std::mutex lifecycle_mutex_;
    std::mutex input_mutex_;
    std::atomic<bool> closing_{false};
    std::atomic<bool> light_theme_{false};
    std::atomic<uint64_t> resize_generation_{0};
    std::wstring display_pending_;
    std::atomic<winrt::Microsoft::Terminal::TerminalConnection::ConnectionState> state_{
        winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::NotConnected};

    winrt::event<winrt::Microsoft::Terminal::TerminalConnection::TerminalOutputHandler> output_handlers_;
    winrt::event<winrt::Windows::Foundation::TypedEventHandler<
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection,
        winrt::Windows::Foundation::IInspectable>> state_handlers_;
};

}
