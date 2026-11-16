#include "app/auto_update.h"
#include "app/update_manifest.h"
#include "app/update_path.h"
#include "app/auto_update_testing.h"
#include "aicli_build_identity.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace aicli::update {
namespace {
using namespace std::chrono_literals;
constexpr UINT_PTR kPollTimer = 1;
constexpr ULONGLONG kCheckInterval = 6ULL * 60 * 60 * 1000;
HWND main_window{}, timer_window{};
ULONGLONG next_check{};
InstallCallbacks install_callbacks;
std::filesystem::path verified_installer;
HANDLE installer_process{};
ULONGLONG idle_since{};
constexpr ULONGLONG kIdleBeforeInstall = 3000;
#ifdef AICLI_UPDATE_TESTING
testing::Services test_services;
#endif
struct Job {
    std::atomic<bool> cancelled{false};
    std::atomic<int> progress{0};
    Release release;
    std::filesystem::path file;
    std::wstring error;
    std::future<void> worker;
};
std::unique_ptr<Job> automatic_check;
std::unique_ptr<Job> manual_check;
std::unique_ptr<Job> download_job;
CheckStatus check_status;
std::optional<Release> available_release;

std::wstring Wide(const std::string& text) { return {text.begin(), text.end()}; }
std::string CurrentVersion() {
    const std::wstring value = AICLI_VERSION;
    std::string version;
    for (const auto character : value) version += static_cast<char>(character);
    return version;
}

struct HttpHandle {
    HINTERNET value{};
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    operator HINTERNET() const { return value; }
};

void Fetch(const std::wstring& url, std::uint64_t limit, Job& job,
           const std::function<void(const char*, DWORD)>& consume) {
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        throw std::runtime_error("Invalid HTTPS URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HttpHandle session{WinHttpOpen(L"AiCli-Updater/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) throw std::runtime_error("Cannot create HTTP session");
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);
    HttpHandle connection{WinHttpConnect(session, host.c_str(), parts.nPort, 0)};
    if (!connection) throw std::runtime_error("Cannot connect");
    HttpHandle request{WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request) throw std::runtime_error("Cannot create request");
    // Keep certificate verification enabled and forbid HTTPS-to-HTTP redirects.
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)))
        throw std::runtime_error("Cannot set redirect policy");
    if (job.cancelled || !WinHttpSendRequest(request, L"Cache-Control: no-cache\r\n", static_cast<DWORD>(-1),
                                           nullptr, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr))
        throw std::runtime_error("Request failed");
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
        throw std::runtime_error("Server returned an error");
    char buffer[64 * 1024];
    std::uint64_t total{};
    const auto deadline = std::chrono::steady_clock::now() + 20min;
    for (;;) {
        if (job.cancelled || std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("Download cancelled or timed out");
        DWORD count{};
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &count)) throw std::runtime_error("Download failed");
        if (!count) break;
        total += count;
        if (total > limit) throw std::runtime_error("Download too large");
        consume(buffer, count);
        job.progress = static_cast<int>(total * 100 / limit);
    }
}

void Check(Job& job) {
    try {
#ifdef AICLI_UPDATE_TESTING
        if (test_services.check) { job.release = test_services.check(); return; }
#endif
        std::string text;
        Fetch(kManifestUrl, 64 * 1024, job, [&](const char* data, DWORD count) { text.append(data, count); });
        job.release = ParseDesktopRelease(text);
    } catch (...) {
        job.error = L"无法检查更新，请检查网络后重试。更新源可能暂时不可用。";
    }
}

void Download(Job& job) {
    std::filesystem::path directory;
    try {
#ifdef AICLI_UPDATE_TESTING
        if (test_services.download) {
            job.file = test_services.download(job.cancelled, job.progress);
            if (job.file.empty() && !job.cancelled) throw std::runtime_error("Download failed");
            return;
        }
#endif
        PWSTR local{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
            throw std::runtime_error("Cannot locate local app data");
        std::filesystem::path root(local);
        CoTaskMemFree(local);
        root /= L"AiCli/updates";
        std::filesystem::create_directories(root);
        // Remove only old updater-created directories, never a live download.
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            std::error_code error;
            if (entry.is_directory() && !entry.is_symlink() &&
                entry.path().filename().wstring().starts_with(L"update-") &&
                std::filesystem::file_time_type::clock::now() - entry.last_write_time() > 48h)
                std::filesystem::remove_all(entry.path(), error);
        }
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Cannot create update directory");
        wchar_t name[40]{};
        StringFromGUID2(guid, name, 40);
        directory = root / (std::wstring(L"update-") + name);
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Cannot create update directory");
        const auto partial = directory / L"installer.part";
        std::ofstream output(partial, std::ios::binary);
        if (!output) throw std::runtime_error("Cannot open download file");
        struct Hash {
            BCRYPT_ALG_HANDLE algorithm{};
            BCRYPT_HASH_HANDLE hash{};
            ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
        } hash;
        if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptCreateHash(hash.algorithm, &hash.hash, nullptr, 0, nullptr, 0, 0) < 0)
            throw std::runtime_error("Cannot initialize SHA256");
        std::uint64_t total{};
        Fetch(Wide(job.release.url), job.release.size, job, [&](const char* data, DWORD count) {
            output.write(data, count);
            if (!output || BCryptHashData(hash.hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), count, 0) < 0)
                throw std::runtime_error("Cannot save download");
            total += count;
        });
        output.close();
        if (!output || total != job.release.size || job.cancelled) throw std::runtime_error("Incomplete download");
        unsigned char digest[32]{};
        if (BCryptFinishHash(hash.hash, digest, sizeof(digest), 0) < 0) throw std::runtime_error("Cannot hash download");
        std::string hex;
        for (const auto byte : digest) { hex += "0123456789abcdef"[byte >> 4]; hex += "0123456789abcdef"[byte & 15]; }
        if (hex != job.release.sha256) throw std::runtime_error("Checksum mismatch");
        job.file = directory / L"AiCli-Setup.exe";
        std::filesystem::rename(partial, job.file);
        // An elevated installer cannot see a packaged app's logical AppData
        // overlay. Launch the physical file, including on existing packages.
        job.file = PhysicalInstallerPath(job.file);
    } catch (...) {
        job.file.clear();
        job.error = L"更新下载或完整性校验失败，未运行安装程序。请稍后重试。";
        if (!directory.empty()) { std::error_code error; std::filesystem::remove_all(directory, error); }
    }
}

bool Ready(Job& job) { return job.worker.wait_for(0ms) == std::future_status::ready; }

void SetStatus(CheckState state, std::wstring message, int progress = 0) {
    CheckStatus status{state, std::move(message), available_release.has_value(), progress};
    if (status == check_status) return;
    check_status = std::move(status);
    if (IsWindow(main_window)) PostMessageW(main_window, kStatusChangedMessage, 0, 0);
}

bool TransferActive() {
    return download_job || !verified_installer.empty() || installer_process ||
           check_status.state == CheckState::Installing;
}

std::filesystem::path InstallationDirectory() {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error("Cannot find installation");
    return PhysicalInstallerPath(std::filesystem::path(std::wstring(path.data(), length))).parent_path();
}

void InstallWhenIdle() {
    if (verified_installer.empty() || installer_process ||
        check_status.state == CheckState::Installing) return;
    if (install_callbacks.can_install && !install_callbacks.can_install()) {
        idle_since = 0;
        SetStatus(CheckState::WaitingToInstall, L"更新已下载，等待当前任务完成后自动安装。可取消等待。");
        return;
    }
    if (!idle_since) idle_since = GetTickCount64();
    if (GetTickCount64() - idle_since < kIdleBeforeInstall) {
        SetStatus(CheckState::WaitingToInstall, L"更新已下载，即将自动安装并恢复已启动项目。可取消等待。");
        return;
    }
    idle_since = 0;
    try {
        const auto parameters = L"/SILENT /NOCANCEL /NORESTART /SP- /AICLIUPDATE /DIR=\"" +
            InstallationDirectory().wstring() + L"\"";
        if (install_callbacks.prepare_restart &&
            !install_callbacks.prepare_restart(Wide(available_release->version)))
            throw std::runtime_error("Cannot checkpoint running projects");
        SetStatus(CheckState::Installing, L"正在安装更新，完成后自动打开并恢复已启动项目。");
        bool launched = false;
#ifdef AICLI_UPDATE_TESTING
        if (test_services.launch) launched = test_services.launch(verified_installer, parameters);
        else
#endif
        {
            SHELLEXECUTEINFOW launch{sizeof(launch)};
            launch.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
            launch.hwnd = main_window;
            launch.lpVerb = L"open";
            launch.lpFile = verified_installer.c_str();
            launch.lpParameters = parameters.c_str();
            const auto directory = verified_installer.parent_path().wstring();
            launch.lpDirectory = directory.c_str();
            launch.nShow = SW_SHOWNORMAL;
            launched = ShellExecuteExW(&launch) != FALSE;
            if (launched) installer_process = launch.hProcess;
        }
        if (!launched) throw std::runtime_error("Installer launch cancelled or failed");
        verified_installer.clear();
    } catch (...) {
        if (install_callbacks.cancel_restart) install_callbacks.cancel_restart();
        // Retain a verified file for retry, including after UAC is cancelled.
        SetStatus(CheckState::InstallFailed, L"安装未启动。可重试安装，或取消后稍后更新。");
    }
}

void CompleteCheck(std::unique_ptr<Job>& pending, bool manual) {
    if (!pending || !Ready(*pending)) return;
    auto job = std::move(pending);
    job->worker.get();
    if (job->cancelled) return;
    if (!job->error.empty()) {
        if (manual) SetStatus(CheckState::Failed, L"暂时无法检查更新，请检查网络后重试。");
        return;
    }
    if (IsNewerVersion(job->release.version, CurrentVersion())) {
        available_release = std::move(job->release);
        SetStatus(CheckState::Available,
            L"发现新版本 " + Wide(available_release->version) + L"，可以下载更新。");
    } else {
        available_release.reset();
        SetStatus(CheckState::UpToDate, L"当前已是最新版本。");
    }
}

void BeginAutomaticCheck() {
    if (TransferActive() || automatic_check || manual_check) return;
    next_check = GetTickCount64() + kCheckInterval;
    automatic_check = std::make_unique<Job>();
    auto* job = automatic_check.get();
    job->worker = std::async(std::launch::async, [job] { Check(*job); });
}

void Poll() {
    if (!IsWindow(main_window)) return;
    CompleteCheck(automatic_check, false);
    CompleteCheck(manual_check, true);
    if (download_job) {
        if (!Ready(*download_job)) {
            SetStatus(CheckState::Downloading,
                download_job->cancelled ? L"正在取消下载…" : L"正在下载并校验更新…",
                download_job->progress.load());
        } else {
            auto job = std::move(download_job);
            job->worker.get();
            if (job->cancelled) {
                SetStatus(CheckState::Available, L"已取消下载，可稍后重新下载更新。");
            } else if (!job->error.empty() || job->file.empty()) {
                SetStatus(CheckState::DownloadFailed, L"下载或校验失败，未安装更新。请重试。");
            } else {
                verified_installer = std::move(job->file);
                SetStatus(CheckState::WaitingToInstall, L"更新已下载，等待当前任务完成后自动安装。");
            }
        }
    }
    if (installer_process && WaitForSingleObject(installer_process, 0) == WAIT_OBJECT_0) {
        DWORD exit_code{};
        GetExitCodeProcess(installer_process, &exit_code);
        CloseHandle(installer_process);
        installer_process = nullptr;
        if (install_callbacks.cancel_restart) install_callbacks.cancel_restart();
        SetStatus(CheckState::InstallFailed,
            exit_code ? L"安装未完成，当前项目仍可使用。请重试更新。"
                      : L"安装程序已结束，请重新打开 AiCli 确认版本。");
    }
    if (check_status.state == CheckState::WaitingToInstall) InstallWhenIdle();
    if (!TransferActive() && GetTickCount64() >= next_check) BeginAutomaticCheck();
}

LRESULT CALLBACK TimerProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_TIMER) { Poll(); return 0; }
    return DefWindowProcW(window, message, w, l);
}
}

void Start(HWND window, InstallCallbacks callbacks) {
    Stop();
    main_window = window;
    install_callbacks = std::move(callbacks);
    WNDCLASSW type{};
    type.lpfnWndProc = TimerProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"AiCliUpdateTimer";
    RegisterClassW(&type);
    timer_window = CreateWindowW(type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, nullptr);
    next_check = GetTickCount64() + 10000;
    if (timer_window) SetTimer(timer_window, kPollTimer, 500, nullptr);
}

void Stop() {
    if (timer_window) DestroyWindow(timer_window);
    timer_window = main_window = nullptr;
    if (automatic_check) { automatic_check->cancelled = true; automatic_check.reset(); }
    if (manual_check) { manual_check->cancelled = true; manual_check.reset(); }
    if (download_job) { download_job->cancelled = true; download_job.reset(); }
    if (installer_process) CloseHandle(installer_process);
    installer_process = nullptr;
    verified_installer.clear();
    idle_since = 0;
    install_callbacks = {};
    available_release.reset();
    check_status = {};
}

void CheckForUpdates() {
    if (TransferActive() || manual_check) return;
    if (automatic_check) automatic_check->cancelled = true;
    SetStatus(CheckState::Checking, L"正在检查更新，请稍候…");
    manual_check = std::make_unique<Job>();
    auto* job = manual_check.get();
    job->worker = std::async(std::launch::async, [job] { Check(*job); });
    next_check = GetTickCount64() + kCheckInterval;
}

CheckStatus GetCheckStatus() { return check_status; }
bool HasAvailableUpdate() { return available_release.has_value(); }
bool IsInstalling() { return check_status.state == CheckState::Installing; }

void DownloadAvailableUpdate(HWND) {
    if (download_job || manual_check || IsInstalling() || !available_release) return;
    if (automatic_check) automatic_check->cancelled = true;
    if (!verified_installer.empty()) {
        idle_since = 0;
        SetStatus(CheckState::WaitingToInstall, L"即将重试安装…");
        return;
    }
    SetStatus(CheckState::Downloading, L"正在下载并校验更新…");
    download_job = std::make_unique<Job>();
    download_job->release = *available_release;
    auto* job = download_job.get();
    job->worker = std::async(std::launch::async, [job] { Download(*job); });
}

void CancelDownloadOrInstall() {
    if (IsInstalling()) return;
    if (download_job) { download_job->cancelled = true; return; }
    if (!verified_installer.empty()) {
        verified_installer.clear();
        idle_since = 0;
        SetStatus(CheckState::Available, L"已取消等待，可稍后重新下载更新。");
    }
}

#ifdef AICLI_UPDATE_TESTING
namespace testing {
void SetServices(Services services) { test_services = std::move(services); }
void BeginAutomaticCheck() { ::aicli::update::BeginAutomaticCheck(); }
void Poll() { ::aicli::update::Poll(); }
}
#endif
}
