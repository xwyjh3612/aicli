#include "conpty_connection_bridge.h"

#include <conpty.h>
#include <objbase.h>
#include <winternl.h>
#include <algorithm>
#include <filesystem>
#include <map>
#include <vector>

namespace aicli::terminal {
namespace {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept {
        const HANDLE value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HANDLE value = nullptr) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_{};
};

struct DuplexPipe {
    UniqueHandle server;
    UniqueHandle client;
};

using NtCreateFileFunction = NTSTATUS(NTAPI*)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
    ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtCreateNamedPipeFileFunction = NTSTATUS(NTAPI*)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG,
    ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, PLARGE_INTEGER);

constexpr ULONG kFilePipeByteStreamType = 0;
constexpr ULONG kFilePipeByteStreamMode = 0;
constexpr ULONG kFilePipeQueueOperation = 0;
constexpr std::wstring_view kCodexInputBackground = L"\x1b[48;2;41;41;41m";
constexpr std::wstring_view kLightInputBackground = L"\x1b[48;2;232;235;239m";

bool NtSucceeded(NTSTATUS status) noexcept {
    return status >= 0;
}

UNICODE_STRING UnicodeString(std::wstring_view value) noexcept {
    UNICODE_STRING result{};
    result.Buffer = const_cast<PWSTR>(value.data());
    result.Length = static_cast<USHORT>(value.size() * sizeof(wchar_t));
    result.MaximumLength = result.Length;
    return result;
}

// This is the same anonymous NPFS pipe topology used by Windows Terminal
// 1.24's Utils::CreateOverlappedPipe(PIPE_ACCESS_DUPLEX). Using the native
// API is intentional: CreatePipe cannot create a full-duplex overlapped
// anonymous pipe, while a discoverable named pipe would require additional
// ACL and anti-squatting machinery.
bool CreateOverlappedDuplexPipe(DuplexPipe& result) noexcept {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    const auto nt_create_file = reinterpret_cast<NtCreateFileFunction>(
        GetProcAddress(ntdll, "NtCreateFile"));
    const auto nt_create_named_pipe = reinterpret_cast<NtCreateNamedPipeFileFunction>(
        GetProcAddress(ntdll, "NtCreateNamedPipeFile"));
    if (!nt_create_file || !nt_create_named_pipe) return false;

    IO_STATUS_BLOCK status_block{};
    constexpr std::wstring_view pipe_path = L"\\Device\\NamedPipe\\";
    auto path = UnicodeString(pipe_path);
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.ObjectName = &path;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;

    HANDLE directory_handle = nullptr;
    NTSTATUS status = nt_create_file(
        &directory_handle,
        SYNCHRONIZE | GENERIC_READ,
        &attributes,
        &status_block,
        nullptr,
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT,
        nullptr,
        0);
    UniqueHandle directory{directory_handle};
    if (!NtSucceeded(status) || !directory) return false;

    UNICODE_STRING empty_path{};
    attributes.RootDirectory = directory.get();
    attributes.ObjectName = &empty_path;
    LARGE_INTEGER timeout{};
    timeout.QuadPart = -10'0000'0000LL;

    HANDLE server_handle = nullptr;
    status = nt_create_named_pipe(
        &server_handle,
        SYNCHRONIZE | GENERIC_READ | GENERIC_WRITE,
        &attributes,
        &status_block,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_CREATE,
        0,
        kFilePipeByteStreamType,
        kFilePipeByteStreamMode,
        kFilePipeQueueOperation,
        1,
        128 * 1024,
        128 * 1024,
        &timeout);
    UniqueHandle server{server_handle};
    if (!NtSucceeded(status) || !server) return false;

    attributes.RootDirectory = server.get();
    HANDLE client_handle = nullptr;
    status = nt_create_file(
        &client_handle,
        SYNCHRONIZE | GENERIC_READ | GENERIC_WRITE,
        &attributes,
        &status_block,
        nullptr,
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE,
        nullptr,
        0);
    UniqueHandle client{client_handle};
    if (!NtSucceeded(status) || !client) return false;

    result.server = std::move(server);
    result.client = std::move(client);
    return true;
}

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
    // Match the environment exposed by Windows Terminal on Windows. Setting
    // TERM/COLORTERM here makes terminal-detection libraries select a Unix
    // xterm path, which changes how inline TUIs perform viewport reflow and
    // can prevent completed Codex rows from reaching native scrollback.
    variables.erase(L"TERM");
    variables.erase(L"COLORTERM");
    variables.erase(L"TERM_PROGRAM");

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
    std::wstring startup_command,
    std::function<void(std::wstring_view)> output_observer) :
    working_directory_(std::move(working_directory)),
    startup_command_(std::move(startup_command)),
    output_observer_(std::move(output_observer)) {
    wchar_t system_directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    std::filesystem::path shell = length > 0 && length < MAX_PATH
        ? std::filesystem::path(system_directory) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe"
        : std::filesystem::path(L"powershell.exe");
    shell_command_line_ = L"\"" + shell.wstring() + L"\" -NoLogo -NoExit";
    // Windows Terminal 1.24 uses grapheme-based width measurement by default.
    // Keeping ConPTY and TermControl on the same model prevents cursor drift
    // for CJK text, emoji and combining sequences.
    pseudo_console_flags_ = PSEUDOCONSOLE_GLYPH_WIDTH_GRAPHEMES;
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
    if (const auto measurement = settings.TryLookup(L"textMeasurement")) {
        const auto value = winrt::unbox_value_or<winrt::hstring>(measurement, L"graphemes");
        pseudo_console_flags_ &= ~PSEUDOCONSOLE_GLYPH_WIDTH__MASK;
        if (value == L"wcswidth") {
            pseudo_console_flags_ |= PSEUDOCONSOLE_GLYPH_WIDTH_WCSWIDTH;
        } else if (value == L"console") {
            pseudo_console_flags_ |= PSEUDOCONSOLE_GLYPH_WIDTH_CONSOLE;
        } else {
            pseudo_console_flags_ |= PSEUDOCONSOLE_GLYPH_WIDTH_GRAPHEMES;
        }
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
    DuplexPipe transport;
    if (!CreateOverlappedDuplexPipe(transport)) return false;

    const COORD dimensions{
        static_cast<SHORT>(std::clamp<uint32_t>(columns_, 20, SHRT_MAX)),
        static_cast<SHORT>(std::clamp<uint32_t>(rows_, 5, SHRT_MAX))};
    HPCON created_pseudo_console = nullptr;
    // Windows Terminal 1.24 passes the same full-duplex client handle for
    // both directions. ConPTY owns its duplicated endpoints after this call;
    // our server handle is the sole transport used by ReadLoop/WriteInput.
    const HRESULT create_result = ConptyCreatePseudoConsole(
        dimensions, transport.client.get(), transport.client.get(),
        pseudo_console_flags_,
        &created_pseudo_console);
    if (FAILED(create_result)) return false;

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attribute_size));
    if (!attribute_list ||
        !InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_size) ||
        !UpdateProcThreadAttribute(attribute_list, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   created_pseudo_console, sizeof(created_pseudo_console),
                                   nullptr, nullptr)) {
        if (attribute_list) HeapFree(GetProcessHeap(), 0, attribute_list);
        ConptyClosePseudoConsole(created_pseudo_console);
        return false;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
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
    if (!created) {
        ConptyClosePseudoConsole(created_pseudo_console);
        return false;
    }

    pseudo_console_ = created_pseudo_console;
    pipe_ = transport.server.release();
    process_ = process.hProcess;
    CloseHandle(process.hThread);
    if (FAILED(ConptyReleasePseudoConsole(pseudo_console_))) {
        ConptyClosePseudoConsole(pseudo_console_);
        pseudo_console_ = nullptr;
        CloseHandle(pipe_);
        pipe_ = nullptr;
        TerminateProcess(process_, 1);
        CloseHandle(process_);
        process_ = nullptr;
        return false;
    }
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
    if (data.empty()) return;
    const auto bytes = Utf8(std::wstring_view{
        reinterpret_cast<const wchar_t*>(data.data()), data.size()});
    std::scoped_lock lock(input_mutex_);
    if (!pipe_ || closing_) return;
    UniqueHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event) return;
    size_t offset = 0;
    while (offset < bytes.size() && pipe_ && !closing_) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        ResetEvent(event.get());
        DWORD written = 0;
        const auto remaining = static_cast<DWORD>(std::min<size_t>(
            bytes.size() - offset, static_cast<size_t>(MAXDWORD)));
        if (!WriteFile(pipe_, bytes.data() + offset, remaining, &written, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING ||
                WaitForSingleObject(event.get(), INFINITE) != WAIT_OBJECT_0 ||
                !GetOverlappedResult(pipe_, &overlapped, &written, FALSE)) {
                break;
            }
        }
        if (written == 0) break;
        offset += written;
    }
}

void ConPtyConnectionBridge::Resize(uint32_t rows, uint32_t columns) {
    rows_ = rows;
    columns_ = columns;
    if (!pseudo_console_) return;
    const COORD dimensions{
        static_cast<SHORT>(std::clamp<uint32_t>(columns, 20, SHRT_MAX)),
        static_cast<SHORT>(std::clamp<uint32_t>(rows, 5, SHRT_MAX))};
    ConptyResizePseudoConsole(pseudo_console_, dimensions);
}

void ConPtyConnectionBridge::SetLightTheme(bool light_theme) noexcept {
    light_theme_.store(light_theme, std::memory_order_relaxed);
}

void ConPtyConnectionBridge::RefreshViewport() {
    if (!pseudo_console_) return;

    // A settings-only update changes TermControl's palette, but a full-screen
    // TUI such as Codex owns the colors already stored in the terminal cells.
    // Send a real ConPTY size transition so the application receives the
    // normal terminal resize notification and redraws without restarting the
    // process or replacing the native TermControl input surface.
    const uint32_t rows = rows_;
    const uint32_t columns = columns_;
    const uint32_t probe_columns = columns > 20 ? columns - 1 : columns + 1;
    Resize(rows, probe_columns);
    Resize(rows, columns);
}

void ConPtyConnectionBridge::ReadLoop() {
    // Windows Terminal's ConptyConnection reads 128 KiB at a time. Keeping
    // the same batch size prevents one Codex TUI repaint from being presented
    // as many small frames with the cursor at several transient positions.
    char buffer[128 * 1024];
    std::string pending;
    UniqueHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event) return;
    while (!closing_ && pipe_) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        ResetEvent(event.get());
        DWORD read = 0;
        if (!ReadFile(pipe_, buffer, sizeof(buffer), &read, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING ||
                WaitForSingleObject(event.get(), INFINITE) != WAIT_OBJECT_0 ||
                !GetOverlappedResult(pipe_, &overlapped, &read, FALSE)) {
                break;
            }
        }
        if (read == 0 || closing_) break;
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
        ConptyClosePseudoConsole(pseudo_console_);
        pseudo_console_ = nullptr;
    }
    if (pipe_) CancelIoEx(pipe_, nullptr);
    if (process_) {
        TerminateProcess(process_, 0);
    }

    lock.unlock();
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) {
        reader_.join();
    }
    std::scoped_lock input_lock(input_mutex_);
    lock.lock();
    if (pipe_) {
        CloseHandle(pipe_);
        pipe_ = nullptr;
    }
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    SetState(winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Closed);
}

void ConPtyConnectionBridge::EmitOutput(std::wstring_view output) noexcept {
    try {
        const auto display_output = TransformDisplayOutput(output);
        if (!display_output.empty()) output_handlers_(winrt::hstring{display_output});
    } catch (...) {
        try {
            output_handlers_(winrt::hstring{output});
        } catch (...) {
        }
    }
    if (output_observer_) {
        try {
            output_observer_(output);
        } catch (...) {
        }
    }
}

std::wstring ConPtyConnectionBridge::TransformDisplayOutput(
    std::wstring_view output) {
    display_pending_.append(output);
    std::wstring rendered;
    rendered.reserve(display_pending_.size());
    const bool light = light_theme_.load(std::memory_order_relaxed);

    size_t position = 0;
    while (position < display_pending_.size()) {
        const size_t escape = display_pending_.find(L'\x1b', position);
        if (escape == std::wstring::npos) {
            rendered.append(display_pending_, position, std::wstring::npos);
            position = display_pending_.size();
            break;
        }
        rendered.append(display_pending_, position, escape - position);
        if (escape + 1 >= display_pending_.size()) {
            position = escape;
            break;
        }
        if (display_pending_[escape + 1] != L'[') {
            rendered.push_back(display_pending_[escape]);
            position = escape + 1;
            continue;
        }

        size_t final = escape + 2;
        while (final < display_pending_.size() &&
               !(display_pending_[final] >= 0x40 && display_pending_[final] <= 0x7e)) {
            ++final;
        }
        if (final >= display_pending_.size()) {
            position = escape;
            break;
        }

        const std::wstring_view sequence{
            display_pending_.data() + escape, final - escape + 1};
        if (light && sequence == kCodexInputBackground) {
            rendered.append(kLightInputBackground);
        } else if (!light && sequence == kLightInputBackground) {
            rendered.append(kCodexInputBackground);
        } else {
            rendered.append(sequence);
        }
        position = final + 1;
    }

    if (position > 0) display_pending_.erase(0, position);
    return rendered;
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
