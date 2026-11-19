#include "features/project_group_views.h"
#include "data/data_store.h"
#include <windows.h>
#include <winsqlite/winsqlite3.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
namespace views = aicli::project_views;
using aicli::Project;
void Require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}
std::vector<std::wstring> Ids(const std::vector<Project>& projects, const std::vector<int>& rows) {
    std::vector<std::wstring> result;
    for (int row : rows) result.push_back(projects.at(row).id);
    return result;
}
void Sql(const std::filesystem::path& database, const char* sql) {
    sqlite3* connection{};
    Require(sqlite3_open16(database.c_str(), &connection) == SQLITE_OK, "open fixture SQL connection");
    const auto result = sqlite3_exec(connection, sql, nullptr, nullptr, nullptr);
    sqlite3_close(connection);
    Require(result == SQLITE_OK, "execute fixture SQL");
}
std::vector<Project> Seed(aicli::DataStore& store, const std::filesystem::path& root, int count) {
    Require(store.AddGroup(L"A") && store.AddGroup(L"B"), "create real groups");
    std::vector<Project> projects;
    for (int index = 0; index < count; ++index) {
        Project project;
        project.name = L"Project " + std::to_wstring(index);
        project.type = store.LoadProjectTypes().front().name;
        project.group = index % 2 ? L"B" : L"A";
        project.folder = root.wstring(); project.command = L"echo fixture";
        Require(store.SaveProject(project), "save fixture project");
        projects.push_back(project);
    }
    return projects;
}
void TestRunning(aicli::DataStore& store, const std::filesystem::path& database) {
    const auto projects = store.LoadProjects();
    views::UsageHistory history; history.Load(store, projects);
    const auto baseline = Ids(projects, views::Build(projects, views::kAll, L"A", {}, {}, history));
    const auto other = std::find_if(projects.begin(), projects.end(), [&](const auto& p) {return p.group != projects.front().group;});
    Require(other != projects.end(), "fixture contains multiple real groups");
    const std::vector<std::wstring> visible{projects[0].id, projects[2].id, other->id};
    std::unordered_set<std::wstring> running(visible.begin(), visible.end());
    Require(Ids(projects, views::Build(projects, views::kRunning, L"A", running, {}, history)) == visible,
        "running view includes live sessions across groups");
    const auto order = views::MoveRunning(projects, {}, visible, 0, 2);
    Require(order.has_value(), "running view permits cross-group sorting");
    const std::vector<std::wstring> expected{visible[1], visible[2], visible[0]};
    Require(Ids(projects, views::Build(projects, views::kRunning, L"A", running, *order, history)) == expected,
        "running view follows independent order");
    for (size_t index = 0; index < baseline.size(); ++index)
        if (!running.contains(baseline[index])) Require(order->at(index) == baseline[index], "stopped projects retain order slots");
    Require(views::SaveRunningOrder(store, *order), "persist independent running order");
    store.Close(); Require(store.Initialize(database.wstring()), "reopen fixture");
    const auto reloaded = store.LoadProjects();
    Require(Ids(reloaded, views::Build(reloaded, views::kAll, L"A", {}, {}, history)) == baseline,
        "running sorting leaves real group order untouched");
    for (size_t index = 0; index < projects.size(); ++index)
        Require(projects[index].group == reloaded[index].group && projects[index].sort_order == reloaded[index].sort_order,
            "running sorting does not move projects or alter real sort_order");
    const auto saved = views::LoadRunningOrder(store);
    Require(saved == *order && Ids(reloaded, views::Build(reloaded, views::kRunning, L"A", running, saved, history)) == expected,
        "running order survives restart");
    running.erase(visible[2]);
    Require(Ids(projects, views::Build(projects, views::kRunning, L"A", running, saved, history)) ==
        std::vector<std::wstring>{visible[1], visible[0]}, "stopping a session preserves remaining order");
    Require(!views::MoveRunning(projects, saved, visible, -1, 1) && !views::MoveRunning(projects, saved, visible, 1, 1),
        "invalid and no-op moves rejected");
    Require(!views::MoveRunning(projects, saved, {visible[0], L"deleted"}, 0, 1) &&
        !views::MoveRunning(projects, saved, {visible[0], visible[0]}, 0, 1), "deleted and duplicate captured IDs rejected");
    auto changed = projects;
    changed.erase(changed.begin());
    auto added = projects.front(); added.id = L"new-project"; changed.push_back(added);
    std::unordered_set<std::wstring> live;
    for (const auto& p : changed) live.insert(p.id);
    auto dirty = saved; dirty.insert(dirty.begin(), L"deleted"); dirty.push_back(saved.front());
    const auto completed = Ids(changed, views::Build(changed, views::kRunning, L"A", live, dirty, history));
    Require(completed.size() == changed.size() && completed.back() == added.id &&
        std::unordered_set<std::wstring>(completed.begin(), completed.end()).size() == completed.size(),
        "deleted and duplicated saved IDs ignored; new project appended");
}
void TestUsage(aicli::DataStore& store, const std::filesystem::path& database) {
    const auto projects = store.LoadProjects();
    const auto groups = store.LoadGroups();
    views::UsageHistory usage; usage.Load(store, projects);
    Require(views::Build(projects, views::kRecent, L"A", {}, {}, usage).empty() &&
        views::Build(projects, views::kFrequent, L"A", {}, {}, usage).empty(), "unused projects excluded from automatic views");
    for (int index = 0; index < 12; ++index)
        Require(usage.RecordUse(store, projects[index].id, 100000 + index * 40000), "record explicit use");
    Require(usage.RecordUse(store, projects[0].id, 700000) && usage.RecordUse(store, projects[0].id, 700001), "record select and launch");
    Require(usage.Records().at(projects[0].id).count == 2, "select and immediate launch count once");
    Require(!usage.RecordUse(store, L"missing", 800000), "unknown project cannot enter usage history");
    Require(usage.RecordUse(store, projects[1].id, 800000), "record another frequent project");
    Require(usage.RecordUse(store, projects[2].id, 800000), "record tie");
    const auto recent = Ids(projects, views::Build(projects, views::kRecent, L"A", {}, {}, usage));
    const auto frequent = Ids(projects, views::Build(projects, views::kFrequent, L"A", {}, {}, usage));
    Require(recent.size() == 10 && frequent.size() == 10, "automatic groups limit results to top ten");
    const auto tie_first = std::min(projects[1].id, projects[2].id);
    Require(recent.front() == tie_first && frequent.front() == tie_first && frequent[2] == projects[0].id,
        "recent sorts by last use; frequent sorts by count then last use with stable ties");
    Require(std::find(recent.begin(), recent.end(), projects[12].id) == recent.end(), "never used project stays absent");
    const auto count = usage.Records().at(projects[0].id).count;
    for (int index = 0; index < 3; ++index) (void)views::Build(projects, views::kRecent, L"A", {}, {}, usage);
    Require(usage.Records().at(projects[0].id).count == count, "view refresh never increments use count");
    Sql(database, "CREATE TRIGGER fail_usage BEFORE INSERT ON settings WHEN NEW.key='native_project_usage_v1' BEGIN SELECT RAISE(ABORT,'fixture failure'); END;");
    Require(!usage.RecordUse(store, projects[12].id, 900000) && !usage.Records().contains(projects[12].id),
        "failed new usage write rolls back in-memory insertion");
    Require(!usage.RecordUse(store, projects[0].id, 900000) && usage.Records().at(projects[0].id).count == count,
        "failed existing usage write rolls back previous record");
    Sql(database, "DROP TRIGGER fail_usage;");
    store.Close(); Require(store.Initialize(database.wstring()), "reopen usage fixture");
    views::UsageHistory restored; restored.Load(store, store.LoadProjects());
    Require(Ids(projects, views::Build(projects, views::kRecent, L"A", {}, {}, restored)) == recent &&
        Ids(projects, views::Build(projects, views::kFrequent, L"A", {}, {}, restored)) == frequent,
        "automatic rankings survive restart without counting restored sessions");
    Require(store.DeleteProject(projects[1].id), "delete used fixture project");
    restored.Load(store, store.LoadProjects());
    Require(!restored.Records().contains(projects[1].id), "deleted project removed from loaded usage");
    Require(store.LoadGroups().size() == groups.size(), "virtual system groups never enter persisted group table");
    Require(views::Label(views::kRecent) == L"最近" && views::Label(views::kFrequent) == L"常用" &&
        views::IsReservedName(L"最近") && views::IsSystem(views::kRecent) && !views::IsSystem(L"最近"),
        "system keys use readable labels without stealing legacy user group names");
}
void TestLegacy(const std::filesystem::path& root) {
    aicli::DataStore store;
    const auto database = root / L"legacy.db";
    Require(store.Initialize(database.wstring()), "initialize legacy fixture");
    const auto projects = Seed(store, root, 2);
    for (int index = 0; index < 1005; ++index)
        Require(store.AppendEvent(projects[0].id, L"conversation.running", "{}") > 0, "seed paginated launch history");
    Require(store.AppendEvent(projects[1].id, L"terminal.output", "{}") > 0, "seed non-use terminal event");
    views::UsageHistory usage; usage.Load(store, store.LoadProjects());
    Require(usage.Records().at(projects[0].id).count == 1005 && !usage.Records().contains(projects[1].id),
        "legacy launch history bootstraps across pages, ignoring terminal output");
    Sql(database, "DELETE FROM events;");
    views::UsageHistory restored; restored.Load(store, store.LoadProjects());
    Require(restored.Records().at(projects[0].id).count == 1005, "event trimming cannot reduce persisted usage count");
    Require(store.SetSetting(L"native_project_usage_v1", L"invalid-json"), "corrupt fixture setting");
    restored.Load(store, store.LoadProjects());
    Require(restored.Records().empty(), "invalid usage setting recovers safely");
    Require(store.SetSetting(L"native_running_project_order_v1", L"{}") && views::LoadRunningOrder(store).empty(),
        "invalid running order safely falls back");
}
}
int main() {
    std::filesystem::path root;
    try {
        auto temp = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
        if (temp.filename().empty()) temp = temp.parent_path();
        root = temp / (L"aicli-system-groups-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        Require(root.parent_path() == temp, "fixture stays inside temporary directory");
        std::filesystem::create_directories(root);
        {
            aicli::DataStore store;
            const auto database = root / L"views.db";
            Require(store.Initialize(database.wstring()), "initialize fixture");
            Seed(store, root, 15);
            TestRunning(store, database);
            TestUsage(store, database);
        }
        TestLegacy(root);
        Require(root.parent_path() == temp, "verify cleanup target");
        std::filesystem::remove_all(root);
        std::cout << "Running order, recent/frequent ranking, persistence, failure rollback and legacy bootstrap passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::wcerr << L"Test artifacts: " << root.wstring() << '\n'; return 1;
    }
}
