#include "data/data_store.h"

#include <windows.h>
#include <objbase.h>
#include <winsqlite/winsqlite3.h>

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
                        codex->default_command == L"codex --yolo --no-alt-screen resume --last",
                    L"Codex builtin type and command");
        ok &= Check(claude && claude->builtin &&
                        claude->default_command ==
                            L"claude --dangerously-skip-permissions --continue; if ($LASTEXITCODE -ne 0) { claude --dangerously-skip-permissions }",
                    L"Claude builtin type and command");

        if (claude) {
            constexpr wchar_t kLegacyClaudeCommand[] =
                L"claude --dangerously-skip-permissions --continue || claude --dangerously-skip-permissions";
            constexpr wchar_t kCurrentClaudeCommand[] =
                L"claude --dangerously-skip-permissions --continue; if ($LASTEXITCODE -ne 0) { claude --dangerously-skip-permissions }";
            error.clear();
            ok &= Check(store.UpdateProjectType(
                            claude->id, L"Claude", kLegacyClaudeCommand, &error),
                        (L"prepare legacy Claude type migration: " + error).c_str());
            aicli::Project legacy_claude_project;
            legacy_claude_project.name = L"旧 Claude 默认命令";
            legacy_claude_project.type = L"Claude";
            legacy_claude_project.group = L"默认分组";
            legacy_claude_project.folder = database.parent_path().wstring();
            legacy_claude_project.command = kLegacyClaudeCommand;
            error.clear();
            ok &= Check(store.SaveProject(legacy_claude_project, &error),
                        (L"prepare legacy Claude project migration: " + error).c_str());

            store.Close();
            error.clear();
            ok &= Check(store.Initialize(database.wstring(), &error),
                        (L"reinitialize for Claude migration: " + error).c_str());
            types = store.LoadProjectTypes();
            codex = Find(types, [](const aicli::ProjectType& value) {
                return value.name == L"Codex";
            });
            claude = Find(types, [](const aicli::ProjectType& value) {
                return value.name == L"Claude";
            });
            ok &= Check(claude && claude->default_command == kCurrentClaudeCommand,
                        L"legacy Claude builtin command migrates to PowerShell-compatible if form");
            const auto migrated_projects = store.LoadProjects();
            const auto* migrated_project = Find(
                migrated_projects, [&](const aicli::Project& value) {
                    return value.id == legacy_claude_project.id;
                });
            ok &= Check(migrated_project &&
                            migrated_project->command == kCurrentClaudeCommand,
                        L"legacy Claude project command migrates exactly");

            error.clear();
            ok &= Check(store.UpdateProjectType(
                            claude->id, L"Claude", L"claude custom", &error),
                        (L"prepare custom Claude preservation: " + error).c_str());
            store.Close();
            error.clear();
            ok &= Check(store.Initialize(database.wstring(), &error),
                        (L"reinitialize custom Claude preservation: " + error).c_str());
            types = store.LoadProjectTypes();
            codex = Find(types, [](const aicli::ProjectType& value) {
                return value.name == L"Codex";
            });
            claude = Find(types, [](const aicli::ProjectType& value) {
                return value.name == L"Claude";
            });
            ok &= Check(claude && claude->default_command == L"claude custom",
                        L"custom Claude command is not overwritten by migration");
            error.clear();
            ok &= Check(claude && store.UpdateProjectType(
                            claude->id, L"Claude", kCurrentClaudeCommand, &error),
                        (L"restore Claude preset after migration test: " + error).c_str());
            types = store.LoadProjectTypes();
            codex = Find(types, [](const aicli::ProjectType& value) {
                return value.name == L"Codex";
            });
            claude = Find(types, [](const aicli::ProjectType& value) {
                return value.name == L"Claude";
            });
        }
        ok &= Check(codex && store.DefaultProjectTypeId() == codex->id,
                    L"Codex is the fallback default project type");
        if (claude) {
            error.clear();
            ok &= Check(store.SetDefaultProjectType(claude->id, &error),
                        (L"set Claude as default project type: " + error).c_str());
            ok &= Check(store.ConfiguredDefaultProjectTypeId() == claude->id &&
                            store.DefaultProjectTypeId() == claude->id,
                        L"setting one default project type replaces the previous default");
            error.clear();
            ok &= Check(store.ClearDefaultProjectType(&error),
                        (L"clear default project type: " + error).c_str());
            ok &= Check(store.ConfiguredDefaultProjectTypeId() == 0 && codex &&
                            store.DefaultProjectTypeId() == codex->id,
                        L"clearing the explicit default falls back safely for creation");
        }
        if (codex) {
            const auto codex_id = codex->id;
            constexpr wchar_t kLegacyCodexCommand[] = L"codex --yolo resume --last";
            constexpr wchar_t kCurrentCodexCommand[] =
                L"codex --yolo --no-alt-screen resume --last";
            constexpr wchar_t kLegacyTrustedCodexCommand[] =
                L"codex --yolo -c ('projects={' + (ConvertTo-Json -InputObject (Get-Location).Path -Compress) + '={trust_level=\"trusted\"}}') resume --last";
            constexpr wchar_t kCurrentTrustedCodexCommand[] =
                L"codex --yolo --no-alt-screen -c ('projects={' + (ConvertTo-Json -InputObject (Get-Location).Path -Compress) + '={trust_level=\"trusted\"}}') resume --last";
            error.clear();
            ok &= Check(store.UpdateProjectType(
                            codex_id, L"Codex", kLegacyCodexCommand, &error),
                        (L"prepare legacy Codex type migration: " + error).c_str());
            aicli::Project legacy_codex_project;
            legacy_codex_project.name = L"旧 Codex 默认命令";
            legacy_codex_project.type = L"Codex";
            legacy_codex_project.group = L"默认分组";
            legacy_codex_project.folder = database.parent_path().wstring();
            legacy_codex_project.command = kLegacyTrustedCodexCommand;
            error.clear();
            ok &= Check(store.SaveProject(legacy_codex_project, &error),
                        (L"prepare legacy Codex project migration: " + error).c_str());
            store.Close();
            error.clear();
            ok &= Check(store.Initialize(database.wstring(), &error),
                        (L"reinitialize for Codex migration: " + error).c_str());
            types = store.LoadProjectTypes();
            codex = Find(types, [](const aicli::ProjectType& value) {
                return value.name == L"Codex";
            });
            ok &= Check(codex && codex->default_command == kCurrentCodexCommand,
                        L"legacy Codex builtin command migrates to inline scrollback mode");
            const auto migrated_codex_projects = store.LoadProjects();
            const auto* migrated_codex_project = Find(
                migrated_codex_projects, [&](const aicli::Project& value) {
                    return value.id == legacy_codex_project.id;
                });
            ok &= Check(migrated_codex_project &&
                            migrated_codex_project->command == kCurrentTrustedCodexCommand,
                        L"Codex project with directory trust arguments migrates to inline scrollback");
            error.clear();
            ok &= Check(store.UpdateProjectType(codex_id, L"Codex X", L"codex custom", &error),
                        (L"builtin project type can be customized: " + error).c_str());
            types = store.LoadProjectTypes();
            const auto* customized_codex = Find(types, [](const aicli::ProjectType& value) {
                return value.name == L"Codex X";
            });
            ok &= Check(customized_codex && customized_codex->builtin &&
                            customized_codex->default_command == L"codex custom",
                        L"customized builtin type remains a protected preset");
            error.clear();
            ok &= Check(store.UpdateProjectType(codex_id, L"Codex",
                                                kCurrentCodexCommand, &error),
                        (L"restore Codex preset after edit test: " + error).c_str());
            error.clear();
            ok &= Check(!store.DeleteProjectType(codex_id, &error),
                        L"builtin project type cannot be deleted");
        }

        auto groups = store.LoadGroups();
        const auto* default_group = Find(groups, [](const aicli::ProjectGroup& value) {
            return value.name == L"默认分组";
        });
        ok &= Check(default_group && default_group->builtin, L"default builtin group");
        if (default_group) {
            error.clear();
            ok &= Check(store.RenameGroup(default_group->id, L"默认", &error),
                        (L"default group can be renamed: " + error).c_str());
            groups = store.LoadGroups();
            const auto* renamed_default = Find(groups, [](const aicli::ProjectGroup& value) {
                return value.name == L"默认";
            });
            ok &= Check(renamed_default && renamed_default->builtin,
                        L"renamed default group remains builtin");
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

        aicli::Project second_project;
        second_project.name = L"第二个排序项目";
        second_project.type = L"Gemini";
        second_project.group = L"实验组";
        second_project.folder = database.parent_path().wstring();
        second_project.command = L"gemini --continue";
        error.clear();
        ok &= Check(store.SaveProject(second_project, &error),
                    (L"save second project: " + error).c_str());
        error.clear();
        ok &= Check(store.ReorderProjects(L"实验组", {second_project.id, project.id}, &error),
                    (L"persist project drag order: " + error).c_str());
        auto ordered_projects = store.LoadProjects();
        std::vector<std::wstring> experimental_ids;
        for (const auto& value : ordered_projects) {
            if (value.group == L"实验组") experimental_ids.push_back(value.id);
        }
        ok &= Check(experimental_ids.size() == 2 &&
                        experimental_ids[0] == second_project.id &&
                        experimental_ids[1] == project.id,
                    L"projects load in persisted drag order");
        ok &= Check(store.UpdateProjectStatus(second_project.id, L"running"),
                    L"status update succeeds without changing drag order");
        ordered_projects = store.LoadProjects();
        experimental_ids.clear();
        for (const auto& value : ordered_projects) {
            if (value.group == L"实验组") experimental_ids.push_back(value.id);
        }
        ok &= Check(experimental_ids.size() == 2 && experimental_ids[0] == second_project.id,
                    L"status update keeps persisted project order");

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
        error.clear();
        ok &= Check(store.AcceptSignedRequest(device.phone_id, L"request-3", 3, Now(), &error),
                    (L"accept newer concurrent request: " + error).c_str());
        error.clear();
        ok &= Check(store.AcceptSignedRequest(device.phone_id, L"request-2", 2, Now(), &error),
                    (L"accept signed request delivered out of order: " + error).c_str());
        error.clear();
        ok &= Check(!store.AcceptSignedRequest(device.phone_id, L"request-2-copy", 2, Now(), &error),
                    L"duplicate sequence is rejected inside reorder window");
        error.clear();
        ok &= Check(store.AcceptSignedRequest(device.phone_id, L"request-300", 300, Now(), &error),
                    (L"advance signed request window: " + error).c_str());
        error.clear();
        ok &= Check(!store.AcceptSignedRequest(device.phone_id, L"request-4", 4, Now(), &error),
                    L"request outside reorder window is rejected");
        ok &= Check(store.RevokeDevice(device.phone_id), L"device can be revoked");
        error.clear();
        ok &= Check(!store.AcceptSignedRequest(device.phone_id, L"request-301", 301, Now(), &error),
                    L"revoked device cannot submit signed requests");

        const auto event_id = store.AppendEvent(project.id, L"terminal.output", "{\"text\":\"ok\"}");
        ok &= Check(event_id > 0, L"terminal event is persisted");
        const auto events = store.LoadEvents(project.id, 0, 20);
        ok &= Check(!events.empty() && events.back().event_type == L"terminal.output",
                    L"terminal event history can be loaded");
        error.clear();
        ok &= Check(store.SaveTerminalSnapshot(project.id, L"current terminal screen", &error),
                    (L"save terminal snapshot: " + error).c_str());
        const auto snapshot = store.LoadTerminalSnapshot(project.id);
        ok &= Check(snapshot && *snapshot == L"current terminal screen",
                    L"latest terminal snapshot replaces repaint history");

        error.clear();
        ok &= Check(store.DeleteProject(second_project.id, &error),
                    (L"delete second project: " + error).c_str());
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

    const auto legacy_database = database.parent_path() / L"legacy-project-order.db";
    std::filesystem::remove(legacy_database, filesystem_error);
    sqlite3* legacy_raw{};
    if (sqlite3_open16(legacy_database.c_str(), &legacy_raw) == SQLITE_OK) {
        const char* legacy_schema =
            "CREATE TABLE projects(id TEXT PRIMARY KEY,name TEXT NOT NULL,type_name TEXT NOT NULL,"
            "group_name TEXT NOT NULL,folder TEXT NOT NULL,command TEXT NOT NULL,status TEXT NOT NULL,"
            "created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);"
            "INSERT INTO projects VALUES('older','Older','Codex','默认分组','C:/','codex','created',1,10);"
            "INSERT INTO projects VALUES('newer','Newer','Codex','默认分组','C:/','codex','created',2,20);";
        ok &= Check(sqlite3_exec(legacy_raw, legacy_schema, nullptr, nullptr, nullptr) == SQLITE_OK,
                    L"create legacy project schema");
        sqlite3_close_v2(legacy_raw);
        aicli::DataStore migrated_store;
        std::wstring migration_error;
        ok &= Check(migrated_store.Initialize(legacy_database.wstring(), &migration_error),
                    (L"migrate legacy project order: " + migration_error).c_str());
        const auto migrated_projects = migrated_store.LoadProjects();
        ok &= Check(migrated_projects.size() == 2 && migrated_projects[0].id == L"newer" &&
                        migrated_projects[1].id == L"older",
                    L"legacy updated-time order is preserved during sort migration");
        migrated_store.Close();
    } else {
        ok &= Check(false, L"open legacy migration database");
    }
    CoUninitialize();

    std::filesystem::remove(database, filesystem_error);
    std::filesystem::remove(database.wstring() + L"-wal", filesystem_error);
    std::filesystem::remove(database.wstring() + L"-shm", filesystem_error);
    std::filesystem::remove(legacy_database, filesystem_error);
    std::filesystem::remove(legacy_database.wstring() + L"-wal", filesystem_error);
    std::filesystem::remove(legacy_database.wstring() + L"-shm", filesystem_error);

    if (!ok) return 1;
    std::wcout << L"AI CLI data-store tests passed.\n";
    return 0;
}
