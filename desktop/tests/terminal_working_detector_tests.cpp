#include "terminal/terminal_working_detector.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using aicli::terminal::AgentTitleProtocol;
using aicli::terminal::TerminalWorkingDetector;

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

std::wstring Title(std::wstring_view value, bool string_terminator = false) {
    std::wstring result = L"\x1b]2;";
    result.append(value);
    result.append(string_terminator ? L"\x1b\\" : L"\x07");
    return result;
}

class StateRecorder final {
public:
    void Push(bool working) {
        {
            std::scoped_lock lock(mutex_);
            states_.push_back(working);
        }
        cv_.notify_all();
    }

    bool WaitFor(std::vector<bool> expected,
                 std::chrono::milliseconds timeout = 1s) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return states_.size() >= expected.size();
        }) && states_ == expected;
    }

    std::vector<bool> Snapshot() const {
        std::scoped_lock lock(mutex_);
        return states_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<bool> states_;
};

bool TestProtocolSelection() {
    using aicli::terminal::WorkingProtocolForSession;
    bool ok = true;
    ok &= Check(WorkingProtocolForSession(L"Claude", L"custom") ==
                    AgentTitleProtocol::Claude,
                "Claude type selects Claude title protocol");
    ok &= Check(WorkingProtocolForSession(L"custom", L"CLAUDE --continue") ==
                    AgentTitleProtocol::Claude,
                "Claude command selects Claude title protocol case-insensitively");
    ok &= Check(WorkingProtocolForSession(L"Codex", L"custom") ==
                    AgentTitleProtocol::Codex,
                "Codex type selects Codex title protocol");
    ok &= Check(!WorkingProtocolForSession(L"Codex", L"opencode").has_value(),
                "OpenCode command uses the generic fallback even with Codex type");
    ok &= Check(!WorkingProtocolForSession(L"Shell", L"pwsh").has_value(),
                "unrelated sessions do not select a title protocol");
    return ok;
}

bool TestClaudeOldAndNewTitles() {
    StateRecorder recorder;
    TerminalWorkingDetector detector(
        AgentTitleProtocol::Claude,
        [&](bool working) { recorder.Push(working); }, 300ms);

    detector.Observe(Title(L"✳ Claude Code"));
    bool ok = Check(recorder.WaitFor({false}),
                    "Claude idle title establishes the ready boundary");

    detector.Observe(L"\x1b]2;⠂ Claude");
    detector.Observe(L" Code\x1b\\");
    ok &= Check(recorder.WaitFor({false, true}),
                "split legacy Claude working title is recognized");

    detector.Observe(Title(L"✳ Conversation"));
    ok &= Check(recorder.WaitFor({false, true, false}),
                "Claude idle title immediately clears legacy working state");

    detector.Observe(Title(L"◐ Claude Code"));
    ok &= Check(recorder.WaitFor({false, true, false, true}),
                "new Claude first working frame is recognized");
    detector.Observe(Title(L"✳ Conversation"));
    detector.Observe(Title(L"◑ Claude Code"));
    ok &= Check(recorder.WaitFor({false, true, false, true, false, true}),
                "new Claude second working frame is recognized");
    return ok;
}

bool TestCodexTitleAndStartupGuard() {
    StateRecorder recorder;
    TerminalWorkingDetector detector(
        AgentTitleProtocol::Codex,
        [&](bool working) { recorder.Push(working); }, 300ms);

    detector.Observe(L"PowerShell startup output");
    std::this_thread::sleep_for(50ms);
    bool ok = Check(recorder.Snapshot().empty(),
                    "startup output alone does not trigger the fallback");

    detector.Observe(L"prompt repaint" + Title(L"project-folder"));
    ok &= Check(recorder.WaitFor({false}),
                "ordinary Codex title establishes idle without startup fallback");
    detector.Observe(Title(L"⠋ Working"));
    ok &= Check(recorder.WaitFor({false, true}),
                "Codex braille title still triggers working state");
    detector.Observe(Title(L"project-folder"));
    ok &= Check(recorder.WaitFor({false, true, false}),
                "ordinary Codex title immediately restores idle state");
    return ok;
}

bool TestChangedProtocolStillUsesFallback() {
    constexpr auto lease = 250ms;

    StateRecorder claude_recorder;
    TerminalWorkingDetector claude(
        AgentTitleProtocol::Claude,
        [&](bool working) { claude_recorder.Push(working); }, lease);
    claude.Observe(L"startup output without a recognized idle title");
    std::this_thread::sleep_for(50ms);
    bool ok = Check(claude_recorder.Snapshot().empty(),
                    "changed Claude startup protocol remains guarded");
    claude.ObserveInput(L"question\r");
    claude.Observe(L"changed Claude output");
    ok &= Check(claude_recorder.WaitFor({true}),
                "real submission unlocks fallback when Claude idle protocol changes");
    ok &= Check(claude_recorder.WaitFor({true, false}, 1s),
                "changed Claude fallback expires normally");

    StateRecorder codex_recorder;
    TerminalWorkingDetector codex(
        AgentTitleProtocol::Codex,
        [&](bool working) { codex_recorder.Push(working); }, lease);
    codex.Observe(Title(L"project-folder"));
    ok &= Check(codex_recorder.WaitFor({false}),
                "Codex reaches its normal idle boundary");
    codex.ObserveInput(L"question\r");
    codex.Observe(L"prompt repaint" + Title(L"project-folder"));
    codex.Observe(L"changed Codex output");
    ok &= Check(codex_recorder.WaitFor({false, true}),
                "idle repaint does not block fallback when Codex protocol changes");
    ok &= Check(codex_recorder.WaitFor({false, true, false}, 1s),
                "changed Codex fallback expires normally");
    return ok;
}

bool TestActivityLeaseAndFiltering() {
    StateRecorder recorder;
    constexpr auto lease = 300ms;
    TerminalWorkingDetector detector(
        AgentTitleProtocol::Claude,
        [&](bool working) { recorder.Push(working); }, lease);

    detector.Observe(Title(L"✳ Claude Code"));
    bool ok = Check(recorder.WaitFor({false}),
                    "activity test reaches Claude idle state");

    detector.Observe(L"\x1b[?25l\x1b[2K\r\n\x1b[0m\x1b(B");
    std::this_thread::sleep_for(50ms);
    ok &= Check(recorder.Snapshot() == std::vector<bool>{false},
                "pure terminal controls do not count as activity");

    detector.ObserveInput(L"\r");
    detector.Observe(L"\x1b]2;unrecognized title");
    detector.Observe(L"\x1b\\");
    ok &= Check(recorder.WaitFor({false, true}),
                "unknown Claude title animation uses the fallback lease");
    detector.Observe(Title(L"✳ Claude Code"));
    ok &= Check(recorder.WaitFor({false, true, false}),
                "known Claude idle title clears unknown-title fallback");

    detector.ObserveInput(L"question\r");
    detector.Observe(L"\x1b[32mWorking...\x1b[0m\r");
    ok &= Check(recorder.WaitFor({false, true, false, true}),
                "visible terminal output starts the fallback lease");

    std::this_thread::sleep_for(100ms);
    detector.Observe(L"more output");
    std::this_thread::sleep_for(150ms);
    ok &= Check(recorder.Snapshot() ==
                    std::vector<bool>({false, true, false, true}),
                "continued terminal output renews the lease");
    ok &= Check(recorder.WaitFor({false, true, false, true, false}, 1s),
                "activity fallback stops after its renewed lease expires");
    return ok;
}

bool TestGenericCliSubmissionGate() {
    StateRecorder recorder;
    constexpr auto lease = 250ms;
    TerminalWorkingDetector detector(
        std::nullopt,
        [&](bool working) { recorder.Push(working); }, lease);

    detector.Observe(L"Gemini startup and prompt");
    detector.ObserveInput(L"hello");
    detector.Observe(L"hello");
    std::this_thread::sleep_for(50ms);
    bool ok = Check(recorder.Snapshot().empty(),
                    "generic CLI startup and typing echo stay idle");

    detector.ObserveInput(L"\r");
    detector.Observe(L"\x1b[2K\r");
    std::this_thread::sleep_for(50ms);
    ok &= Check(recorder.Snapshot().empty(),
                "terminal controls after submit are not meaningful output");

    detector.Observe(L"Gemini is working");
    ok &= Check(recorder.WaitFor({true}),
                "custom CLI output after Enter starts the generic fallback");
    detector.ObserveInput(L"next question");
    ok &= Check(recorder.WaitFor({true, false}),
                "starting the next input immediately clears generic fallback");

    detector.Observe(L"next question");
    std::this_thread::sleep_for(50ms);
    ok &= Check(recorder.Snapshot() == std::vector<bool>({true, false}),
                "input repaint cannot restart generic fallback");

    detector.ObserveInput(L"\r");
    detector.Observe(L"more work");
    ok &= Check(recorder.WaitFor({true, false, true}),
                "a later custom CLI submission can start fallback again");
    detector.ObserveInput(std::wstring(1, L'\x03'));
    ok &= Check(recorder.WaitFor({true, false, true, false}),
                "Ctrl+C immediately clears generic fallback");

    detector.ObserveInput(L"again\r");
    detector.Observe(L"working again");
    ok &= Check(recorder.WaitFor({true, false, true, false, true}),
                "generic fallback starts before navigation-key test");
    ok &= Check(recorder.WaitFor({true, false, true, false, true, false}, 1s),
                "generic fallback reaches idle before navigation-key test");
    detector.ObserveInput(L"\x1b[A");
    detector.Observe(L"history repaint");
    std::this_thread::sleep_for(50ms);
    ok &= Check(recorder.Snapshot() ==
                    std::vector<bool>({true, false, true, false, true, false}),
                "direction-key repaint cannot restart generic fallback");
    return ok;
}

bool TestEnhancedKeyboardSubmissionGate() {
    constexpr auto lease = 250ms;
    StateRecorder recorder;
    TerminalWorkingDetector detector(
        std::nullopt,
        [&](bool working) { recorder.Push(working); }, lease);

    detector.Observe(L"Opencode prompt");
    detector.ObserveInput(L"question");
    detector.Observe(L"question");
    std::this_thread::sleep_for(50ms);
    bool ok = Check(recorder.Snapshot().empty(),
                    "enhanced-keyboard text entry stays idle");

    detector.ObserveInput(L"\x1b[13u");
    detector.Observe(L"\x1b[2K\r");
    std::this_thread::sleep_for(50ms);
    ok &= Check(recorder.Snapshot().empty(),
                "Kitty Enter does not spin on control-only repaint");
    detector.Observe(L"Opencode is working");
    ok &= Check(recorder.WaitFor({true}),
                "Kitty CSI 13 u opens the generic fallback gate");
    ok &= Check(recorder.WaitFor({true, false}, 1s),
                "enhanced-keyboard fallback expires normally");

    StateRecorder combined_recorder;
    TerminalWorkingDetector combined(
        std::nullopt,
        [&](bool working) { combined_recorder.Push(working); }, lease);
    combined.ObserveInput(L"prompt text\x1b[13u");
    combined.Observe(L"combined-batch output");
    ok &= Check(combined_recorder.WaitFor({true}),
                "enhanced Enter is found inside a combined input batch");

    StateRecorder modified_recorder;
    TerminalWorkingDetector modified(
        std::nullopt,
        [&](bool working) { modified_recorder.Push(working); }, lease);
    modified.ObserveInput(L"\x1b[27;5;13~");
    modified.Observe(L"Opencode modified-key output");
    ok &= Check(modified_recorder.WaitFor({true}),
                "modifyOtherKeys Enter opens the generic fallback gate");
    modified.ObserveInput(L"\x1b[99;5u");
    ok &= Check(modified_recorder.WaitFor({true, false}),
                "enhanced-keyboard Ctrl+C clears the fallback immediately");
    return ok;
}

bool TestIdleOverridesActivityInSameBatch() {
    StateRecorder recorder;
    constexpr auto lease = 250ms;
    TerminalWorkingDetector detector(
        AgentTitleProtocol::Claude,
        [&](bool working) { recorder.Push(working); }, lease);

    detector.Observe(Title(L"✳ Claude Code"));
    detector.ObserveInput(L"question\r");
    detector.Observe(L"tool output");
    bool ok = Check(recorder.WaitFor({false, true}),
                    "visible output starts activity before idle override");

    detector.Observe(L"final answer" + Title(L"✳ Conversation") +
                     Title(L"plain shell title"));
    ok &= Check(recorder.WaitFor({false, true, false}),
                "explicit idle title wins over visible output in the same batch");
    std::this_thread::sleep_for(lease + 100ms);
    ok &= Check(recorder.Snapshot() ==
                    std::vector<bool>({false, true, false}),
                "idle override does not leave a stale lease armed");

    detector.Observe(Title(L"✳ Conversation") + Title(L"◐ Claude Code"));
    ok &= Check(recorder.WaitFor({false, true, false, true}),
                "later explicit working title wins within the same batch");
    detector.Observe(Title(L"✳ Conversation"));
    ok &= Check(recorder.WaitFor({false, true, false, true, false}),
                "later explicit idle title still stops working immediately");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= TestProtocolSelection();
    ok &= TestClaudeOldAndNewTitles();
    ok &= TestCodexTitleAndStartupGuard();
    ok &= TestChangedProtocolStillUsesFallback();
    ok &= TestActivityLeaseAndFiltering();
    ok &= TestIdleOverridesActivityInSameBatch();
    ok &= TestGenericCliSubmissionGate();
    ok &= TestEnhancedKeyboardSubmissionGate();
    if (ok) std::cout << "AI CLI terminal working detector tests passed.\n";
    return ok ? 0 : 1;
}
