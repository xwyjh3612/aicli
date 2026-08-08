#pragma once

#include "ui/layout_metrics.h"
#include "ui/theme.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace aicli::ui {

class ModalWorkspaceScope;

namespace detail {
inline LRESULT CALLBACK ModalScrimProc(HWND, UINT, WPARAM, LPARAM);
inline LRESULT CALLBACK ModalOwnerSubclassProc(HWND, UINT, WPARAM, LPARAM,
                                               UINT_PTR, DWORD_PTR);
inline ATOM EnsureModalScrimClass();
inline std::vector<ModalWorkspaceScope*>& ModalHosts();
inline int& ModalActivationDepth();
} // namespace detail

// A modal is composed like an H5 dialog:
//
//   owner workspace
//        +-- layered popup scrim (rgba-like opacity)
//        +-- ordinary opaque popup surface (native controls)
//
// The scrim and surface are top-level owned windows.  They never become
// children of the XAML Island, and the surface never uses WS_EX_TRANSPARENT.
// This keeps edit controls fully opaque and prevents focus changes from
// repainting the terminal/workspace below the dialog.
class ModalWorkspaceScope {
public:
    ModalWorkspaceScope(HWND owner, int preserve_top,
                        const ThemePalette* palette = nullptr)
        : owner_(owner), preserve_top_(preserve_top), palette_(palette),
          subclass_id_(reinterpret_cast<UINT_PTR>(this)) {
        if (!owner_ || !detail::EnsureModalScrimClass()) return;

        // Keep a process-local modal depth so the root title bar can ignore
        // the temporary WM_ACTIVATE transition caused by the owned popup.
        // The application itself remains active while a modal is open.
        detail::ModalHosts().push_back(this);
        ++detail::ModalActivationDepth();
        active_ = true;
        SetWindowSubclass(owner_, detail::ModalOwnerSubclassProc,
                          subclass_id_, reinterpret_cast<DWORD_PTR>(this));

        const RECT bounds = WorkspaceScreenRect();
        scrim_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"AicliModalScrim", L"",
            WS_POPUP,
            bounds.left, bounds.top,
            std::max(1L, bounds.right - bounds.left),
            std::max(1L, bounds.bottom - bounds.top),
            owner_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!scrim_) return;

        SetWindowLongPtrW(scrim_, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(this));
        ApplyTheme();
        SetWindowPos(scrim_, HWND_TOP, bounds.left, bounds.top,
                     std::max(1L, bounds.right - bounds.left),
                     std::max(1L, bounds.bottom - bounds.top),
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        UpdateWindow(scrim_);
    }

    ModalWorkspaceScope(const ModalWorkspaceScope&) = delete;
    ModalWorkspaceScope& operator=(const ModalWorkspaceScope&) = delete;

    ~ModalWorkspaceScope() {
        // Popup destruction exposes the owner in the middle of the close
        // sequence.  Suppress owner redraw for that short transaction and
        // invalidate the covered workspace once after both layers are gone;
        // otherwise Windows can present an erase frame followed by several
        // child paints, which looks like the whole application is flashing.
        HWND repaint_owner = owner_;
        RECT repaint_rect{};
        bool redraw_owner = repaint_owner && IsWindow(repaint_owner) &&
                            GetClientRect(repaint_owner, &repaint_rect);
        if (redraw_owner) {
            const int bottom = std::max(0, static_cast<int>(repaint_rect.bottom));
            const int top = std::clamp(preserve_top_ < 0 ? 0 : preserve_top_,
                                       0, bottom);
            repaint_rect.top = top;
            SendMessageW(repaint_owner, WM_SETREDRAW, FALSE, 0);
        }
        if (surface_ && IsWindow(surface_)) DestroyWindow(surface_);
        surface_ = nullptr;
        if (scrim_ && IsWindow(scrim_)) DestroyWindow(scrim_);
        scrim_ = nullptr;

        if (owner_ && IsWindow(owner_)) {
            RemoveWindowSubclass(owner_, detail::ModalOwnerSubclassProc,
                                 subclass_id_);
        }
        auto& hosts = detail::ModalHosts();
        hosts.erase(std::remove(hosts.begin(), hosts.end(), this), hosts.end());
        if (active_) {
            --detail::ModalActivationDepth();
            active_ = false;
        }
        if (redraw_owner && IsWindow(repaint_owner)) {
            SendMessageW(repaint_owner, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(repaint_owner, &repaint_rect, nullptr,
                         RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
        }
    }

    void AttachSurface(HWND surface) {
        surface_ = surface;
        if (!surface_ || !IsWindow(surface_)) return;
        Reposition();
        SetWindowPos(surface_, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER |
                         SWP_NOACTIVATE |
                         SWP_SHOWWINDOW);
    }

    // Activate only within this GUI thread.  Unlike SetForegroundWindow this
    // does not steal the foreground from another application and does not
    // produce the foreground-window flash when the modal is opened.
    void ActivateSurface(HWND focus = nullptr) {
        if (!surface_ || !IsWindow(surface_)) return;
        SetActiveWindow(surface_);
        if (focus && IsWindow(focus)) SetFocus(focus);
        else SetFocus(surface_);
    }

    void Reposition() {
        if (!owner_ || !IsWindow(owner_)) return;
        const RECT bounds = WorkspaceScreenRect();
        const int width = std::max(1L, bounds.right - bounds.left);
        const int height = std::max(1L, bounds.bottom - bounds.top);
        if (scrim_ && IsWindow(scrim_)) {
            SetWindowPos(scrim_, HWND_TOP, bounds.left, bounds.top,
                         width, height,
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        if (surface_ && IsWindow(surface_)) {
            // Moving the full workspace-sized popup triggers one WM_SIZE in
            // the surface.  Its window region then clips it to the actual
            // rounded panel, leaving the scrim visible everywhere else.
            SetWindowPos(surface_, HWND_TOP, bounds.left, bounds.top,
                         width, height,
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }

    void ApplyTheme() {
        if (!scrim_ || !IsWindow(scrim_)) return;
        const float opacity = std::clamp(
            palette_ ? palette_->scrim_opacity : 0.54F, 0.0F, 1.0F);
        const BYTE alpha = static_cast<BYTE>(std::lround(opacity * 255.0F));
        SetLayeredWindowAttributes(scrim_, RGB(0, 0, 0), alpha, LWA_ALPHA);
        InvalidateRect(scrim_, nullptr, FALSE);
    }

    HWND owner() const noexcept { return owner_; }
    HWND surface() const noexcept { return surface_; }
    COLORREF scrim_color() const noexcept {
        return palette_ ? palette_->scrim : RGB(0, 0, 0);
    }

private:
    RECT WorkspaceScreenRect() const {
        RECT client{};
        if (!owner_ || !GetClientRect(owner_, &client)) return {0, 0, 1, 1};
        const int bottom = std::max(0, static_cast<int>(client.bottom));
        const int top = std::clamp(preserve_top_ < 0 ? 0 : preserve_top_,
                                   0, bottom);
        POINT origin{client.left, top};
        ClientToScreen(owner_, &origin);
        return {origin.x, origin.y,
                origin.x + std::max(1L, client.right - client.left),
                origin.y + std::max(1L, client.bottom - top)};
    }

    friend LRESULT CALLBACK detail::ModalScrimProc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK detail::ModalOwnerSubclassProc(
        HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    HWND owner_{};
    HWND scrim_{};
    HWND surface_{};
    int preserve_top_{};
    const ThemePalette* palette_{};
    UINT_PTR subclass_id_{};
    bool active_{};
};

namespace detail {

inline std::vector<ModalWorkspaceScope*>& ModalHosts() {
    static std::vector<ModalWorkspaceScope*> hosts;
    return hosts;
}

inline int& ModalActivationDepth() {
    static int depth = 0;
    return depth;
}

inline LRESULT CALLBACK ModalScrimProc(HWND window, UINT message,
                                       WPARAM wparam, LPARAM lparam) {
    auto* host = reinterpret_cast<ModalWorkspaceScope*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH brush = CreateSolidBrush(
            host ? host->scrim_color() : RGB(0, 0, 0));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATEANDEAT;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        // The scrim deliberately does not activate itself.  A click outside
        // the panel is consumed, while the already-active modal keeps focus.
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

inline LRESULT CALLBACK ModalOwnerSubclassProc(
    HWND owner, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass_id, DWORD_PTR reference) {
    auto* host = reinterpret_cast<ModalWorkspaceScope*>(reference);
    if (host) {
        if (message == WM_WINDOWPOSCHANGED || message == WM_SIZE ||
            message == WM_DPICHANGED) {
            host->Reposition();
        } else if (message == WM_NCDESTROY) {
            host->owner_ = nullptr;
            RemoveWindowSubclass(owner, ModalOwnerSubclassProc, subclass_id);
        }
    }
    return DefSubclassProc(owner, message, wparam, lparam);
}

inline ATOM EnsureModalScrimClass() {
    static ATOM atom = [] {
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = ModalScrimProc;
        klass.lpszClassName = L"AicliModalScrim";
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = nullptr;
        return RegisterClassW(&klass);
    }();
    return atom;
}

} // namespace detail

inline int ModalTopForOwner(HWND owner) {
    return LayoutMetrics::ForWindow(owner).title_bar_height;
}

inline void SetModalSurfaceRegion(HWND surface, const RECT& panel,
                                  int radius) {
    if (!surface) return;
    HRGN region = CreateRoundRectRgn(panel.left, panel.top,
                                     panel.right + 1, panel.bottom + 1,
                                     radius, radius);
    if (!SetWindowRgn(surface, region, FALSE)) DeleteObject(region);
}

inline void RefreshModalThemes() {
    for (auto* host : detail::ModalHosts()) {
        if (host) host->ApplyTheme();
    }
}

inline bool HasActiveModal(HWND owner) {
    return std::any_of(detail::ModalHosts().begin(), detail::ModalHosts().end(),
                       [owner](const ModalWorkspaceScope* host) {
                           return host && host->owner() == owner;
                       });
}

inline bool HasAnyActiveModal() {
    return detail::ModalActivationDepth() > 0;
}

inline void ActivateModalSurface(HWND surface, HWND focus = nullptr) {
    if (!surface || !IsWindow(surface)) return;
    SetActiveWindow(surface);
    if (focus && IsWindow(focus)) SetFocus(focus);
    else SetFocus(surface);
}

inline void RestoreModalOwner(HWND owner, HWND focus = nullptr) {
    if (!owner || !IsWindow(owner)) return;
    // SetActiveWindow changes the active window in this thread only.  It does
    // not pull the application in front of another application, which is the
    // important distinction from SetForegroundWindow here.
    SetActiveWindow(owner);
    if (focus && IsWindow(focus)) SetFocus(focus);
    else SetFocus(owner);
}

} // namespace aicli::ui
