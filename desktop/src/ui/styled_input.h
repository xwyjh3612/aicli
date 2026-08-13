#pragma once

#include <windows.h>
#include <string>

#include "ui/theme.h"

namespace aicli::ui {

HWND CreateStyledInput(HWND parent, int control_id, HFONT font,
                       const std::wstring& placeholder = L"",
                       bool read_only = false, bool tab_stop = true);
void SetStyledInputPlaceholder(HWND control, const std::wstring& placeholder);
std::wstring GetStyledInputText(HWND control);
void SetStyledInputText(HWND control, const std::wstring& value);
void SetStyledInputTheme(HWND control, const ThemePalette& palette);
void SetStyledInputFocusBorderOnly(HWND control, bool enabled = true);
void SetStyledInputLegacyPainting(HWND control, bool enabled = true);
void LayoutStyledInput(HWND control, const RECT& outer, int inset = 2, int radius = 5);
bool IsStyledInput(HWND control);

}
