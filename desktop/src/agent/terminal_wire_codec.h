#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aicli::agent::wire {

// Binary WebSocket envelope used only after the Android client explicitly
// requests packed-lz4-v1.  It contains the existing JSON events verbatim, so
// unpacking feeds exactly the same event stream into the existing VT renderer.
constexpr std::string_view kPackedTerminalTransport = "packed-lz4-v1";
constexpr std::size_t kMaximumPackedBodyBytes = 4 * 1024 * 1024;

bool IsTerminalStreamEvent(std::string_view event_json) noexcept;

std::vector<unsigned char> PackTerminalEvents(
    const std::vector<std::string>& events);

// Exposed for protocol tests and future desktop consumers.  The production
// desktop path only packs; Android performs the corresponding unpack.
bool UnpackTerminalEvents(std::span<const unsigned char> packet,
                          std::vector<std::string>& events);

} // namespace aicli::agent::wire
