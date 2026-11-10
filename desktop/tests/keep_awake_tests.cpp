#include "app/keep_awake.h"
#include <iostream>
#include <stdexcept>

namespace {
// Model Windows' independent idle timer and persistent execution-state request.
// Releasing the request alone must not reset an already-expired timer.
int idle_seconds{};
bool required{};
bool display_touched{};
bool fail_next{};
constexpr int kSleepAfter = 300;
EXECUTION_STATE WINAPI SetState(EXECUTION_STATE flags) {
    if (fail_next) { fail_next = false; return 0; }
    display_touched |= (flags & ES_DISPLAY_REQUIRED) != 0;
    if (flags & ES_SYSTEM_REQUIRED) idle_seconds = 0;
    if (flags & ES_CONTINUOUS) required = (flags & ES_SYSTEM_REQUIRED) != 0;
    return ES_CONTINUOUS;
}
bool CanSleep() { return !required && idle_seconds >= kSleepAfter; }
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        aicli::app::KeepAwakeRequest request(SetState);
        Require(request.Update(true), "Task should acquire wake request");
        idle_seconds = 3600;
        Require(!CanSleep(), "Long-running task should prevent idle sleep");
        request.Update(true); // Another task ends, but at least one still works.
        Require(!CanSleep(), "Other running tasks must keep the request active");
        Require(request.Update(false), "Last task should release wake request");
        Require(!required && idle_seconds == 0, "Last task must restart the system idle interval");
        idle_seconds = kSleepAfter - 1;
        request.Update(false);
        Require(idle_seconds == kSleepAfter - 1 && !CanSleep(), "Idle refresh must not extend the timeout");
        ++idle_seconds;
        Require(CanSleep(), "System may sleep after its full configured timeout");
        Require(!display_touched, "System sleep control must not force the screen on");
        fail_next = true;
        Require(!request.Update(true), "Failed wake request must be reported");
        Require(request.Update(true) && required, "Failed acquisition must be retryable");
        idle_seconds = 900;
        fail_next = true;
        Require(!request.Update(false) && required, "Do not release before successfully resetting idle time");
        Require(request.Update(false) && idle_seconds == 0 && !required, "Release must be retryable");
        std::cout << "Keep-awake idle timer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
