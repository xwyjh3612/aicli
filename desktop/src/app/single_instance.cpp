#include "app/single_instance.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <string_view>
#include <vector>

namespace aicli::app {
namespace {

constexpr DWORD kActivationRetryMilliseconds = 50;
constexpr int kActivationRetryCount = 60;

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
            iswspace(message.back()))) {
        message.pop_back();
    }
    return message;
}

std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) return {};
        if (length < buffer.size() - 1 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return std::wstring(buffer.data(), length);
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(std::min<size_t>(32768, buffer.size() * 2));
    }
}

std::wstring NormalizeExecutablePath(std::wstring path) {
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    std::transform(path.begin(), path.end(), path.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });
    return path;
}

std::uint64_t HashExecutablePath(std::wstring_view path) {
    // FNV-1a over UTF-16 code units is stable across processes and does not
    // depend on the standard library implementation.
    std::uint64_t hash = 14695981039346656037ull;
    for (const wchar_t value : path) {
        const auto unit = static_cast<std::uint16_t>(value);
        hash ^= static_cast<std::uint8_t>(unit & 0xffu);
        hash *= 1099511628211ull;
        hash ^= static_cast<std::uint8_t>((unit >> 8u) & 0xffu);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::wstring MutexName(std::wstring_view executable_path) {
    std::array<wchar_t, 64> name{};
    swprintf_s(name.data(), name.size(), L"Local\\AiCli.SingleInstance.%016llX",
               static_cast<unsigned long long>(HashExecutablePath(executable_path)));
    return name.data();
}

std::wstring ProcessExecutablePath(DWORD process_id) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                       process_id);
    if (!process) return {};

    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    const BOOL queried = QueryFullProcessImageNameW(
        process, 0, buffer.data(), &length);
    CloseHandle(process);
    if (!queried) return {};
    return NormalizeExecutablePath(std::wstring(buffer.data(), length));
}

struct WindowSearch {
    std::wstring executable_path;
    DWORD current_process_id{};
    HWND best_window{};
    int best_score{-1};
};

BOOL CALLBACK FindWindowCallback(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (!process_id || process_id == search.current_process_id ||
        ProcessExecutablePath(process_id) != search.executable_path) {
        return TRUE;
    }

    int score = 0;
    if (IsWindowVisible(window)) score += 100;
    if (!GetWindow(window, GW_OWNER)) score += 50;
    if (!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) score += 20;
    if (GetWindowTextLengthW(window) > 0) score += 10;
    if (IsIconic(window)) score += 5;
    if (score > search.best_score) {
        search.best_score = score;
        search.best_window = window;
    }
    return TRUE;
}

HWND FindExistingWindow(const std::wstring& executable_path) {
    WindowSearch search{executable_path, GetCurrentProcessId()};
    EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&search));
    return search.best_window;
}

void ActivateWindow(HWND main_window) {
    if (!main_window) return;
    HWND target = GetLastActivePopup(main_window);
    if (!target || !IsWindow(target)) target = main_window;

    if (IsIconic(main_window)) ShowWindowAsync(main_window, SW_RESTORE);
    else if (!IsWindowVisible(main_window)) ShowWindowAsync(main_window, SW_SHOW);
    if (target != main_window && IsIconic(target)) ShowWindowAsync(target, SW_RESTORE);

    SetWindowPos(target, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW |
                     SWP_ASYNCWINDOWPOS);
    BringWindowToTop(target);
    if (!SetForegroundWindow(target)) {
        FLASHWINFO flash{sizeof(flash), target, FLASHW_TRAY | FLASHW_TIMERNOFG,
                         3, 0};
        FlashWindowEx(&flash);
    }
}

} // namespace

SingleInstanceGuard::~SingleInstanceGuard() {
    if (!mutex_) return;
    if (owns_mutex_) ReleaseMutex(mutex_);
    CloseHandle(mutex_);
}

SingleInstanceResult SingleInstanceGuard::Acquire(std::wstring* error_message) {
    if (error_message) error_message->clear();
    if (mutex_) return owns_mutex_ ? SingleInstanceResult::primary
                                   : SingleInstanceResult::error;

    const std::wstring executable_path =
        NormalizeExecutablePath(CurrentExecutablePath());
    if (executable_path.empty()) {
        if (error_message) *error_message = L"无法获取当前程序路径。";
        return SingleInstanceResult::error;
    }

    const std::wstring mutex_name = MutexName(executable_path);
    SetLastError(ERROR_SUCCESS);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (!mutex) {
        if (error_message) {
            *error_message = L"无法建立单实例锁：" +
                             WindowsErrorMessage(GetLastError());
        }
        return SingleInstanceResult::error;
    }

    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        mutex_ = mutex;
        owns_mutex_ = true;
        return SingleInstanceResult::primary;
    }

    for (int attempt = 0; attempt < kActivationRetryCount; ++attempt) {
        if (HWND window = FindExistingWindow(executable_path)) {
            ActivateWindow(window);
            CloseHandle(mutex);
            return SingleInstanceResult::activated_existing;
        }

        const DWORD wait = WaitForSingleObject(mutex, kActivationRetryMilliseconds);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            mutex_ = mutex;
            owns_mutex_ = true;
            return SingleInstanceResult::primary;
        }
        if (wait == WAIT_FAILED) {
            if (error_message) {
                *error_message = L"检查已有实例时失败：" +
                                 WindowsErrorMessage(GetLastError());
            }
            CloseHandle(mutex);
            return SingleInstanceResult::error;
        }
    }

    CloseHandle(mutex);
    return SingleInstanceResult::existing_starting;
}

} // namespace aicli::app
