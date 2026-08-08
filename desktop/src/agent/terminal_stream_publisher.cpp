#include "agent/terminal_stream_publisher.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/text_utils.h"

namespace aicli::agent {
namespace {

constexpr std::size_t kMaximumRetainedBytes = 12 * 1024 * 1024;
constexpr std::size_t kMaximumRetainedChunks = 32768;

std::string Serialize(const nlohmann::json& value) {
    return value.dump(-1, ' ', false,
                      nlohmann::json::error_handler_t::replace);
}

} // namespace

void TerminalStreamPublisher::SetBroadcastCallback(
    BroadcastCallback callback) {
    std::scoped_lock lock(mutex_);
    broadcast_callback_ = std::move(callback);
}

void TerminalStreamPublisher::BeginSession(
    const std::wstring& project_id,
    std::uint32_t rows,
    std::uint32_t columns) {
    if (project_id.empty()) return;
    std::string event;
    BroadcastCallback broadcast;
    {
        std::scoped_lock lock(mutex_);
        StreamState state;
        state.session_id = NewSessionId();
        state.rows = std::max<std::uint32_t>(5, rows);
        state.columns = std::max<std::uint32_t>(20, columns);
        auto [iterator, inserted] = streams_.insert_or_assign(
            project_id, std::move(state));
        (void)inserted;
        event = ResetEvent(project_id, iterator->second, 0,
                           iterator->second.rows, iterator->second.columns,
                           false, "session-start");
        broadcast = broadcast_callback_;
    }
    if (broadcast) broadcast(project_id, event);
}

void TerminalStreamPublisher::RemoveSession(
    const std::wstring& project_id) {
    std::scoped_lock lock(mutex_);
    streams_.erase(project_id);
}

void TerminalStreamPublisher::PublishOutput(
    const std::wstring& project_id,
    std::wstring_view output) {
    if (project_id.empty() || output.empty()) return;
    const std::string utf8 = aicli::text::Utf8Text(output);
    if (utf8.empty()) return;

    std::string event;
    BroadcastCallback broadcast;
    {
        std::scoped_lock lock(mutex_);
        auto iterator = streams_.find(project_id);
        if (iterator == streams_.end()) return;
        auto& state = iterator->second;
        const auto sequence = state.next_sequence++;
        event = DataEvent(project_id, state, sequence, utf8);
        Chunk chunk;
        chunk.type = ChunkType::Data;
        chunk.sequence = sequence;
        chunk.rows_before = state.rows;
        chunk.columns_before = state.columns;
        chunk.rows = state.rows;
        chunk.columns = state.columns;
        chunk.data = utf8;
        chunk.event_json = event;
        chunk.retained_bytes = chunk.data.size() + chunk.event_json.size();
        state.retained_bytes += chunk.retained_bytes;
        state.chunks.push_back(std::move(chunk));
        Trim(state);
        broadcast = broadcast_callback_;
    }
    if (broadcast) broadcast(project_id, event);
}

void TerminalStreamPublisher::PublishResize(
    const std::wstring& project_id,
    std::uint32_t rows,
    std::uint32_t columns) {
    if (project_id.empty()) return;
    rows = std::max<std::uint32_t>(5, rows);
    columns = std::max<std::uint32_t>(20, columns);

    std::string event;
    BroadcastCallback broadcast;
    {
        std::scoped_lock lock(mutex_);
        auto iterator = streams_.find(project_id);
        if (iterator == streams_.end()) return;
        auto& state = iterator->second;
        if (state.rows == rows && state.columns == columns) return;
        const auto sequence = state.next_sequence++;
        event = ResizeEvent(project_id, state, sequence, rows, columns);
        Chunk chunk;
        chunk.type = ChunkType::Resize;
        chunk.sequence = sequence;
        chunk.rows_before = state.rows;
        chunk.columns_before = state.columns;
        chunk.rows = rows;
        chunk.columns = columns;
        chunk.event_json = event;
        chunk.retained_bytes = chunk.event_json.size();
        state.rows = rows;
        state.columns = columns;
        state.retained_bytes += chunk.retained_bytes;
        state.chunks.push_back(std::move(chunk));
        Trim(state);
        broadcast = broadcast_callback_;
    }
    if (broadcast) broadcast(project_id, event);
}

TerminalReplayBatch TerminalStreamPublisher::Replay(
    const std::wstring& project_id,
    const std::wstring& requested_session_id,
    std::uint64_t after_sequence) const {
    TerminalReplayBatch result;
    std::scoped_lock lock(mutex_);
    const auto iterator = streams_.find(project_id);
    if (iterator == streams_.end()) return result;

    const auto& state = iterator->second;
    result.session_id = state.session_id;
    result.last_sequence = state.next_sequence - 1;
    const std::uint64_t base_sequence = state.chunks.empty()
        ? result.last_sequence
        : state.chunks.front().sequence - 1;
    const bool same_session = !requested_session_id.empty() &&
                              requested_session_id == state.session_id;
    const bool can_resume = same_session &&
                            after_sequence >= base_sequence &&
                            after_sequence <= result.last_sequence;

    std::uint64_t replay_after = after_sequence;
    if (!can_resume) {
        const std::uint32_t reset_rows = state.chunks.empty()
            ? state.rows : state.chunks.front().rows_before;
        const std::uint32_t reset_columns = state.chunks.empty()
            ? state.columns : state.chunks.front().columns_before;
        result.events.push_back(ResetEvent(
            project_id, state, base_sequence, reset_rows, reset_columns,
            base_sequence != 0, same_session ? "history-truncated"
                                            : "session-bootstrap"));
        replay_after = base_sequence;
    }

    for (const auto& chunk : state.chunks) {
        if (chunk.sequence > replay_after) {
            result.events.push_back(chunk.event_json);
        }
    }
    return result;
}

std::wstring TerminalStreamPublisher::NewSessionId() {
    GUID value{};
    if (FAILED(CoCreateGuid(&value))) {
        return std::to_wstring(GetTickCount64());
    }
    wchar_t text[40]{};
    const int length = StringFromGUID2(value, text, ARRAYSIZE(text));
    if (length <= 1) return std::to_wstring(GetTickCount64());
    std::wstring result{text, static_cast<std::size_t>(length - 1)};
    if (result.size() >= 2 && result.front() == L'{' && result.back() == L'}') {
        result = result.substr(1, result.size() - 2);
    }
    return result;
}

std::string TerminalStreamPublisher::ResetEvent(
    const std::wstring& project_id,
    const StreamState& state,
    std::uint64_t base_sequence,
    std::uint32_t rows,
    std::uint32_t columns,
    bool truncated,
    std::string_view reason) {
    return Serialize({
        {"conversationId", aicli::text::Utf8Text(project_id)},
        {"event_type", "terminal.reset"},
        {"type", "terminal.reset"},
        {"terminalSessionId", aicli::text::Utf8Text(state.session_id)},
        {"baseSequence", base_sequence},
        {"payload", {
            {"rows", rows},
            {"columns", columns},
            {"truncated", truncated},
            {"reason", reason},
        }},
    });
}

std::string TerminalStreamPublisher::DataEvent(
    const std::wstring& project_id,
    const StreamState& state,
    std::uint64_t sequence,
    std::string_view data) {
    return Serialize({
        {"conversationId", aicli::text::Utf8Text(project_id)},
        {"event_type", "terminal.data"},
        {"type", "terminal.data"},
        {"terminalSessionId", aicli::text::Utf8Text(state.session_id)},
        {"sequence", sequence},
        {"payload", {{"encoding", "utf8"}, {"data", data}}},
    });
}

std::string TerminalStreamPublisher::ResizeEvent(
    const std::wstring& project_id,
    const StreamState& state,
    std::uint64_t sequence,
    std::uint32_t rows,
    std::uint32_t columns) {
    return Serialize({
        {"conversationId", aicli::text::Utf8Text(project_id)},
        {"event_type", "terminal.resize"},
        {"type", "terminal.resize"},
        {"terminalSessionId", aicli::text::Utf8Text(state.session_id)},
        {"sequence", sequence},
        {"payload", {{"rows", rows}, {"columns", columns}}},
    });
}

void TerminalStreamPublisher::Trim(StreamState& state) {
    while (!state.chunks.empty() &&
           (state.retained_bytes > kMaximumRetainedBytes ||
            state.chunks.size() > kMaximumRetainedChunks)) {
        state.retained_bytes -= state.chunks.front().retained_bytes;
        state.chunks.pop_front();
    }
}

} // namespace aicli::agent
