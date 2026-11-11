#pragma once
#include <windows.h>

namespace aicli::ui {
// Batch changes within a GDI child pane. Never use this on the workspace or
// any top-level window: DefWindowProc(WM_SETREDRAW) changes WS_VISIBLE, which
// can discard the owner's/XAML Island's composition while a popup closes.
class ChildRedrawScope {
public:
    explicit ChildRedrawScope(HWND window) : window_(window) {
        const auto style = GetWindowLongPtrW(window_, GWL_STYLE);
        suspended_ = (style & WS_CHILD) && IsWindowVisible(window_);
        if (suspended_) SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
    }
    ~ChildRedrawScope() {
        if (!suspended_ || !IsWindow(window_)) return;
        SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
    }
    ChildRedrawScope(const ChildRedrawScope&) = delete;
    ChildRedrawScope& operator=(const ChildRedrawScope&) = delete;
private:
    HWND window_{};
    bool suspended_{};
};
}
