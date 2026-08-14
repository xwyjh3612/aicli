#include "agent/terminal_wire_codec.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace aicli::agent::wire {
namespace {

constexpr std::array<unsigned char, 4> kMagic{'A', 'I', 'C', 'L'};
constexpr unsigned char kVersion = 1;
constexpr unsigned char kFlagLz4 = 0x01;
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kCompressionThreshold = 256;
constexpr std::size_t kHashBits = 16;
constexpr std::size_t kHashSize = 1u << kHashBits;

std::uint32_t ReadU32(const unsigned char* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

void AppendU32(std::vector<unsigned char>& output, std::uint32_t value) {
    output.push_back(static_cast<unsigned char>(value));
    output.push_back(static_cast<unsigned char>(value >> 8));
    output.push_back(static_cast<unsigned char>(value >> 16));
    output.push_back(static_cast<unsigned char>(value >> 24));
}

std::uint32_t ReadSequence(const unsigned char* data) noexcept {
    std::uint32_t value{};
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::size_t HashSequence(std::uint32_t value) noexcept {
    return static_cast<std::size_t>((value * 2654435761u) >>
                                    (32 - kHashBits));
}

void AppendLength(std::vector<unsigned char>& output, std::size_t length) {
    while (length >= 255) {
        output.push_back(255);
        length -= 255;
    }
    output.push_back(static_cast<unsigned char>(length));
}

std::vector<unsigned char> CompressLz4(std::span<const unsigned char> input) {
    std::vector<unsigned char> output;
    if (input.empty()) return output;
    output.reserve(input.size() + input.size() / 255 + 16);

    std::array<std::int32_t, kHashSize> table{};
    table.fill(-1);
    const std::size_t size = input.size();
    const std::size_t last_match = size >= 4 ? size - 4 : 0;
    std::size_t anchor = 0;
    std::size_t position = 0;

    while (size >= 4 && position <= last_match) {
        const auto sequence = ReadSequence(input.data() + position);
        const auto hash = HashSequence(sequence);
        const auto reference_value = table[hash];
        table[hash] = static_cast<std::int32_t>(position);

        if (reference_value < 0) {
            ++position;
            continue;
        }
        const auto reference = static_cast<std::size_t>(reference_value);
        if (position <= reference || position - reference > 0xffff ||
            ReadSequence(input.data() + reference) != sequence) {
            ++position;
            continue;
        }

        std::size_t match_length = 4;
        while (position + match_length < size &&
               input[reference + match_length] == input[position + match_length]) {
            ++match_length;
        }

        const std::size_t literal_length = position - anchor;
        const std::size_t token_index = output.size();
        output.push_back(0);
        output[token_index] = static_cast<unsigned char>(
            std::min<std::size_t>(literal_length, 15) << 4);
        if (literal_length >= 15) AppendLength(output, literal_length - 15);
        output.insert(output.end(), input.begin() + anchor,
                      input.begin() + position);

        const auto offset = static_cast<std::uint16_t>(position - reference);
        output.push_back(static_cast<unsigned char>(offset));
        output.push_back(static_cast<unsigned char>(offset >> 8));

        const std::size_t encoded_match_length = match_length - 4;
        output[token_index] |= static_cast<unsigned char>(
            std::min<std::size_t>(encoded_match_length, 15));
        if (encoded_match_length >= 15) {
            AppendLength(output, encoded_match_length - 15);
        }

        position += match_length;
        anchor = position;
        // Prime the table near the end of the match. This materially improves
        // repeated ANSI repaint compression without changing the wire format.
        if (position >= 2 && position - 2 <= last_match) {
            table[HashSequence(ReadSequence(input.data() + position - 2))] =
                static_cast<std::int32_t>(position - 2);
        }
        if (position >= 1 && position - 1 <= last_match) {
            table[HashSequence(ReadSequence(input.data() + position - 1))] =
                static_cast<std::int32_t>(position - 1);
        }
    }

    const std::size_t literal_length = size - anchor;
    const std::size_t token_index = output.size();
    output.push_back(static_cast<unsigned char>(
        std::min<std::size_t>(literal_length, 15) << 4));
    if (literal_length >= 15) AppendLength(output, literal_length - 15);
    output.insert(output.end(), input.begin() + anchor, input.end());
    (void)token_index;
    return output;
}

bool ReadExtendedLength(std::span<const unsigned char> input,
                        std::size_t& position,
                        std::size_t& length) noexcept {
    for (;;) {
        if (position >= input.size()) return false;
        const auto value = input[position++];
        if (length > kMaximumPackedBodyBytes - value) return false;
        length += value;
        if (value != 255) return true;
    }
}

bool DecompressLz4(std::span<const unsigned char> input,
                   std::size_t expected_size,
                   std::vector<unsigned char>& output) {
    output.clear();
    if (expected_size > kMaximumPackedBodyBytes) return false;
    output.reserve(expected_size);
    std::size_t position = 0;

    while (position < input.size()) {
        const auto token = input[position++];
        std::size_t literal_length = token >> 4;
        if (literal_length == 15 &&
            !ReadExtendedLength(input, position, literal_length)) {
            return false;
        }
        if (literal_length > input.size() - position ||
            literal_length > expected_size - output.size()) {
            return false;
        }
        output.insert(output.end(), input.begin() + position,
                      input.begin() + position + literal_length);
        position += literal_length;
        if (position == input.size()) break;

        if (input.size() - position < 2) return false;
        const std::size_t offset = static_cast<std::size_t>(input[position]) |
                                   (static_cast<std::size_t>(input[position + 1]) << 8);
        position += 2;
        if (offset == 0 || offset > output.size()) return false;

        std::size_t match_length = token & 0x0f;
        if (match_length == 15 &&
            !ReadExtendedLength(input, position, match_length)) {
            return false;
        }
        if (match_length > kMaximumPackedBodyBytes - 4) return false;
        match_length += 4;
        if (match_length > expected_size - output.size()) return false;
        for (std::size_t index = 0; index < match_length; ++index) {
            output.push_back(output[output.size() - offset]);
        }
    }
    return output.size() == expected_size;
}

} // namespace

bool IsTerminalStreamEvent(std::string_view event_json) noexcept {
    return event_json.find("\"terminal.") != std::string_view::npos;
}

std::vector<unsigned char> PackTerminalEvents(
    const std::vector<std::string>& events) {
    if (events.empty() ||
        events.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }

    std::size_t body_size = 0;
    for (const auto& event : events) {
        if (event.size() > std::numeric_limits<std::uint32_t>::max() ||
            body_size > kMaximumPackedBodyBytes - 4 ||
            event.size() > kMaximumPackedBodyBytes - body_size - 4) {
            return {};
        }
        body_size += 4 + event.size();
    }
    if (body_size == 0 || body_size > kMaximumPackedBodyBytes) return {};

    std::vector<unsigned char> body;
    body.reserve(body_size);
    for (const auto& event : events) {
        AppendU32(body, static_cast<std::uint32_t>(event.size()));
        body.insert(body.end(), event.begin(), event.end());
    }

    auto compressed = body.size() >= kCompressionThreshold
        ? CompressLz4(body)
        : std::vector<unsigned char>{};
    const bool use_compression = !compressed.empty() &&
        compressed.size() + 8 < body.size();
    const auto& payload = use_compression ? compressed : body;

    std::vector<unsigned char> packet;
    packet.reserve(kHeaderBytes + payload.size());
    packet.insert(packet.end(), kMagic.begin(), kMagic.end());
    packet.push_back(kVersion);
    packet.push_back(use_compression ? kFlagLz4 : 0);
    packet.push_back(0);
    packet.push_back(0);
    AppendU32(packet, static_cast<std::uint32_t>(body.size()));
    AppendU32(packet, static_cast<std::uint32_t>(events.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

bool UnpackTerminalEvents(std::span<const unsigned char> packet,
                          std::vector<std::string>& events) {
    events.clear();
    if (packet.size() < kHeaderBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), packet.begin()) ||
        packet[4] != kVersion || (packet[5] & ~kFlagLz4) != 0 ||
        packet[6] != 0 || packet[7] != 0) {
        return false;
    }
    const std::size_t body_size = ReadU32(packet.data() + 8);
    const std::size_t event_count = ReadU32(packet.data() + 12);
    if (body_size == 0 || body_size > kMaximumPackedBodyBytes ||
        event_count == 0 || event_count > body_size / 4) {
        return false;
    }

    const auto payload = packet.subspan(kHeaderBytes);
    std::vector<unsigned char> decoded;
    if ((packet[5] & kFlagLz4) != 0) {
        if (!DecompressLz4(payload, body_size, decoded)) return false;
    } else {
        if (payload.size() != body_size) return false;
        decoded.assign(payload.begin(), payload.end());
    }

    events.reserve(event_count);
    std::size_t position = 0;
    for (std::size_t index = 0; index < event_count; ++index) {
        if (decoded.size() - position < 4) return false;
        const std::size_t length = ReadU32(decoded.data() + position);
        position += 4;
        if (length > decoded.size() - position) return false;
        events.emplace_back(reinterpret_cast<const char*>(decoded.data() + position),
                            length);
        position += length;
    }
    if (position != decoded.size()) {
        events.clear();
        return false;
    }
    return true;
}

} // namespace aicli::agent::wire
