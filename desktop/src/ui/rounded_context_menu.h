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
    // Terminal popups keep ordinary typing away from the running CLI.
    bool consume_keyboard_input{};
    // Opens a submenu for the hovered/clicked entry. Returning true means
    // the submenu handled the action and the parent menu should close;
    // returning false keeps the parent menu available.
    std::function<bool(int, const RECT&)> submenu_handler;
    // XAML InputSite can receive mouse pointer messages despite Win32 capture.
    // Terminal popups route those messages before they reach the CLI surface.
    bool route_mouse_pointer_input{};
};

struct RoundedContextMenuResult {
    int command{};
    RECT item_screen_rect{};
};

std::optional<RoundedContextMenuResult> ShowRoundedContextMenu(
    const RoundedContextMenuOptions& options,
    std::vector<RoundedContextMenuEntry> entries);

} // namespace aicli::ui
