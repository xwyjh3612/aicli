#pragma once

#include <windows.h>

namespace aicli::ui {

inline constexpr wchar_t kProjectListViewClassName[] =
    L"AiCli.ProjectListView";

struct ProjectListViewTheme {
    COLORREF background{};
    COLORREF scrollbar{};
    COLORREF scrollbar_hover{};
    COLORREF scrollbar_drag{};
};

bool RegisterProjectListViewClass(HINSTANCE instance);
HWND CreateProjectListView(HWND parent, int control_id, HINSTANCE instance);
void SetProjectListViewTheme(HWND window,
                             const ProjectListViewTheme& theme);
bool BeginProjectListScrollbarDrag(HWND window, POINT point);
bool UpdateProjectListScrollbarDrag(HWND window, POINT point);
bool EndProjectListScrollbarDrag(HWND window);
void CancelProjectListScrollbarDrag(HWND window);

} // namespace aicli::ui
