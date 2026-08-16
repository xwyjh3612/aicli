#include "features/mobile_project_creation.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <system_error>

#include "data/data_store.h"

namespace aicli::mobile_projects {
namespace {

std::wstring Trim(std::wstring value) {
    const auto visible = [](wchar_t character) {
        return !std::iswspace(static_cast<wint_t>(character));
    };
    const auto first = std::find_if(value.begin(), value.end(), visible);
    const auto last = std::find_if(value.rbegin(), value.rend(), visible).base();
    if (first >= last) return {};
    return std::wstring(first, last);
}

std::wstring Upper(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towupper(static_cast<wint_t>(character)));
    });
    return value;
}

bool IsReservedWindowsName(const std::wstring& value) {
    std::wstring base = value;
    const auto dot = base.find(L'.');
    if (dot != std::wstring::npos) base.resize(dot);
    base = Upper(base);
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") {
        return true;
    }
    if (base.size() == 4 &&
        (base.rfind(L"COM", 0) == 0 || base.rfind(L"LPT", 0) == 0) &&
        base[3] >= L'1' && base[3] <= L'9') {
        return true;
    }
    return false;
}

bool ValidateProjectName(const std::wstring& value, std::wstring* error) {
    if (value.empty()) {
        if (error) *error = L"项目名称不能为空";
        return false;
    }
    if (value.size() > 80) {
        if (error) *error = L"项目名称不能超过 80 个字符";
        return false;
    }
    if (value == L"." || value == L".." || value.back() == L'.' || value.back() == L' ') {
        if (error) *error = L"项目名称不能以点或空格结尾";
        return false;
    }
    static constexpr wchar_t invalid[] = L"<>:\"/\\|?*";
    for (const wchar_t character : value) {
        if (character < 32 || std::wcschr(invalid, character)) {
            if (error) *error = L"项目名称包含 Windows 不允许的字符";
            return false;
        }
    }
    if (IsReservedWindowsName(value)) {
        if (error) *error = L"项目名称是 Windows 保留名称";
        return false;
    }
    return true;
}

template <typename Item>
const Item* FindById(const std::vector<Item>& items, std::int64_t id) {
    const auto found = std::find_if(items.begin(), items.end(), [id](const Item& item) {
        return item.id == id;
    });
    return found == items.end() ? nullptr : &*found;
}

} // namespace

std::filesystem::path SuggestedWorkspaceDirectory() {
    std::error_code error;
    for (const wchar_t* drive : {L"D:\\", L"E:\\"}) {
        const std::filesystem::path root(drive);
        if (std::filesystem::exists(root, error) && !error) {
            return (root / L"AiCli").lexically_normal();
        }
        error.clear();
    }
    return {};
}

std::filesystem::path WorkspaceDirectory(DataStore& store) {
    const std::wstring configured = Trim(store.GetSetting(L"mobile_workspace_root"));
    if (!configured.empty()) {
        return std::filesystem::path(configured).lexically_normal();
    }

    // Older builds stored the project directory itself. Migrate the common
    // "...\\conversation" value to its parent so existing project locations
    // remain unchanged after introducing one shared mobile workspace.
    const std::wstring legacy = Trim(store.GetSetting(L"mobile_project_root"));
    if (!legacy.empty()) {
        std::filesystem::path migrated = std::filesystem::path(legacy).lexically_normal();
        if (Upper(migrated.filename().wstring()) == L"CONVERSATION" &&
            migrated.has_parent_path()) {
            migrated = migrated.parent_path();
        }
        store.SetSetting(L"mobile_workspace_root", migrated.wstring());
        return migrated;
    }

    return SuggestedWorkspaceDirectory();
}

std::filesystem::path ProjectRootDirectory(DataStore& store) {
    const auto workspace = WorkspaceDirectory(store);
    return workspace.empty() ? std::filesystem::path{}
                             : (workspace / L"conversation").lexically_normal();
}

std::filesystem::path AttachmentRootDirectory(DataStore& store) {
    const auto workspace = WorkspaceDirectory(store);
    return workspace.empty() ? std::filesystem::path{}
                             : (workspace / L"attachments").lexically_normal();
}

CreationOptions LoadCreationOptions(DataStore& store) {
    CreationOptions options;
    options.root_directory = ProjectRootDirectory(store).wstring();
    options.project_types = store.LoadProjectTypes();
    options.groups = store.LoadGroups();

    const auto preferred_type = std::find_if(
        options.project_types.begin(), options.project_types.end(),
        [](const ProjectType& type) { return type.name == L"Codex"; });
    if (preferred_type != options.project_types.end()) {
        options.default_type_id = preferred_type->id;
    } else if (!options.project_types.empty()) {
        options.default_type_id = options.project_types.front().id;
    }

    const auto preferred_group = std::find_if(
        options.groups.begin(), options.groups.end(),
        [](const ProjectGroup& group) { return group.name == L"默认分组"; });
    if (preferred_group != options.groups.end()) {
        options.default_group_id = preferred_group->id;
    } else if (!options.groups.empty()) {
        options.default_group_id = options.groups.front().id;
    }
    return options;
}

CreateResult CreateProject(DataStore& store, const CreateRequest& request) {
    CreateResult result;
    result.project.name = Trim(request.name);
    if (!ValidateProjectName(result.project.name, &result.error)) return result;

    const CreationOptions options = LoadCreationOptions(store);
    const ProjectType* type = FindById(options.project_types, request.type_id);
    const ProjectGroup* group = FindById(options.groups, request.group_id);
    if (!type) {
        result.error = L"所选项目类型不存在，请刷新后重试";
        return result;
    }
    if (!group) {
        result.error = L"所选项目分组不存在，请刷新后重试";
        return result;
    }
    if (type->default_command.empty()) {
        result.error = L"所选项目类型没有配置启动命令";
        return result;
    }

    std::filesystem::path root(options.root_directory);
    if (root.empty() || !root.is_absolute()) {
        result.http_status = 503;
        result.error = L"请先在电脑端设置有效的手机工作目录";
        return result;
    }
    root = root.lexically_normal();
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if (filesystem_error) {
        result.http_status = 500;
        result.error = L"无法创建手机项目根目录：" + root.wstring();
        return result;
    }

    const std::filesystem::path folder = (root / result.project.name).lexically_normal();
    if (folder.parent_path() != root) {
        result.error = L"项目目录超出了电脑端设置的根目录";
        return result;
    }
    if (std::filesystem::exists(folder, filesystem_error)) {
        result.http_status = 409;
        result.error = L"同名项目目录已经存在：" + folder.wstring();
        return result;
    }
    filesystem_error.clear();
    if (!std::filesystem::create_directory(folder, filesystem_error) || filesystem_error) {
        result.http_status = 500;
        result.error = L"无法创建项目目录：" + folder.wstring();
        return result;
    }

    result.project.type = type->name;
    result.project.group = group->name;
    result.project.folder = folder.wstring();
    result.project.command = type->default_command;
    result.project.status = L"created";
    if (!store.SaveProject(result.project, &result.error)) {
        result.http_status = 500;
        std::error_code rollback_error;
        std::filesystem::remove(folder, rollback_error);
        if (result.error.empty()) result.error = L"项目配置保存失败";
        return result;
    }

    result.success = true;
    result.http_status = 201;
    return result;
}

} // namespace aicli::mobile_projects
