#include "data/data_migration.h"
#include "app/update_path.h"
#include <windows.h>
#include <appmodel.h>
#include <shlobj.h>
#include <winsqlite/winsqlite3.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
sqlite3* Seed(const fs::path& path, const char* value) {
    fs::create_directories(path.parent_path());
    sqlite3* db{};
    Require(sqlite3_open16(path.c_str(), &db) == SQLITE_OK, "open seed");
    Require(sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; CREATE TABLE IF NOT EXISTS test(value TEXT); DELETE FROM test;",
        nullptr, nullptr, nullptr) == SQLITE_OK, "schema");
    const auto sql = std::string("INSERT INTO test VALUES('") + value + "');";
    Require(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, "seed value");
    return db;
}
std::string Read(const fs::path& path) {
    sqlite3* db{};
    Require(sqlite3_open16(path.c_str(), &db) == SQLITE_OK, "read database");
    sqlite3_stmt* query{};
    Require(sqlite3_prepare_v2(db, "SELECT value FROM test;", -1, &query, nullptr) == SQLITE_OK, "query");
    Require(sqlite3_step(query) == SQLITE_ROW, "row");
    const std::string result(reinterpret_cast<const char*>(sqlite3_column_text(query, 0)));
    sqlite3_finalize(query);
    sqlite3_close(db);
    return result;
}
fs::path Local() {
    PWSTR local{};
    Require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_NO_PACKAGE_REDIRECTION, nullptr, &local)), "local appdata");
    const fs::path root(local);
    CoTaskMemFree(local);
    return root;
}

int Probe(const std::wstring& mode, const fs::path& report) {
    ShowWindow(GetConsoleWindow(), SW_HIDE);
    const auto root = Local() / L"AI CLI Upgrade Probe 1006";
    if (mode == L"--probe-write") {
        sqlite3_close(Seed(root / L"aicli.db", "old-package-user-data"));
        std::ofstream(root / L"agent-server.pem") << "TEST CERTIFICATE ONLY";
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        fs::copy_file(executable, root / L"download.exe", fs::copy_options::overwrite_existing);
        std::ofstream output(report, std::ios::binary);
        output << (root / L"download.exe").string() << '\n'
               << aicli::update::PhysicalInstallerPath(root / L"download.exe").string() << '\n';
    } else if (mode == L"--probe-migrate") {
        UINT32 size = 256;
        wchar_t family[256]{};
        Require(GetCurrentPackageFamilyName(&size, family) == ERROR_SUCCESS, "package family");
        std::wstring error;
        Require(aicli::PrepareDataUpgrade(root, Local() / L"Packages" / family / L"LocalCache/Local" / root.filename(), false, &error), "packaged migration");
        Require(Read(root / L"aicli.db") == "old-package-user-data", "preserved old package database");
        Require(fs::exists(root / L"agent-server.pem"), "preserved certificate");
        std::ofstream(report) << "migration passed";
    } else if (mode == L"--probe-child") {
        std::ofstream(report) << "physical installer launch passed";
    } else return 2;
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        if (argc == 4 && std::wstring(argv[1]) == L"--activate") {
            IApplicationActivationManager* manager{};
            Require(SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager))), "activation manager");
            DWORD pid{};
            const auto result = manager->ActivateApplication(argv[2], argv[3], AO_NOERRORUI, &pid);
            manager->Release();
            Require(SUCCEEDED(result), "activate probe package");
            const auto process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            Require(process != nullptr, "probe process");
            Require(WaitForSingleObject(process, 30000) == WAIT_OBJECT_0, "probe timed out");
            DWORD code{};
            GetExitCodeProcess(process, &code);
            CloseHandle(process);
            return static_cast<int>(code);
        }
        if (argc == 3) return Probe(argv[1], argv[2]);
        Require(argc == 1, "unexpected arguments");
        GUID guid{};
        CoCreateGuid(&guid);
        wchar_t name[40]{};
        StringFromGUID2(guid, name, 40);
        const auto root = fs::temp_directory_path() / (std::wstring(L"aicli-upgrade-test-") + name);
        const auto stable = root / L"用户数据 stable";
        const auto legacy = root / L"package/LocalCache/Local/AI CLI";
        sqlite3_close(Seed(stable / L"aicli.db", "stable-old"));
        sqlite3* live = Seed(legacy / L"aicli.db", "private-with-live-wal");
        fs::create_directories(legacy / L"mobile_uploads");
        std::ofstream(legacy / L"agent-server.pem") << "TEST CERTIFICATE ONLY";
        std::ofstream(legacy / L"mobile_uploads/test.txt") << "attachment";
        std::wstring error;
        Require(aicli::PrepareDataUpgrade(stable, legacy, true, &error), "pre-registration backup");
        Require(!fs::exists(stable / L".unvirtualized-data-v1"), "backup must not mark migration before registration");
        Require(Read(stable / L"aicli.db") == "stable-old", "backup must not replace current data");
        Require(aicli::PrepareDataUpgrade(stable, legacy, false, &error), "migrate live WAL");
        Require(Read(stable / L"aicli.db") == "private-with-live-wal", "WAL contents preserved");
        Require(fs::exists(stable / L"agent-server.pem") && fs::exists(stable / L"mobile_uploads/test.txt"), "cert and uploads preserved");
        int snapshots = 0;
        for (const auto& backup : fs::directory_iterator(stable / L"backups")) {
            Require(Read(backup.path() / L"stable/aicli.db") == "stable-old", "stable conflict backup preserved");
            Require(Read(backup.path() / L"package/aicli.db") == "private-with-live-wal", "package backup contains WAL");
            ++snapshots;
        }
        Require(snapshots == 2, "separate pre-install and migration backups");
        sqlite3_close(live);
        sqlite3_close(Seed(stable / L"aicli.db", "newer-user-edit"));
        Require(aicli::PrepareDataUpgrade(stable, legacy, false, &error), "repeat startup");
        Require(Read(stable / L"aicli.db") == "newer-user-edit", "stale overlay must never overwrite newer data");
        Require(aicli::PrepareDataUpgrade(stable, legacy, true, &error), "future upgrade backup");
        const auto corrupt = root / L"corrupt";
        fs::create_directories(corrupt);
        std::ofstream(corrupt / L"aicli.db") << "not sqlite";
        Require(!aicli::PrepareDataUpgrade(root / L"failed", corrupt, false, &error), "corrupt legacy data must abort");
        Require(!fs::exists(root / L"failed/.unvirtualized-data-v1"), "failure must not commit marker");
        Require(aicli::PrepareDataUpgrade(root / L"clean", root / L"absent", false, &error), "fresh install");
        Require(!fs::exists(root / L"clean/aicli.db"), "migration must not create an empty database");
        const auto unicodeFile = root / L"更新 安装程序.exe";
        std::ofstream(unicodeFile) << "test";
        Require(fs::equivalent(unicodeFile, aicli::update::PhysicalInstallerPath(unicodeFile)), "Unicode physical installer path");
        bool rejected = false;
        try { aicli::update::PhysicalInstallerPath(root / L"missing.exe"); } catch (...) { rejected = true; }
        Require(rejected, "missing installer rejected");
        fs::remove_all(root);
        std::cout << "Data upgrade and physical installer path tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
