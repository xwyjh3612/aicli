#pragma once

#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/models.h"
#include "ui/theme.h"

namespace aicli
{
class DataStore;
}
namespace aicli::ui
{

struct ProjectEditorFonts
{
    HFONT body{}, medium{}, hint{}, title{}, subtitle{}, icon{};
};

struct ProjectEditorOptions
{
    DataStore &store;
    const std::vector<Project> &projects;
    const std::vector<ProjectType> &types;
    const std::vector<ProjectGroup> &groups;
    const ThemePalette &palette;
    std::function<ProjectEditorFonts()> fonts;
    std::function<std::wstring()> selected_group;
    std::function<std::wstring()> default_group;
    std::function<void()> layout_owner;
};

// Owns the project form's fields, draft, dropdowns and hover state. The window
// provides current models and styling; persistence remains with its caller.
class ProjectEditorDialog
{
  public:
    explicit ProjectEditorDialog(ProjectEditorOptions options);
    ~ProjectEditorDialog();
    ProjectEditorDialog(const ProjectEditorDialog &) = delete;
    ProjectEditorDialog &operator=(const ProjectEditorDialog &) = delete;

    bool Show(HWND owner, Project &result, const Project *initial = nullptr);
    HWND window() const noexcept;
    void RefreshFonts();
    void RefreshTheme();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aicli::ui
