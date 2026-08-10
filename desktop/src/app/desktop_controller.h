#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "agent/conversation_bridge.h"
#include "agent/remote_agent.h"
#include "agent/terminal_snapshot_publisher.h"
#include "agent/terminal_stream_publisher.h"
#include "app/runtime_components.h"
#include "core/models.h"
#include "data/data_store.h"
#include "features/git_repository.h"
#include "terminal/xaml_island_adapter.h"
#include "ui/theme.h"

namespace aicli::app {

// The Qt front-end still receives the existing worker-thread notifications as
// native messages.  Keeping these messages in the controller means the UI
// toolkit can change without changing the secure agent or terminal code.
constexpr UINT kQtRemoteInputMessage = WM_APP + 40;
constexpr UINT kQtRemoteKeyMessage = WM_APP + 41;
constexpr UINT kQtRemoteControlMessage = WM_APP + 42;
constexpr UINT kQtPairingPendingMessage = WM_APP + 43;
constexpr UINT kQtAgentStatusMessage = WM_APP + 44;
constexpr UINT kQtTerminalDirtyMessage = WM_APP + 45;
constexpr UINT kQtWorkingStateMessage = WM_APP + 46;

class DesktopController final {
public:
    using VoidCallback = std::function<void()>;
    using StatusCallback = std::function<void(const std::wstring&)>;
    using PairingCallback = std::function<void(const agent::PendingPairing&)>;
    using AgentCallback = std::function<void(bool online, const std::wstring&)>;

    DesktopController();
    ~DesktopController();

    DesktopController(const DesktopController&) = delete;
    DesktopController& operator=(const DesktopController&) = delete;

    bool PrepareRuntime(std::wstring* error = nullptr);
    bool Initialize(HWND ui_window, std::wstring* error = nullptr);
    bool AttachTerminal(HWND parent, int width, int height,
                        std::wstring* error = nullptr);
    void Shutdown();

    // Handles only messages posted by RemoteAgent and the terminal publisher.
    // Returns true when the message was consumed.
    bool HandleNativeMessage(UINT message, WPARAM wparam, LPARAM lparam);
    bool PreTranslateTerminalMessage(const MSG& message) noexcept;
    bool TakeTerminalDirty() noexcept;
    void PublishDirtySnapshots();

    bool StartProject(const std::wstring& project_id, bool restart = false);
    void StopProject(const std::wstring& project_id,
                     const std::wstring& status = L"cancelled");
    bool HasSession(const std::wstring& project_id) const;
    bool IsProjectWorking(const std::wstring& project_id) const;
    bool AnyProjectWorking() const;
    bool ActivateProject(const std::wstring& project_id, bool focus = true);
    void ResizeTerminal(int width, int height);
    void SetTerminalVisible(bool visible);
    void SetSelectedProject(const std::wstring& project_id);

    bool SaveProject(Project project, std::wstring* error = nullptr);
    bool DeleteProject(const std::wstring& project_id, std::wstring* error = nullptr);
    bool MoveProjectToGroup(const std::wstring& project_id,
                            const std::wstring& group,
                            std::wstring* error = nullptr);
    bool ReorderVisibleProjects(const std::wstring& group,
                                const std::vector<std::wstring>& project_ids,
                                std::wstring* error = nullptr);

    void ReloadModels();
    void SetGroup(const std::wstring& group);
    void SetTheme(bool light);
    void RefreshTerminalViewport();

    std::vector<int> VisibleProjectIndices(const std::wstring& query) const;
    int ProjectIndex(const std::wstring& project_id) const;
    const Project* FindProject(const std::wstring& project_id) const;
    Project* FindProject(const std::wstring& project_id);
    std::wstring DefaultCommandForType(const std::wstring& type) const;
    std::size_t ProjectCount(const std::wstring& group) const;

    const std::vector<Project>& projects() const noexcept { return projects_; }
    const std::vector<ProjectType>& projectTypes() const noexcept { return project_types_; }
    const std::vector<ProjectGroup>& groups() const noexcept { return groups_; }
    const std::wstring& selectedProjectId() const noexcept { return selected_project_id_; }
    const std::wstring& group() const noexcept { return group_name_; }
    const aicli::ui::ThemePalette& theme() const noexcept { return theme_manager_.palette(); }
    bool lightTheme() const noexcept { return theme_manager_.light(); }

    DataStore& store() noexcept { return store_; }
    agent::RemoteAgent& agent() noexcept { return remote_agent_; }
    const agent::RemoteAgent& agent() const noexcept { return remote_agent_; }

    std::size_t BoundDeviceCount();
    aicli::git::RepositoryStatus GitStatus(const std::wstring& folder) const;
    std::size_t WorkingSetMegabytes() const;

    void SetStateChangedCallback(VoidCallback callback) { state_changed_ = std::move(callback); }
    void SetStatusCallback(StatusCallback callback) { status_changed_ = std::move(callback); }
    void SetPairingCallback(PairingCallback callback) { pairing_pending_ = std::move(callback); }
    void SetAgentCallback(AgentCallback callback) { agent_status_ = std::move(callback); }

    bool RestartAgent(const agent::AgentConfig& config, std::wstring* error = nullptr);
    agent::AgentConfig CurrentAgentConfig();

private:
    struct RemoteInputMessage;
    struct RemoteKeyMessage;
    struct RemoteControlMessage;
    struct AgentStatusMessage;
    struct WorkingStateMessage;

    bool InitializePersistentState(std::wstring* error);
    void ConfigureRemoteAgent();
    void NotifyState();
    void NotifyStatus(const std::wstring& text);
    bool SelectProjectFromRemote(const std::wstring& project_id);
    bool HandleRemoteInput(const std::shared_ptr<RemoteInputMessage>& request);
    bool HandleRemoteKey(const std::shared_ptr<RemoteKeyMessage>& request);
    void HandleRemoteControl(std::unique_ptr<RemoteControlMessage> request);
    void HandlePairing(std::unique_ptr<agent::PendingPairing> pairing);
    void HandleAgentStatus(std::unique_ptr<AgentStatusMessage> status);
    void HandleWorkingState(std::unique_ptr<WorkingStateMessage> state);
    void SetProjectWorkingState(const std::wstring& project_id, bool working);
    bool StartCommandSession(Project& project);
    void RefreshConversationCache();
    std::string ConversationsJson() const;
    std::string EventsJson(const std::wstring& project_id,
                           std::int64_t after) const;

    HWND ui_window_{};
    HWND terminal_owner_{};
    WinRtComponentRegistry components_;
    DataStore store_;
    agent::RemoteAgent remote_agent_;
    agent::ConversationBridge conversation_bridge_;
    terminal::XamlIslandAdapter terminal_;
    agent::TerminalSnapshotPublisher terminal_snapshots_;
    agent::TerminalStreamPublisher terminal_streams_;
    ui::ThemeManager theme_manager_;
    std::vector<Project> projects_;
    std::vector<ProjectType> project_types_;
    std::vector<ProjectGroup> groups_;
    std::wstring group_name_{L"默认分组"};
    std::wstring selected_project_id_;
    std::wstring startup_project_id_;
    std::unordered_set<std::wstring> working_projects_;
    bool initialized_{};
    bool runtime_prepared_{};
    std::atomic_bool terminal_dirty_{};
    VoidCallback state_changed_;
    StatusCallback status_changed_;
    PairingCallback pairing_pending_;
    AgentCallback agent_status_;
};

} // namespace aicli::app
