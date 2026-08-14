#pragma once

#include <windows.h>

#include <string>

namespace aicli::app {

enum class SingleInstanceResult {
    primary,
    activated_existing,
    existing_starting,
    error,
};

// Keeps one process alive for each distinct executable path. This lets the
// Qt/non-Qt and Debug/Release builds run side by side while preventing two
// copies of the exact same build from sharing one SQLite database.
class SingleInstanceGuard final {
public:
    SingleInstanceGuard() = default;
    ~SingleInstanceGuard();

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    SingleInstanceResult Acquire(std::wstring* error_message = nullptr);

private:
    HANDLE mutex_{};
    bool owns_mutex_{};
};

} // namespace aicli::app
