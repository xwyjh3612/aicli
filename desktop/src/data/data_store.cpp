#include "data/data_store.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>

namespace aicli {
namespace {

// Signed requests are created concurrently by the Android history, project
// list and WebSocket paths. Their sequence allocation is ordered, but network
// delivery (especially through FRP) is not. Keep a bounded reordering window
// while request-id and per-device sequence uniqueness still reject replays.
constexpr std::int64_t kSignedRequestReorderWindow = 256;
constexpr wchar_t kDefaultProjectTypeSetting[] = L"default_project_type_id";

struct StatementDeleter {
    void operator()(sqlite3_stmt* statement) const noexcept {
        if (statement) sqlite3_finalize(statement);
    }
};
using Statement = std::unique_ptr<sqlite3_stmt, StatementDeleter>;

std::int64_t UnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::wstring ColumnText(sqlite3_stmt* statement, int column) {
    const auto* value = static_cast<const wchar_t*>(sqlite3_column_text16(statement, column));
    return value ? std::wstring{value} : std::wstring{};
}

void BindText(sqlite3_stmt* statement, int index, const std::wstring& value) {
    sqlite3_bind_text16(statement, index, value.c_str(),
                        static_cast<int>(value.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
}

void BindUtf8(sqlite3_stmt* statement, int index, const std::string& value) {
    sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
}

std::wstring NewId() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return L"project-" + std::to_wstring(UnixSeconds());
    wchar_t value[40]{};
    StringFromGUID2(guid, value, static_cast<int>(std::size(value)));
    std::wstring result{value};
    if (result.size() > 2) result = result.substr(1, result.size() - 2);
    return result;
}

bool IsBlank(const std::wstring& value) {
    return value.empty() || std::all_of(value.begin(), value.end(), iswspace);
}

Statement Prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* raw{};
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) return {};
    return Statement{raw};
}

}

DataStore::~DataStore() {
    Close();
}

bool DataStore::Initialize(const std::wstring& database_path, std::wstring* error) {
    std::scoped_lock lock(mutex_);
    Close();
    try {
        const std::filesystem::path path{database_path};
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    } catch (...) {
        if (error) *error = L"无法创建数据目录";
        return false;
    }
    if (sqlite3_open16(database_path.c_str(), &db_) != SQLITE_OK) {
        if (error) *error = LastError();
        Close();
        return false;
    }
    database_path_ = database_path;
    sqlite3_busy_timeout(db_, 5000);
    if (!Execute("PRAGMA journal_mode=WAL;", error) ||
        !Execute("PRAGMA synchronous=NORMAL;", error) ||
        !Execute("PRAGMA foreign_keys=ON;", error) ||
        !CreateSchema(error) || !SeedDefaults(error)) {
        Close();
        return false;
    }
    return true;
}

void DataStore::Close() {
    std::scoped_lock lock(mutex_);
    if (db_) sqlite3_close_v2(db_);
    db_ = nullptr;
    database_path_.clear();
}

bool DataStore::Ready() const noexcept {
    std::scoped_lock lock(mutex_);
    return db_ != nullptr;
}

bool DataStore::Execute(const char* sql, std::wstring* error) {
    char* raw_error{};
    const int result = sqlite3_exec(db_, sql, nullptr, nullptr, &raw_error);
    if (result == SQLITE_OK) return true;
    if (error) {
        if (raw_error) {
            const int count = MultiByteToWideChar(CP_UTF8, 0, raw_error, -1, nullptr, 0);
            std::wstring message(static_cast<size_t>(std::max(1, count)), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, raw_error, -1, message.data(), count);
            if (!message.empty() && message.back() == L'\0') message.pop_back();
            *error = std::move(message);
        } else {
            *error = LastError();
        }
    }
    if (raw_error) sqlite3_free(raw_error);
    return false;
}

bool DataStore::CreateSchema(std::wstring* error) {
    static constexpr char schema[] = R"SQL(
CREATE TABLE IF NOT EXISTS project_types(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE COLLATE NOCASE,
    default_command TEXT NOT NULL,
    builtin INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS groups(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE COLLATE NOCASE,
    builtin INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS projects(
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    note TEXT NOT NULL DEFAULT '',
    type_name TEXT NOT NULL,
    group_name TEXT NOT NULL,
    folder TEXT NOT NULL,
    command TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'created',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS devices(
    phone_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    model TEXT NOT NULL,
    public_key_base64 TEXT NOT NULL,
    active INTEGER NOT NULL DEFAULT 1,
    paired_at INTEGER NOT NULL,
    last_seen INTEGER NOT NULL,
    last_sequence INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS device_requests(
    request_id TEXT PRIMARY KEY,
    phone_id TEXT NOT NULL,
    sequence INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    FOREIGN KEY(phone_id) REFERENCES devices(phone_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS events(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS terminal_snapshots(
    project_id TEXT PRIMARY KEY,
    text TEXT NOT NULL,
    updated_at INTEGER NOT NULL,
    FOREIGN KEY(project_id) REFERENCES projects(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_events_project_id ON events(project_id, id);
CREATE INDEX IF NOT EXISTS idx_device_requests_created ON device_requests(created_at);
CREATE INDEX IF NOT EXISTS idx_device_requests_phone_sequence
    ON device_requests(phone_id, sequence);
CREATE TABLE IF NOT EXISTS settings(
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
)SQL";
    if (!Execute(schema, error)) return false;

    bool has_sort_order = false;
    bool has_note = false;
    if (auto columns = Prepare(db_, "PRAGMA table_info(projects);"); columns) {
        while (sqlite3_step(columns.get()) == SQLITE_ROW) {
            const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(columns.get(), 1));
            if (!name) continue;
            if (strcmp(name, "sort_order") == 0) has_sort_order = true;
            else if (strcmp(name, "note") == 0) has_note = true;
        }
    }
    if (!has_note &&
        !Execute("ALTER TABLE projects ADD COLUMN note TEXT NOT NULL DEFAULT '';", error)) {
        return false;
    }
    if (has_sort_order) return true;
    if (!Execute("ALTER TABLE projects ADD COLUMN sort_order INTEGER NOT NULL DEFAULT 0;", error)) {
        return false;
    }

    // Preserve the list order users had before drag sorting existed.
    auto projects = Prepare(db_,
        "SELECT id,group_name FROM projects "
        "ORDER BY group_name COLLATE NOCASE,updated_at DESC,created_at DESC,id;");
    auto update = Prepare(db_, "UPDATE projects SET sort_order=? WHERE id=?;");
    if (!projects || !update) {
        if (error) *error = LastError();
        return false;
    }
    std::wstring current_group;
    std::int64_t order = 0;
    while (sqlite3_step(projects.get()) == SQLITE_ROW) {
        const auto group = ColumnText(projects.get(), 1);
        if (group != current_group) {
            current_group = group;
            order = 0;
        }
        sqlite3_reset(update.get());
        sqlite3_clear_bindings(update.get());
        sqlite3_bind_int64(update.get(), 1, order++);
        BindText(update.get(), 2, ColumnText(projects.get(), 0));
        if (sqlite3_step(update.get()) != SQLITE_DONE) {
            if (error) *error = LastError();
            return false;
        }
    }
    return true;
}

bool DataStore::SeedDefaults(std::wstring* error) {
    // Codex 0.163 defaults to an alternate screen that is not part of the
    // terminal scrollback. Inline mode keeps the full transcript available to
    // the desktop scrollbar and the Android terminal view.
    if (!Execute("INSERT OR IGNORE INTO project_types(name,default_command,builtin) VALUES"
                 "('Codex','codex --yolo --no-alt-screen resume --last',1),"
                 "('Claude','claude --dangerously-skip-permissions --continue; if ($LASTEXITCODE -ne 0) { claude --dangerously-skip-permissions }',1);",
                 error) ||
        !Execute("UPDATE project_types "
                 "SET default_command='codex --yolo --no-alt-screen resume --last' "
                 "WHERE builtin=1 AND name='Codex' "
                 "AND default_command IN ('codex --yolo resume --last',"
                 "'codex --yolo --no-alt-screen resume --last',"
                 "'codex --yolo --no-alt-screen -c tui.terminal_resize_reflow_max_rows=10000 resume --last');",
                 error) ||
        !Execute("UPDATE project_types "
                 "SET default_command='claude --dangerously-skip-permissions --continue; if ($LASTEXITCODE -ne 0) { claude --dangerously-skip-permissions }' "
                 "WHERE builtin=1 AND name='Claude' "
                 "AND default_command IN ("
                 "'claude --dangerously-skip-permissions --continue',"
                 "'claude --dangerously-skip-permissions --continue || claude --dangerously-skip-permissions');",
                 error) ||
        !Execute("UPDATE projects "
                 "SET command='claude --dangerously-skip-permissions --continue; if ($LASTEXITCODE -ne 0) { claude --dangerously-skip-permissions }' "
                 "WHERE type_name='Claude' AND command IN ("
                 "'claude --dangerously-skip-permissions --continue',"
                 "'claude --dangerously-skip-permissions --continue || claude --dangerously-skip-permissions');",
                 error) ||
        !Execute("UPDATE projects "
                 "SET command='codex --yolo --no-alt-screen resume --last' "
                 "WHERE type_name='Codex' AND command IN ('codex --yolo resume --last',"
                 "'codex --yolo --no-alt-screen resume --last',"
                 "'codex --yolo --no-alt-screen -c tui.terminal_resize_reflow_max_rows=10000 resume --last');",
                 error) ||
        !Execute("INSERT OR IGNORE INTO groups(name,builtin) VALUES('默认分组',1);", error) ||
        !Execute("INSERT OR IGNORE INTO settings(key,value) VALUES"
                 "('theme','dark'),('listen_port','4280'),('listen_address','0.0.0.0'),"
                 "('remote_address',''),('last_group','默认分组'),('last_project_id','');",
                 error)) {
        return false;
    }
    return true;
}

std::vector<ProjectType> DataStore::LoadProjectTypes() {
    std::scoped_lock lock(mutex_);
    std::vector<ProjectType> result;
    if (!db_) return result;
    auto statement = Prepare(db_, "SELECT id,name,default_command,builtin FROM project_types ORDER BY builtin DESC,id;");
    while (statement && sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back({sqlite3_column_int64(statement.get(), 0),
                          ColumnText(statement.get(), 1), ColumnText(statement.get(), 2),
                          sqlite3_column_int(statement.get(), 3) != 0});
    }
    return result;
}

std::vector<ProjectGroup> DataStore::LoadGroups() {
    std::scoped_lock lock(mutex_);
    std::vector<ProjectGroup> result;
    if (!db_) return result;
    auto statement = Prepare(db_, "SELECT id,name,builtin FROM groups ORDER BY builtin DESC,id;");
    while (statement && sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back({sqlite3_column_int64(statement.get(), 0), ColumnText(statement.get(), 1),
                          sqlite3_column_int(statement.get(), 2) != 0});
    }
    return result;
}

std::vector<Project> DataStore::LoadProjects() {
    std::scoped_lock lock(mutex_);
    std::vector<Project> result;
    if (!db_) return result;
    auto statement = Prepare(db_,
        "SELECT id,name,note,type_name,group_name,folder,command,status,sort_order "
        "FROM projects ORDER BY group_name COLLATE NOCASE,sort_order,id;");
    while (statement && sqlite3_step(statement.get()) == SQLITE_ROW) {
        Project project;
        project.id = ColumnText(statement.get(), 0);
        project.name = ColumnText(statement.get(), 1);
        project.note = ColumnText(statement.get(), 2);
        project.type = ColumnText(statement.get(), 3);
        project.group = ColumnText(statement.get(), 4);
        project.folder = ColumnText(statement.get(), 5);
        project.command = ColumnText(statement.get(), 6);
        project.status = ColumnText(statement.get(), 7);
        project.sort_order = sqlite3_column_int64(statement.get(), 8);
        result.push_back(std::move(project));
    }
    return result;
}

bool DataStore::SaveProject(Project& project, std::wstring* error) {
    std::scoped_lock lock(mutex_);
    if (!db_ || IsBlank(project.name) || IsBlank(project.type) || IsBlank(project.group) ||
        IsBlank(project.folder) || IsBlank(project.command)) {
        if (error) *error = L"项目字段不完整";
        return false;
    }
    const bool inserting = project.id.empty();
    if (inserting) project.id = NewId();
    if (project.status.empty()) project.status = L"created";
    bool moved_to_another_group = false;
    if (!inserting) {
        auto existing = Prepare(db_, "SELECT group_name FROM projects WHERE id=?;");
        if (existing) {
            BindText(existing.get(), 1, project.id);
            moved_to_another_group = sqlite3_step(existing.get()) == SQLITE_ROW &&
                ColumnText(existing.get(), 0) != project.group;
        }
    }
    if (inserting || moved_to_another_group) {
        auto order = Prepare(db_,
            "SELECT COALESCE(MAX(sort_order)+1,0) FROM projects WHERE group_name=?;");
        if (!order) {
            if (error) *error = LastError();
            return false;
        }
        BindText(order.get(), 1, project.group);
        if (sqlite3_step(order.get()) == SQLITE_ROW) {
            project.sort_order = sqlite3_column_int64(order.get(), 0);
        }
    }
    auto statement = Prepare(db_, R"SQL(
INSERT INTO projects(id,name,note,type_name,group_name,folder,command,status,sort_order,created_at,updated_at)
VALUES(?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(id) DO UPDATE SET name=excluded.name,note=excluded.note,type_name=excluded.type_name,
group_name=excluded.group_name,folder=excluded.folder,command=excluded.command,
status=excluded.status,sort_order=excluded.sort_order,updated_at=excluded.updated_at;
)SQL");
    if (!statement) {
        if (error) *error = LastError();
        return false;
    }
    BindText(statement.get(), 1, project.id);
    BindText(statement.get(), 2, project.name);
    BindText(statement.get(), 3, project.note);
    BindText(statement.get(), 4, project.type);
    BindText(statement.get(), 5, project.group);
    BindText(statement.get(), 6, project.folder);
    BindText(statement.get(), 7, project.command);
    BindText(statement.get(), 8, project.status);
    sqlite3_bind_int64(statement.get(), 9, project.sort_order);
    const auto now = UnixSeconds();
    sqlite3_bind_int64(statement.get(), 10, now);
    sqlite3_bind_int64(statement.get(), 11, now);
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
    if (error) *error = LastError();
    return false;
}

bool DataStore::DeleteProject(const std::wstring& project_id, std::wstring* error) {
    std::scoped_lock lock(mutex_);
    auto statement = Prepare(db_, "DELETE FROM projects WHERE id=?;");
    if (!statement) return false;
    BindText(statement.get(), 1, project_id);
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
    if (error) *error = LastError();
    return false;
}

bool DataStore::UpdateProjectStatus(const std::wstring& project_id, const std::wstring& status) {
    std::scoped_lock lock(mutex_);
    auto statement = Prepare(db_, "UPDATE projects SET status=?,updated_at=? WHERE id=?;");
    if (!statement) return false;
    BindText(statement.get(), 1, status);
    sqlite3_bind_int64(statement.get(), 2, UnixSeconds());
    BindText(statement.get(), 3, project_id);
    return sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool DataStore::ReorderProjects(const std::wstring& group_name,
                                const std::vector<std::wstring>& project_ids,
                                std::wstring* error) {
    std::scoped_lock lock(mutex_);
    if (!db_ || IsBlank(group_name) || project_ids.empty()) return false;
    if (!Execute("BEGIN IMMEDIATE;", error)) return false;

    bool success = true;
    auto statement = Prepare(db_,
        "UPDATE projects SET sort_order=? WHERE id=? AND group_name=?;");
    if (!statement) {
        success = false;
    } else {
        for (size_t index = 0; index < project_ids.size(); ++index) {
            sqlite3_reset(statement.get());
            sqlite3_clear_bindings(statement.get());
            sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(index));
            BindText(statement.get(), 2, project_ids[index]);
            BindText(statement.get(), 3, group_name);
            if (sqlite3_step(statement.get()) != SQLITE_DONE ||
                sqlite3_changes(db_) != 1) {
                success = false;
                break;
            }
        }
    }
    if (success) success = Execute("COMMIT;", error);
    if (!success) {
        Execute("ROLLBACK;", nullptr);
        if (error && error->empty()) *error = LastError();
    }
    return success;
}

bool DataStore::AddProjectType(const std::wstring& name, const std::wstring& command,
                               std::wstring* error) {
    std::scoped_lock lock(mutex_);
    if (IsBlank(name) || IsBlank(command)) {
        if (error) *error = L"类型名称和默认启动命令不能为空";
        return false;
    }
    auto statement = Prepare(db_, "INSERT INTO project_types(name,default_command,builtin) VALUES(?,?,0);");
    if (!statement) return false;
    BindText(statement.get(), 1, name);
    BindText(statement.get(), 2, command);
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
    if (error) *error = L"项目类型名称已存在";
    return false;
}

bool DataStore::UpdateProjectType(std::int64_t id, const std::wstring& name,
                                  const std::wstring& command, std::wstring* error) {
    std::scoped_lock lock(mutex_);
    auto existing = Prepare(db_, "SELECT name,builtin FROM project_types WHERE id=?;");
    if (!existing) return false;
    sqlite3_bind_int64(existing.get(), 1, id);
    if (sqlite3_step(existing.get()) != SQLITE_ROW) return false;
    const auto old_name = ColumnText(existing.get(), 0);
    // Codex and Claude are seeded as built-in defaults, but remain editable.
    // The builtin flag is intentionally retained so the defaults cannot be
    // deleted accidentally while their name/command can be customized.
    if (IsBlank(name) || IsBlank(command)) {
        if (error) *error = L"类型名称和默认启动命令不能为空";
        return false;
    }
    if (!Execute("BEGIN IMMEDIATE;", error)) return false;
    auto update = Prepare(db_, "UPDATE project_types SET name=?,default_command=? WHERE id=?;");
    if (!update) { Execute("ROLLBACK;"); return false; }
    BindText(update.get(), 1, name);
    BindText(update.get(), 2, command);
    sqlite3_bind_int64(update.get(), 3, id);
    if (sqlite3_step(update.get()) != SQLITE_DONE) {
        Execute("ROLLBACK;");
        if (error) *error = L"项目类型名称已存在";
        return false;
    }
    auto projects = Prepare(db_, "UPDATE projects SET type_name=?,updated_at=? WHERE type_name=?;");
    if (!projects) { Execute("ROLLBACK;"); return false; }
    BindText(projects.get(), 1, name);
    sqlite3_bind_int64(projects.get(), 2, UnixSeconds());
    BindText(projects.get(), 3, old_name);
    if (sqlite3_step(projects.get()) != SQLITE_DONE) {
        Execute("ROLLBACK;");
        return false;
    }
    return Execute("COMMIT;", error);
}

bool DataStore::DeleteProjectType(std::int64_t id, std::wstring* error) {
    std::scoped_lock lock(mutex_);
    auto query = Prepare(db_, "SELECT name,builtin FROM project_types WHERE id=?;");
    if (!query) return false;
    sqlite3_bind_int64(query.get(), 1, id);
    if (sqlite3_step(query.get()) != SQLITE_ROW) return false;
    const auto name = ColumnText(query.get(), 0);
    if (sqlite3_column_int(query.get(), 1) != 0) {
        if (error) *error = L"内置项目类型不能删除";
        return false;
    }
    if (IsProjectTypeInUse(name)) {
        if (error) *error = L"该项目类型仍被项目使用，不能删除";
        return false;
    }
    auto statement = Prepare(db_, "DELETE FROM project_types WHERE id=?;");
    if (!statement) return false;
    sqlite3_bind_int64(statement.get(), 1, id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) return false;

    if (GetSetting(kDefaultProjectTypeSetting) == std::to_wstring(id)) {
        SetSetting(kDefaultProjectTypeSetting, L"");
    }
    return true;
}

bool DataStore::IsProjectTypeInUse(const std::wstring& name) {
    std::scoped_lock lock(mutex_);
    auto statement = Prepare(db_, "SELECT 1 FROM projects WHERE type_name=? LIMIT 1;");
    if (!statement) return false;
    BindText(statement.get(), 1, name);
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

std::int64_t DataStore::ConfiguredDefaultProjectTypeId() {
    std::scoped_lock lock(mutex_);
    std::int64_t configured_id{};
    try {
        const auto configured = GetSetting(kDefaultProjectTypeSetting);
        if (!configured.empty()) configured_id = std::stoll(configured);
    } catch (...) {
        configured_id = 0;
    }
    if (configured_id <= 0) return 0;
    auto statement = Prepare(db_, "SELECT 1 FROM project_types WHERE id=? LIMIT 1;");
    if (!statement) return 0;
    sqlite3_bind_int64(statement.get(), 1, configured_id);
    return sqlite3_step(statement.get()) == SQLITE_ROW ? configured_id : 0;
}

std::int64_t DataStore::DefaultProjectTypeId() {
    std::scoped_lock lock(mutex_);
    const auto configured_id = ConfiguredDefaultProjectTypeId();
    if (configured_id > 0) return configured_id;
    const auto types = LoadProjectTypes();
    return types.empty() ? 0 : types.front().id;
}

bool DataStore::SetDefaultProjectType(std::int64_t id, std::wstring* error) {
    std::scoped_lock lock(mutex_);
    auto statement = Prepare(db_, "SELECT 1 FROM project_types WHERE id=? LIMIT 1;");
    if (!statement) {
        if (error) *error = L"无法读取项目类型";
        return false;
    }
    sqlite3_bind_int64(statement.get(), 1, id);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        if (error) *error = L"要设为默认的项目类型不存在";
        return false;
    }
    if (!SetSetting(kDefaultProjectTypeSetting, std::to_wstring(id))) {
        if (error) *error = L"保存默认项目类型失败";
        return false;
    }
    return true;
}

bool DataStore::ClearDefaultProjectType(std::wstring* error) {
    std::scoped_lock lock(mutex_);
    if (!SetSetting(kDefaultProjectTypeSetting, L"")) {
        if (error) *error = L"取消默认项目类型失败";
        return false;
    }
    return true;
}

bool DataStore::AddGroup(const std::wstring& name, std::wstring* error) {
    std::scoped_lock lock(mutex_);
    if (IsBlank(name)) {
        if (error) *error = L"分组名称不能为空";
        return false;
    }
    auto statement = Prepare(db_, "INSERT INTO groups(name,builtin) VALUES(?,0);");
    if (!statement) return false;
    BindText(statement.get(), 1, name);
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
    if (error) *error = L"分组名称已存在";
    return false;
}

bool DataStore::RenameGroup(std::int64_t id, const std::wstring& new_name,
                            std::wstring* error) {
    std::scoped_lock lock(mutex_);
    auto query = Prepare(db_, "SELECT name,builtin FROM groups WHERE id=?;");
    if (!query) return false;
    sqlite3_bind_int64(query.get(), 1, id);
    if (sqlite3_step(query.get()) != SQLITE_ROW) return false;
    const auto old_name = ColumnText(query.get(), 0);
    if (IsBlank(new_name)) {
        if (error) *error = L"分组名称不能为空";
        return false;
    }
    if (!Execute("BEGIN IMMEDIATE;", error)) return false;
    auto update = Prepare(db_, "UPDATE groups SET name=? WHERE id=?;");
    if (!update) { Execute("ROLLBACK;"); return false; }
    BindText(update.get(), 1, new_name);
    sqlite3_bind_int64(update.get(), 2, id);
    if (sqlite3_step(update.get()) != SQLITE_DONE) {
        Execute("ROLLBACK;");
        if (error) *error = L"分组名称已存在";
        return false;
    }
    auto projects = Prepare(db_, "UPDATE projects SET group_name=?,updated_at=? WHERE group_name=?;");
    if (!projects) { Execute("ROLLBACK;"); return false; }
    BindText(projects.get(), 1, new_name);
    sqlite3_bind_int64(projects.get(), 2, UnixSeconds());
    BindText(projects.get(), 3, old_name);
    if (sqlite3_step(projects.get()) != SQLITE_DONE) {
        Execute("ROLLBACK;");
        return false;
    }
    return Execute("COMMIT;", error);
}

bool DataStore::DeleteGroup(std::int64_t id, std::wstring* error) {
    std::scoped_lock lock(mutex_);
    auto query = Prepare(db_, "SELECT name,builtin FROM groups WHERE id=?;");
    if (!query) return false;
    sqlite3_bind_int64(query.get(), 1, id);
    if (sqlite3_step(query.get()) != SQLITE_ROW) return false;
    const auto name = ColumnText(query.get(), 0);
    if (sqlite3_column_int(query.get(), 1) != 0) {
        if (error) *error = L"默认分组不能删除";
        return false;
    }
    if (IsGroupInUse(name)) {
        if (error) *error = L"该分组中仍有项目，请先移动项目";
        return false;
    }
    auto statement = Prepare(db_, "DELETE FROM groups WHERE id=?;");
    if (!statement) return false;
    sqlite3_bind_int64(statement.get(), 1, id);
    return sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool DataStore::IsGroupInUse(const std::wstring& name) {
    std::scoped_lock lock(mutex_);
    auto statement = Prepare(db_, "SELECT 1 FROM projects WHERE group_name=? LIMIT 1;");
    if (!statement) return false;
    BindText(statement.get(), 1, name);
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

std::wstring DataStore::GetSetting(const std::wstring& key, const std::wstring& fallback) {
    std::scoped_lock lock(mutex_);
    auto statement = Prepare(db_, "SELECT value FROM settings WHERE key=?;");
    if (!statement) return fallback;
    BindText(statement.get(), 1, key);
    return sqlite3_step(statement.get()) == SQLITE_ROW ? ColumnText(statement.get(), 0) : fallback;
}

bool DataStore::SetSetting(const std::wstring& key, const std::wstring& value) {
    std::scoped_lock lock(mutex_);
    auto statement = Prepare(db_, "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;");
    if (!statement) return false;
    BindText(statement.get(), 1, key);
    BindText(statement.get(), 2, value);
    return sqlite3_step(statement.get()) == SQLITE_DONE;
}

std::vector<DeviceRecord> DataStore::LoadDevices(bool include_revoked) {
    std::scoped_lock lock(mutex_);
    std::vector<DeviceRecord> result;
    auto statement = Prepare(db_, include_revoked
        ? "SELECT phone_id,name,model,public_key_base64,active,paired_at,last_seen,last_sequence FROM devices ORDER BY active DESC,paired_at DESC;"
        : "SELECT phone_id,name,model,public_key_base64,active,paired_at,last_seen,last_sequence FROM devices WHERE active=1 ORDER BY paired_at DESC;");
    while (statement && sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back({ColumnText(statement.get(), 0), ColumnText(statement.get(), 1),
                          ColumnText(statement.get(), 2), ColumnText(statement.get(), 3),
                          sqlite3_column_int(statement.get(), 4) != 0,
                          sqlite3_column_int64(statement.get(), 5),
                          sqlite3_column_int64(statement.get(), 6),
                          sqlite3_column_int64(statement.get(), 7)});
    }
    return result;
}

std::optional<DeviceRecord> DataStore::FindDevice(const std::wstring& phone_id) {
    std::scoped_lock lock(mutex_);
    auto statement = Prepare(db_, "SELECT phone_id,name,model,public_key_base64,active,paired_at,last_seen,last_sequence FROM devices WHERE phone_id=?;");
    if (!statement) return std::nullopt;
    BindText(statement.get(), 1, phone_id);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return DeviceRecord{ColumnText(statement.get(), 0), ColumnText(statement.get(), 1),
                        ColumnText(statement.get(), 2), ColumnText(statement.get(), 3),
                        sqlite3_column_int(statement.get(), 4) != 0,
                        sqlite3_column_int64(statement.get(), 5),
                        sqlite3_column_int64(statement.get(), 6),
                        sqlite3_column_int64(statement.get(), 7)};
}

bool DataStore::UpsertDevice(const DeviceRecord& device, std::wstring* error) {
    std::scoped_lock lock(mutex_);
    auto statement = Prepare(db_, R"SQL(
INSERT INTO devices(phone_id,name,model,public_key_base64,active,paired_at,last_seen,last_sequence)
VALUES(?,?,?,?,1,?,?,0)
ON CONFLICT(phone_id) DO UPDATE SET name=excluded.name,model=excluded.model,
public_key_base64=excluded.public_key_base64,active=1,last_seen=excluded.last_seen,last_sequence=0;
)SQL");
    if (!statement) return false;
    BindText(statement.get(), 1, device.phone_id);
    BindText(statement.get(), 2, device.name);
    BindText(statement.get(), 3, device.model);
    BindText(statement.get(), 4, device.public_key_base64);
    const auto now = UnixSeconds();
    sqlite3_bind_int64(statement.get(), 5, device.paired_at ? device.paired_at : now);
    sqlite3_bind_int64(statement.get(), 6, now);
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
    if (error) *error = LastError();
    return false;
}

bool DataStore::UpdateDeviceName(const std::wstring& phone_id,
                                 const std::wstring& name,
                                 std::wstring* error) {
    std::scoped_lock lock(mutex_);
    if (phone_id.empty() || IsBlank(name)) {
        if (error) *error = L"手机名称不能为空";
        return false;
    }
    if (name.size() > 64) {
        if (error) *error = L"手机名称不能超过 64 个字符";
        return false;
    }
    auto statement = Prepare(
        db_, "UPDATE devices SET name=?,last_seen=? WHERE phone_id=? AND active=1;");
    if (!statement) {
        if (error) *error = LastError();
        return false;
    }
    BindText(statement.get(), 1, name);
    sqlite3_bind_int64(statement.get(), 2, UnixSeconds());
    BindText(statement.get(), 3, phone_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error) *error = LastError();
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        if (error) *error = L"手机未绑定或已被解绑";
        return false;
    }
    return true;
}

bool DataStore::RevokeDevice(const std::wstring& phone_id) {
    std::scoped_lock lock(mutex_);
    auto statement = Prepare(db_, "UPDATE devices SET active=0,last_sequence=0 WHERE phone_id=?;");
    if (!statement) return false;
    BindText(statement.get(), 1, phone_id);
    return sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool DataStore::TouchDevice(const std::wstring& phone_id) {
    std::scoped_lock lock(mutex_);
    auto statement = Prepare(db_, "UPDATE devices SET last_seen=? WHERE phone_id=? AND active=1;");
    if (!statement) return false;
    sqlite3_bind_int64(statement.get(), 1, UnixSeconds());
    BindText(statement.get(), 2, phone_id);
    return sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool DataStore::AcceptSignedRequest(const std::wstring& phone_id,
                                    const std::wstring& request_id,
                                    std::int64_t sequence, std::int64_t timestamp,
                                    std::wstring* error) {
    std::scoped_lock lock(mutex_);
    const auto now = UnixSeconds();
    if (request_id.empty() || sequence <= 0 || timestamp < now - 300 || timestamp > now + 300) {
        if (error) *error = L"请求时间或序列无效";
        return false;
    }
    if (!Execute("BEGIN IMMEDIATE;", error)) return false;
    auto query = Prepare(db_, "SELECT active,last_sequence FROM devices WHERE phone_id=?;");
    if (!query) { Execute("ROLLBACK;"); return false; }
    BindText(query.get(), 1, phone_id);
    if (sqlite3_step(query.get()) != SQLITE_ROW ||
        sqlite3_column_int(query.get(), 0) == 0) {
        Execute("ROLLBACK;");
        if (error) *error = L"设备已撤销";
        return false;
    }
    const auto last_sequence = sqlite3_column_int64(query.get(), 1);
    if (sequence <= last_sequence &&
        last_sequence - sequence >= kSignedRequestReorderWindow) {
        Execute("ROLLBACK;");
        if (error) *error = L"请求序列超出允许窗口";
        return false;
    }
    auto replay = Prepare(db_,
        "SELECT 1 FROM device_requests WHERE phone_id=? AND sequence=? LIMIT 1;");
    if (!replay) { Execute("ROLLBACK;"); return false; }
    BindText(replay.get(), 1, phone_id);
    sqlite3_bind_int64(replay.get(), 2, sequence);
    if (sqlite3_step(replay.get()) == SQLITE_ROW) {
        Execute("ROLLBACK;");
        if (error) *error = L"请求序列已使用";
        return false;
    }
    auto insert = Prepare(db_, "INSERT INTO device_requests(request_id,phone_id,sequence,created_at) VALUES(?,?,?,?);");
    if (!insert) { Execute("ROLLBACK;"); return false; }
    BindText(insert.get(), 1, request_id);
    BindText(insert.get(), 2, phone_id);
    sqlite3_bind_int64(insert.get(), 3, sequence);
    sqlite3_bind_int64(insert.get(), 4, now);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) {
        Execute("ROLLBACK;");
        if (error) *error = L"请求已处理";
        return false;
    }
    auto update = Prepare(db_,
        "UPDATE devices SET last_sequence=MAX(last_sequence,?),last_seen=? WHERE phone_id=?;");
    if (!update) { Execute("ROLLBACK;"); return false; }
    sqlite3_bind_int64(update.get(), 1, sequence);
    sqlite3_bind_int64(update.get(), 2, now);
    BindText(update.get(), 3, phone_id);
    if (sqlite3_step(update.get()) != SQLITE_DONE) {
        Execute("ROLLBACK;");
        return false;
    }
    // Keep every sequence still inside the acceptance window even when the
    // phone has been idle for days. Older rows can be removed because their
    // sequence is rejected by the window before request-id lookup.
    auto cleanup = Prepare(db_,
        "DELETE FROM device_requests "
        "WHERE created_at<? AND sequence <= ("
        "SELECT last_sequence-? FROM devices "
        "WHERE devices.phone_id=device_requests.phone_id);");
    if (cleanup) {
        sqlite3_bind_int64(cleanup.get(), 1, now - 86400);
        sqlite3_bind_int64(cleanup.get(), 2, kSignedRequestReorderWindow);
        sqlite3_step(cleanup.get());
    }
    return Execute("COMMIT;", error);
}

std::int64_t DataStore::AppendEvent(const std::wstring& project_id,
                                    const std::wstring& event_type,
                                    const std::string& payload_json) {
    std::scoped_lock lock(mutex_);
    auto statement = Prepare(db_, "INSERT INTO events(project_id,event_type,payload_json,created_at) VALUES(?,?,?,?);");
    if (!statement) return 0;
    BindText(statement.get(), 1, project_id);
    BindText(statement.get(), 2, event_type);
    BindUtf8(statement.get(), 3, payload_json);
    sqlite3_bind_int64(statement.get(), 4, UnixSeconds());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) return 0;
    const auto id = sqlite3_last_insert_rowid(db_);
    if (++append_count_ % 256 == 0) TrimEventsIfNeeded();
    return id;
}

std::vector<EventRecord> DataStore::LoadEvents(const std::wstring& project_id,
                                               std::int64_t after_event_id,
                                               int limit,
                                               bool include_terminal_output) {
    std::scoped_lock lock(mutex_);
    std::vector<EventRecord> result;
    auto statement = Prepare(db_, include_terminal_output
        ? "SELECT id,project_id,event_type,payload_json,created_at FROM events "
          "WHERE project_id=? AND id>? ORDER BY id LIMIT ?;"
        : "SELECT id,project_id,event_type,payload_json,created_at FROM events "
          "WHERE project_id=? AND id>? AND event_type<>'terminal.output' ORDER BY id LIMIT ?;");
    if (!statement) return result;
    BindText(statement.get(), 1, project_id);
    sqlite3_bind_int64(statement.get(), 2, std::max<std::int64_t>(0, after_event_id));
    sqlite3_bind_int(statement.get(), 3, std::clamp(limit, 1, 5000));
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        const auto* payload = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 3));
        result.push_back({sqlite3_column_int64(statement.get(), 0),
                          ColumnText(statement.get(), 1), ColumnText(statement.get(), 2),
                          payload ? std::string{payload} : std::string{},
                          sqlite3_column_int64(statement.get(), 4)});
    }
    return result;
}

bool DataStore::SaveTerminalSnapshot(const std::wstring& project_id,
                                     const std::wstring& text,
                                     std::wstring* error) {
    std::scoped_lock lock(mutex_);
    if (!db_ || project_id.empty()) return false;
    auto statement = Prepare(db_, R"SQL(
INSERT INTO terminal_snapshots(project_id,text,updated_at) VALUES(?,?,?)
ON CONFLICT(project_id) DO UPDATE SET text=excluded.text,updated_at=excluded.updated_at;
)SQL");
    if (!statement) {
        if (error) *error = LastError();
        return false;
    }
    BindText(statement.get(), 1, project_id);
    BindText(statement.get(), 2, text);
    sqlite3_bind_int64(statement.get(), 3, UnixSeconds());
    if (sqlite3_step(statement.get()) == SQLITE_DONE) return true;
    if (error) *error = LastError();
    return false;
}

std::optional<std::wstring> DataStore::LoadTerminalSnapshot(
    const std::wstring& project_id) {
    std::scoped_lock lock(mutex_);
    if (!db_ || project_id.empty()) return std::nullopt;
    auto statement = Prepare(db_,
        "SELECT text FROM terminal_snapshots WHERE project_id=?;");
    if (!statement) return std::nullopt;
    BindText(statement.get(), 1, project_id);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    return ColumnText(statement.get(), 0);
}

void DataStore::TrimEventsIfNeeded() {
    if (!db_ || database_path_.empty()) return;
    std::error_code error;
    const auto size = std::filesystem::file_size(database_path_, error);
    if (error || size <= 100ull * 1024ull * 1024ull) return;
    Execute("DELETE FROM events WHERE id IN (SELECT id FROM events WHERE event_type='terminal.output' ORDER BY id LIMIT 5000);", nullptr);
    Execute("PRAGMA wal_checkpoint(PASSIVE);", nullptr);
}

std::wstring DataStore::LastError() const {
    if (!db_) return L"SQLite 未初始化";
    const char* message = sqlite3_errmsg(db_);
    if (!message) return L"SQLite 操作失败";
    const int count = MultiByteToWideChar(CP_UTF8, 0, message, -1, nullptr, 0);
    std::wstring result(static_cast<size_t>(std::max(1, count)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message, -1, result.data(), count);
    if (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}

std::wstring DefaultDataDirectory() {
#ifdef AICLI_DATA_DIRECTORY_OVERRIDE
    return AICLI_DATA_DIRECTORY_OVERRIDE;
#endif
    if (wchar_t configured[32768]{};
        GetEnvironmentVariableW(L"AICLI_DATA_DIR", configured,
                                static_cast<DWORD>(std::size(configured))) > 0) {
        return configured;
    }
    PWSTR local_app_data{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                       &local_app_data)) && local_app_data) {
        std::filesystem::path result{local_app_data};
        CoTaskMemFree(local_app_data);
#if defined(AICLI_QT_UI_BUILD) && defined(AICLI_DEBUG_BUILD)
        return (result / L"AI CLI Qt Dev").wstring();
#elif defined(AICLI_QT_UI_BUILD)
        return (result / L"AI CLI Qt").wstring();
#elif defined(AICLI_DEBUG_BUILD)
        return (result / L"AI CLI Dev").wstring();
#else
        // Keep the established non-Qt Release directory so existing users do
        // not lose projects, settings, pairings, or conversation state.
        return (result / L"AI CLI").wstring();
#endif
    }
#if defined(AICLI_QT_UI_BUILD) && defined(AICLI_DEBUG_BUILD)
    return (std::filesystem::current_path() / L"data-qt-dev").wstring();
#elif defined(AICLI_QT_UI_BUILD)
    return (std::filesystem::current_path() / L"data-qt").wstring();
#elif defined(AICLI_DEBUG_BUILD)
    return (std::filesystem::current_path() / L"data-dev").wstring();
#else
    return (std::filesystem::current_path() / L"data").wstring();
#endif
}

}
