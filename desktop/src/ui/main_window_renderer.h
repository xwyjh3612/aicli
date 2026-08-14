#pragma once

#include <windows.h>

#include "ui/layout_metrics.h"
#include "ui/theme.h"

namespace aicli::ui {

class FooterStatusController;
class WindowChrome;

// Paints the non-child surface of the main window in one buffered frame.
// HWND creation and message routing stay in the controller; GDI composition
// stays here so WM_PAINT remains a small dispatch case.
void PaintMainWindow(HDC target_dc,
                     const RECT& client,
                     const RECT& dirty,
                     HWND window,
                     HWND search,
                     HWND sidebar_title,
                     HWND project_list,
                     HICON app_icon,
                     HFONT medium_font,
                     HFONT small_font,
                     HFONT git_font,
                     const LayoutMetrics& metrics,
                     const ThemePalette& theme,
                     const WindowChrome& chrome,
                     const FooterStatusController& footer_status);

} // namespace aicli::ui
