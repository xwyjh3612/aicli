#pragma once

#include <windows.h>

#include <optional>

#include "ui/theme.h"

namespace aicli::ui {

struct CaptionButtonIds {
    int minimize{};
    int maximize{};
    int close{};
};

// Owns the custom title bar's transient interaction state and all DWM/frame
// behavior.  The main window keeps command routing, while this class keeps
// caption geometry, hover/press state, hit testing and rendering together.
class WindowChrome {
public:
    explicit WindowChrome(CaptionButtonIds ids) noexcept;

    bool active() const noexcept { return active_; }
    bool SetActive(bool active) noexcept;

    COLORREF Background(const ThemePalette& theme) const noexcept;
    COLORREF Text(const ThemePalette& theme) const noexcept;
    COLORREF MutedText(const ThemePalette& theme) const noexcept;

    void ApplyTheme(HWND window,
                    bool light,
                    COLORREF border_color,
                    bool repaint = true) const;
    void ExtendFrame(HWND window) const;

    RECT ButtonRect(HWND window, int id) const;
    int ButtonAtPoint(HWND window, POINT client_point) const;
    void InvalidateTitleBar(HWND window) const;
    void InvalidateButtons(HWND window) const;
    void DrawButtons(HDC dc, HWND window, const ThemePalette& theme) const;

    bool OnMouseMove(HWND window, POINT client_point);
    void OnMouseLeave(HWND window);
    bool OnLeftButtonDown(HWND window, POINT client_point);
    // nullopt means no caption press was active.  A value of zero means the
    // press was consumed but released outside the original button.
    std::optional<int> OnLeftButtonUp(HWND window, POINT client_point);
    void OnCaptureChanged(HWND window);

    LRESULT HitTest(HWND window, LPARAM screen_coordinates) const;

private:
    CaptionButtonIds ids_;
    bool active_{true};
    int hover_id_{};
    int pressed_id_{};
};

} // namespace aicli::ui
