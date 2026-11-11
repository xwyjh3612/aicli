#pragma once

#include <windows.h>
#include <algorithm>

namespace aicli::ui
{

inline int DpiFor(HWND window) noexcept
{
    return window ? std::max(96, static_cast<int>(GetDpiForWindow(window))) : 96;
}
inline int DpiScale(HWND window, int value) noexcept
{
    return MulDiv(value, DpiFor(window), 96);
}
inline HFONT CreateDialogFont(HWND owner, int height, int weight)
{
    return CreateFontW(-DpiScale(owner, height), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Segoe UI");
}
inline HFONT CreateDialogIconFont(HWND owner, int height)
{
    return CreateFontW(-DpiScale(owner, height), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Segoe MDL2 Assets");
}

} // namespace aicli::ui
