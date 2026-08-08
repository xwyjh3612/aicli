#pragma once

namespace aicli::app {

// Returns 1 when the current process should continue, 0 when activation was
// forwarded to the packaged instance, and -1 on activation failure.
int EnsurePackagedActivation(const wchar_t* application_user_model_id,
                              const wchar_t* arguments);

} // namespace aicli::app
