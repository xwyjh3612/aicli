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

    // The EDIT already owns the exact requested field surface and changes its
    // fill on focus. Do not paint a second, larger outline/underline behind it;
    // that old fixed geometry became visibly offset after the sidebar was
    // tightened to six pixels. The native terminal also fills its complete
    // work area now, so it no longer needs a surrounding decorative frame.
    (void)search;

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
