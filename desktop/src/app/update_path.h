#pragma once
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace aicli::update {
inline std::filesystem::path PhysicalInstallerPath(const std::filesystem::path& logical) {
    const auto file = CreateFileW(logical.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Installer does not exist");
    const auto needed = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    std::vector<wchar_t> buffer(needed + 1);
    const auto count = needed ? GetFinalPathNameByHandleW(file, buffer.data(),
        static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
    CloseHandle(file);
    if (!count || count >= buffer.size()) throw std::runtime_error("Cannot resolve installer path");
    std::wstring result(buffer.data(), count);
    // ShellExecute does not consistently accept the extended-length prefix.
    if (result.starts_with(L"\\\\?\\UNC\\")) result = L"\\\\" + result.substr(8);
    else if (result.starts_with(L"\\\\?\\")) result.erase(0, 4);
    return result;
}
}
