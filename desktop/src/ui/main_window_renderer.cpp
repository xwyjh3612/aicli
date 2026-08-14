#include "ui/main_window_renderer.h"

#include <algorithm>

#include "ui/drawing_utils.h"
#include "ui/footer_status.h"
#include "ui/styled_input.h"
#include "ui/window_chrome.h"

namespace aicli::ui {

void PaintMainWindow(HDC target_dc,
                     const RECT& client,
                     const RECT& dirty,
                     HWND window,
                     HWND search,
                     HICON app_icon,
                     HFONT medium_font,
                     HFONT small_font,
                     HFONT git_font,
                     const LayoutMetrics& metrics,
                     const ThemePalette& theme,
                     const WindowChrome& chrome,
                     const FooterStatusController& footer_status) {
    BufferedWindowPaint buffered(target_dc, client, dirty);
    HDC dc = buffered.dc();

    HBRUSH background = CreateSolidBrush(theme.window);
    FillRect(dc, &client, background);
    DeleteObject(background);

    HBRUSH title_bar = CreateSolidBrush(chrome.Background(theme));
    RECT top{0, 0, client.right, metrics.title_bar_height};
    FillRect(dc, &top, title_bar);
    DeleteObject(title_bar);
    chrome.DrawButtons(dc, window, theme);

    HBRUSH panel_brush = CreateSolidBrush(theme.panel);
    RECT side{
        0,
        metrics.title_bar_height,
        metrics.sidebar_width,
        client.bottom - metrics.footer_height};
    RECT footer{
        0,
        client.bottom - metrics.footer_height,
        client.right,
        client.bottom};
    FillRect(dc, &side, panel_brush);
    FillRect(dc, &footer, panel_brush);
    DeleteObject(panel_brush);

    PaintStyledInputParentFrame(search, dc);

    HPEN separator = CreatePen(PS_SOLID, 1, theme.border);
    auto previous_pen = static_cast<HPEN>(SelectObject(dc, separator));
    MoveToEx(dc, metrics.sidebar_width - 1, metrics.title_bar_height, nullptr);
    LineTo(dc, metrics.sidebar_width - 1,
           client.bottom - metrics.footer_height);
    MoveToEx(dc, 0, metrics.title_bar_height - 1, nullptr);
    LineTo(dc, client.right, metrics.title_bar_height - 1);
    MoveToEx(dc, 0, client.bottom - metrics.footer_height, nullptr);
    LineTo(dc, client.right, client.bottom - metrics.footer_height);
    // Separate the sidebar heading/action row from the project items without
    // adding a border around the list itself.
    const int sidebar_divider_y = metrics.title_bar_height + metrics.Scale(80);
    MoveToEx(dc, metrics.Scale(6), sidebar_divider_y, nullptr);
    LineTo(dc, metrics.sidebar_width - metrics.Scale(6), sidebar_divider_y);
    SelectObject(dc, previous_pen);
    DeleteObject(separator);

    if (app_icon) {
        DrawIconEx(dc, metrics.Scale(14), metrics.Scale(8), app_icon,
                   metrics.Scale(24), metrics.Scale(24),
                   0, nullptr, DI_NORMAL);
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, chrome.Text(theme));
    SelectObject(dc, medium_font);
    TextOutW(dc, metrics.Scale(48), metrics.Scale(10), L"AI CLI", 6);

    footer_status.Draw(
        dc, client, metrics.footer_height, small_font, git_font, theme);

    // DWM glass changes shade as activation changes, so the dark theme owns
    // its one-pixel outline in client space. This keeps every edge exactly
    // #12151B while retaining the compositor's shadow and resize behavior.
    if (theme.mode == ThemeMode::Dark && !IsZoomed(window) &&
        client.right > 1 && client.bottom > 1) {
        const COLORREF frame = RGB(18, 21, 27);
        HPEN frame_pen = CreatePen(PS_SOLID, 1, frame);
        const auto old_pen = static_cast<HPEN>(SelectObject(dc, frame_pen));
        MoveToEx(dc, 0, 0, nullptr);
        LineTo(dc, client.right - 1, 0);
        LineTo(dc, client.right - 1, client.bottom - 1);
        LineTo(dc, 0, client.bottom - 1);
        LineTo(dc, 0, 0);
        // GDI LineTo excludes the final endpoint on some DPI/rounded-corner
        // combinations. Explicitly own the extreme top-right client pixel so
        // DWM cannot expose a one-pixel white speck beside the close button.
        SetPixelV(dc, client.right - 1, 0, frame);
        SelectObject(dc, old_pen);
        DeleteObject(frame_pen);
    }
}

} // namespace aicli::ui
