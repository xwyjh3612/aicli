#pragma once

#include <windows.h>

struct ITaskbarList3;

namespace aicli::ui {

// Owns the Windows taskbar's native indeterminate progress state.  The shell
// animates this itself, so the application only switches it on when at least
// one supported AI CLI turn is working and clears it when all turns become idle.
class TaskbarWorkingIndicator {
public:
    TaskbarWorkingIndicator() = default;
    ~TaskbarWorkingIndicator();

    TaskbarWorkingIndicator(const TaskbarWorkingIndicator&) = delete;
    TaskbarWorkingIndicator& operator=(const TaskbarWorkingIndicator&) = delete;

    void Attach(HWND window);
    void SetWorking(bool working);
    void OnTaskbarButtonCreated();
    void Detach();

private:
    bool EnsureInterface();
    void Apply();

    HWND window_{};
    ITaskbarList3* taskbar_{};
    bool working_{};
};

} // namespace aicli::ui
