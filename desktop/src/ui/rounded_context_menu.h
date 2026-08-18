#pragma once

#include "ui/theme.h"

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aicli::ui {

struct RoundedContextMenuEntry {
    int command{};
    std::wstring label;
    std::wstring icon;
    bool enabled{true};
    bool danger{};
    bool separator_before{};
    bool submenu{};
};

struct RoundedContextMenuOptions {
    HWND owner{};
    POINT screen_position{};
    ThemePalette palette{};
    HFONT text_font{};
    HFONT icon_font{};
    int dpi{96};
    int width{224};
    int row_height{36};
    // Opens a submenu for the hovered/clicked entry. Returning true means
    // the submenu handled the action and the parent menu should close;
    // returning false keeps the parent menu available.
    std::function<bool(int, const RECT&)> submenu_handler;
};

struct RoundedContextMenuResult {
    int command{};
    RECT item_screen_rect{};
};

std::optional<RoundedContextMenuResult> ShowRoundedContextMenu(
    const RoundedContextMenuOptions& options,
    std::vector<RoundedContextMenuEntry> entries);

} // namespace aicli::ui
