#include "app/project_launcher.h"

#include <windows.h>

#include <filesystem>
#include <vector>

namespace aicli::app {
namespace {

std::wstring WindowsErrorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (!length || !buffer) return L"Windows 错误 " + std::to_wstring(error);

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

std::filesystem::path CommandPromptPath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const UINT length = GetSystemDirectoryW(
            buffer.data(), static_cast<UINT>(buffer.size()));
        if (!length) return L"cmd.exe";
        if (length < buffer.size()) {
            return std::filesystem::path(
                       std::wstring(buffer.data(), static_cast<size_t>(length))) /
                   L"cmd.exe";
        }
        buffer.resize(static_cast<size_t>(length) + 1);
    }
}

std::filesystem::path PowerShellPath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const UINT length = GetSystemDirectoryW(
            buffer.data(), static_cast<UINT>(buffer.size()));
        if (!length) return L"powershell.exe";
        if (length < buffer.size()) {
            return std::filesystem::path(
                       std::wstring(buffer.data(), static_cast<size_t>(length))) /
                   L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
        }
        buffer.resize(static_cast<size_t>(length) + 1);
    }
}

bool OpenShellAt(const std::filesystem::path& executable,
                 const std::wstring& command_line,
                 const wchar_t* shell_name,
                 const std::wstring& working_directory,
                 std::wstring* error_message) {
    if (error_message) error_message->clear();
    if (working_directory.empty()) {
        if (error_message) *error_message = L"当前项目还没有设置项目目录。";
        return false;
    }

    std::error_code filesystem_error;
    if (!std::filesystem::is_directory(working_directory, filesystem_error)) {
        if (error_message) *error_message = L"项目目录不存在或无法访问。";
        return false;
    }

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    std::wstring mutable_command_line = command_line;
    if (!CreateProcessW(executable.c_str(),
                        mutable_command_line.data(), nullptr,
                        nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                        working_directory.c_str(), &startup, &process)) {
        if (error_message) {
            *error_message = L"无法启动 " + std::wstring(shell_name) + L"：" +
                             WindowsErrorMessage(GetLastError());
        }
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace

bool OpenCommandPromptAt(const std::wstring& working_directory,
                         std::wstring* error_message) {
    const std::filesystem::path command_prompt = CommandPromptPath();
    const std::wstring command_line = L"\"" + command_prompt.wstring() + L"\"";
    return OpenShellAt(command_prompt, command_line, L"CMD",
                       working_directory, error_message);
}

bool OpenPowerShellAt(const std::wstring& working_directory,
                      std::wstring* error_message) {
    const std::filesystem::path powershell = PowerShellPath();
    const std::wstring command_line =
        L"\"" + powershell.wstring() + L"\" -NoLogo -NoExit";
    return OpenShellAt(powershell, command_line, L"PowerShell",
                       working_directory, error_message);
}

} // namespace aicli::app
