#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

namespace aicli::ui {

class GdiplusRuntime {
public:
    GdiplusRuntime();
    ~GdiplusRuntime();

    GdiplusRuntime(const GdiplusRuntime&) = delete;
    GdiplusRuntime& operator=(const GdiplusRuntime&) = delete;

private:
    ULONG_PTR token_{};
};

// Draws a complete parent-window frame into a compatible bitmap and presents
// only the invalidated region in one BitBlt.  Child HWNDs and the XAML Island
// remain independent; this removes visible fill/text phases from timer-driven
// footer repaints.
class BufferedWindowPaint {
public:
    BufferedWindowPaint(HDC target, RECT client, RECT dirty);
    ~BufferedWindowPaint();

    BufferedWindowPaint(const BufferedWindowPaint&) = delete;
    BufferedWindowPaint& operator=(const BufferedWindowPaint&) = delete;

    HDC dc() const noexcept { return memory_ ? memory_ : target_; }

private:
    HDC target_{};
    HDC memory_{};
    HBITMAP bitmap_{};
    HGDIOBJ previous_bitmap_{};
    RECT client_{};
    RECT dirty_{};
};

Gdiplus::Color SmoothColor(COLORREF color, BYTE alpha = 255);
COLORREF BlendBackdropColor(COLORREF base, COLORREF overlay, BYTE alpha);
void AddRoundedPath(Gdiplus::GraphicsPath& path,
                    const Gdiplus::RectF& rect,
                    float radius);
void DrawSmoothRoundedRect(HDC dc,
                           const RECT& rect,
                           float radius,
                           COLORREF fill,
                           COLORREF stroke,
                           float stroke_width = 1.0f,
                           bool dashed = false);
void ApplyRoundedRegion(HWND window, int width, int height, int radius);
void DrawRoundedOutline(HDC dc, RECT rect, COLORREF color, int radius);

} // namespace aicli::ui
