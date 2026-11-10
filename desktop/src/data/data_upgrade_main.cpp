#include "data/data_migration.h"
#include <windows.h>
#include <shlobj.h>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    // This unpackaged helper runs as the installing user before registration.
    if (argc != 2 || std::wstring(argv[1]) != L"AiCli.Desktop.Release_jw3zr00rmb0ym") return 2;
    PWSTR local{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_NO_PACKAGE_REDIRECTION, nullptr, &local))) return 3;
    const std::filesystem::path root(local);
    CoTaskMemFree(local);
    std::wstring error;
    if (!aicli::PrepareDataUpgrade(root / L"AI CLI", root / L"Packages" / argv[1] / L"LocalCache/Local/AI CLI", true, &error)) {
        std::wcerr << error << L'\n';
        return 1;
    }
    return 0;
}
