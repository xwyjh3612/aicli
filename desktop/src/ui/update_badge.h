#pragma once
#include "ui/drawing_utils.h"

namespace aicli::ui {
inline void DrawUpdateBadge(HDC dc, const RECT& bounds, UINT dpi) {
    const float scale = static_cast<float>(dpi ? dpi : 96) / 96.0f;
    const float size = 5.0f * scale;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(SmoothColor(RGB(238, 68, 78)));
    const float left = static_cast<float>(bounds.right) - 7.0f * scale - size;
    const float top = static_cast<float>(bounds.top) + 6.0f * scale;
    graphics.FillEllipse(&brush, left, top, size, size);
}
}
