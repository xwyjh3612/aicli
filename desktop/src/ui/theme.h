#pragma once

#include <windows.h>

namespace aicli::ui {

// A theme is a value object.  Windows, dialogs, controls and the terminal
// all read the same palette; no view is allowed to invent a second palette.
enum class ThemeMode {
    Dark,
    Light,
};

struct ThemePalette {
    ThemeMode mode{ThemeMode::Dark};

    COLORREF window{};
    COLORREF panel{};
    COLORREF surface{};
    COLORREF border{};
    COLORREF text{};
    COLORREF muted{};
    COLORREF accent{};
    COLORREF accent_strong{};
    COLORREF field{};
    COLORREF field_focus{};
    COLORREF selection{};
    COLORREF list_surface{};
    COLORREF list_text{};
    COLORREF hover{};
    COLORREF pressed{};
    COLORREF titlebar{};
    COLORREF titlebar_text{};
    COLORREF titlebar_muted{};
    COLORREF titlebar_inactive{};
    COLORREF titlebar_inactive_text{};
    COLORREF titlebar_hover{};
    COLORREF titlebar_pressed{};
    COLORREF titlebar_close_hover{};
    COLORREF titlebar_close_pressed{};
    COLORREF scrim{};
    // CSS-like opacity for the modal backdrop.  0.0 is transparent and 1.0
    // is opaque, so visual tuning is a single semantic token rather than a
    // collection of Win32 alpha constants spread across dialogs.
    float scrim_opacity{0.54F};
    COLORREF danger{};
    COLORREF warning{};
    COLORREF success{};

    bool light() const noexcept { return mode == ThemeMode::Light; }
};

ThemePalette MakeThemePalette(ThemeMode mode);
inline ThemePalette MakeThemePalette(bool light) {
    return MakeThemePalette(light ? ThemeMode::Light : ThemeMode::Dark);
}

class ThemeManager {
public:
    ThemeManager();

    const ThemePalette& palette() const noexcept { return palette_; }
    ThemePalette& palette() noexcept { return palette_; }
    ThemeMode mode() const noexcept { return palette_.mode; }
    bool light() const noexcept { return palette_.light(); }

    // Loading and changing are deliberately separate operations.  A caller
    // can update persistent storage first, then publish one complete palette
    // transaction to all views.
    void LoadMode(ThemeMode mode) noexcept { palette_ = MakeThemePalette(mode); }

private:
    ThemePalette palette_;
};

} // namespace aicli::ui
