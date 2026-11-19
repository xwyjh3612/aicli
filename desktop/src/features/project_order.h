#pragma once

#include "core/models.h"
#include <algorithm>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace aicli::project_order {

struct Plan {
    std::wstring group;
    std::vector<std::wstring> ids;
};

// Visible rows may omit stopped projects. Reorder only the slots occupied by
// visible projects, preserving every hidden project's position in the group.
inline std::optional<Plan> MoveVisible(const std::vector<Project>& projects,
                                      const std::vector<std::wstring>& visible,
                                      int source, int destination,
                                      std::wstring_view default_group) {
    if (source < 0 || destination < 0 || source >= static_cast<int>(visible.size()) ||
        destination >= static_cast<int>(visible.size()) || source == destination) return std::nullopt;
    const auto find = [&](const std::wstring& id) {
        return std::find_if(projects.begin(), projects.end(), [&](const Project& p) { return p.id == id; });
    };
    const auto moving = find(visible[source]);
    if (moving == projects.end()) return std::nullopt;
    Plan plan{moving->group.empty() ? std::wstring(default_group) : moving->group, {}};
    std::vector<std::wstring> shown;
    std::unordered_set<std::wstring> unique;
    int from = -1, to = -1;
    for (int row = 0; row < static_cast<int>(visible.size()); ++row) {
        const auto project = find(visible[row]);
        if (project == projects.end() || !unique.insert(project->id).second) return std::nullopt;
        const auto group = project->group.empty() ? std::wstring(default_group) : project->group;
        if (row >= std::min(source, destination) && row <= std::max(source, destination) && group != plan.group)
            return std::nullopt;
        if (group != plan.group) continue;
        if (row == source) from = static_cast<int>(shown.size());
        if (row == destination) to = static_cast<int>(shown.size());
        shown.push_back(project->id);
    }
    if (from < 0 || to < 0) return std::nullopt;
    const std::unordered_set<std::wstring> shown_ids(shown.begin(), shown.end());
    const auto id = shown[from];
    shown.erase(shown.begin() + from);
    shown.insert(shown.begin() + to, id);
    size_t next = 0;
    for (const auto& project : projects) {
        const auto group = project.group.empty() ? std::wstring(default_group) : project.group;
        if (group == plan.group)
            plan.ids.push_back(shown_ids.contains(project.id) ? shown[next++] : project.id);
    }
    return plan;
}

} // namespace aicli::project_order
