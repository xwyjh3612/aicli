#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

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

// Parses the OSC 0/2 titles published by Codex and Claude.  Both front-ends
// consume this same semantic Working/Idle signal; only their rendering differs.
class TerminalWorkingDetector final {
public:
    using StateCallback = std::function<void(bool working)>;

    TerminalWorkingDetector(AgentTitleProtocol protocol, StateCallback callback);

    void Observe(std::wstring_view output);

private:
    bool TitleShowsWorking(std::wstring_view title) const;
    bool TitleShowsIdle(std::wstring_view title) const;
    void Publish(bool working);
    void ParseTerminalTitles();

    AgentTitleProtocol protocol_{};
    StateCallback callback_;
    std::wstring pending_;
    std::optional<bool> working_;
    bool ready_seen_{};
    std::mutex mutex_;
};

} // namespace aicli::terminal
