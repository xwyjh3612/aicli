#pragma once

#include <windows.h>

namespace aicli::app {
// Execution-state requests belong to their calling thread. Update this object
// on the UI thread, including when the last task stops or the app shuts down.
class KeepAwakeRequest {
public:
    using SetState = EXECUTION_STATE(WINAPI*)(EXECUTION_STATE);
    explicit KeepAwakeRequest(SetState set_state = ::SetThreadExecutionState)
        : set_state_(set_state) {}

    bool Update(bool required) noexcept {
        if (required == active_) return true;
        if (required) {
            if (!set_state_(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)) return false;
            active_ = true;
        } else {
            // Clearing ES_CONTINUOUS alone can expose an already-expired idle
            // timer. Pulse the system timer first, then release our request so
            // Windows waits its full configured sleep interval from now.
            if (!set_state_(ES_SYSTEM_REQUIRED)) return false;
            if (!set_state_(ES_CONTINUOUS)) return false;
            active_ = false;
        }
        return true;
    }

private:
    SetState set_state_;
    bool active_{};
};
}
