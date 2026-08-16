#pragma once

#include "ui/theme.h"

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aicli::ui {

enum class RoundedPopupPlacement {
    Below,
    Right,
};

struct RoundedSelectEntry {
    std::wstring value;
    std::wstring label;
    std::wstring detail;
    std::wstring icon;
    bool selected{};
    bool separator_before{};
};

struct RoundedSelectPopupOptions {
    HWND owner{};
    HWND anchor{};
    ThemePalette palette{};
    HFONT text_font{};
    HFONT detail_font{};
    HFONT icon_font{};
    int dpi{96};
    int row_height{34};
    int maximum_visible_rows{9};
    int minimum_width{184};
    int maximum_width{300};
    bool match_anchor_width{};
    bool use_screen_anchor{};
    RECT screen_anchor{};
    RoundedPopupPlacement placement{RoundedPopupPlacement::Below};
    std::function<void(bool)> open_state_changed;
};

std::optional<std::wstring> ShowRoundedSelectPopup(
    const RoundedSelectPopupOptions& options,
    std::vector<RoundedSelectEntry> entries);

} // namespace aicli::ui
