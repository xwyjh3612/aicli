#pragma once
#include <windows.h>
#include <string>

namespace aicli::ui
{
bool ShowProjectTypeEditorDialog(HWND owner, const std::wstring &title,
                                 const std::wstring &subtitle, const std::wstring &initial_name,
                                 const std::wstring &initial_command, bool initial_default,
                                 bool light_theme, std::wstring &name, std::wstring &command,
                                 bool &make_default);
}
