#include "terminal/terminal_working_detector.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <utility>

namespace aicli::terminal {

std::optional<AgentTitleProtocol> WorkingProtocolForSession(
    std::wstring_view type,
    std::wstring_view command) {
    const auto equals = [](std::wstring_view value, std::wstring_view expected) {
        return value.size() == expected.size() &&
               _wcsnicmp(value.data(), expected.data(), value.size()) == 0;
    };
    if (equals(type, L"Claude")) return AgentTitleProtocol::Claude;
    if (equals(type, L"Codex")) return AgentTitleProtocol::Codex;

    const auto contains = [](std::wstring_view value, std::wstring_view needle) {
        return std::search(
                   value.begin(), value.end(), needle.begin(), needle.end(),
                   [](wchar_t left, wchar_t right) {
                       return std::towlower(left) == std::towlower(right);
                   }) != value.end();
    };
    if (contains(command, L"claude")) return AgentTitleProtocol::Claude;
    if (contains(command, L"codex")) return AgentTitleProtocol::Codex;
    return std::nullopt;
}

TerminalWorkingDetector::TerminalWorkingDetector(
    AgentTitleProtocol protocol,
    StateCallback callback)
    : protocol_(protocol), callback_(std::move(callback)) {}

void TerminalWorkingDetector::Observe(std::wstring_view output) {
    if (output.empty()) return;
    std::scoped_lock lock(mutex_);
    pending_.append(output);
    ParseTerminalTitles();
}

bool TerminalWorkingDetector::TitleShowsWorking(std::wstring_view title) const {
    const std::wstring_view frames = protocol_ == AgentTitleProtocol::Claude
        ? std::wstring_view{L"⠂⠐"}
        : std::wstring_view{L"⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"};
    return title.find_first_of(frames) != std::wstring_view::npos;
}

bool TerminalWorkingDetector::TitleShowsIdle(std::wstring_view title) const {
    // Claude first publishes a plain shell title, then uses the star marker
    // for its stable idle state ("✳ Claude Code" or a conversation title).
    if (protocol_ == AgentTitleProtocol::Claude) {
        return title.find(L'✳') != std::wstring_view::npos;
    }
    // Codex uses an ordinary non-spinner project title while idle.
    return !TitleShowsWorking(title);
}

void TerminalWorkingDetector::Publish(bool working) {
    if (working_.has_value() && *working_ == working) return;
    working_ = working;
    if (callback_) callback_(working);
}

void TerminalWorkingDetector::ParseTerminalTitles() {
    for (;;) {
        const size_t start = pending_.find(L"\x1b]");
        if (start == std::wstring::npos) {
            if (pending_.size() > 1) {
                const bool keep_escape = pending_.back() == L'\x1b';
                pending_.erase(0, pending_.size() - (keep_escape ? 1u : 0u));
            }
            return;
        }
        if (start > 0) pending_.erase(0, start);

        const size_t separator = pending_.find(L';', 2);
        if (separator == std::wstring::npos) {
            if (pending_.size() > 64) pending_.erase(0, 2);
            return;
        }
        if (separator > 16) {
            pending_.erase(0, 2);
            continue;
        }

        const size_t bell = pending_.find(L'\x07', separator + 1);
        const size_t string_terminator = pending_.find(L"\x1b\\", separator + 1);
        size_t end = std::wstring::npos;
        size_t terminator_length = 0;
        if (bell != std::wstring::npos &&
            (string_terminator == std::wstring::npos || bell < string_terminator)) {
            end = bell;
            terminator_length = 1;
        } else if (string_terminator != std::wstring::npos) {
            end = string_terminator;
            terminator_length = 2;
        }
        if (end == std::wstring::npos) {
            if (pending_.size() > 4096) pending_.erase(0, 2);
            return;
        }

        const std::wstring_view command{pending_.data() + 2, separator - 2};
        if (command == L"0" || command == L"2") {
            const std::wstring_view title{
                pending_.data() + separator + 1, end - separator - 1};
            const bool working = TitleShowsWorking(title);
            const bool idle = TitleShowsIdle(title);
            // Startup may publish unrelated titles or transient animation.
            // The first protocol-specific idle title is the ready boundary.
            if (!ready_seen_) {
                if (idle) ready_seen_ = true;
                Publish(false);
            } else if (working) {
                Publish(true);
            } else if (idle) {
                Publish(false);
            }
        }
        pending_.erase(0, end + terminator_length);
    }
}

} // namespace aicli::terminal
