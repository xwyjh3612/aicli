#pragma once

#include "core/models.h"
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aicli { class DataStore; }

namespace aicli::project_views {
inline constexpr wchar_t kAll[] = L"全部";
inline constexpr wchar_t kRunning[] = L"已启动";
// Distinct keys keep existing user groups named 最近/常用 intact.
inline constexpr wchar_t kRecent[] = L"@system/recent";
inline constexpr wchar_t kFrequent[] = L"@system/frequent";
inline constexpr size_t kAutomaticGroupLimit = 10;

inline bool IsAutomatic(std::wstring_view group) { return group == kRecent || group == kFrequent; }
inline bool IsSystem(std::wstring_view group) {
    return group == kAll || group == kRunning || IsAutomatic(group);
}
inline std::wstring Label(std::wstring_view group) {
    if (group == kRecent) return L"最近";
    if (group == kFrequent) return L"常用";
    return std::wstring(group);
}
inline const wchar_t* Icon(std::wstring_view group) {
    return group == kAll ? L"\xE71D" : group == kRunning ? L"\xE768" :
        group == kRecent ? L"\xE823" : group == kFrequent ? L"\xE734" : L"\xE8B7";
}
inline bool IsReservedName(std::wstring_view name) {
    return IsSystem(name) || name == L"最近" || name == L"常用";
}

struct Usage {
    std::int64_t last_used_ms{}, last_counted_ms{}, count{};
};
class UsageHistory {
public:
    void Load(DataStore& store, const std::vector<Project>& projects);
    // Coalesce selecting and immediately starting the same project into one use.
    bool RecordUse(DataStore& store, const std::wstring& id, std::int64_t now_ms = 0);
    const std::unordered_map<std::wstring, Usage>& Records() const { return records_; }
private:
    std::unordered_map<std::wstring, Usage> records_;
};

std::vector<std::wstring> LoadRunningOrder(DataStore& store);
bool SaveRunningOrder(DataStore& store, const std::vector<std::wstring>& order);
std::vector<int> Build(const std::vector<Project>& projects, std::wstring_view group,
    std::wstring_view default_group, const std::unordered_set<std::wstring>& running,
    const std::vector<std::wstring>& running_order, const UsageHistory& usage);
std::optional<std::vector<std::wstring>> MoveRunning(const std::vector<Project>& projects,
    const std::vector<std::wstring>& saved_order, const std::vector<std::wstring>& visible,
    int source, int destination);

} // namespace aicli::project_views
