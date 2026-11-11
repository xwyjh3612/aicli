#pragma once

#include "agent/remote_agent.h"
#include "ui/group_name_dialog.h"
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
    bool open_binding{};
};

using AgentRestartCallback =
    std::function<bool(const aicli::agent::AgentConfig&, std::wstring* error)>;
using ThemeChangedCallback = std::function<void(bool light_theme)>;
using AppearanceChangedCallback = std::function<void()>;

ManagementDialogResult ShowManagementDialog(
    HWND owner,
    ManagementMode mode,
    aicli::DataStore& store,
    aicli::agent::RemoteAgent& agent,
    AgentRestartCallback restart_agent = {},
    ThemeChangedCallback theme_changed = {},
    AppearanceChangedCallback appearance_changed = {});
} // namespace aicli::ui
