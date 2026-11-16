#pragma once
#include "ui/drawing_utils.h"

namespace aicli::ui {
inline void DrawUpdateBadge(HDC dc, const RECT& bounds, UINT dpi, int dots = 1) {
    const float scale = static_cast<float>(dpi ? dpi : 96) / 96.0f;
    const float size = 5.0f * scale;
    const float gap = 3.0f * scale;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(SmoothColor(RGB(238, 68, 78)));
    const float left = static_cast<float>(bounds.right) - 7.0f * scale -
                       dots * size - (dots - 1) * gap;
    const float top = static_cast<float>(bounds.top) + 6.0f * scale;
    for (int index = 0; index < dots; ++index)
        graphics.FillEllipse(&brush, left + index * (size + gap), top, size, size);
}
}
