#include "ui/main_window_renderer.h"

#include <algorithm>

#include "ui/drawing_utils.h"
#include "ui/footer_status.h"
#include "ui/styled_input.h"
#include "ui/window_chrome.h"

namespace aicli::ui {

namespace {

void DrawHorizontalGradientSeparator(HDC dc,
                                     int width,
                                     int y,
                                     int peak_x,
                                     COLORREF edge_color,
                                     COLORREF peak_color) {
    if (!dc || width <= 0) return;
    peak_x = std::clamp(peak_x, 1, std::max(1, width - 1));

    const auto vertex = [y](int x, COLORREF color) {
        TRIVERTEX result{};
        result.x = x;
        result.y = y;
        result.Red = static_cast<COLOR16>(GetRValue(color) << 8);
        result.Green = static_cast<COLOR16>(GetGValue(color) << 8);
        result.Blue = static_cast<COLOR16>(GetBValue(color) << 8);
        result.Alpha = 0xFF00;
        return result;
    };
    const auto draw_segment = [&](int left, int right,
                                  COLORREF left_color,
                                  COLORREF right_color) {
        if (right <= left) return;
        TRIVERTEX vertices[2]{
            vertex(left, left_color), vertex(right, right_color)};
        vertices[1].y = y + 1;
        GRADIENT_RECT gradient{0, 1};
        GradientFill(dc, vertices, 2, &gradient, 1, GRADIENT_FILL_RECT_H);
    };

    draw_segment(0, peak_x, edge_color, peak_color);
    draw_segment(peak_x, width, peak_color, edge_color);
}

} // namespace

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
                     COLORREF sidebar_background,
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

    HBRUSH sidebar_brush = CreateSolidBrush(sidebar_background);
    RECT side{
        0,
        metrics.title_bar_height,
        metrics.sidebar_width,
        client.bottom - metrics.footer_height};
    FillRect(dc, &side, sidebar_brush);
    DeleteObject(sidebar_brush);

    HBRUSH panel_brush = CreateSolidBrush(theme.titlebar);
    RECT footer{
        0,
        client.bottom - metrics.footer_height,
        client.right,
        client.bottom};
    FillRect(dc, &footer, panel_brush);
    DeleteObject(panel_brush);

    PaintStyledInputParentFrame(search, dc);

    HPEN separator = CreatePen(PS_SOLID, 1, theme.border);
    auto previous_pen = static_cast<HPEN>(SelectObject(dc, separator));
    MoveToEx(dc, metrics.sidebar_width - 1, metrics.title_bar_height, nullptr);
    LineTo(dc, metrics.sidebar_width - 1,
           client.bottom - metrics.footer_height);
    if (theme.mode == ThemeMode::Light) {
        MoveToEx(dc, 0, metrics.title_bar_height - 1, nullptr);
        LineTo(dc, client.right, metrics.title_bar_height - 1);
        MoveToEx(dc, 0, client.bottom - metrics.footer_height, nullptr);
        LineTo(dc, client.right, client.bottom - metrics.footer_height);
    }
    // Separate the sidebar heading/action row from the project items without
    // adding a border around the list itself.
    int sidebar_divider_y = metrics.title_bar_height + metrics.Scale(88);
    RECT title_rect{};
    RECT list_rect{};
    if (sidebar_title && project_list &&
        GetWindowRect(sidebar_title, &title_rect) &&
        GetWindowRect(project_list, &list_rect)) {
        MapWindowPoints(HWND_DESKTOP, window,
                        reinterpret_cast<POINT*>(&title_rect), 2);
        MapWindowPoints(HWND_DESKTOP, window,
                        reinterpret_cast<POINT*>(&list_rect), 2);
        if (list_rect.top > title_rect.bottom) {
            sidebar_divider_y =
                title_rect.bottom + (list_rect.top - title_rect.bottom) / 2;
        }
    }
    sidebar_divider_y += metrics.Scale(2);
    MoveToEx(dc, metrics.Scale(2), sidebar_divider_y, nullptr);
    LineTo(dc, metrics.sidebar_width - metrics.Scale(2), sidebar_divider_y);
    SelectObject(dc, previous_pen);
    DeleteObject(separator);

    if (theme.mode == ThemeMode::Dark) {
        constexpr COLORREF gradient_peak = RGB(34, 39, 83);
        DrawHorizontalGradientSeparator(
            dc, client.right, metrics.title_bar_height - 1,
            client.right / 3, theme.titlebar, gradient_peak);
        DrawHorizontalGradientSeparator(
            dc, client.right, client.bottom - metrics.footer_height,
            client.right * 2 / 3, theme.titlebar, gradient_peak);
    }

    if (app_icon) {
        DrawIconEx(dc, metrics.Scale(14), metrics.Scale(8), app_icon,
                   metrics.Scale(24), metrics.Scale(24),
                   0, nullptr, DI_NORMAL);
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, chrome.Text(theme));
    SelectObject(dc, medium_font);
    TextOutW(dc, metrics.Scale(48), metrics.Scale(10), L"AI CLI", 6);

    ThemePalette footer_theme = theme;
    footer_theme.panel = theme.titlebar;
    footer_status.Draw(
        dc, client, metrics.footer_height, small_font, git_font, footer_theme);

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
