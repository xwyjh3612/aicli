#pragma once

#include "core/models.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace aicli {

class DataStore {
public:
    DataStore() = default;
    ~DataStore();
    DataStore(const DataStore&) = delete;
    DataStore& operator=(const DataStore&) = delete;

    bool Initialize(const std::wstring& database_path, std::wstring* error = nullptr);
    void Close();
    bool Ready() const noexcept;

    std::vector<ProjectType> LoadProjectTypes();
    std::vector<ProjectGroup> LoadGroups();
    std::vector<Project> LoadProjects();

    bool SaveProject(Project& project, std::wstring* error = nullptr);
    bool DeleteProject(const std::wstring& project_id, std::wstring* error = nullptr);
    bool UpdateProjectStatus(const std::wstring& project_id, const std::wstring& status);
    bool ReorderProjects(const std::wstring& group_name,
                         const std::vector<std::wstring>& project_ids,
                         std::wstring* error = nullptr);

    bool AddProjectType(const std::wstring& name, const std::wstring& default_command,
                        std::wstring* error = nullptr);
    bool UpdateProjectType(std::int64_t id, const std::wstring& name,
                           const std::wstring& default_command,
                           std::wstring* error = nullptr);
    bool DeleteProjectType(std::int64_t id, std::wstring* error = nullptr);
    bool IsProjectTypeInUse(const std::wstring& name);
    std::int64_t ConfiguredDefaultProjectTypeId();
    std::int64_t DefaultProjectTypeId();
    bool SetDefaultProjectType(std::int64_t id, std::wstring* error = nullptr);
    bool ClearDefaultProjectType(std::wstring* error = nullptr);

    bool AddGroup(const std::wstring& name, std::wstring* error = nullptr);
    bool RenameGroup(std::int64_t id, const std::wstring& new_name,
                     std::wstring* error = nullptr);
    bool DeleteGroup(std::int64_t id, std::wstring* error = nullptr);
    bool IsGroupInUse(const std::wstring& name);

    std::wstring GetSetting(const std::wstring& key, const std::wstring& fallback = L"");
    bool SetSetting(const std::wstring& key, const std::wstring& value);

    std::vector<DeviceRecord> LoadDevices(bool include_revoked = false);
    std::optional<DeviceRecord> FindDevice(const std::wstring& phone_id);
    bool UpsertDevice(const DeviceRecord& device, std::wstring* error = nullptr);
    bool UpdateDeviceName(const std::wstring& phone_id, const std::wstring& name,
                          std::wstring* error = nullptr);
    bool RevokeDevice(const std::wstring& phone_id);
    bool TouchDevice(const std::wstring& phone_id);
    bool AcceptSignedRequest(const std::wstring& phone_id, const std::wstring& request_id,
                             std::int64_t sequence, std::int64_t timestamp,
                             std::wstring* error = nullptr);

    std::int64_t AppendEvent(const std::wstring& project_id,
                             const std::wstring& event_type,
                             const std::string& payload_json);
    std::vector<EventRecord> LoadEvents(const std::wstring& project_id,
                                        std::int64_t after_event_id,
                                        int limit = 1000,
                                        bool include_terminal_output = true);
    bool SaveTerminalSnapshot(const std::wstring& project_id,
                              const std::wstring& text,
                              std::wstring* error = nullptr);
    std::optional<std::wstring> LoadTerminalSnapshot(const std::wstring& project_id);

    const std::wstring& DatabasePath() const noexcept { return database_path_; }

private:
    bool Execute(const char* sql, std::wstring* error = nullptr);
    bool CreateSchema(std::wstring* error);
    bool SeedDefaults(std::wstring* error);
    void TrimEventsIfNeeded();
    std::wstring LastError() const;

    mutable std::recursive_mutex mutex_;
    sqlite3* db_{};
    std::wstring database_path_;
    std::uint64_t append_count_{};
};

std::wstring DefaultDataDirectory();

}
