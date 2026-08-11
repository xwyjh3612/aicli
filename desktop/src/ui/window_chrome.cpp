#include "ui/window_chrome.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>

#include "ui/drawing_utils.h"
#include "ui/layout_metrics.h"

namespace aicli::ui {

namespace {

// DWMWA_BORDER_COLOR uses this sentinel to suppress the compositor-owned
// outline. A maximized custom-frame window must not retain that outline: it
// otherwise remains visible as a one-pixel strip at the monitor edge.
constexpr COLORREF kDwmColorNone = static_cast<COLORREF>(0xFFFFFFFEu);

COLORREF CompositorBorder(HWND window, COLORREF normal_border) noexcept {
    return IsZoomed(window) ? kDwmColorNone : normal_border;
}

MARGINS CompositorMargins(HWND window) noexcept {
    const int rim = IsZoomed(window) ? 0 : 1;
    return MARGINS{rim, rim, rim, rim};
}

} // namespace

WindowChrome::WindowChrome(CaptionButtonIds ids) noexcept : ids_(ids) {}

bool WindowChrome::SetActive(bool active) noexcept {
    if (active_ == active) return false;
    active_ = active;
    return true;
}

COLORREF WindowChrome::Background(const ThemePalette& theme) const noexcept {
    return active_ ? theme.titlebar : theme.titlebar_inactive;
}

COLORREF WindowChrome::Text(const ThemePalette& theme) const noexcept {
    return active_ ? theme.titlebar_text : theme.titlebar_inactive_text;
}

COLORREF WindowChrome::MutedText(const ThemePalette& theme) const noexcept {
    return active_ ? theme.titlebar_muted : theme.titlebar_inactive_text;
}

void WindowChrome::ApplyTheme(HWND window,
                              bool light,
                              COLORREF border_color,
                              bool repaint) const {
    if (!window) return;
    BOOL dark = light ? FALSE : TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &dark, sizeof(dark));
    const COLORREF compositor_border = CompositorBorder(window, border_color);
    DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR,
                          &compositor_border, sizeof(compositor_border));
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    if (repaint) {
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_FRAME | RDW_NOERASE |
                         RDW_ALLCHILDREN);
    }
}

void WindowChrome::ExtendFrame(HWND window) const {
    if (!window) return;
    DWMNCRENDERINGPOLICY rendering = DWMNCRP_ENABLED;
    DwmSetWindowAttribute(window, DWMWA_NCRENDERING_POLICY,
                          &rendering, sizeof(rendering));
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corners, sizeof(corners));
    const MARGINS compositor_rim = CompositorMargins(window);
    DwmExtendFrameIntoClientArea(window, &compositor_rim);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
}

void WindowChrome::UpdateSizeState(HWND window,
                                   COLORREF border_color) const {
    if (!window) return;
    const COLORREF compositor_border = CompositorBorder(window, border_color);
    DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR,
                          &compositor_border, sizeof(compositor_border));
    const MARGINS compositor_rim = CompositorMargins(window);
    DwmExtendFrameIntoClientArea(window, &compositor_rim);
}

RECT WindowChrome::ButtonRect(HWND window, int id) const {
    RECT client{};
    GetClientRect(window, &client);
    const auto metrics = LayoutMetrics::ForWindow(window);
    const int width = std::max(1, metrics.caption_button_width);
    const int offset = id == ids_.close ? 1 : (id == ids_.maximize ? 2 : 3);
    return RECT{
        std::max(0, static_cast<int>(client.right) - width * offset),
        0,
        std::max(0, static_cast<int>(client.right) - width * (offset - 1)),
        metrics.title_bar_height};
}

int WindowChrome::ButtonAtPoint(HWND window, POINT client_point) const {
    for (const int id : {ids_.close, ids_.maximize, ids_.minimize}) {
        const RECT rect = ButtonRect(window, id);
        if (PtInRect(&rect, client_point)) return id;
    }
    return 0;
}

void WindowChrome::InvalidateTitleBar(HWND window) const {
    if (!window) return;
    const int height = LayoutMetrics::ForWindow(window).title_bar_height;
    RECT rect{};
    GetClientRect(window, &rect);
    rect.bottom = std::min<LONG>(rect.bottom, height);
    RedrawWindow(window, &rect, nullptr,
                 RDW_INVALIDATE | RDW_FRAME | RDW_NOERASE |
                     RDW_ALLCHILDREN);
}

void WindowChrome::InvalidateButtons(HWND window) const {
    if (!window) return;
    RECT client{};
    GetClientRect(window, &client);
    const auto metrics = LayoutMetrics::ForWindow(window);
    RECT controls{
        std::max(0, static_cast<int>(client.right) -
                        metrics.caption_button_width * 3),
        0, client.right, metrics.title_bar_height};
    InvalidateRect(window, &controls, FALSE);
}

void WindowChrome::DrawButtons(HDC dc,
                               HWND window,
                               const ThemePalette& theme) const {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    for (const int id : {ids_.minimize, ids_.maximize, ids_.close}) {
        const RECT rect = ButtonRect(window, id);
        COLORREF fill = Background(theme);
        const bool hover = hover_id_ == id;
        const bool pressed = pressed_id_ == id;
        if (hover) {
            fill = id == ids_.close
                ? theme.titlebar_close_hover
                : theme.titlebar_hover;
        }
        if (pressed) {
            fill = id == ids_.close
                ? theme.titlebar_close_pressed
                : theme.titlebar_pressed;
        }
        Gdiplus::SolidBrush background(SmoothColor(fill));
        graphics.FillRectangle(
            &background,
            static_cast<Gdiplus::REAL>(rect.left),
            static_cast<Gdiplus::REAL>(rect.top),
            static_cast<Gdiplus::REAL>(rect.right - rect.left),
            static_cast<Gdiplus::REAL>(rect.bottom - rect.top));

        const COLORREF glyph = id == ids_.close && hover
            ? RGB(255, 255, 255)
            : Text(theme);
        Gdiplus::Pen pen(SmoothColor(glyph), 1.25F);
        pen.SetStartCap(Gdiplus::LineCapSquare);
        pen.SetEndCap(Gdiplus::LineCapSquare);
        const float cx = (rect.left + rect.right) / 2.0F;
        const float cy = (rect.top + rect.bottom) / 2.0F;
        if (id == ids_.minimize) {
            graphics.DrawLine(&pen, cx - 7.0F, cy + 1.0F,
                              cx + 7.0F, cy + 1.0F);
        } else if (id == ids_.close) {
            graphics.DrawLine(&pen, cx - 6.0F, cy - 6.0F,
                              cx + 6.0F, cy + 6.0F);
            graphics.DrawLine(&pen, cx + 6.0F, cy - 6.0F,
                              cx - 6.0F, cy + 6.0F);
        } else if (IsZoomed(window)) {
            graphics.DrawRectangle(&pen, cx - 4.0F, cy - 6.0F,
                                   11.0F, 11.0F);
            Gdiplus::SolidBrush cover(SmoothColor(fill));
            graphics.FillRectangle(&cover, cx - 7.0F, cy - 3.0F,
                                   11.0F, 11.0F);
            graphics.DrawRectangle(&pen, cx - 7.0F, cy - 3.0F,
                                   11.0F, 11.0F);
        } else {
            graphics.DrawRectangle(&pen, cx - 7.0F, cy - 6.0F,
                                   14.0F, 13.0F);
        }
    }
}

bool WindowChrome::OnMouseMove(HWND window, POINT client_point) {
    const int hover = ButtonAtPoint(window, client_point);
    if (hover != hover_id_) {
        hover_id_ = hover;
        InvalidateButtons(window);
    }
    if (hover || pressed_id_) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
    }
    return hover != 0;
}

void WindowChrome::OnMouseLeave(HWND window) {
    if (!hover_id_) return;
    hover_id_ = 0;
    InvalidateButtons(window);
}

bool WindowChrome::OnLeftButtonDown(HWND window, POINT client_point) {
    const int button = ButtonAtPoint(window, client_point);
    if (!button) return false;
    pressed_id_ = button;
    SetCapture(window);
    InvalidateButtons(window);
    return true;
}

std::optional<int> WindowChrome::OnLeftButtonUp(HWND window,
                                                POINT client_point) {
    if (!pressed_id_) return std::nullopt;
    const int pressed = pressed_id_;
    pressed_id_ = 0;
    if (GetCapture() == window) ReleaseCapture();
    InvalidateButtons(window);
    return ButtonAtPoint(window, client_point) == pressed ? pressed : 0;
}

void WindowChrome::OnCaptureChanged(HWND window) {
    if (!pressed_id_) return;
    pressed_id_ = 0;
    InvalidateButtons(window);
}

LRESULT WindowChrome::HitTest(HWND window, LPARAM screen_coordinates) const {
    const auto metrics = LayoutMetrics::ForWindow(window);
    POINT screen{GET_X_LPARAM(screen_coordinates),
                 GET_Y_LPARAM(screen_coordinates)};
    RECT window_rect{};
    GetWindowRect(window, &window_rect);
    const UINT dpi = GetDpiForWindow(window);
    const int frame = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                      GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    if (!IsZoomed(window)) {
        const bool left = screen.x >= window_rect.left &&
                          screen.x < window_rect.left + frame;
        const bool right = screen.x < window_rect.right &&
                           screen.x >= window_rect.right - frame;
        const bool top = screen.y >= window_rect.top &&
                         screen.y < window_rect.top + frame;
        const bool bottom = screen.y < window_rect.bottom &&
                            screen.y >= window_rect.bottom - frame;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }

    POINT client = screen;
    ScreenToClient(window, &client);
    if (ButtonAtPoint(window, client)) return HTCLIENT;
    if (client.y >= 0 && client.y < metrics.title_bar_height) {
        HWND child = ChildWindowFromPointEx(
            window, client,
            CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (child && child != window) return HTCLIENT;
        return HTCAPTION;
    }
    return HTCLIENT;
}

} // namespace aicli::ui
