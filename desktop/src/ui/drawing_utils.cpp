#include "ui/drawing_utils.h"

#include <algorithm>

namespace aicli::ui {

GdiplusRuntime::GdiplusRuntime() {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token_, &input, nullptr);
}

GdiplusRuntime::~GdiplusRuntime() {
    if (token_) Gdiplus::GdiplusShutdown(token_);
}

BufferedWindowPaint::BufferedWindowPaint(HDC target, RECT client, RECT dirty)
    : target_(target), client_(client), dirty_(dirty) {
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (!target_ || width <= 0 || height <= 0) return;
    memory_ = CreateCompatibleDC(target_);
    bitmap_ = memory_ ? CreateCompatibleBitmap(target_, width, height) : nullptr;
    if (!memory_ || !bitmap_) {
        if (bitmap_) DeleteObject(bitmap_);
        if (memory_) DeleteDC(memory_);
        bitmap_ = nullptr;
        memory_ = nullptr;
        return;
    }
    previous_bitmap_ = SelectObject(memory_, bitmap_);
}

BufferedWindowPaint::~BufferedWindowPaint() {
    if (!memory_) return;
    RECT present{};
    if (IntersectRect(&present, &client_, &dirty_)) {
        BitBlt(target_, present.left, present.top,
               present.right - present.left, present.bottom - present.top,
               memory_, present.left, present.top, SRCCOPY);
    }
    SelectObject(memory_, previous_bitmap_);
    DeleteObject(bitmap_);
    DeleteDC(memory_);
}

Gdiplus::Color SmoothColor(COLORREF color, BYTE alpha) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

COLORREF BlendBackdropColor(COLORREF base, COLORREF overlay, BYTE alpha) {
    const auto blend = [alpha](BYTE from, BYTE to) -> BYTE {
        return static_cast<BYTE>(
            (static_cast<unsigned>(from) * (255u - alpha) +
             static_cast<unsigned>(to) * alpha) /
            255u);
    };
    return RGB(blend(GetRValue(base), GetRValue(overlay)),
               blend(GetGValue(base), GetGValue(overlay)),
               blend(GetBValue(base), GetBValue(overlay)));
}

void AddRoundedPath(Gdiplus::GraphicsPath& path,
                    const Gdiplus::RectF& rect,
                    float radius) {
    const float diameter = std::min(radius * 2.0f, std::min(rect.Width, rect.Height));
    if (diameter <= 1.0f) {
        path.AddRectangle(rect);
        return;
    }
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter,
                diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter,
                90.0f, 90.0f);
    path.CloseFigure();
}

void DrawSmoothRoundedRect(HDC dc,
                           const RECT& rect,
                           float radius,
                           COLORREF fill,
                           COLORREF stroke,
                           float stroke_width,
                           bool dashed) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::RectF bounds{
        static_cast<float>(rect.left) + stroke_width / 2.0f,
        static_cast<float>(rect.top) + stroke_width / 2.0f,
        std::max(1.0f, static_cast<float>(rect.right - rect.left) - stroke_width),
        std::max(1.0f, static_cast<float>(rect.bottom - rect.top) - stroke_width)};
    Gdiplus::GraphicsPath path;
    AddRoundedPath(path, bounds, radius);
    Gdiplus::SolidBrush brush(SmoothColor(fill));
    graphics.FillPath(&brush, &path);
    Gdiplus::Pen pen(SmoothColor(stroke), stroke_width);
    if (dashed) pen.SetDashStyle(Gdiplus::DashStyleDash);
    graphics.DrawPath(&pen, &path);
}

void ApplyRoundedRegion(HWND window, int width, int height, int radius) {
    if (!window || width <= 0 || height <= 0) return;
    SetWindowRgn(window,
                 CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius),
                 TRUE);
}

void DrawRoundedOutline(HDC dc, RECT rect, COLORREF color, int radius) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    const auto previous_pen = SelectObject(dc, pen);
    const auto previous_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, previous_pen);
    SelectObject(dc, previous_brush);
    DeleteObject(pen);
}

} // namespace aicli::ui
