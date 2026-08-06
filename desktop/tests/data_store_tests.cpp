#include "data/data_store.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

bool Check(bool condition, const wchar_t* message) {
    if (condition) return true;
    std::wcerr << L"FAILED: " << message << L'\n';
    return false;
}

template <typename T, typename Predicate>
const T* Find(const std::vector<T>& values, Predicate predicate) {
    const auto found = std::find_if(values.begin(), values.end(), predicate);
    return found == values.end() ? nullptr : &*found;
}

std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::wcerr << L"Usage: aicli_data_store_tests <temporary-database-path>\n";
        return 2;
    }

    const std::filesystem::path database{argv[1]};
    std::error_code filesystem_error;
    std::filesystem::create_directories(database.parent_path(), filesystem_error);
    std::filesystem::remove(database, filesystem_error);
    std::filesystem::remove(database.wstring() + L"-wal", filesystem_error);
    std::filesystem::remove(database.wstring() + L"-shm", filesystem_error);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = true;
    {
        aicli::DataStore store;
        std::wstring error;
        ok &= Check(store.Initialize(database.wstring(), &error),
                    (L"initialize: " + error).c_str());

        auto types = store.LoadProjectTypes();
        const auto* codex = Find(types, [](const aicli::ProjectType& value) {
            return value.name == L"Codex";
        });
        const auto* claude = Find(types, [](const aicli::ProjectType& value) {
            return value.name == L"Claude";
        });
        ok &= Check(codex && codex->builtin &&
                        codex->default_command == L"codex --yolo resume --last",
                    L"Codex builtin type and command");
        ok &= Check(claude && claude->builtin &&
                        claude->default_command ==
                            L"claude --dangerously-skip-permissions --continue",
                    L"Claude builtin type and command");
        if (codex) {
            error.clear();
            ok &= Check(!store.UpdateProjectType(codex->id, L"Codex X", L"codex", &error),
                        L"builtin project type cannot be renamed");
            error.clear();
            ok &= Check(!store.DeleteProjectType(codex->id, &error),
                        L"builtin project type cannot be deleted");
        }

        auto groups = store.LoadGroups();
        const auto* default_group = Find(groups, [](const aicli::ProjectGroup& value) {
            return value.name == L"默认分组";
        });
        ok &= Check(default_group && default_group->builtin, L"default builtin group");
        if (default_group) {
            error.clear();
            ok &= Check(!store.RenameGroup(default_group->id, L"默认", &error),
                        L"default group cannot be renamed");
            error.clear();
            ok &= Check(!store.DeleteGroup(default_group->id, &error),
                        L"default group cannot be deleted");
        }

        error.clear();
        ok &= Check(store.AddProjectType(L"Gemini", L"gemini --continue", &error),
                    (L"add custom project type: " + error).c_str());
        error.clear();
        ok &= Check(store.AddGroup(L"实验组", &error),
                    (L"add custom group: " + error).c_str());

        aicli::Project project;
        project.name = L"数据层测试项目";
        project.type = L"Gemini";
        project.group = L"实验组";
        project.folder = database.parent_path().wstring();
        project.command = L"gemini --continue";
        error.clear();
        ok &= Check(store.SaveProject(project, &error),
                    (L"save custom project: " + error).c_str());
        ok &= Check(!project.id.empty(), L"new project receives a stable id");

        types = store.LoadProjectTypes();
        const auto* gemini = Find(types, [](const aicli::ProjectType& value) {
            return value.name == L"Gemini";
        });
        groups = store.LoadGroups();
        const auto* experimental = Find(groups, [](const aicli::ProjectGroup& value) {
            return value.name == L"实验组";
        });
        ok &= Check(gemini && !gemini->builtin, L"custom type is persisted");
        ok &= Check(experimental && !experimental->builtin, L"custom group is persisted");

        if (gemini) {
            error.clear();
            ok &= Check(store.UpdateProjectType(gemini->id, L"Gemini CLI", L"gemini resume", &error),
                        (L"rename custom type: " + error).c_str());
        }
        if (experimental) {
            error.clear();
            ok &= Check(store.RenameGroup(experimental->id, L"实验组 2", &error),
                        (L"rename custom group: " + error).c_str());
        }

        auto projects = store.LoadProjects();
        const auto* updated_project = Find(projects, [&](const aicli::Project& value) {
            return value.id == project.id;
        });
        ok &= Check(updated_project && updated_project->type == L"Gemini CLI" &&
                        updated_project->group == L"实验组 2",
                    L"type/group rename updates existing projects atomically");

        types = store.LoadProjectTypes();
        const auto* renamed_type = Find(types, [](const aicli::ProjectType& value) {
            return value.name == L"Gemini CLI";
        });
        groups = store.LoadGroups();
        const auto* renamed_group = Find(groups, [](const aicli::ProjectGroup& value) {
            return value.name == L"实验组 2";
        });
        if (renamed_type) {
            error.clear();
            ok &= Check(!store.DeleteProjectType(renamed_type->id, &error),
                        L"in-use project type cannot be deleted");
        }
        if (renamed_group) {
            error.clear();
            ok &= Check(!store.DeleteGroup(renamed_group->id, &error),
                        L"non-empty group cannot be deleted");
        }

        aicli::DeviceRecord device;
        device.phone_id = L"phone-test";
        device.name = L"Test Phone";
        device.model = L"Test Model";
        device.public_key_base64 = L"test-public-key";
        error.clear();
        ok &= Check(store.UpsertDevice(device, &error),
                    (L"upsert device: " + error).c_str());
        error.clear();
        ok &= Check(store.AcceptSignedRequest(device.phone_id, L"request-1", 1, Now(), &error),
                    (L"accept first signed request: " + error).c_str());
        error.clear();
        ok &= Check(!store.AcceptSignedRequest(device.phone_id, L"request-1", 1, Now(), &error),
                    L"duplicate request and sequence are rejected");
        ok &= Check(store.RevokeDevice(device.phone_id), L"device can be revoked");
        error.clear();
        ok &= Check(!store.AcceptSignedRequest(device.phone_id, L"request-2", 2, Now(), &error),
                    L"revoked device cannot submit signed requests");

        const auto event_id = store.AppendEvent(project.id, L"terminal.output", "{\"text\":\"ok\"}");
        ok &= Check(event_id > 0, L"terminal event is persisted");
        const auto events = store.LoadEvents(project.id, 0, 20);
        ok &= Check(!events.empty() && events.back().event_type == L"terminal.output",
                    L"terminal event history can be loaded");

        error.clear();
        ok &= Check(store.DeleteProject(project.id, &error),
                    (L"delete project: " + error).c_str());
        if (renamed_type) {
            error.clear();
            ok &= Check(store.DeleteProjectType(renamed_type->id, &error),
                        (L"delete unused type: " + error).c_str());
        }
        if (renamed_group) {
            error.clear();
            ok &= Check(store.DeleteGroup(renamed_group->id, &error),
                        (L"delete empty group: " + error).c_str());
        }
        store.Close();
    }
    CoUninitialize();

    std::filesystem::remove(database, filesystem_error);
    std::filesystem::remove(database.wstring() + L"-wal", filesystem_error);
    std::filesystem::remove(database.wstring() + L"-shm", filesystem_error);

    if (!ok) return 1;
    std::wcout << L"AI CLI data-store tests passed.\n";
    return 0;
}
