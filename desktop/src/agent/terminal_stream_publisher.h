#pragma once

#include "agent/terminal_stream_protocol.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aicli::agent {

// Publishes the original ConPTY VT stream without flattening cursor movement,
// colors or in-place TUI repaint operations into plain text snapshots.
class TerminalStreamPublisher {
public:
    using BroadcastCallback =
        std::function<void(const std::wstring&, const std::string&)>;

    void SetBroadcastCallback(BroadcastCallback callback);

    void BeginSession(const std::wstring& project_id,
                      std::uint32_t rows = 32,
                      std::uint32_t columns = 120);
    void RemoveSession(const std::wstring& project_id);
    void PublishOutput(const std::wstring& project_id,
                       std::wstring_view output);
    void PublishResize(const std::wstring& project_id,
                       std::uint32_t rows,
                       std::uint32_t columns);

    TerminalReplayBatch Replay(const std::wstring& project_id,
                               const std::wstring& requested_session_id,
                               std::uint64_t after_sequence) const;

private:
    enum class ChunkType { Data, Resize };

    struct Chunk {
        ChunkType type{ChunkType::Data};
        std::uint64_t sequence{};
        std::uint32_t rows_before{};
        std::uint32_t columns_before{};
        std::uint32_t rows{};
        std::uint32_t columns{};
        std::string data;
        std::string event_json;
        std::size_t retained_bytes{};
    };

    struct StreamState {
        std::wstring session_id;
        std::uint64_t next_sequence{1};
        std::uint32_t rows{32};
        std::uint32_t columns{120};
        std::deque<Chunk> chunks;
        std::size_t retained_bytes{};
    };

    static std::wstring NewSessionId();
    static std::string ResetEvent(const std::wstring& project_id,
                                  const StreamState& state,
                                  std::uint64_t base_sequence,
                                  std::uint32_t rows,
                                  std::uint32_t columns,
                                  bool truncated,
                                  std::string_view reason);
    static std::string DataEvent(const std::wstring& project_id,
                                 const StreamState& state,
                                 std::uint64_t sequence,
                                 std::string_view data);
    static std::string ResizeEvent(const std::wstring& project_id,
                                   const StreamState& state,
                                   std::uint64_t sequence,
                                   std::uint32_t rows,
                                   std::uint32_t columns);
    static void Trim(StreamState& state);

    mutable std::mutex mutex_;
    std::unordered_map<std::wstring, StreamState> streams_;
    BroadcastCallback broadcast_callback_;
};

} // namespace aicli::agent
