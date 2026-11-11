#pragma once
#include <windows.h>
#include <string>

namespace aicli::ui
{
bool ShowGroupNameDialog(HWND owner, const std::wstring &title, const std::wstring &subtitle,
                         const std::wstring &initial_name, bool light_theme, std::wstring &name);
}
