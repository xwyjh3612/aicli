#pragma once

#include "ui/theme.h"

#include <windows.h>

#include <functional>

namespace aicli::ui {

struct OverlayScrollMetrics {
    int content_extent{};
    int viewport_extent{};
    int position{};
};

class OverlayScrollbar {
public:
    using MetricsProvider = std::function<OverlayScrollMetrics()>;
    using PositionSetter = std::function<void(int)>;

    OverlayScrollbar();
    ~OverlayScrollbar();

    OverlayScrollbar(const OverlayScrollbar&) = delete;
    OverlayScrollbar& operator=(const OverlayScrollbar&) = delete;

    bool Attach(HWND host, MetricsProvider metrics, PositionSetter set_position);
    void Detach();
    void SetTheme(const ThemePalette& palette, COLORREF background);
    void Layout();
    void Sync();
    void NotifyActivity();

    HWND window() const noexcept { return window_; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message,
                                       WPARAM wparam, LPARAM lparam);
    static bool EnsureWindowClass();

    int Scale(int value) const noexcept;
    int MaximumPosition() const noexcept;
    RECT ThumbRect() const;
    void RefreshMetrics();
    void SetPosition(int position);
    void Paint(HDC dc);

    HWND host_{};
    HWND window_{};
    MetricsProvider metrics_provider_;
    PositionSetter position_setter_;
    OverlayScrollMetrics metrics_{};
    ThemePalette palette_{};
    COLORREF background_{};
    int dpi_{96};
    bool hovered_{};
    bool dragging_{};
    int drag_offset_{};
};

} // namespace aicli::ui
