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
    bool has_selection, bool can_paste, bool can_clear_input, bool has_link) {
    std::vector<aicli::ui::RoundedContextMenuEntry> entries{
        {static_cast<int>(TerminalMenuCommand::CopySelection), L"复制选中内容", L"\xE8C8", has_selection},
        {static_cast<int>(TerminalMenuCommand::Paste), L"粘贴", L"\xE77F", can_paste},
        {static_cast<int>(TerminalMenuCommand::SelectAll), L"全选", L"\xE8B3", true, false, true},
        {static_cast<int>(TerminalMenuCommand::CopyAll), L"复制全部", L"\xE8C8"},
    };
    if (can_clear_input) entries.push_back({static_cast<int>(TerminalMenuCommand::ClearInput),
        L"清空输入", L"\xE75C", true, false, true});
    if (has_link) entries.push_back({static_cast<int>(TerminalMenuCommand::OpenLink),
        L"打开链接", L"\xE8A7", true, false, !can_clear_input});
    return entries;
}

std::optional<TerminalMenuCommand> ShowTerminalContextMenu(
    HWND owner, POINT position, bool light, bool has_selection, bool can_paste,
    bool can_clear_input, bool has_link) {
    aicli::ui::RoundedContextMenuOptions options;
    options.owner = owner;
    const MenuFont text{aicli::ui::CreateDialogFont(owner, 14, FW_NORMAL)};
    const MenuFont icons{aicli::ui::CreateDialogIconFont(owner, 16)};
    options.text_font = text.value;
    options.icon_font = icons.value;
    options.screen_position = position;
    options.palette = aicli::ui::MakeThemePalette(light);
    options.consume_keyboard_input = true;
    options.route_mouse_pointer_input = true;
    options.dpi = static_cast<int>(GetDpiForWindow(owner));
    const auto result = aicli::ui::ShowRoundedContextMenu(
        options, TerminalContextMenuEntries(has_selection, can_paste, can_clear_input, has_link));
    if (!result) return std::nullopt;
    return static_cast<TerminalMenuCommand>(result->command);
}

} // namespace aicli::terminal
