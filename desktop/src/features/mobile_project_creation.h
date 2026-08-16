#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/models.h"

namespace aicli {
class DataStore;
}

namespace aicli::mobile_projects {

struct CreationOptions {
    std::wstring root_directory;
    std::vector<ProjectType> project_types;
    std::vector<ProjectGroup> groups;
    std::int64_t default_type_id{};
    std::int64_t default_group_id{};
};

struct CreateRequest {
    std::wstring name;
    std::int64_t type_id{};
    std::int64_t group_id{};
};

struct CreateResult {
    bool success{};
    int http_status{400};
    Project project;
    std::wstring error;
};

std::filesystem::path SuggestedWorkspaceDirectory();
std::filesystem::path WorkspaceDirectory(DataStore& store);
std::filesystem::path ProjectRootDirectory(DataStore& store);
std::filesystem::path AttachmentRootDirectory(DataStore& store);
CreationOptions LoadCreationOptions(DataStore& store);
CreateResult CreateProject(DataStore& store, const CreateRequest& request);

} // namespace aicli::mobile_projects
