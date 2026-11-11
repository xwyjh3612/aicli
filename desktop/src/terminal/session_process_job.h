#pragma once

#include <windows.h>

namespace aicli::terminal {

// Each native terminal owns its entire process tree. The handle is deliberately
// non-inheritable: even TerminateProcess on AiCli closes its last job handle and
// releases the CLI's session locks. Assign a suspended shell before resuming it.
class SessionProcessJob {
public:
    SessionProcessJob() = default;
    SessionProcessJob(const SessionProcessJob&) = delete;
    SessionProcessJob& operator=(const SessionProcessJob&) = delete;
    ~SessionProcessJob() { Reset(); }

    bool Initialize() noexcept {
        Reset();
        handle_ = CreateJobObjectW(nullptr, nullptr);
        if (!handle_) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(handle_, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))) {
            Reset();
            return false;
        }
        return true;
    }

    bool Assign(HANDLE suspended_process) noexcept {
        return handle_ && AssignProcessToJobObject(handle_, suspended_process);
    }

    bool TerminateAndWait(DWORD timeout_ms = 5000) noexcept {
        if (!handle_) return true;
        if (!TerminateJobObject(handle_, 0)) return false;
        const auto deadline = GetTickCount64() + timeout_ms;
        for (;;) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if (!QueryInformationJobObject(handle_, JobObjectBasicAccountingInformation,
                                           &accounting, sizeof(accounting), nullptr)) return false;
            if (accounting.ActiveProcesses == 0) return true;
            if (GetTickCount64() >= deadline) return false;
            Sleep(10);
        }
    }

    void Reset() noexcept {
        if (handle_) CloseHandle(handle_);
        handle_ = nullptr;
    }

private:
    HANDLE handle_{};
};

}
