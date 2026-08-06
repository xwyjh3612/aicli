#include "conpty_connection_bridge.h"

#include <objbase.h>
#include <algorithm>
#include <filesystem>
#include <map>
#include <vector>

namespace aicli::terminal {
namespace {

std::wstring PowerShellLiteral(std::wstring value) {
    size_t position = 0;
    while ((position = value.find(L'\'', position)) != std::wstring::npos) {
        value.insert(position, 1, L'\'');
        position += 2;
    }
    return L"'" + value + L"'";
}

std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

struct EnvironmentNameLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const noexcept {
        return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                    right.c_str(), static_cast<int>(right.size()),
                                    TRUE) == CSTR_LESS_THAN;
    }
};

std::wstring GuidString(bool include_braces) {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return {};
    wchar_t value[40]{};
    StringFromGUID2(guid, value, static_cast<int>(std::size(value)));
    std::wstring result{value};
    if (!include_braces && result.size() > 2) {
        result = result.substr(1, result.size() - 2);
    }
    return result;
}

std::vector<wchar_t> BuildTerminalEnvironment() {
    std::map<std::wstring, std::wstring, EnvironmentNameLess> variables;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* current = block; *current; current += wcslen(current) + 1) {
            std::wstring entry{current};
            const size_t separator = entry[0] == L'=' ? entry.find(L'=', 1) : entry.find(L'=');
            if (separator != std::wstring::npos) {
                variables[entry.substr(0, separator)] = entry.substr(separator + 1);
            }
        }
        FreeEnvironmentStringsW(block);
    }

    variables[L"WT_SESSION"] = GuidString(false);
    variables[L"WT_PROFILE_ID"] = GuidString(true);
    // Some Windows TUI libraries fall back to legacy Console APIs when TERM
    // is absent. That fallback cannot represent SGR 2 (dim/faint), so Codex
    // loses its muted text hierarchy even though the ConPTY transport works.
    variables[L"TERM"] = L"xterm-256color";
    variables[L"COLORTERM"] = L"truecolor";

    size_t required = 1;
    for (const auto& [name, value] : variables) required += name.size() + value.size() + 2;
    std::vector<wchar_t> result;
    result.reserve(required);
    for (const auto& [name, value] : variables) {
        result.insert(result.end(), name.begin(), name.end());
        result.push_back(L'=');
        result.insert(result.end(), value.begin(), value.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

}

ConPtyConnectionBridge::ConPtyConnectionBridge(
    std::wstring working_directory,
    std::wstring startup_command) :
    working_directory_(std::move(working_directory)),
    startup_command_(std::move(startup_command)) {
    wchar_t system_directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    std::filesystem::path shell = length > 0 && length < MAX_PATH
        ? std::filesystem::path(system_directory) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe"
        : std::filesystem::path(L"powershell.exe");
    shell_command_line_ = L"\"" + shell.wstring() + L"\" -NoLogo -NoExit";
}

ConPtyConnectionBridge::~ConPtyConnectionBridge() {
    Close();
}

void ConPtyConnectionBridge::Initialize(
    const winrt::Windows::Foundation::Collections::ValueSet& settings) {
    if (!settings) return;
    if (const auto rows = settings.TryLookup(L"initialRows")) {
        rows_ = winrt::unbox_value_or<uint32_t>(rows, rows_);
    }
    if (const auto columns = settings.TryLookup(L"initialCols")) {
        columns_ = winrt::unbox_value_or<uint32_t>(columns, columns_);
    }
}

void ConPtyConnectionBridge::Start() {
    std::scoped_lock lock(lifecycle_mutex_);
    if (process_ || closing_) return;

    SetState(winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Connecting);
    if (!CreateProcessAttachedToConPty()) {
        EmitOutput(L"\x1b[31mUnable to start the terminal session.\x1b[0m\r\n");
        SetState(winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Failed);
        return;
    }

    reader_ = std::thread([this] { ReadLoop(); });
    SetState(winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Connected);

    const auto startup_input = BuildStartupInput();
    if (!startup_input.empty()) {
        const auto* first = reinterpret_cast<const char16_t*>(startup_input.data());
        WriteInput(winrt::array_view<const char16_t>{
            first, static_cast<uint32_t>(startup_input.size())});
    }
}

bool ConPtyConnectionBridge::CreateProcessAttachedToConPty() {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;

    if (!CreatePipe(&input_read, &input_write, &attributes, 0)) return false;
    if (!CreatePipe(&output_read, &output_write, &attributes, 0)) {
        CloseHandle(input_read);
        CloseHandle(input_write);
        return false;
    }

    SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);

    const COORD dimensions{
        static_cast<SHORT>(std::clamp<uint32_t>(columns_, 20, SHRT_MAX)),
        static_cast<SHORT>(std::clamp<uint32_t>(rows_, 5, SHRT_MAX))};
    const HRESULT create_result = CreatePseudoConsole(
        dimensions, input_read, output_write, 0, &pseudo_console_);
    CloseHandle(input_read);
    CloseHandle(output_write);
    if (FAILED(create_result)) {
        CloseHandle(input_write);
        CloseHandle(output_read);
        pseudo_console_ = nullptr;
        return false;
    }

    input_ = input_write;
    output_ = output_read;

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attribute_size));
    if (!attribute_list ||
        !InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_size) ||
        !UpdateProcThreadAttribute(attribute_list, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   pseudo_console_, sizeof(pseudo_console_),
                                   nullptr, nullptr)) {
        if (attribute_list) HeapFree(GetProcessHeap(), 0, attribute_list);
        return false;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attribute_list;
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> command_line(shell_command_line_.begin(), shell_command_line_.end());
    command_line.push_back(L'\0');
    auto environment = BuildTerminalEnvironment();

    const BOOL created = CreateProcessW(
        nullptr, command_line.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        environment.data(), working_directory_.empty() ? nullptr : working_directory_.c_str(),
        &startup.StartupInfo, &process);

    DeleteProcThreadAttributeList(attribute_list);
    HeapFree(GetProcessHeap(), 0, attribute_list);
    if (!created) return false;

    process_ = process.hProcess;
    CloseHandle(process.hThread);
    return true;
}

std::wstring ConPtyConnectionBridge::BuildStartupInput() const {
    std::wstring input =
        L"$OutputEncoding=[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new();"
        L"[Console]::InputEncoding=[System.Text.UTF8Encoding]::new()\r";
    if (!working_directory_.empty()) {
        input += L"Set-Location -LiteralPath " + PowerShellLiteral(working_directory_) + L"\r";
    }
    if (!startup_command_.empty()) {
        input += startup_command_ + L"\r";
    }
    return input;
}

void ConPtyConnectionBridge::WriteInput(winrt::array_view<const char16_t> data) {
    if (!input_ || data.empty()) return;
    const auto bytes = Utf8(std::wstring_view{
        reinterpret_cast<const wchar_t*>(data.data()), data.size()});
    DWORD written = 0;
    WriteFile(input_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

void ConPtyConnectionBridge::Resize(uint32_t rows, uint32_t columns) {
    rows_ = rows;
    columns_ = columns;
    if (!pseudo_console_) return;
    const COORD dimensions{
        static_cast<SHORT>(std::clamp<uint32_t>(columns, 20, SHRT_MAX)),
        static_cast<SHORT>(std::clamp<uint32_t>(rows, 5, SHRT_MAX))};
    ResizePseudoConsole(pseudo_console_, dimensions);
}

void ConPtyConnectionBridge::ReadLoop() {
    char buffer[8192];
    DWORD read = 0;
    std::string pending;
    while (!closing_ && output_ &&
           ReadFile(output_, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        pending.append(buffer, read);
        size_t usable = pending.size();
        int count = 0;
        while (usable > 0) {
            count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        pending.data(), static_cast<int>(usable),
                                        nullptr, 0);
            if (count > 0) break;
            --usable;
        }
        if (usable > 0 && count > 0) {
            std::wstring text(static_cast<size_t>(count), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                pending.data(), static_cast<int>(usable),
                                text.data(), count);
            EmitOutput(text);
            pending.erase(0, usable);
        }
        if (pending.size() > 8) {
            const int fallback_count = MultiByteToWideChar(
                CP_UTF8, 0, pending.data(), static_cast<int>(pending.size()), nullptr, 0);
            std::wstring text(static_cast<size_t>(fallback_count), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, pending.data(),
                                static_cast<int>(pending.size()), text.data(), fallback_count);
            EmitOutput(text);
            pending.clear();
        }
    }
    if (!closing_) {
        SetState(winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Closed);
    }
}

void ConPtyConnectionBridge::Close() noexcept {
    std::unique_lock lock(lifecycle_mutex_);
    if (closing_.exchange(true)) return;
    SetState(winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Closing);

    if (pseudo_console_) {
        ClosePseudoConsole(pseudo_console_);
        pseudo_console_ = nullptr;
    }
    if (input_) {
        CloseHandle(input_);
        input_ = nullptr;
    }
    if (process_) {
        TerminateProcess(process_, 0);
    }
    if (output_) {
        CloseHandle(output_);
        output_ = nullptr;
    }

    lock.unlock();
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) {
        reader_.join();
    }
    lock.lock();
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    SetState(winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Closed);
}

void ConPtyConnectionBridge::EmitOutput(std::wstring_view output) noexcept {
    try {
        output_handlers_(winrt::hstring{output});
    } catch (...) {
    }
}

void ConPtyConnectionBridge::SetState(
    winrt::Microsoft::Terminal::TerminalConnection::ConnectionState state) noexcept {
    if (state_.exchange(state) == state) return;
    try {
        state_handlers_(*this, nullptr);
    } catch (...) {
    }
}

winrt::event_token ConPtyConnectionBridge::TerminalOutput(
    const winrt::Microsoft::Terminal::TerminalConnection::TerminalOutputHandler& handler) {
    return output_handlers_.add(handler);
}

void ConPtyConnectionBridge::TerminalOutput(const winrt::event_token& token) noexcept {
    output_handlers_.remove(token);
}

winrt::event_token ConPtyConnectionBridge::StateChanged(
    const winrt::Windows::Foundation::TypedEventHandler<
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection,
        winrt::Windows::Foundation::IInspectable>& handler) {
    return state_handlers_.add(handler);
}

void ConPtyConnectionBridge::StateChanged(const winrt::event_token& token) noexcept {
    state_handlers_.remove(token);
}

winrt::guid ConPtyConnectionBridge::SessionId() const noexcept {
    return session_id_;
}

winrt::Microsoft::Terminal::TerminalConnection::ConnectionState
ConPtyConnectionBridge::State() const noexcept {
    return state_.load();
}

}
