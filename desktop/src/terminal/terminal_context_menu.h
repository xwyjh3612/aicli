#pragma once

#include "ui/rounded_context_menu.h"

namespace aicli::terminal {

enum class TerminalMenuCommand { CopySelection = 1, Paste, SelectAll, CopyAll };

std::vector<aicli::ui::RoundedContextMenuEntry> TerminalContextMenuEntries(
    bool has_selection, bool can_paste);
std::optional<TerminalMenuCommand> ShowTerminalContextMenu(
    HWND owner, POINT position, bool light, bool has_selection, bool can_paste);

} // namespace aicli::terminal
