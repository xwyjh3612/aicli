#pragma once

#include <cstdint>
#include <cstddef>
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

// One immutable page from the rendered terminal image used to bootstrap a
// phone. The token pins the exact image that was shown during the WebSocket
// handshake, so output produced while the user scrolls cannot shift the older
// rows underneath a pagination request.
struct TerminalHistoryPage {
    std::wstring token;
    std::vector<std::string> lines;
    std::size_t start_line{};
    std::size_t total_lines{};
    bool has_more{};
    bool found{};
};

} // namespace aicli::agent
