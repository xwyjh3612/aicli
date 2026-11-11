#include "terminal/session_process_job.h"
#include "terminal/conpty_connection_bridge.h"
#include "app/powershell_runtime.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using aicli::terminal::SessionProcessJob;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Process {
    HANDLE handle{};
    DWORD id{};
    Process() = default;
    Process(HANDLE value, DWORD pid) : handle(value), id(pid) {}
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&& other) noexcept : handle(other.handle), id(other.id) { other.handle = nullptr; }
    ~Process() {
        if (handle) {
            // Every instance is a process launched/opened by this isolated test.
            if (WaitForSingleObject(handle, 0) == WAIT_TIMEOUT) {
                TerminateProcess(handle, 99);
                WaitForSingleObject(handle, 5000);
            }
            CloseHandle(handle);
        }
    }
    bool Exited(DWORD timeout = 5000) const { return WaitForSingleObject(handle, timeout) == WAIT_OBJECT_0; }
};

fs::path Executable() {
    wchar_t path[32768]{};
    Require(GetModuleFileNameW(nullptr, path, 32768) != 0, "Get executable path");
    return path;
}

std::wstring Quote(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }

fs::path HelperExecutable() {
    const auto executable = Executable();
    return executable.filename() == L"aicli.exe" ? executable.parent_path() / L"helper.exe" : executable;
}

Process Spawn(const fs::path& executable, const std::wstring& arguments, SessionProcessJob* job = nullptr,
              const fs::path& output = {}) {
    auto command = Quote(executable) + L" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    HANDLE log = INVALID_HANDLE_VALUE;
    HANDLE input = INVALID_HANDLE_VALUE;
    if (!output.empty()) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        log = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(log != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE, "Create helper log handles");
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = startup.hStdError = log;
        startup.hStdInput = input;
    }
    PROCESS_INFORMATION info{};
    const auto created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, !output.empty(),
                           CREATE_NO_WINDOW | (job ? CREATE_SUSPENDED : 0), nullptr, nullptr,
                           &startup, &info);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    Require(created != FALSE, "Create isolated helper process");
    Process process(info.hProcess, info.dwProcessId);
    if (job) {
        const bool assigned = job->Assign(info.hProcess);
        if (!assigned) { CloseHandle(info.hThread); Require(false, "Assign suspended shell to job"); }
        const auto resumed = ResumeThread(info.hThread);
        CloseHandle(info.hThread);
        Require(resumed != static_cast<DWORD>(-1), "Resume owned shell");
    } else {
        CloseHandle(info.hThread);
    }
    return process;
}

void Signal(const fs::path& directory, const char* name, DWORD value = GetCurrentProcessId()) {
    std::ofstream file(directory / name);
    file << value;
    Require(static_cast<bool>(file), "Write helper signal");
}

DWORD AwaitSignal(const fs::path& directory, const char* name) {
    const auto deadline = GetTickCount64() + 10000;
    do {
        std::ifstream file(directory / name);
        DWORD value{};
        if (file >> value) return value;
        Sleep(20);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error(std::string("Timed out awaiting ") + name);
}

Process OpenHelper(DWORD id) {
    HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
    Require(handle != nullptr, "Open isolated descendant process");
    return {handle, id};
}

bool LockAvailable(const fs::path& directory) {
    HANDLE lock = CreateFileW((directory / "session.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return false;
    CloseHandle(lock);
    return true;
}

std::wstring PowerShellLiteral(const fs::path& path) {
    std::wstring result = L"'";
    for (const auto c : path.wstring()) {
        result += c;
        if (c == L'\'') result += c;
    }
    return result + L"'";
}

auto StartBridge(const fs::path& directory) {
    auto connection = winrt::make_self<aicli::terminal::ConPtyConnectionBridge>(
        directory.wstring(),
        L"[IO.File]::WriteAllText(" + PowerShellLiteral(directory / "shell.pid") + L", [string]$PID); "
        L"$nodeProbe='not-installed'; if (Get-Command node -ErrorAction SilentlyContinue) { "
        L"$nodeProbe = & node -e \"process.stdout.write('node-runtime-ok')\" }; "
        L"[IO.File]::WriteAllText(" + PowerShellLiteral(directory / "node.result") + L", [string]$nodeProbe); & " +
            PowerShellLiteral(directory.parent_path() / L"helper.exe") + L" --root " + PowerShellLiteral(directory));
    connection->Start();
    Require(connection->State() == winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Connected,
            "Real ConPTY shell starts with process ownership");
    return connection;
}

int Helper(int argc, wchar_t** argv) {
    const std::wstring mode = argv[1];
    const fs::path directory = argv[2];
    if (mode == L"--leaf") {
        HANDLE lock = CreateFileW((directory / "session.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(lock != INVALID_HANDLE_VALUE, "Acquire simulated Codex session lock");
        Signal(directory, "leaf.pid");
        Sleep(INFINITE);
    } else if (mode == L"--branch") {
        auto leaf = Spawn(HelperExecutable(), L"--leaf " + Quote(directory));
        Signal(directory, "branch.pid");
        Sleep(INFINITE);
    } else if (mode == L"--root" || mode == L"--installer-root") {
        auto branch = Spawn(HelperExecutable(), L"--branch " + Quote(directory));
        Signal(directory, "root.pid");
        if (mode == L"--installer-root") {
            Require(argc == 5, "Installer root helper arguments");
            auto updater = Spawn(HelperExecutable(), L"--updater " + Quote(directory) + L" " + Quote(argv[3]) + L" " + Quote(argv[4]));
            Sleep(INFINITE);
        }
        Sleep(INFINITE);
    } else if (mode == L"--job-owner") {
        SessionProcessJob job;
        Require(job.Initialize(), "Initialize owner session job");
        auto root = Spawn(Executable(), L"--root " + Quote(directory), &job);
        Signal(directory, "owner.ready");
        Sleep(INFINITE);
    } else if (mode == L"--bridge-owner") {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto connection = StartBridge(directory);
        Signal(directory, "owner.ready");
        Sleep(INFINITE);
    } else if (mode == L"--updater") {
        Require(argc == 5, "Updater helper arguments");
        Signal(directory, "updater.pid");
        // The test runner opens all descendant handles before allowing cleanup.
        AwaitSignal(directory, "cleanup.go");
        wchar_t system[MAX_PATH]{};
        Require(GetSystemDirectoryW(system, MAX_PATH) != 0, "Get PowerShell directory");
        auto script = Spawn(fs::path(system) / "WindowsPowerShell/v1.0/powershell.exe",
            L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + Quote(argv[3]) +
            L" -InstallDirectory " + Quote(argv[4]) + L" -IncludeProcessTrees", nullptr, directory / "cleanup.log");
        Require(script.Exited(20000), "Installer cleanup finishes");
        DWORD code{};
        GetExitCodeProcess(script.handle, &code);
        Signal(directory, "cleanup.result", code);
        // A successful write proves both the installer and its calling process
        // survived termination of their AiCli ancestor.
        return static_cast<int>(code);
    }
    return 1;
}

struct Tree {
    Process root;
    Process branch;
    Process leaf;
    Tree(Process process, const fs::path& directory) : root(std::move(process)),
        branch(OpenHelper(AwaitSignal(directory, "branch.pid"))),
        leaf(OpenHelper(AwaitSignal(directory, "leaf.pid"))) {
        Require(!LockAvailable(directory), "Running grandchild holds the session lock");
    }
    void RequireClosed(const fs::path& directory) {
        const bool root_exited = root.Exited();
        const bool branch_exited = branch.Exited();
        const bool leaf_exited = leaf.Exited();
        if (!root_exited || !branch_exited || !leaf_exited)
            std::cerr << "Exit states root/branch/leaf: " << root_exited << '/' << branch_exited << '/' << leaf_exited << '\n';
        Require(root_exited && branch_exited && leaf_exited, "Entire terminal process tree exits");
        Require(LockAvailable(directory), "Session lock released for the next launch");
    }
};

void Run(const fs::path& base, const fs::path& cleanup_script) {
    const auto make_directory = [&](const wchar_t* name) {
        const auto directory = base / name;
        fs::create_directories(directory);
        return directory;
    };
    const auto unrelated_directory = make_directory(L"unrelated");
    auto unrelated = Spawn(Executable(), L"--leaf " + Quote(unrelated_directory));
    AwaitSignal(unrelated_directory, "leaf.pid");

    const auto normal = make_directory(L"normal");
    const auto independent = make_directory(L"independent");
    SessionProcessJob first;
    SessionProcessJob second;
    Require(first.Initialize() && second.Initialize(), "Create independent terminal jobs");
    Tree first_tree(Spawn(Executable(), L"--root " + Quote(normal), &first), normal);
    Tree second_tree(Spawn(Executable(), L"--root " + Quote(independent), &second), independent);
    Require(first.TerminateAndWait(), "Normal terminal close completes synchronously");
    first.Reset();
    first_tree.RequireClosed(normal);
    Require(!second_tree.leaf.Exited(0) && !LockAvailable(independent), "Closing one tab preserves the other tab");
    // Closing the last non-inherited handle alone must also kill descendants.
    second.Reset();
    second_tree.RequireClosed(independent);
    std::cout << "Normal close and independent terminal jobs passed\n";

    for (bool nested : {false, true}) {
        const auto forced = make_directory(nested ? L"nested-forced" : L"forced");
        SessionProcessJob outer;
        if (nested) Require(outer.Initialize(), "Initialize inherited outer job");
        auto owner = Spawn(Executable(), L"--job-owner " + Quote(forced), nested ? &outer : nullptr);
        AwaitSignal(forced, "owner.ready");
        Tree tree(OpenHelper(AwaitSignal(forced, "root.pid")), forced);
        Require(TerminateProcess(owner.handle, 0) != FALSE, "Simulate installer forcing AiCli to exit");
        Require(owner.Exited(), "Forced owner exits");
        tree.RequireClosed(forced);
    }
    std::cout << "Forced application exit and nested job ownership passed\n";

    const auto conpty = make_directory(L"conpty");
    // Real CLIs live outside AiCli's sparse package directory. A helper inside
    // that directory would activate a different MSIX package when launched by
    // Store PowerShell, which deliberately breaks Windows job inheritance.
    fs::copy_file(Executable(), base / "helper.exe");
    fs::copy_file(Executable().parent_path() / "conpty.dll", base / "conpty.dll");
    auto connection = StartBridge(conpty);
    auto shell = OpenHelper(AwaitSignal(conpty, "shell.pid"));
    Tree conpty_tree(OpenHelper(AwaitSignal(conpty, "root.pid")), conpty);
    std::ifstream node_result(conpty / "node.result");
    std::string node_probe;
    node_result >> node_probe;
    Require(node_probe == "node-runtime-ok" || node_probe == "not-installed", "Node runtime remains executable from the owned shell");
    BOOL in_job{};
    IsProcessInJob(conpty_tree.root.handle, nullptr, &in_job);
    Require(in_job != FALSE, "Store PowerShell descendants remain in the owning job");
    connection->Close();
    Require(shell.Exited(), "ConPTY PowerShell shell exits");
    conpty_tree.RequireClosed(conpty);
    const auto forced_conpty = make_directory(L"forced-conpty");
    auto bridge_owner = Spawn(Executable(), L"--bridge-owner " + Quote(forced_conpty));
    AwaitSignal(forced_conpty, "owner.ready");
    auto forced_shell = OpenHelper(AwaitSignal(forced_conpty, "shell.pid"));
    Tree forced_tree(OpenHelper(AwaitSignal(forced_conpty, "root.pid")), forced_conpty);
    Require(TerminateProcess(bridge_owner.handle, 0) != FALSE && bridge_owner.Exited(), "Force exit real ConPTY owner");
    Require(forced_shell.Exited(), "Forced app exit releases real PowerShell shell");
    forced_tree.RequireClosed(forced_conpty);
    std::cout << "Real ConPTY/PowerShell normal and forced cleanup passed\n";

    aicli::app::ActivateWindowsPowerShellFallback();
    const auto fallback = make_directory(L"windows-powershell");
    auto fallback_connection = StartBridge(fallback);
    auto fallback_shell = OpenHelper(AwaitSignal(fallback, "shell.pid"));
    Tree fallback_tree(OpenHelper(AwaitSignal(fallback, "root.pid")), fallback);
    fallback_connection->Close();
    Require(fallback_shell.Exited(), "Windows PowerShell 5.1 shell exits");
    fallback_tree.RequireClosed(fallback);
    std::cout << "Windows PowerShell 5.1 fallback cleanup passed\n";

    Require(fs::is_regular_file(cleanup_script), "Legacy installer cleanup script exists");
    const auto install = make_directory(L"legacy-install");
    const auto legacy_state = make_directory(L"legacy-state");
    fs::copy_file(Executable(), install / "aicli.exe");
    fs::copy_file(Executable(), install / "helper.exe");
    fs::copy_file(Executable().parent_path() / "conpty.dll", install / "conpty.dll");
    Tree legacy(Spawn(install / "aicli.exe", L"--installer-root " + Quote(legacy_state) +
                      L" " + Quote(cleanup_script) + L" " + Quote(install)), legacy_state);
    auto updater = OpenHelper(AwaitSignal(legacy_state, "updater.pid"));
    // Same filenames in a different install path must survive cleanup.
    const auto other_install = make_directory(L"other-install");
    const auto other_state = make_directory(L"other-state");
    fs::copy_file(Executable(), other_install / "aicli.exe");
    fs::copy_file(Executable(), other_install / "helper.exe");
    fs::copy_file(Executable().parent_path() / "conpty.dll", other_install / "conpty.dll");
    Tree other(Spawn(other_install / "aicli.exe", L"--root " + Quote(other_state)), other_state);
    Signal(legacy_state, "cleanup.go", 1);
    Require(AwaitSignal(legacy_state, "cleanup.result") == 0, "Legacy installer cleanup succeeds");
    Require(updater.Exited(), "Updater survives to report completion");
    legacy.RequireClosed(legacy_state);
    Require(!other.root.Exited(0) && !other.leaf.Exited(0) && !LockAvailable(other_state),
            "Other install path and its CLI remain running");
    Require(!unrelated.Exited(0) && !LockAvailable(unrelated_directory), "Unrelated CLI remains running");
    std::cout << "Legacy upgrade cleanup, protected updater and unrelated processes passed\n";
}
}

int wmain(int argc, wchar_t** argv) {
    fs::path base;
    try {
        if (argc >= 3 && std::wstring(argv[1]).starts_with(L"--")) return Helper(argc, argv);
        Require(argc == 2, "Pass close-running-terminals.ps1 path");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto temp_root = fs::absolute(fs::temp_directory_path()).lexically_normal();
        if (temp_root.filename().empty()) temp_root = temp_root.parent_path();
        base = temp_root / (L"aicli-session-tests-" + std::to_wstring(GetCurrentProcessId()) +
                            L"-" + std::to_wstring(GetTickCount64()));
        Require(base.parent_path() == temp_root, "Test directory stays within the temporary workspace");
        fs::create_directory(base);
        Run(base, fs::absolute(argv[1]));
        fs::remove_all(base);
        std::cout << "Session process lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (!base.empty()) std::wcerr << L"Test artifacts: " << base.wstring() << L'\n';
        return 1;
    }
}
