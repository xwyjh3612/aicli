#include "agent/terminal_stream_publisher.h"
#include "agent/terminal_wire_codec.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

std::wstring Snapshot(int first, int count) {
    std::wstring value;
    for (int index = 0; index < count; ++index) {
        if (!value.empty()) value.push_back(L'\n');
        wchar_t line[32]{};
        swprintf_s(line, L"line-%04d", first + index);
        value.append(line);
    }
    return value;
}

} // namespace

int main() {
    using aicli::agent::TerminalStreamPublisher;

    TerminalStreamPublisher publisher;
    const std::wstring project = L"terminal-stream-test";
    publisher.BeginSession(project, 32, 120);
    publisher.PublishOutput(project, L"old-vt-data");
    publisher.PublishSnapshot(project, Snapshot(0, 1000),
                              publisher.CurrentSequence(project));
    publisher.PublishOutput(project, L"new-vt-data");

    bool ok = true;
    const auto replay = publisher.Replay(project, L"", 0);
    ok &= Check(replay.events.size() == 3,
                "bootstrap contains reset, snapshot and one new VT frame");
    if (replay.events.size() >= 3) {
        const auto reset = nlohmann::json::parse(replay.events[0]);
        const auto bootstrap = nlohmann::json::parse(replay.events[1]);
        const auto data = nlohmann::json::parse(replay.events[2]);
        ok &= Check(reset.value("type", "") == "terminal.reset",
                    "first event is terminal.reset");
        ok &= Check(reset.value("baseSequence", 0ULL) == 1,
                    "reset begins at rendered snapshot sequence");
        ok &= Check(bootstrap.value("type", "") == "terminal.bootstrap",
                    "second event is terminal.bootstrap");
        const auto& payload = bootstrap.at("payload");
        ok &= Check(payload.at("lines").size() == 200,
                    "bootstrap sends exactly the final 200 rows");
        ok &= Check(payload.at("lines").front() == "line-0800" &&
                        payload.at("lines").back() == "line-0999",
                    "bootstrap contains the final rendered rows");
        ok &= Check(data.value("type", "") == "terminal.data" &&
                        data.at("payload").value("data", "") == "new-vt-data",
                    "only VT produced after the snapshot is replayed");

        const auto token = payload.value("historyToken", "");
        const auto history = publisher.History(
            project, std::wstring(token.begin(), token.end()), 800, 200);
        ok &= Check(history.found && history.lines.size() == 200,
                    "older rendered history page is retained");
        ok &= Check(history.start_line == 600 && history.has_more,
                    "history page reports the next stable offset");
        ok &= Check(history.lines.front() == "line-0600" &&
                        history.lines.back() == "line-0799",
                    "history page returns the preceding 200 rows");
    }

    const auto resumed = publisher.Replay(project, replay.session_id, 1);
    ok &= Check(resumed.events.size() == 1,
                "same-session reconnect receives only the missing increment");
    if (!resumed.events.empty()) {
        const auto data = nlohmann::json::parse(resumed.events.front());
        ok &= Check(data.value("sequence", 0ULL) == 2,
                    "incremental replay preserves terminal sequence numbers");
    }

    std::vector<std::string> wire_events;
    for (int frame = 0; frame < 80; ++frame) {
        wire_events.push_back(
            R"({"conversationId":"terminal-stream-test","event_type":"terminal.data","payload":{"encoding":"utf8","data":"\u001b[2K\r\u001b[32mWorking...\u001b[0m"},"sequence":)" +
            std::to_string(frame + 1) +
            R"(,"terminalSessionId":"session-test","type":"terminal.data"})");
    }
    const auto packed = aicli::agent::wire::PackTerminalEvents(wire_events);
    std::vector<std::string> unpacked;
    ok &= Check(!packed.empty(), "packed terminal batch is produced");
    ok &= Check(aicli::agent::wire::UnpackTerminalEvents(packed, unpacked),
                "packed terminal batch can be decoded");
    ok &= Check(unpacked == wire_events,
                "packed terminal batch preserves every JSON event exactly");
    std::size_t original_wire_bytes = 0;
    for (const auto& event : wire_events) original_wire_bytes += event.size();
    ok &= Check(packed.size() * 4 < original_wire_bytes,
                "repeated TUI repaints compress to less than one quarter");
    if (!packed.empty()) {
        std::cout << "TUI fixture bytes: " << original_wire_bytes
                  << " -> " << packed.size() << '\n';
    }
    if (!packed.empty()) {
        auto damaged = packed;
        damaged[0] = 'X';
        ok &= Check(!aicli::agent::wire::UnpackTerminalEvents(damaged, unpacked),
                    "invalid terminal packet is rejected");
        damaged = packed;
        damaged.resize(damaged.size() / 2);
        ok &= Check(!aicli::agent::wire::UnpackTerminalEvents(damaged, unpacked),
                    "truncated terminal packet is rejected");
    }

    std::mt19937 random{0x41c11u};
    for (int round = 0; round < 160 && ok; ++round) {
        std::vector<std::string> generated;
        const int count = 1 + static_cast<int>(random() % 12);
        for (int event_index = 0; event_index < count; ++event_index) {
            const std::size_t length = 1 + random() % 4096;
            std::string value;
            value.reserve(length + 32);
            while (value.size() < length) {
                if ((random() & 3u) == 0) {
                    value.append("\\u001b[2K\\rWorking...");
                } else {
                    value.push_back(static_cast<char>(' ' + random() % 95));
                }
            }
            value.resize(length);
            generated.push_back(std::move(value));
        }
        const auto random_packet =
            aicli::agent::wire::PackTerminalEvents(generated);
        std::vector<std::string> random_decoded;
        ok &= Check(!random_packet.empty() &&
                        aicli::agent::wire::UnpackTerminalEvents(
                            random_packet, random_decoded) &&
                        random_decoded == generated,
                    "mixed terminal payload round-trip is lossless");
    }

    if (ok) std::cout << "AI CLI terminal stream tests passed.\n";
    return ok ? 0 : 1;
}
