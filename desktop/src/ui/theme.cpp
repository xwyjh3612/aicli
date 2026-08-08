#include "ui/theme.h"

namespace aicli::ui {

ThemePalette MakeThemePalette(ThemeMode mode) {
    ThemePalette palette;
    palette.mode = mode;
    if (mode == ThemeMode::Light) {
        palette.window = RGB(246, 248, 251);
        palette.panel = RGB(255, 255, 255);
        palette.surface = RGB(239, 243, 248);
        palette.border = RGB(215, 221, 230);
        palette.text = RGB(28, 34, 43);
        palette.muted = RGB(101, 111, 125);
        palette.accent = RGB(46, 118, 211);
        palette.accent_strong = RGB(39, 102, 190);
        palette.field = RGB(239, 243, 248);
        palette.field_focus = RGB(231, 238, 249);
        palette.selection = RGB(226, 237, 251);
        palette.list_surface = RGB(250, 251, 253);
        palette.list_text = RGB(67, 76, 89);
        palette.hover = RGB(232, 236, 243);
        palette.pressed = RGB(214, 222, 233);
        palette.titlebar = RGB(255, 255, 255);
        palette.titlebar_text = RGB(28, 34, 43);
        palette.titlebar_muted = RGB(101, 111, 125);
        palette.titlebar_inactive = RGB(232, 234, 238);
        palette.titlebar_inactive_text = RGB(92, 98, 108);
        palette.titlebar_hover = RGB(232, 236, 243);
        palette.titlebar_pressed = RGB(214, 222, 233);
        palette.titlebar_close_hover = RGB(232, 86, 99);
        palette.titlebar_close_pressed = RGB(198, 57, 70);
        palette.scrim = RGB(0, 0, 0);
        palette.scrim_opacity = 0.34F;
        palette.danger = RGB(198, 57, 70);
        palette.warning = RGB(177, 113, 16);
        palette.success = RGB(28, 139, 91);
    } else {
        palette.window = RGB(12, 14, 18);
        palette.panel = RGB(18, 21, 27);
        palette.surface = RGB(25, 29, 37);
        palette.border = RGB(45, 51, 62);
        palette.text = RGB(238, 241, 247);
        palette.muted = RGB(139, 148, 162);
        palette.accent = RGB(83, 156, 255);
        palette.accent_strong = RGB(42, 108, 214);
        palette.field = RGB(37, 40, 47);
        palette.field_focus = RGB(43, 48, 57);
        palette.selection = RGB(31, 57, 87);
        palette.list_surface = RGB(31, 34, 41);
        palette.list_text = RGB(207, 214, 225);
        palette.hover = RGB(37, 42, 51);
        palette.pressed = RGB(48, 56, 68);
        palette.titlebar = RGB(18, 21, 27);
        palette.titlebar_text = RGB(238, 241, 247);
        palette.titlebar_muted = RGB(139, 148, 162);
        palette.titlebar_inactive = RGB(61, 64, 70);
        palette.titlebar_inactive_text = RGB(214, 217, 223);
        palette.titlebar_hover = RGB(37, 42, 51);
        palette.titlebar_pressed = RGB(48, 56, 68);
        palette.titlebar_close_hover = RGB(120, 38, 47);
        palette.titlebar_close_pressed = RGB(157, 44, 55);
        palette.scrim = RGB(0, 0, 0);
        palette.scrim_opacity = 0.54F;
        palette.danger = RGB(157, 44, 55);
        palette.warning = RGB(245, 178, 72);
        palette.success = RGB(73, 203, 145);
    }
    return palette;
}

ThemeManager::ThemeManager() : palette_(MakeThemePalette(ThemeMode::Dark)) {}

} // namespace aicli::ui
