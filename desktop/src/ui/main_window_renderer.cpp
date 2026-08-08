#include "ui/main_window_renderer.h"

#include <algorithm>

#include "ui/drawing_utils.h"
#include "ui/footer_status.h"
#include "ui/window_chrome.h"

namespace aicli::ui {

void PaintMainWindow(HDC target_dc,
                     const RECT& client,
                     const RECT& dirty,
                     HWND window,
                     HWND search,
                     HFONT medium_font,
                     HFONT small_font,
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

    HPEN separator = CreatePen(PS_SOLID, 1, theme.border);
    auto previous_pen = static_cast<HPEN>(SelectObject(dc, separator));
    MoveToEx(dc, metrics.sidebar_width - 1, metrics.title_bar_height, nullptr);
    LineTo(dc, metrics.sidebar_width - 1,
           client.bottom - metrics.footer_height);
    MoveToEx(dc, 0, metrics.title_bar_height - 1, nullptr);
    LineTo(dc, client.right, metrics.title_bar_height - 1);
    MoveToEx(dc, 0, client.bottom - metrics.footer_height, nullptr);
    LineTo(dc, client.right, client.bottom - metrics.footer_height);
    SelectObject(dc, previous_pen);
    DeleteObject(separator);

    const COLORREF search_border = GetFocus() == search
        ? theme.accent
        : theme.border;
    DrawRoundedOutline(dc, RECT{14, 68, 272, 108}, search_border, 10);
    RECT search_line{
        22, 105, 264, GetFocus() == search ? 107 : 106};
    HBRUSH search_brush = CreateSolidBrush(
        GetFocus() == search ? theme.accent : theme.muted);
    FillRect(dc, &search_line, search_brush);
    DeleteObject(search_brush);
    DrawRoundedOutline(
        dc, RECT{302, 110, client.right - 14, client.bottom - 40},
        theme.border, 10);

    HBRUSH logo = CreateSolidBrush(theme.accent_strong);
    HPEN logo_pen = CreatePen(PS_SOLID, 1, RGB(75, 143, 244));
    auto previous_logo_brush = SelectObject(dc, logo);
    auto previous_logo_pen = SelectObject(dc, logo_pen);
    RoundRect(dc, 14, 10, 46, 42, 9, 9);
    SelectObject(dc, previous_logo_brush);
    SelectObject(dc, previous_logo_pen);
    DeleteObject(logo);
    DeleteObject(logo_pen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SelectObject(dc, medium_font);
    TextOutW(dc, 22, 16, L">_", 2);
    SetTextColor(dc, chrome.Text(theme));
    TextOutW(dc, 58, 11, L"AI CLI", 6);

    footer_status.Draw(
        dc, client, metrics.footer_height, small_font, theme);
}

} // namespace aicli::ui
