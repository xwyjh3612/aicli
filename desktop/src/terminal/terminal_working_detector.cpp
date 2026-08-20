#include "terminal/terminal_working_detector.h"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <utility>

namespace aicli::terminal {

namespace {

constexpr wchar_t kEscape = L'\x1b';

bool IsControlStringIntroducer(wchar_t value) {
    return value == L']' || value == L'P' || value == L'^' ||
           value == L'_' || value == L'X';
}

bool IsCsiFinal(wchar_t value) {
    return value >= 0x40 && value <= 0x7e;
}

} // namespace

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
    std::optional<AgentTitleProtocol> protocol,
    StateCallback callback,
    std::chrono::milliseconds activity_lease)
    : protocol_(protocol), callback_(std::move(callback)),
      ready_seen_(!protocol.has_value()),
      activity_lease_(std::max(activity_lease, std::chrono::milliseconds{1})) {
    activity_thread_ = std::thread([this] { ActivityLoop(); });
}

TerminalWorkingDetector::~TerminalWorkingDetector() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    activity_cv_.notify_all();
    if (activity_thread_.joinable()) activity_thread_.join();
}

void TerminalWorkingDetector::Observe(std::wstring_view output) {
    if (output.empty()) return;
    std::unique_lock lock(mutex_);
    const bool activity = ObserveActivity(output);
    const bool active_before_titles = title_working_ || activity_active_;
    pending_.append(output);
    const TitleSignal signal = ParseTerminalTitles();

    // An explicit idle title is authoritative.  In particular, a prompt
    // repaint in the same ConPTY batch must not immediately re-arm the
    // activity fallback after Claude has published its idle marker.
    if (signal == TitleSignal::Idle) {
        title_working_ = false;
        if (active_before_titles) fallback_armed_ = false;
        activity_active_ = false;
        activity_cv_.notify_all();
        return;
    }

    // Do not infer work from startup output until the front-end has published
    // its first protocol-specific idle title.  This prevents PowerShell and
    // the CLI's initial screen construction from lighting the row.
    if (signal == TitleSignal::Working) {
        title_working_ = true;
        activity_active_ = false;
        Publish(true);
        activity_cv_.notify_all();
        return;
    }

    const bool fallback_activity = signal == TitleSignal::Activity || activity;
    if (!ready_seen_ || !fallback_armed_ || !fallback_activity) return;

    activity_active_ = true;
    activity_deadline_ = std::chrono::steady_clock::now() + activity_lease_;
    Publish(true);
    activity_cv_.notify_all();
}

void TerminalWorkingDetector::ObserveInput(std::wstring_view input) {
    if (input.empty()) return;
    std::unique_lock lock(mutex_);

    bool submitted = false;
    bool cancelled = false;
    bool editing = false;
    if (input.starts_with(L"\x1b[200~")) {
        // Bracketed paste can contain CR characters that are part of the
        // pasted text. The real Enter key arrives separately.
        editing = true;
    } else {
        if (input.front() == L'\x1b') {
            const wchar_t final = input.back();
            const bool navigation_key = input.size() == 1 ||
                final == L'A' || final == L'B' || final == L'C' ||
                final == L'D' || final == L'H' || final == L'F' ||
                final == L'~';
            const bool alt_character = input.size() >= 2 &&
                input[1] != L'[' && input[1] != L'O' && input[1] >= L' ';
            if (!navigation_key && !alt_character) {
                // Terminal query responses (DA/DSR, cursor reports, etc.) are
                // written to the same connection but are not user editing.
                return;
            }
            editing = true;
        }
        for (const wchar_t value : input) {
            if (value == L'\x03') {
                cancelled = true;
            } else if (value == L'\r' || value == L'\n') {
                submitted = true;
            } else if (value == L'\b' || value == L'\x7f' || value == L'\t' ||
                       value >= L' ') {
                editing = true;
            }
        }
    }

    if (cancelled) {
        fallback_armed_ = false;
        title_working_ = false;
        activity_active_ = false;
        if (working_.value_or(false)) Publish(false);
        activity_cv_.notify_all();
        return;
    }
    if (submitted) {
        // The startup command is written through the bridge without being
        // observed, so an observed Enter is a real user submission.  Treat
        // it as a second ready boundary: if Claude or Codex changes its idle
        // title protocol, meaningful output can still drive the fallback.
        ready_seen_ = true;
        fallback_armed_ = true;
        activity_active_ = false;
        activity_cv_.notify_all();
        return;
    }
    if (editing) {
        // Text entry may cause a full TUI repaint. Close the fallback gate
        // before that echo arrives so merely composing a prompt never spins.
        fallback_armed_ = false;
        if (!title_working_) {
            activity_active_ = false;
            if (working_.value_or(false)) Publish(false);
        }
        activity_cv_.notify_all();
    }
}

bool TerminalWorkingDetector::TitleShowsWorking(std::wstring_view title) const {
    if (!protocol_) return false;
    const std::wstring_view frames = *protocol_ == AgentTitleProtocol::Claude
        // Claude Code changed its title animation in 2.1.x.  Keep the old
        // braille frames as well so older installed versions continue to
        // work.
        ? std::wstring_view{L"⠂⠐◐◑"}
        : std::wstring_view{L"⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"};
    return title.find_first_of(frames) != std::wstring_view::npos;
}

bool TerminalWorkingDetector::TitleShowsIdle(std::wstring_view title) const {
    if (!protocol_) return false;
    // Claude first publishes a plain shell title, then uses the star marker
    // for its stable idle state ("✳ Claude Code" or a conversation title).
    if (*protocol_ == AgentTitleProtocol::Claude) {
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

bool TerminalWorkingDetector::ObserveActivity(std::wstring_view output) {
    activity_pending_.append(output);
    bool meaningful = false;
    size_t index = 0;

    while (index < activity_pending_.size()) {
        const wchar_t value = activity_pending_[index];
        if (value == kEscape) {
            // Keep an incomplete escape sequence for the next ConPTY batch;
            // otherwise the text of a split OSC title could look like user
            // output and spuriously start the spinner.
            if (index + 1 >= activity_pending_.size()) break;
            const wchar_t introducer = activity_pending_[index + 1];
            if (introducer == L'[') {
                size_t end = index + 2;
                while (end < activity_pending_.size() &&
                       !IsCsiFinal(activity_pending_[end])) {
                    ++end;
                }
                if (end >= activity_pending_.size()) break;
                index = end + 1;
                continue;
            }
            if (IsControlStringIntroducer(introducer)) {
                size_t end = index + 2;
                bool terminated = false;
                while (end < activity_pending_.size()) {
                    if (activity_pending_[end] == L'\x07') {
                        ++end;
                        terminated = true;
                        break;
                    }
                    if (activity_pending_[end] == kEscape &&
                        end + 1 < activity_pending_.size() &&
                        activity_pending_[end + 1] == L'\\') {
                        end += 2;
                        terminated = true;
                        break;
                    }
                    ++end;
                }
                if (!terminated) break;
                index = end;
                continue;
            }
            // Other ESC sequences may contain intermediate bytes, for example
            // ESC ( B when selecting a character set.  Consume the complete
            // sequence so its final byte is not mistaken for visible output.
            size_t end = index + 1;
            while (end < activity_pending_.size() &&
                   activity_pending_[end] >= 0x20 &&
                   activity_pending_[end] <= 0x2f) {
                ++end;
            }
            if (end >= activity_pending_.size()) break;
            index = end + 1;
            continue;
        }
        if (value == 0x9b) {
            size_t end = index + 1;
            while (end < activity_pending_.size() &&
                   !IsCsiFinal(activity_pending_[end])) {
                ++end;
            }
            if (end >= activity_pending_.size()) break;
            index = end + 1;
            continue;
        }

        // C0 controls, DEL and whitespace are terminal plumbing rather than
        // evidence that the agent is producing work.  Any other character is
        // treated as visible activity, including Unicode box-drawing glyphs.
        const bool c1_control = value >= 0x80 && value <= 0x9f;
        if (value >= 0x20 && value != 0x7f && !c1_control &&
            !std::iswspace(value)) {
            meaningful = true;
        }
        ++index;
    }

    if (index > 0) activity_pending_.erase(0, index);
    return meaningful;
}

TerminalWorkingDetector::TitleSignal
TerminalWorkingDetector::ParseTerminalTitles() {
    TitleSignal signal = TitleSignal::None;
    if (!protocol_) {
        pending_.clear();
        return signal;
    }
    for (;;) {
        const size_t start = pending_.find(L"\x1b]");
        if (start == std::wstring::npos) {
            if (pending_.size() > 1) {
                const bool keep_escape = pending_.back() == L'\x1b';
                pending_.erase(0, pending_.size() - (keep_escape ? 1u : 0u));
            }
            return signal;
        }
        if (start > 0) pending_.erase(0, start);

        const size_t separator = pending_.find(L';', 2);
        if (separator == std::wstring::npos) {
            if (pending_.size() > 64) pending_.erase(0, 2);
            return signal;
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
            return signal;
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
                if (idle) {
                    ready_seen_ = true;
                    signal = TitleSignal::Idle;
                }
                Publish(false);
            } else if (idle) {
                signal = TitleSignal::Idle;
                Publish(false);
            } else if (working) {
                signal = TitleSignal::Working;
                Publish(true);
            } else if (signal == TitleSignal::None) {
                // Claude may change its title animation again.  Once startup
                // has reached a known idle state, an otherwise unknown OSC
                // 0/2 title is safe to treat as short-lived activity.  Codex
                // has no unknown case because every non-spinner title is idle.
                signal = TitleSignal::Activity;
            }
        }
        pending_.erase(0, end + terminator_length);
    }
}

void TerminalWorkingDetector::ActivityLoop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (!activity_active_) {
            activity_cv_.wait(lock, [this] {
                return stopping_ || activity_active_;
            });
            continue;
        }

        const auto deadline = activity_deadline_;
        if (activity_cv_.wait_until(lock, deadline, [this, deadline] {
                return stopping_ || !activity_active_ ||
                       activity_deadline_ != deadline;
            })) {
            continue;
        }

        if (activity_active_ &&
            std::chrono::steady_clock::now() >= activity_deadline_) {
            activity_active_ = false;
            if (!title_working_) Publish(false);
        }
    }
}

} // namespace aicli::terminal
