#include "features/project_group_views.h"
#include "core/text_utils.h"
#include "data/data_store.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace aicli::project_views {
namespace {
constexpr wchar_t kUsageKey[] = L"native_project_usage_v1";
constexpr wchar_t kRunningOrderKey[] = L"native_running_project_order_v1";
constexpr std::int64_t kUseCoalesceMs = 30000;

bool SaveUsage(DataStore& store, const std::unordered_map<std::wstring, Usage>& records) {
    nlohmann::json values = nlohmann::json::object();
    for (const auto& [id, usage] : records)
        values[text::Utf8Text(id)] = {usage.last_used_ms, usage.last_counted_ms, usage.count};
    return store.SetSetting(kUsageKey, text::WideText(nlohmann::json{{"schema", 1}, {"projects", values}}.dump()));
}

std::vector<std::wstring> CompleteOrder(const std::vector<Project>& projects,
                                      const std::vector<std::wstring>& saved) {
    std::unordered_set<std::wstring> known;
    for (const auto& project : projects) known.insert(project.id);
    std::vector<std::wstring> order;
    for (const auto& id : saved) if (known.erase(id)) order.push_back(id);
    for (const auto& project : projects) if (known.erase(project.id)) order.push_back(project.id);
    return order;
}
}

void UsageHistory::Load(DataStore& store, const std::vector<Project>& projects) {
    records_.clear();
    const auto saved = store.GetSetting(kUsageKey);
    if (!saved.empty()) {
        try {
            const auto data = nlohmann::json::parse(text::Utf8Text(saved));
            if (data.at("schema") != 1 || !data.at("projects").is_object()) throw std::runtime_error("invalid usage history");
            for (const auto& project : projects) {
                const auto found = data.at("projects").find(text::Utf8Text(project.id));
                if (found == data.at("projects").end()) continue;
                if (!found->is_array() || found->size() != 3) continue;
                Usage usage{found->at(0).get<std::int64_t>(), found->at(1).get<std::int64_t>(), found->at(2).get<std::int64_t>()};
                if (usage.last_used_ms > 0 && usage.last_counted_ms > 0 && usage.count > 0)
                    records_.emplace(project.id, usage);
            }
            return;
        } catch (...) { records_.clear(); }
    }
    // Bootstrap existing installations from retained launch events once.
    // Later event trimming cannot reduce the persistent usage counters.
    for (const auto& project : projects) {
        std::int64_t after{};
        Usage usage;
        while (true) {
            const auto events = store.LoadEvents(project.id, after, 1000, false);
            for (const auto& event : events) {
                after = std::max(after, event.id);
                if (event.event_type != L"conversation.running") continue;
                usage.last_used_ms = std::max(usage.last_used_ms, event.created_at * 1000);
                ++usage.count;
            }
            if (events.size() < 1000) break;
        }
        if (usage.count) {
            usage.last_counted_ms = usage.last_used_ms;
            records_.emplace(project.id, usage);
        }
    }
    (void)SaveUsage(store, records_);
}

bool UsageHistory::RecordUse(DataStore& store, const std::wstring& id, std::int64_t now_ms) {
    const auto projects = store.LoadProjects();
    if (std::none_of(projects.begin(), projects.end(), [&](const Project& p) { return p.id == id; })) return false;
    if (!now_ms) now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (now_ms <= 0) return false;
    const auto found = records_.find(id);
    const bool existed = found != records_.end();
    Usage next = existed ? found->second : Usage{};
    const auto previous = next;
    next.last_used_ms = std::max(now_ms, next.last_used_ms);
    if (!next.count || now_ms - next.last_counted_ms >= kUseCoalesceMs) {
        if (next.count < std::numeric_limits<std::int64_t>::max()) ++next.count;
        next.last_counted_ms = now_ms;
    }
    records_[id] = next;
    if (SaveUsage(store, records_)) return true;
    if (!existed) records_.erase(id);
    else records_[id] = previous;
    return false;
}

std::vector<std::wstring> LoadRunningOrder(DataStore& store) {
    std::vector<std::wstring> order;
    try {
        const auto data = nlohmann::json::parse(text::Utf8Text(store.GetSetting(kRunningOrderKey, L"[]")));
        if (!data.is_array()) return {};
        for (const auto& id : data) if (id.is_string()) order.push_back(text::WideText(id.get<std::string>()));
    } catch (...) {}
    return order;
}
bool SaveRunningOrder(DataStore& store, const std::vector<std::wstring>& order) {
    nlohmann::json data = nlohmann::json::array();
    for (const auto& id : order) data.push_back(text::Utf8Text(id));
    return store.SetSetting(kRunningOrderKey, text::WideText(data.dump()));
}

std::vector<int> Build(const std::vector<Project>& projects, std::wstring_view group,
    std::wstring_view default_group, const std::unordered_set<std::wstring>& running,
    const std::vector<std::wstring>& running_order, const UsageHistory& history) {
    std::vector<int> rows;
    for (int index = 0; index < static_cast<int>(projects.size()); ++index) {
        const auto& project = projects[index];
        if (group == kRunning && !running.contains(project.id)) continue;
        if (IsAutomatic(group) && !history.Records().contains(project.id)) continue;
        if (!IsSystem(group) && (project.group.empty() ? default_group : std::wstring_view(project.group)) != group) continue;
        rows.push_back(index);
    }
    if (group == kRunning) {
        const auto order = CompleteOrder(projects, running_order);
        std::unordered_map<std::wstring, size_t> rank;
        for (size_t index = 0; index < order.size(); ++index) rank.emplace(order[index], index);
        std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) { return rank.at(projects[a].id) < rank.at(projects[b].id); });
    } else if (IsAutomatic(group)) {
        std::sort(rows.begin(), rows.end(), [&](int a, int b) {
            const auto& left = history.Records().at(projects[a].id);
            const auto& right = history.Records().at(projects[b].id);
            if (group == kFrequent && left.count != right.count) return left.count > right.count;
            if (left.last_used_ms != right.last_used_ms) return left.last_used_ms > right.last_used_ms;
            return projects[a].id < projects[b].id;
        });
        if (rows.size() > kAutomaticGroupLimit) rows.resize(kAutomaticGroupLimit);
    }
    return rows;
}

std::optional<std::vector<std::wstring>> MoveRunning(const std::vector<Project>& projects,
    const std::vector<std::wstring>& saved_order, const std::vector<std::wstring>& visible,
    int source, int destination) {
    if (source < 0 || destination < 0 || source >= static_cast<int>(visible.size()) ||
        destination >= static_cast<int>(visible.size()) || source == destination) return std::nullopt;
    auto full = CompleteOrder(projects, saved_order);
    const std::unordered_set<std::wstring> known(full.begin(), full.end());
    std::unordered_set<std::wstring> shown;
    for (const auto& id : visible) if (!known.contains(id) || !shown.insert(id).second) return std::nullopt;
    auto reordered = visible;
    const auto moving = reordered[source];
    reordered.erase(reordered.begin() + source);
    reordered.insert(reordered.begin() + destination, moving);
    size_t next{};
    for (auto& id : full) if (shown.contains(id)) id = reordered[next++];
    return full;
}
} // namespace aicli::project_views
