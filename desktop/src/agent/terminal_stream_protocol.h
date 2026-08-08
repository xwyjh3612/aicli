#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aicli::agent {

// A stable snapshot of the events needed to resume one terminal stream.
// Events are already serialized as WebSocket JSON text frames.
struct TerminalReplayBatch {
    std::vector<std::string> events;
    std::wstring session_id;
    std::uint64_t last_sequence{};
};

} // namespace aicli::agent
