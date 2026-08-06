#pragma once

#include "agent/remote_agent.h"
#include "data/data_store.h"

#include <functional>
#include <string>

namespace aicli::ui {

enum class ManagementMode {
    Settings,
    Groups,
    ProjectTypes,
    Devices,
};

struct ManagementDialogResult {
    bool accepted{};
    std::wstring selected_group;
};

using AgentRestartCallback =
    std::function<bool(const aicli::agent::AgentConfig&, std::wstring* error)>;

ManagementDialogResult ShowManagementDialog(
    HWND owner,
    ManagementMode mode,
    aicli::DataStore& store,
    aicli::agent::RemoteAgent& agent,
    AgentRestartCallback restart_agent = {});

} // namespace aicli::ui

