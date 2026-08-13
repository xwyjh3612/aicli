#pragma once

#include <windows.h>

#include <array>

struct ITaskbarList3;

namespace aicli::ui {

enum class TaskbarWorkingStyle {
    ProgressBar,
    OverlaySpinner,
};

// Owns the selected Windows taskbar working indication. The progress style is
// animated by Explorer; the overlay style keeps the application icon intact
// and advances a small spinner in its lower-right corner on the UI thread.
class TaskbarWorkingIndicator {
public:
    TaskbarWorkingIndicator() = default;
    ~TaskbarWorkingIndicator();

    TaskbarWorkingIndicator(const TaskbarWorkingIndicator&) = delete;
    TaskbarWorkingIndicator& operator=(const TaskbarWorkingIndicator&) = delete;

    void Attach(HWND window);
    void SetStyle(TaskbarWorkingStyle style);
    void SetWorking(bool working);
    void OnTaskbarButtonCreated();
    void Detach();

private:
    static void CALLBACK OverlayTimerProc(HWND window, UINT message,
                                          UINT_PTR timer_id, DWORD time);

    bool EnsureInterface();
    bool EnsureOverlayFrames();
    void StartOverlayAnimation();
    void StopOverlayAnimation();
    void AdvanceOverlayFrame();
    void ClearTaskbarVisuals();
    void DestroyOverlayFrames();
    void Apply();

    HWND window_{};
    ITaskbarList3* taskbar_{};
    std::array<HICON, 8> overlay_frames_{};
    UINT_PTR overlay_timer_id_{};
    std::size_t overlay_frame_{};
    TaskbarWorkingStyle style_{TaskbarWorkingStyle::ProgressBar};
    bool working_{};
};

} // namespace aicli::ui
