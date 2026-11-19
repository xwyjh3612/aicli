#include "terminal/terminal_context_menu.h"
#include "ui/dialog_metrics.h"

namespace aicli::terminal {
namespace {
struct MenuFont {
    HFONT value{};
    ~MenuFont() { if (value) DeleteObject(value); }
};
}

std::vector<aicli::ui::RoundedContextMenuEntry> TerminalContextMenuEntries(
    bool has_selection, bool can_paste) {
    return {
        {static_cast<int>(TerminalMenuCommand::CopySelection), L"复制选中内容", L"\xE8C8", has_selection},
        {static_cast<int>(TerminalMenuCommand::Paste), L"粘贴", L"\xE77F", can_paste},
        {static_cast<int>(TerminalMenuCommand::SelectAll), L"全选", L"\xE8B3", true, false, true},
        {static_cast<int>(TerminalMenuCommand::CopyAll), L"复制全部", L"\xE8C8"},
    };
}

std::optional<TerminalMenuCommand> ShowTerminalContextMenu(
    HWND owner, POINT position, bool light, bool has_selection, bool can_paste) {
    aicli::ui::RoundedContextMenuOptions options;
    options.owner = owner;
    const MenuFont text{aicli::ui::CreateDialogFont(owner, 14, FW_NORMAL)};
    const MenuFont icons{aicli::ui::CreateDialogIconFont(owner, 16)};
    options.text_font = text.value;
    options.icon_font = icons.value;
    options.screen_position = position;
    options.palette = aicli::ui::MakeThemePalette(light);
    options.consume_keyboard_input = true;
    options.dpi = static_cast<int>(GetDpiForWindow(owner));
    const auto result = aicli::ui::ShowRoundedContextMenu(
        options, TerminalContextMenuEntries(has_selection, can_paste));
    if (!result) return std::nullopt;
    return static_cast<TerminalMenuCommand>(result->command);
}

} // namespace aicli::terminal
