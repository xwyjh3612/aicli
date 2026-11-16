#pragma once

#include "ui/drawing_utils.h"
#include <cmath>
#include <cstdint>
#include <string_view>

namespace aicli::ui {

enum class ProjectStatusIcon { Idle, Running, Working, Paused, Warning };

inline float ProjectAnimationPhase(std::wstring_view project_id, std::uint32_t salt) noexcept {
    // Stable phases survive filtering and reordering, including the drag preview.
    std::uint32_t hash = 2166136261u ^ salt;
    for (const wchar_t unit : project_id) {
        hash ^= static_cast<std::uint32_t>(unit);
        hash *= 16777619u;
    }
    return static_cast<float>(hash % 36000u) / 100.0f;
}

inline void DrawProjectStatusIcon(Gdiplus::Graphics& graphics,
                                 const Gdiplus::RectF& bounds,
                                 ProjectStatusIcon state, COLORREF color,
                                 std::wstring_view project_id,
                                 float spinner_angle, float pulse_angle) {
    const auto saved = graphics.Save();
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float scale = bounds.Width / 20.0f;
    const float x = bounds.X, y = bounds.Y, size = bounds.Width;
    const float cx = x + size / 2.0f, cy = y + bounds.Height / 2.0f;
    Gdiplus::SolidBrush halo(SmoothColor(color, 30));
    if (state == ProjectStatusIcon::Paused || state == ProjectStatusIcon::Warning)
        graphics.FillEllipse(&halo, bounds);
    Gdiplus::Pen pen(SmoothColor(color), 1.65f * scale);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::SolidBrush brush(SmoothColor(color));
    if (state == ProjectStatusIcon::Working) {
        const float angle = std::fmod(spinner_angle +
            ProjectAnimationPhase(project_id, 0x5350494eu), 360.0f);
        Gdiplus::Pen track(SmoothColor(color, 65), 1.65f * scale);
        Gdiplus::Pen spinner(SmoothColor(color), 2.35f * scale);
        track.SetStartCap(Gdiplus::LineCapRound);
        track.SetEndCap(Gdiplus::LineCapRound);
        spinner.SetStartCap(Gdiplus::LineCapRound);
        spinner.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawEllipse(&track, x + 2.0f * scale, y + 2.0f * scale,
                             size - 4.0f * scale, size - 4.0f * scale);
        graphics.DrawArc(&spinner, x + 2.0f * scale, y + 2.0f * scale,
                         size - 4.0f * scale, size - 4.0f * scale, angle, 112.0f);
    } else if (state == ProjectStatusIcon::Running) {
        const float angle = std::fmod(pulse_angle +
            ProjectAnimationPhase(project_id, 0x50554c53u), 360.0f);
        const float pulse = (std::sin(angle * 3.14159265f / 180.0f) + 1.0f) * 0.5f;
        const float diameter = (8.0f + 2.0f * pulse) * scale;
        const auto channel = [pulse](BYTE low, BYTE high) {
            return static_cast<BYTE>(low + (high - low) * pulse + 0.5f);
        };
        Gdiplus::SolidBrush pulse_brush(SmoothColor(RGB(
            channel(0x1f, 0x49), channel(0x79, 0xcb), channel(0x55, 0x91))));
        graphics.FillEllipse(&pulse_brush, cx - diameter / 2.0f, cy - diameter / 2.0f,
                             diameter, diameter);
    } else if (state == ProjectStatusIcon::Paused) {
        graphics.DrawLine(&pen, cx - 2.5f * scale, cy - 4.0f * scale,
                          cx - 2.5f * scale, cy + 4.0f * scale);
        graphics.DrawLine(&pen, cx + 2.5f * scale, cy - 4.0f * scale,
                          cx + 2.5f * scale, cy + 4.0f * scale);
    } else if (state == ProjectStatusIcon::Warning) {
        Gdiplus::PointF triangle[3]{{cx, y + 3.0f * scale},
            {x + 3.0f * scale, y + size - 3.0f * scale},
            {x + size - 3.0f * scale, y + size - 3.0f * scale}};
        graphics.DrawPolygon(&pen, triangle, 3);
        graphics.DrawLine(&pen, cx, cy - 2.5f * scale, cx, cy + 1.5f * scale);
        graphics.FillEllipse(&brush, cx - 0.9f * scale, cy + 3.0f * scale,
                             1.8f * scale, 1.8f * scale);
    } else {
        graphics.DrawEllipse(&pen, cx - 4.0f * scale, cy - 4.0f * scale,
                             8.0f * scale, 8.0f * scale);
    }
    graphics.Restore(saved);
}

} // namespace aicli::ui
