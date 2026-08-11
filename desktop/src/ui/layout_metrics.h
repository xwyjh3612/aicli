#pragma once

#include <windows.h>

namespace aicli::ui {

// Geometry is owned by the frame, not by individual paint functions.  The
// base values are expressed at 96 DPI and scaled at the window boundary.
struct LayoutMetrics {
    int dpi{96};
    int title_bar_height{40};
    int footer_height{30};
    int sidebar_width{286};
    int caption_button_width{46};
    int resize_border{8};
    int modal_radius{14};
    int control_radius{9};

    static LayoutMetrics ForWindow(HWND window) {
        LayoutMetrics result;
        result.dpi = window ? static_cast<int>(GetDpiForWindow(window)) : 96;
        const auto scale = [result](int value) {
            return MulDiv(value, result.dpi, 96);
        };
        result.title_bar_height = scale(40);
        result.footer_height = scale(30);
        result.sidebar_width = scale(286);
        result.caption_button_width = scale(46);
        result.resize_border = scale(8);
        result.modal_radius = scale(14);
        result.control_radius = scale(9);
        return result;
    }
};

} // namespace aicli::ui
