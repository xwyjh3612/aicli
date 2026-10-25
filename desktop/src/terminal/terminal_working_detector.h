#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace aicli::terminal {

enum class AgentTitleProtocol {
    Codex,
    Claude,
};

// Project types are editable, so detection considers both the displayed type
// and the actual launch command.
std::optional<AgentTitleProtocol> WorkingProtocolForSession(
    std::wstring_view type,
    std::wstring_view command);

// Codex 0.163 defaults to an alternate screen with no terminal scrollback.
// Keep AiCli sessions inline so the desktop scrollbar and Android touch
// scrolling can both reach the complete transcript.
std::wstring EnsureCodexInlineScrollback(
    std::wstring_view type,
    std::wstring_view command);

// Parses the OSC 0/2 titles published by Codex and Claude, then adds a generic
// short lease on meaningful terminal output after the user submits input. The
// generic path works for custom CLI types without knowing their name or UI.
class TerminalWorkingDetector final {
public:
    using StateCallback = std::function<void(bool working)>;

    TerminalWorkingDetector(
        std::optional<AgentTitleProtocol> protocol,
        StateCallback callback,
        std::chrono::milliseconds activity_lease =
            std::chrono::milliseconds{2500});
    ~TerminalWorkingDetector();

    TerminalWorkingDetector(const TerminalWorkingDetector&) = delete;
    TerminalWorkingDetector& operator=(const TerminalWorkingDetector&) = delete;

    void Observe(std::wstring_view output);
    void ObserveInput(std::wstring_view input);

private:
    enum class TitleSignal {
        None,
        Activity,
        Working,
        Idle,
    };

    bool TitleShowsWorking(std::wstring_view title) const;
    bool TitleShowsIdle(std::wstring_view title) const;
    bool ObserveActivity(std::wstring_view output);
    void Publish(bool working);
    TitleSignal ParseTerminalTitles();
    void ActivityLoop();

    std::optional<AgentTitleProtocol> protocol_;
    StateCallback callback_;
    std::wstring pending_;
    std::wstring activity_pending_;
    std::optional<bool> working_;
    bool ready_seen_{};
    bool fallback_armed_{};
    bool title_working_{};
    bool activity_active_{};
    bool stopping_{};
    std::chrono::milliseconds activity_lease_{};
    std::chrono::steady_clock::time_point activity_deadline_{};
    std::condition_variable activity_cv_;
    std::mutex mutex_;
    std::thread activity_thread_;
};

} // namespace aicli::terminal
