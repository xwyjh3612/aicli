#pragma once

#include <windows.h>

#include <array>

struct ITaskbarList3;

namespace aicli::ui {

enum class TaskbarWorkingStyle {
    ProgressBar,
    OverlaySpinner,
    IconDots,
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
    void SetIconDotResources(HINSTANCE instance, UINT first_resource_id);
    void SetStyle(TaskbarWorkingStyle style);
    void SetWorking(bool working);
    void OnTaskbarButtonCreated();
    void Detach();

private:
    static void CALLBACK AnimationTimerProc(HWND window, UINT message,
                                            UINT_PTR timer_id, DWORD time);

    bool EnsureInterface();
    bool EnsureOverlayFrames();
    bool EnsureIconDotFrames();
    void StartAnimation();
    void StopAnimation();
    void ScheduleAnimationFrame();
    void AdvanceAnimationFrame();
    void ApplyIconDotFrame();
    void RestoreWindowIcons();
    void ClearTaskbarVisuals();
    void DestroyOverlayFrames();
    void DestroyIconDotFrames();
    void Apply();

    HWND window_{};
    ITaskbarList3* taskbar_{};
    std::array<HICON, 8> overlay_frames_{};
    std::array<HICON, 3> icon_dot_large_frames_{};
    std::array<HICON, 3> icon_dot_small_frames_{};
    HINSTANCE icon_dot_instance_{};
    UINT icon_dot_first_resource_id_{};
    HICON original_large_icon_{};
    HICON original_small_icon_{};
    UINT_PTR animation_timer_id_{};
    std::size_t animation_frame_{};
    TaskbarWorkingStyle style_{TaskbarWorkingStyle::ProgressBar};
    bool working_{};
    bool icon_dot_applied_{};
};

} // namespace aicli::ui
