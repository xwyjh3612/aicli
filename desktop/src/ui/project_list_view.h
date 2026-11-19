#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace aicli::ui {

inline constexpr wchar_t kProjectListViewClassName[] =
    L"AiCli.ProjectListView";

struct ProjectListViewTheme {
    COLORREF background{};
    COLORREF scrollbar{};
    COLORREF scrollbar_hover{};
    COLORREF scrollbar_drag{};
    COLORREF reorder_hint{RGB(83, 143, 225)};
};

struct ProjectListEntry {
    std::wstring text;
    LPARAM data{};
};

struct ProjectListReorderCallbacks {
    // Returns the inclusive visible range in which this row can be moved.
    std::function<bool(int source, int& first, int& last)> begin;
    std::function<void(int source, int destination)> commit;
};

inline constexpr UINT kProjectItemDragPreview = 0x10000;

void SetProjectListReorderCallbacks(HWND window, ProjectListReorderCallbacks callbacks);
bool IsProjectListReordering(HWND window);
void CancelProjectListReorder(HWND window);
// Replaces model rows atomically without changing the current viewport or
// notifying selection listeners. The caller supplies the selected model row.
void ReplaceProjectListEntries(HWND window, std::vector<ProjectListEntry> entries, int selected);

bool RegisterProjectListViewClass(HINSTANCE instance);
HWND CreateProjectListView(HWND parent, int control_id, HINSTANCE instance);
void SetProjectListViewTheme(HWND window,
                             const ProjectListViewTheme& theme);
bool BeginProjectListScrollbarDrag(HWND window, POINT point);
bool UpdateProjectListScrollbarDrag(HWND window, POINT point);
bool EndProjectListScrollbarDrag(HWND window);
void CancelProjectListScrollbarDrag(HWND window);

} // namespace aicli::ui
