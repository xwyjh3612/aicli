#include "ui/taskbar_working_indicator.h"

#include <shobjidl.h>

namespace aicli::ui {

TaskbarWorkingIndicator::~TaskbarWorkingIndicator() {
    Detach();
}

void TaskbarWorkingIndicator::Attach(HWND window) {
    if (window_ == window) {
        Apply();
        return;
    }
    Detach();
    window_ = window;
    Apply();
}

void TaskbarWorkingIndicator::SetWorking(bool working) {
    if (working_ == working) return;
    working_ = working;
    Apply();
}

void TaskbarWorkingIndicator::OnTaskbarButtonCreated() {
    // Explorer can restart while AI CLI stays open. Recreate the COM object
    // and restore the current state when the new taskbar button appears.
    if (taskbar_) {
        taskbar_->Release();
        taskbar_ = nullptr;
    }
    Apply();
}

void TaskbarWorkingIndicator::Detach() {
    if (taskbar_ && window_) {
        taskbar_->SetProgressState(window_, TBPF_NOPROGRESS);
    }
    if (taskbar_) {
        taskbar_->Release();
        taskbar_ = nullptr;
    }
    window_ = nullptr;
    working_ = false;
}

bool TaskbarWorkingIndicator::EnsureInterface() {
    if (taskbar_) return true;
    ITaskbarList3* taskbar = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&taskbar))) ||
        !taskbar) {
        return false;
    }
    if (FAILED(taskbar->HrInit())) {
        taskbar->Release();
        return false;
    }
    taskbar_ = taskbar;
    return true;
}

void TaskbarWorkingIndicator::Apply() {
    if (!window_ || !EnsureInterface()) return;
    taskbar_->SetProgressState(
        window_, working_ ? TBPF_INDETERMINATE : TBPF_NOPROGRESS);
}

} // namespace aicli::ui
