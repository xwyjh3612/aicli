#include "data/data_migration.h"
#include <windows.h>
#include <appmodel.h>
#include <shlobj.h>
#include <winsqlite/winsqlite3.h>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace aicli {
namespace {
namespace fs = std::filesystem;
constexpr wchar_t kMarker[] = L".unvirtualized-data-v1";
struct CloseDatabase { void operator()(sqlite3* db) const { if (db) sqlite3_close(db); } };
using Database = std::unique_ptr<sqlite3, CloseDatabase>;

Database Open(const fs::path& path) {
    sqlite3* raw{};
    const int code = sqlite3_open16(path.c_str(), &raw);
    Database db(raw);
    if (code != SQLITE_OK) throw std::runtime_error("Cannot open database");
    sqlite3_busy_timeout(db.get(), 5000);
    return db;
}

void Snapshot(const fs::path& source, const fs::path& destination) {
    auto input = Open(source);
    auto output = Open(destination);
    auto* backup = sqlite3_backup_init(output.get(), "main", input.get(), "main");
    if (!backup) throw std::runtime_error("Cannot begin database backup");
    const auto deadline = GetTickCount64() + 10000;
    int result;
    do {
        result = sqlite3_backup_step(backup, -1);
        if (result == SQLITE_BUSY || result == SQLITE_LOCKED) Sleep(50);
    } while ((result == SQLITE_BUSY || result == SQLITE_LOCKED) && GetTickCount64() < deadline);
    const auto finished = sqlite3_backup_finish(backup);
    if (result != SQLITE_DONE || finished != SQLITE_OK)
        throw std::runtime_error("Database backup failed or database is busy");
    sqlite3_stmt* check{};
    if (sqlite3_prepare_v2(output.get(), "PRAGMA quick_check;", -1, &check, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot verify database backup");
    const bool valid = sqlite3_step(check) == SQLITE_ROW &&
        std::string(reinterpret_cast<const char*>(sqlite3_column_text(check, 0))) == "ok";
    sqlite3_finalize(check);
    if (!valid) throw std::runtime_error("Database integrity check failed");
    // Backups must be self-contained, not depend on temporary WAL sidecars.
    if (sqlite3_exec(output.get(), "PRAGMA journal_mode=DELETE;", nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot finalize database backup");
}

bool IsReparsePoint(const fs::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("Cannot inspect data path");
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void CopyTree(const fs::path& source, const fs::path& destination) {
    // Never traverse links out of the data directory or silently omit data.
    if (IsReparsePoint(source)) throw std::runtime_error("Data contains a link; manual migration is required");
    if (fs::exists(destination) && IsReparsePoint(destination)) throw std::runtime_error("Destination contains a link");
    if (fs::is_directory(source)) {
        fs::create_directories(destination);
        for (const auto& child : fs::directory_iterator(source))
            CopyTree(child.path(), destination / child.path().filename());
    } else {
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing);
    }
}

bool Skip(const fs::path& name) {
    const auto text = name.wstring();
    return _wcsicmp(text.c_str(), L"backups") == 0 ||
        _wcsicmp(text.c_str(), kMarker) == 0 ||
        _wcsicmp(text.c_str(), L"aicli.db") == 0 ||
        _wcsicmp(text.c_str(), L"aicli.db-wal") == 0 ||
        _wcsicmp(text.c_str(), L"aicli.db-shm") == 0 ||
        _wcsicmp(text.c_str(), L"aicli.db-journal") == 0;
}

void CopyExtras(const fs::path& source, const fs::path& destination) {
    fs::create_directories(destination);
    for (const auto& entry : fs::directory_iterator(source)) {
        if (!Skip(entry.path().filename())) CopyTree(entry.path(), destination / entry.path().filename());
    }
}

void BackupDirectory(const fs::path& source, const fs::path& destination) {
    if (!fs::exists(source)) return;
    if (IsReparsePoint(source)) throw std::runtime_error("Data directory is a link");
    CopyExtras(source, destination);
    if (fs::exists(source / L"aicli.db")) {
        if (IsReparsePoint(source / L"aicli.db")) throw std::runtime_error("Database is a link");
        Snapshot(source / L"aicli.db", destination / L"aicli.db");
    }
}

void ReplaceDatabase(const fs::path& source, const fs::path& destination) {
    if (fs::exists(destination)) {
        auto db = Open(destination);
        if (sqlite3_wal_checkpoint_v2(db.get(), nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("Close AI CLI before migrating its database");
    }
    // A surviving journal may belong to another open connection. Do not pair
    // the restored database with sidecars from a different database.
    for (const auto suffix : {L"-wal", L"-shm", L"-journal"}) {
        if (fs::exists(destination.wstring() + suffix))
            throw std::runtime_error("Database is still in use; close AI CLI and retry");
    }
    const auto temporary = destination.wstring() + L".migrating";
    fs::copy_file(source, temporary, fs::copy_options::overwrite_existing);
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot commit migrated database");
}
}

bool PrepareDataUpgrade(const fs::path& stable, const fs::path& legacy,
                        bool backup_only, std::wstring* error) {
    fs::path backup;
    try {
        const bool marked = fs::exists(stable / kMarker);
        const bool migrate = !marked && fs::exists(legacy);
        if (marked && !backup_only) return true;
        if (migrate || (backup_only && fs::exists(stable / L"aicli.db"))) {
            GUID guid{};
            if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot create backup identifier");
            wchar_t name[40]{};
            StringFromGUID2(guid, name, 40);
            backup = stable / L"backups" / (std::wstring(L"upgrade-") + name);
            fs::create_directories(backup);
            BackupDirectory(stable, backup / L"stable");
            if (migrate) BackupDirectory(legacy, backup / L"package");
            // Setup may still fail to register the new package. Only the new
            // unvirtualized app may migrate/mark data after registration.
            if (backup_only) return true;
            if (migrate) {
                // Windows previously read the private overlay before real
                // AppData. Preserve that precedence, retaining both backups.
                CopyExtras(backup / L"package", stable);
                if (fs::exists(backup / L"package/aicli.db"))
                    ReplaceDatabase(backup / L"package/aicli.db", stable / L"aicli.db");
            }
        }
        if (backup_only) return true;
        fs::create_directories(stable);
        // Write only after the entire migration succeeds. A failed migration
        // is retried; an old overlay never replaces later user edits.
        const auto temporary = stable / L".unvirtualized-data-v1.tmp";
        { std::ofstream marker(temporary, std::ios::binary | std::ios::trunc);
          marker << "AI CLI data migration v1\n";
          marker.close();
          if (!marker) throw std::runtime_error("Cannot write migration marker"); }
        if (!MoveFileExW(temporary.c_str(), (stable / kMarker).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot commit migration marker");
        return true;
    } catch (...) {
        if (error) *error = L"无法备份或迁移数据，请关闭 AI CLI 后重试，并确认磁盘空间与目录权限。\n数据目录：" +
            stable.wstring() + L"\n旧版目录：" + legacy.wstring() +
            (backup.empty() ? L"" : L"\n本次备份目录：" + backup.wstring());
        return false;
    }
}

bool MigrateCurrentPackageData(const fs::path& stable, std::wstring* error) {
    UINT32 length{};
    if (GetCurrentPackageFamilyName(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER) return true;
    std::vector<wchar_t> family(length);
    if (GetCurrentPackageFamilyName(&length, family.data()) != ERROR_SUCCESS) {
        if (error) *error = L"无法读取当前应用包标识。";
        return false;
    }
    // Explicit test/portable data overrides must never import production data.
    PWSTR local{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_NO_PACKAGE_REDIRECTION, nullptr, &local))) {
        if (error) *error = L"无法读取本地数据目录。";
        return false;
    }
    const fs::path root(local);
    CoTaskMemFree(local);
    if (stable != root / L"AI CLI" && stable != root / L"AI CLI Dev") return true;
    const auto legacy = root / L"Packages" / family.data() / L"LocalCache/Local" / stable.filename();
    return PrepareDataUpgrade(stable, legacy, false, error);
}
}
