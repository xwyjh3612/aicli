#include "app/auto_update.h"
#include "app/update_manifest.h"
#include "app/update_path.h"
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
bool dialog_open{};
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

HRESULT CALLBACK ProgressCallback(HWND window, UINT notification, WPARAM button, LPARAM, LONG_PTR context) {
    auto& job = *reinterpret_cast<Job*>(context);
    if (notification == TDN_TIMER) {
        SendMessageW(window, TDM_SET_PROGRESS_BAR_POS, job.progress.load(), 0);
        if (Ready(job)) SendMessageW(window, TDM_CLICK_BUTTON, IDOK, 0);
    } else if (notification == TDN_BUTTON_CLICKED && button == IDCANCEL) {
        job.cancelled = true;
        SendMessageW(window, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(L"正在取消，请稍候…"));
        return Ready(job) ? S_OK : S_FALSE;
    }
    return S_OK;
}

void Progress(HWND owner, Job& job, const wchar_t* title) {
    TASKDIALOG_BUTTON done{IDOK, L"完成"};
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = owner;
    config.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"AiCli 更新";
    config.pszMainInstruction = title;
    config.pszContent = L"请稍候，可以随时取消。";
    config.cButtons = 1;
    config.pButtons = &done;
    config.pfCallback = ProgressCallback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&job);
    // Disable the synthetic completion button until the worker finishes.
    config.pfCallback = [](HWND window, UINT notification, WPARAM w, LPARAM l, LONG_PTR data) -> HRESULT {
        if (notification == TDN_CREATED) SendMessageW(window, TDM_ENABLE_BUTTON, IDOK, FALSE);
        if (notification == TDN_TIMER && Ready(*reinterpret_cast<Job*>(data)))
            SendMessageW(window, TDM_ENABLE_BUTTON, IDOK, TRUE);
        return ProgressCallback(window, notification, w, l, data);
    };
    if (FAILED(TaskDialogIndirect(&config, nullptr, nullptr, nullptr))) job.cancelled = true;
    job.worker.get();
}

void DownloadAndInstall(HWND owner, const Release& release) {
    Job download;
    download.release = release;
    download.worker = std::async(std::launch::async, [&] { Download(download); });
    Progress(owner, download, L"正在下载更新并校验安装包");
    if (download.cancelled) return;
    if (!download.error.empty()) { MessageBoxW(owner, download.error.c_str(), L"更新失败", MB_OK | MB_ICONERROR); return; }
    if (MessageBoxW(owner, L"更新已下载并通过 SHA-256 校验。\n\n安装会关闭 AiCli 和正在运行的终端会话，请先保存工作。\n现在打开安装程序吗？",
                    L"安装 AiCli 更新", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    // Inno Setup retains the installed directory and performs the existing
    // shutdown/registration flow. Cancelling UAC or Setup leaves AiCli running.
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.hwnd = owner;
    launch.lpVerb = L"open";
    launch.lpFile = download.file.c_str();
    launch.lpParameters = L"/NORESTART";
    const auto directory = download.file.parent_path().wstring();
    launch.lpDirectory = directory.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch)) {
        const auto code = GetLastError();
        if (code == ERROR_CANCELLED) return;
        const auto message = L"安装程序未能启动（Windows 错误 " + std::to_wstring(code) +
            L"）。安装包已保留，可手动打开：\n\n" + download.file.wstring();
        MessageBoxW(owner, message.c_str(), L"AiCli 更新", MB_OK | MB_ICONINFORMATION);
    }
}

void Offer(HWND owner, const Release& release) {
    if (!IsNewerVersion(release.version, CurrentVersion())) return;
    const auto prompt = L"发现新版本 " + Wide(release.version) + L"，当前版本 " + Wide(CurrentVersion()) +
        L"。\n\n是否下载更新？下载完成后会再次确认安装。";
    if (MessageBoxW(owner, prompt.c_str(), L"AiCli 有可用更新", MB_YESNO | MB_ICONINFORMATION) == IDYES)
        DownloadAndInstall(owner, release);
}

LRESULT CALLBACK TimerProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message != WM_TIMER) return DefWindowProcW(window, message, w, l);
    if (dialog_open || !IsWindow(main_window)) return 0;
    if (manual_check && Ready(*manual_check)) {
        auto job = std::move(manual_check);
        job->worker.get();
        if (!job->error.empty()) {
            check_status = {CheckState::Failed, L"暂时无法检查更新，请检查网络后重试。"};
        } else if (IsNewerVersion(job->release.version, CurrentVersion())) {
            available_release = std::move(job->release);
            check_status = {CheckState::Available,
                L"发现新版本 " + Wide(available_release->version) + L"，可以下载更新。"};
        } else {
            check_status = {CheckState::UpToDate, L"当前已是最新版本。"};
        }
    }
    if (automatic_check && Ready(*automatic_check) && IsWindowVisible(main_window) &&
        !IsIconic(main_window) && IsWindowEnabled(main_window)) {
        auto job = std::move(automatic_check);
        job->worker.get();
        dialog_open = true;
        if (job->error.empty() && !job->cancelled) Offer(main_window, job->release);
        dialog_open = false;
    }
    if (!automatic_check && !manual_check && GetTickCount64() >= next_check) {
        next_check = GetTickCount64() + kCheckInterval;
        automatic_check = std::make_unique<Job>();
        auto* job = automatic_check.get();
        job->worker = std::async(std::launch::async, [job] { Check(*job); });
    }
    return 0;
}
}

void Start(HWND window) {
    main_window = window;
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
    available_release.reset();
    check_status = {};
}

void CheckForUpdates() {
    if (dialog_open || manual_check) return;
    if (automatic_check) automatic_check->cancelled = true;
    available_release.reset();
    check_status = {CheckState::Checking, L"正在检查更新，请稍候…"};
    manual_check = std::make_unique<Job>();
    auto* job = manual_check.get();
    job->worker = std::async(std::launch::async, [job] { Check(*job); });
    next_check = GetTickCount64() + kCheckInterval;
}

CheckStatus GetCheckStatus() { return check_status; }

void DownloadAvailableUpdate(HWND owner) {
    if (dialog_open || manual_check || !available_release) return;
    dialog_open = true;
    const auto release = *available_release;
    DownloadAndInstall(owner, release);
    dialog_open = false;
}
}
