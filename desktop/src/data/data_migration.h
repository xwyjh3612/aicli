#pragma once
#include <filesystem>
#include <string>

namespace aicli {
// Must run before opening the application's database. Both locations belong to
// the same Windows user. Backups include an online SQLite snapshot (with WAL).
// backup_only=true is the pre-registration installer phase; false migrates at
// first startup under the new manifest, then never imports the old overlay again.
bool PrepareDataUpgrade(const std::filesystem::path& stable,
                        const std::filesystem::path& legacy,
                        bool backup_only, std::wstring* error);
bool MigrateCurrentPackageData(const std::filesystem::path& stable, std::wstring* error);
}
