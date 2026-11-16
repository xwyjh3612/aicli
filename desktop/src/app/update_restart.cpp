#include "app/update_restart.h"
#include "app/update_manifest.h"
#include "core/text_utils.h"
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace aicli::update {
namespace {
constexpr wchar_t kRestartKey[] = L"update_restart_plan";
}

bool SaveRestartPlan(DataStore& store, std::wstring_view target_version,
                     const RestartPlan& plan) {
    try {
        const auto version = text::Utf8Text(target_version);
        (void)IsNewerVersion(version, version);
        nlohmann::json value{{"schema", 1}, {"version", version},
            {"selected", text::Utf8Text(plan.selected)},
            {"group", text::Utf8Text(plan.group)}, {"projects", nlohmann::json::array()}};
        std::unordered_set<std::wstring> unique;
        for (const auto& id : plan.projects) {
            if (!id.empty() && unique.insert(id).second)
                value["projects"].push_back(text::Utf8Text(id));
        }
        return store.SetSetting(kRestartKey, text::WideText(value.dump()));
    } catch (...) { return false; }
}

void CancelRestartPlan(DataStore& store) { store.SetSetting(kRestartKey, L""); }

std::optional<RestartPlan> TakeRestartPlan(DataStore& store,
                                         std::wstring_view current_version) {
    const auto saved = store.GetSetting(kRestartKey);
    if (saved.empty()) return std::nullopt;
    try {
        const auto value = nlohmann::json::parse(text::Utf8Text(saved));
        if (value.at("schema") != 1 || !value.at("projects").is_array())
            throw std::runtime_error("Invalid restart plan");
        const auto target = value.at("version").get<std::string>();
        if (IsNewerVersion(target, text::Utf8Text(current_version))) return std::nullopt;
        RestartPlan plan;
        plan.selected = text::WideText(value.at("selected").get<std::string>());
        plan.group = text::WideText(value.at("group").get<std::string>());
        const auto projects = store.LoadProjects();
        std::unordered_set<std::wstring> valid;
        for (const auto& project : projects) valid.insert(project.id);
        for (const auto& entry : value.at("projects")) {
            const auto id = text::WideText(entry.get<std::string>());
            if (valid.erase(id)) plan.projects.push_back(id);
        }
        if (!store.SetSetting(kRestartKey, L"")) return std::nullopt;
        return plan;
    } catch (...) {
        CancelRestartPlan(store);
        return std::nullopt;
    }
}
}
