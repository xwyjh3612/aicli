#include <windows.h>
#include <appmodel.h>
#include <shobjidl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

int Activate(const wchar_t* aumid) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IApplicationActivationManager* manager{};
    HRESULT result = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager));
    DWORD process{};
    if (SUCCEEDED(result)) {
        result = manager->ActivateApplication(aumid, nullptr, AO_NOERRORUI, &process);
        manager->Release();
    }
    CoUninitialize();
    if (FAILED(result)) return 1;
    std::cout << process << '\n';
    return 0;
}

// The installer-order test uses its own sparse package and installation folder.
// No production application, data directory or package is opened here.
int wmain(int argc, wchar_t** argv) {
    ShowWindow(GetConsoleWindow(), SW_HIDE);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--activate") return Activate(argv[2]);
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, 32768)) return 2;
    const auto root = std::filesystem::path(path).parent_path();
    UINT32 size{};
    if (GetCurrentPackageId(&size, nullptr) != ERROR_INSUFFICIENT_BUFFER) {
        // Mirror AiCli's self-activation when Explorer starts the physical exe.
        std::ifstream manifest(root / L"AppxManifest.xml");
        const std::string xml{std::istreambuf_iterator<char>(manifest), {}};
        const auto identity = xml.find("<Identity Name=\"");
        if (identity == std::string::npos) return 3;
        const auto begin = identity + std::string_view("<Identity Name=\"").size();
        const auto end = xml.find('"', begin);
        const auto name = xml.substr(begin, end - begin);
        const std::wstring aumid = std::wstring(name.begin(), name.end()) + L"_jw3zr00rmb0ym!App";
        return Activate(aumid.c_str());
    }
    std::vector<BYTE> bytes(size);
    if (GetCurrentPackageId(&size, bytes.data()) != ERROR_SUCCESS) return 4;
    const auto version = reinterpret_cast<PACKAGE_ID*>(bytes.data())->version;
    {
        std::ofstream report(root / L"started-version.txt", std::ios::app);
        report << version.Major << '.' << version.Minor << '.' << version.Build << '.' << version.Revision
               << ' ' << GetCurrentProcessId() << '\n';
    }
    // Stay active so registration would fail if Setup starts us too early.
    while (!std::filesystem::exists(root / L"stop-probe.txt")) Sleep(50);
    return 0;
}
