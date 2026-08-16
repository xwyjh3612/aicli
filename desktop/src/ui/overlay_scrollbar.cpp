#include "ui/overlay_scrollbar.h"

#include "ui/drawing_utils.h"

#include <windowsx.h>

#include <algorithm>
#include <utility>

namespace aicli::ui {
namespace {

constexpr wchar_t kOverlayScrollbarClassName[] = L"AicliOverlayScrollbar";

} // namespace

OverlayScrollbar::OverlayScrollbar()
    : palette_(MakeThemePalette(ThemeMode::Dark)),
      background_(palette_.window) {}

OverlayScrollbar::~OverlayScrollbar() {
    Detach();
}

bool OverlayScrollbar::EnsureWindowClass() {
    static ATOM atom{};
    if (atom) return true;
    WNDCLASSEXW descriptor{sizeof(descriptor)};
    descriptor.lpfnWndProc = WindowProc;
    descriptor.hInstance = GetModuleHandleW(nullptr);
    descriptor.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    descriptor.lpszClassName = kOverlayScrollbarClassName;
    atom = RegisterClassExW(&descriptor);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool OverlayScrollbar::Attach(HWND host, MetricsProvider metrics,
                              PositionSetter set_position) {
    Detach();
    if (!host || !metrics || !set_position || !EnsureWindowClass()) return false;
    host_ = host;
    metrics_provider_ = std::move(metrics);
    position_setter_ = std::move(set_position);
    dpi_ = std::max(96, static_cast<int>(GetDpiForWindow(host)));
    window_ = CreateWindowExW(
        WS_EX_NOPARENTNOTIFY | WS_EX_NOACTIVATE,
        kOverlayScrollbarClassName, L"", WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 1, 1, host, nullptr, GetModuleHandleW(nullptr), this);
    if (!window_) {
        host_ = nullptr;
        metrics_provider_ = {};
        position_setter_ = {};
        return false;
    }
    Layout();
    return true;
}

void OverlayScrollbar::Detach() {
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    window_ = nullptr;
    host_ = nullptr;
    metrics_provider_ = {};
    position_setter_ = {};
    metrics_ = {};
    hovered_ = false;
    dragging_ = false;
}

void OverlayScrollbar::SetTheme(const ThemePalette& palette, COLORREF background) {
    palette_ = palette;
    background_ = background;
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

int OverlayScrollbar::Scale(int value) const noexcept {
    return MulDiv(value, std::max(96, dpi_), 96);
}

int OverlayScrollbar::MaximumPosition() const noexcept {
    return std::max(0, metrics_.content_extent - metrics_.viewport_extent);
}

void OverlayScrollbar::RefreshMetrics() {
    if (!metrics_provider_) {
        metrics_ = {};
        return;
    }
    metrics_ = metrics_provider_();
    metrics_.content_extent = std::max(0, metrics_.content_extent);
    metrics_.viewport_extent = std::max(0, metrics_.viewport_extent);
    metrics_.position = std::clamp(metrics_.position, 0, MaximumPosition());
}

RECT OverlayScrollbar::ThumbRect() const {
    RECT client{};
    if (window_) GetClientRect(window_, &client);
    const int track_top = Scale(4);
    const int track_bottom = std::max(track_top + 1,
                                      static_cast<int>(client.bottom) - Scale(4));
    const int track_height = track_bottom - track_top;
    const int thumb_height = std::min(
        track_height,
        std::max(Scale(28),
                 static_cast<int>((static_cast<long long>(track_height) *
                                   metrics_.viewport_extent) /
                                  std::max(1, metrics_.content_extent))));
    const int travel = std::max(0, track_height - thumb_height);
    const int top = track_top +
        (MaximumPosition() > 0
             ? static_cast<int>((static_cast<long long>(travel) * metrics_.position) /
                                MaximumPosition())
             : 0);
    const int thumb_width = Scale((hovered_ || dragging_) ? 7 : 3);
    const int right = static_cast<int>(client.right) - Scale(2);
    return {right - thumb_width, top, right, top + thumb_height};
}

void OverlayScrollbar::Layout() {
    if (!host_ || !window_) return;
    dpi_ = std::max(96, static_cast<int>(GetDpiForWindow(host_)));
    RECT client{};
    GetClientRect(host_, &client);
    const int width = Scale(12);
    SetWindowPos(window_, HWND_TOP, std::max(0, static_cast<int>(client.right) - width),
                 0, width, std::max(1L, client.bottom - client.top),
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    Sync();
}

void OverlayScrollbar::Sync() {
    if (!window_) return;
    RefreshMetrics();
    const bool visible = metrics_.viewport_extent > 0 &&
                         metrics_.content_extent > metrics_.viewport_extent;
    ShowWindow(window_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible) {
        SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                         SWP_NOOWNERZORDER);
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void OverlayScrollbar::NotifyActivity() {
    Sync();
}

void OverlayScrollbar::SetPosition(int position) {
    RefreshMetrics();
    const int next = std::clamp(position, 0, MaximumPosition());
    if (next != metrics_.position && position_setter_) position_setter_(next);
    RefreshMetrics();
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void OverlayScrollbar::Paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    HBRUSH background = CreateSolidBrush(background_);
    FillRect(dc, &client, background);
    DeleteObject(background);

    const bool light = palette_.light();
    const COLORREF thumb = dragging_
        ? (light ? RGB(116, 127, 142) : RGB(159, 159, 159))
        : (hovered_
               ? (light ? RGB(141, 151, 164) : RGB(159, 159, 159))
               : (light ? RGB(191, 199, 210) : RGB(86, 100, 121)));
    const RECT rect = ThumbRect();
    DrawSmoothRoundedRect(dc, rect, static_cast<float>(Scale(4)),
                          thumb, thumb, 0.0F);
}

LRESULT CALLBACK OverlayScrollbar::WindowProc(HWND window, UINT message,
                                               WPARAM wparam, LPARAM lparam) {
    auto* scrollbar = reinterpret_cast<OverlayScrollbar*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        scrollbar = static_cast<OverlayScrollbar*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(scrollbar));
        return TRUE;
    }
    if (!scrollbar) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        scrollbar->Paint(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!scrollbar->hovered_) {
            scrollbar->hovered_ = true;
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            InvalidateRect(window, nullptr, FALSE);
        }
        if (scrollbar->dragging_) {
            scrollbar->RefreshMetrics();
            RECT client{};
            GetClientRect(window, &client);
            const RECT thumb = scrollbar->ThumbRect();
            const int track_top = scrollbar->Scale(4);
            const int track_bottom = std::max(
                track_top + 1, static_cast<int>(client.bottom) - scrollbar->Scale(4));
            const int travel = std::max(
                1, track_bottom - track_top -
                       static_cast<int>(thumb.bottom - thumb.top));
            const int top = std::clamp(
                GET_Y_LPARAM(lparam) - scrollbar->drag_offset_,
                track_top, track_top + travel);
            scrollbar->SetPosition(static_cast<int>(
                (static_cast<long long>(top - track_top) *
                 scrollbar->MaximumPosition()) / travel));
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (!scrollbar->dragging_) {
            scrollbar->hovered_ = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        scrollbar->RefreshMetrics();
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const RECT thumb = scrollbar->ThumbRect();
        if (PtInRect(&thumb, point)) {
            scrollbar->dragging_ = true;
            scrollbar->drag_offset_ = point.y - thumb.top;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        } else {
            scrollbar->SetPosition(
                scrollbar->metrics_.position +
                (point.y < thumb.top ? -scrollbar->metrics_.viewport_extent
                                     : scrollbar->metrics_.viewport_extent));
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (scrollbar->dragging_) {
            scrollbar->dragging_ = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        scrollbar->dragging_ = false;
        scrollbar->hovered_ = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        if (scrollbar->host_)
            return SendMessageW(scrollbar->host_, WM_MOUSEWHEEL, wparam, lparam);
        return 0;
    case WM_NCDESTROY:
        if (scrollbar->window_ == window) scrollbar->window_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace aicli::ui
