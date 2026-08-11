#include "agent/terminal_stream_publisher.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

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

    if (ok) std::cout << "AI CLI terminal stream tests passed.\n";
    return ok ? 0 : 1;
}
