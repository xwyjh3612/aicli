#pragma once

#include "data/data_store.h"
#include <optional>
#include <string_view>

namespace aicli::update {
struct RestartPlan {
    std::vector<std::wstring> projects;
    std::wstring selected;
    std::wstring group;
};

// Checkpoint before launching Setup. Keep it in the existing SQLite database,
// so package migration protects it together with projects and settings.
bool SaveRestartPlan(DataStore& store, std::wstring_view target_version,
                     const RestartPlan& plan);
void CancelRestartPlan(DataStore& store);
// A failed/cancelled install must not reopen projects in the old version.
// Consume once, only when this or a newer target version actually starts.
std::optional<RestartPlan> TakeRestartPlan(DataStore& store,
                                         std::wstring_view current_version);
}
