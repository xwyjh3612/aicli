#pragma once

#include "ui/rounded_context_menu.h"

namespace aicli::terminal {

enum class TerminalMenuCommand { CopySelection = 1, Paste, SelectAll, CopyAll, ClearInput, OpenLink };

std::vector<aicli::ui::RoundedContextMenuEntry> TerminalContextMenuEntries(
    bool has_selection, bool can_paste, bool can_clear_input = false, bool has_link = false);
std::optional<TerminalMenuCommand> ShowTerminalContextMenu(
    HWND owner, POINT position, bool light, bool has_selection, bool can_paste,
    bool can_clear_input = false, bool has_link = false);

} // namespace aicli::terminal
