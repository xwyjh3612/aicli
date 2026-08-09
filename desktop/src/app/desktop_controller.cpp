#include "app/desktop_controller.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <new>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/process_metrics.h"
#include "core/text_utils.h"

namespace aicli::app {
namespace {

using agent::AgentConfig;
using agent::PendingPairing;
using aicli::Project;

constexpr wchar_t kAllGroups[] = L"全部";

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return value;
}

bool IsGroupName(const std::vector<ProjectGroup>& groups, const std::wstring& value) {
    return std::any_of(groups.begin(), groups.end(), [&](const ProjectGroup& group) {
        return group.name == value;
    });
}

} // namespace

struct DesktopController::RemoteInputMessage {
    std::wstring project_id;
    std::wstring phone_id;
    std::wstring input;
    HANDLE completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_bool cancelled{};
    bool accepted{};
    std::wstring error;

    RemoteInputMessage(std::wstring project, std::wstring phone, std::wstring value)
        : project_id(std::move(project)), phone_id(std::move(phone)), input(std::move(value)) {}
    ~RemoteInputMessage() { if (completed) CloseHandle(completed); }
};

struct DesktopController::RemoteKeyMessage {
    std::wstring project_id;
    std::wstring phone_id;
    std::wstring key;
    HANDLE completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_bool cancelled{};
    bool accepted{};
    std::wstring error;

    RemoteKeyMessage(std::wstring project, std::wstring phone, std::wstring value)
        : project_id(std::move(project)), phone_id(std::move(phone)), key(std::move(value)) {}
    ~RemoteKeyMessage() { if (completed) CloseHandle(completed); }
};

struct DesktopController::RemoteControlMessage {
    std::wstring project_id;
    std::wstring phone_id;
    std::wstring action;
};

struct DesktopController::AgentStatusMessage {
    bool online{};
    std::wstring text;
};

DesktopController::DesktopController()
    : conversation_bridge_(store_, remote_agent_) {}

DesktopController::~DesktopController() {
    Shutdown();
}

bool DesktopController::PrepareRuntime(std::wstring* error) {
    if (runtime_prepared_) return true;
    // The packaged application can activate WinUI/Terminal classes through
    // its package graph even when RoRegisterActivationFactories rejects a
    // duplicate side-by-side registration. The original Win32 host treated
    // this registration as best-effort as well; LocalActivationFactory still
    // loads the exact bundled DLLs used by TermControl.
    if (!components_.Initialize()) {
        OutputDebugStringW(
            L"AI CLI: side-by-side activation registration was skipped; "
            L"continuing with packaged/local activation factories.\n");
        if (error) error->clear();
    }
    runtime_prepared_ = true;
    return true;
}

bool DesktopController::InitializePersistentState(std::wstring* error) {
    const std::filesystem::path database =
        std::filesystem::path(DefaultDataDirectory()) / L"aicli.db";
    if (!store_.Initialize(database.wstring(), error)) return false;

    project_types_ = store_.LoadProjectTypes();
    groups_ = store_.LoadGroups();
    if (groups_.empty()) {
        if (!store_.AddGroup(L"默认分组", error)) return false;
        groups_ = store_.LoadGroups();
    }
    projects_ = store_.LoadProjects();
    for (auto& project : projects_) {
        if (project.status == L"running") {
            project.status = L"created";
            store_.UpdateProjectStatus(project.id, project.status);
        }
    }

    if (store_.GetSetting(L"seeded_projects", L"0") != L"1" && projects_.empty()) {
        const auto cwd = std::filesystem::current_path().wstring();
        Project codex{};
        codex.name = L"默认 Codex 会话";
        codex.type = L"Codex";
        codex.group = L"默认分组";
        codex.folder = cwd;
        codex.command = DefaultCommandForType(codex.type);
        store_.SaveProject(codex, error);

        Project claude{};
        claude.name = L"Claude 工作区";
        claude.type = L"Claude";
        claude.group = L"默认分组";
        claude.folder = cwd;
        claude.command = DefaultCommandForType(claude.type);
        store_.SaveProject(claude, error);
        store_.SetSetting(L"seeded_projects", L"1");
        projects_ = store_.LoadProjects();
    }

    group_name_ = store_.GetSetting(L"last_group", L"默认分组");
    if (group_name_ != kAllGroups && !IsGroupName(groups_, group_name_)) {
        group_name_ = L"默认分组";
    }
    selected_project_id_ = store_.GetSetting(L"last_project_id");
    startup_project_id_ = selected_project_id_;

    const bool light = store_.GetSetting(L"theme", L"dark") == L"light";
    theme_manager_.LoadMode(light ? ui::ThemeMode::Light : ui::ThemeMode::Dark);
    return true;
}

bool DesktopController::Initialize(HWND ui_window, std::wstring* error) {
    if (initialized_) return true;
    ui_window_ = ui_window;
    if (!PrepareRuntime(error)) return false;
    if (!InitializePersistentState(error)) return false;

    terminal_snapshots_.Attach(ui_window_, kQtTerminalDirtyMessage);
    ConfigureRemoteAgent();
    initialized_ = true;
    RefreshConversationCache();
    NotifyState();
    return true;
}

bool DesktopController::AttachTerminal(HWND parent, int width, int height,
                                       std::wstring* error) {
    terminal_owner_ = parent;
    RECT rect{0, 0, std::max(1, width), std::max(1, height)};
    if (terminal_.Attach(parent, rect)) {
        // InitializePersistentState() has already loaded the persisted theme,
        // while XamlIslandAdapter starts with a conservative dark default.
        // Synchronize the island before the first project/session is created;
        // otherwise a light-theme app reopened from disk gets a black shell
        // until the user toggles the theme again.
        terminal_.SetTheme(theme_manager_.light());
        return true;
    }
    if (error) {
        *error = terminal_.LastError().empty()
            ? L"无法初始化 Windows Terminal 控件"
            : terminal_.LastError();
    }
    return false;
}

void DesktopController::Shutdown() {
    if (!initialized_ && !store_.Ready()) return;
    terminal_snapshots_.Detach();
    remote_agent_.Stop();
    terminal_.StopAll();
    terminal_owner_ = nullptr;
    if (store_.Ready()) store_.Close();
    initialized_ = false;
}

void DesktopController::NotifyState() {
    if (state_changed_) state_changed_();
}

void DesktopController::NotifyStatus(const std::wstring& text) {
    if (status_changed_) status_changed_(text);
}

void DesktopController::RefreshConversationCache() {
    conversation_bridge_.RefreshConversations(projects_);
}

std::string DesktopController::ConversationsJson() const {
    return conversation_bridge_.ConversationsJson();
}

std::string DesktopController::EventsJson(const std::wstring& project_id,
                                          std::int64_t after) const {
    return conversation_bridge_.EventsJson(project_id, after);
}

void DesktopController::ConfigureRemoteAgent() {
    terminal_streams_.SetBroadcastCallback(
        [this](const std::wstring& project, const std::string& event) {
            remote_agent_.BroadcastEvent(project, event);
        });
    remote_agent_.SetTerminalReplayCallback(
        [this](const std::wstring& project, const std::wstring& session,
               std::uint64_t after) {
            return terminal_streams_.Replay(project, session, after);
        });
    remote_agent_.SetConversationsCallback([this] { return ConversationsJson(); });
    remote_agent_.SetEventsCallback(
        [this](const std::wstring& project, std::int64_t after) {
            return EventsJson(project, after);
        });

    remote_agent_.SetInputCallback(
        [this](const std::wstring& project, const std::wstring& phone,
               const std::wstring& input, std::wstring* error) {
            auto request = std::make_shared<RemoteInputMessage>(project, phone, input);
            if (!request->completed) {
                if (error) *error = L"无法创建输入同步事件";
                return false;
            }
            auto* envelope = new (std::nothrow) std::shared_ptr<RemoteInputMessage>(request);
            if (!envelope || !ui_window_ ||
                !PostMessageW(ui_window_, kQtRemoteInputMessage, 0,
                              reinterpret_cast<LPARAM>(envelope))) {
                delete envelope;
                if (error) *error = L"桌面窗口未就绪";
                return false;
            }
            if (WaitForSingleObject(request->completed, 30000) != WAIT_OBJECT_0) {
                request->cancelled = true;
                if (error) *error = L"电脑端处理输入超时";
                return false;
            }
            if (!request->accepted && error) {
                *error = request->error.empty() ? L"终端未接受输入" : request->error;
            }
            return request->accepted;
        });

    remote_agent_.SetKeyCallback(
        [this](const std::wstring& project, const std::wstring& phone,
               const std::wstring& key, std::wstring* error) {
            auto request = std::make_shared<RemoteKeyMessage>(project, phone, key);
            if (!request->completed) {
                if (error) *error = L"无法创建按键同步事件";
                return false;
            }
            auto* envelope = new (std::nothrow) std::shared_ptr<RemoteKeyMessage>(request);
            if (!envelope || !ui_window_ ||
                !PostMessageW(ui_window_, kQtRemoteKeyMessage, 0,
                              reinterpret_cast<LPARAM>(envelope))) {
                delete envelope;
                if (error) *error = L"桌面窗口未就绪";
                return false;
            }
            if (WaitForSingleObject(request->completed, 30000) != WAIT_OBJECT_0) {
                request->cancelled = true;
                if (error) *error = L"电脑端处理按键超时";
                return false;
            }
            if (!request->accepted && error) {
                *error = request->error.empty() ? L"终端未接受按键" : request->error;
            }
            return request->accepted;
        });

    remote_agent_.SetControlCallback(
        [this](const std::wstring& project, const std::wstring& phone,
               const std::wstring& action, std::wstring* error) {
            auto* request = new (std::nothrow) RemoteControlMessage{project, phone, action};
            if (!request || !ui_window_ ||
                !PostMessageW(ui_window_, kQtRemoteControlMessage, 0,
                              reinterpret_cast<LPARAM>(request))) {
                delete request;
                if (error) *error = L"桌面窗口未就绪";
                return false;
            }
            return true;
        });

    remote_agent_.SetPairingCallback(
        [this](const PendingPairing& pairing) {
            auto* copy = new (std::nothrow) PendingPairing(pairing);
            if (!copy || !ui_window_ ||
                !PostMessageW(ui_window_, kQtPairingPendingMessage, 0,
                              reinterpret_cast<LPARAM>(copy))) {
                delete copy;
            }
        });
    remote_agent_.SetStatusCallback(
        [this](bool online, const std::wstring& text) {
            auto* status = new (std::nothrow) AgentStatusMessage{online, text};
            if (!status || !ui_window_ ||
                !PostMessageW(ui_window_, kQtAgentStatusMessage, 0,
                              reinterpret_cast<LPARAM>(status))) {
                delete status;
            }
        });

    std::wstring error;
    if (!remote_agent_.Start(&store_, CurrentAgentConfig(), &error)) {
        NotifyStatus(L"Agent 启动失败: " + error);
    } else {
        NotifyStatus(L"就绪");
    }
}

agent::AgentConfig DesktopController::CurrentAgentConfig() {
    agent::AgentConfig config;
    config.bind_address = store_.GetSetting(L"listen_address", L"0.0.0.0");
    config.port = store_.GetSetting(L"listen_port", L"4280");
    config.local_address = store_.GetSetting(L"local_address");
    config.remote_address = store_.GetSetting(L"remote_address");
    config.data_directory = DefaultDataDirectory();
    return config;
}

bool DesktopController::RestartAgent(const agent::AgentConfig& config,
                                     std::wstring* error) {
    remote_agent_.Stop();
    if (!remote_agent_.Start(&store_, config, error)) {
        NotifyStatus(L"Agent 启动失败: " + (error ? *error : L"未知错误"));
        return false;
    }
    NotifyStatus(L"Agent 服务已重启");
    NotifyState();
    return true;
}

std::wstring DesktopController::DefaultCommandForType(const std::wstring& type) const {
    for (const auto& value : project_types_) {
        if (value.name == type && !value.default_command.empty()) return value.default_command;
    }
    if (type == L"Claude") return L"claude --dangerously-skip-permissions --continue";
    return L"codex --yolo --no-alt-screen resume --last";
}

bool DesktopController::StartCommandSession(Project& project) {
    terminal_streams_.BeginSession(project.id);
    if (terminal_.Start(
            project.id,
            project.command.empty() ? DefaultCommandForType(project.type) : project.command,
            project.folder,
            project.name,
            ui_window_,
            nullptr,
            [this, id = project.id](std::wstring_view output) {
                terminal_streams_.PublishOutput(id, output);
                terminal_snapshots_.Queue(id);
            },
            [this, id = project.id](std::uint32_t rows, std::uint32_t columns) {
                terminal_streams_.PublishResize(id, rows, columns);
            })) {
        NotifyStatus(L"原生终端已连接    |    " + project.type);
        return true;
    }
    const auto error = terminal_.LastError().empty()
        ? L"无法创建原生终端控件" : terminal_.LastError();
    terminal_streams_.RemoveSession(project.id);
    project.status = L"created";
    NotifyStatus(L"原生终端启动失败: " + error);
    return false;
}

bool DesktopController::StartProject(const std::wstring& project_id, bool restart) {
    auto* project = FindProject(project_id);
    if (!project) return false;
    selected_project_id_ = project_id;
    store_.SetSetting(L"last_project_id", selected_project_id_);
    if (terminal_.HasSession(project_id)) {
        if (!restart) {
            terminal_.Activate(project_id);
            terminal_.SetVisible(true);
            NotifyState();
            return true;
        }
        terminal_.Stop(project_id);
    }
    if (!StartCommandSession(*project)) {
        NotifyState();
        return false;
    }
    project->status = L"running";
    store_.UpdateProjectStatus(project_id, L"running");
    conversation_bridge_.RecordAndBroadcast(
        project_id, L"conversation.running", nlohmann::json{{"status", "running"}});
    NotifyState();
    return true;
}

void DesktopController::StopProject(const std::wstring& project_id,
                                     const std::wstring& status) {
    terminal_.Stop(project_id);
    terminal_streams_.RemoveSession(project_id);
    if (auto* project = FindProject(project_id)) {
        project->status = status;
        store_.UpdateProjectStatus(project_id, status);
    }
    conversation_bridge_.RecordAndBroadcast(
        project_id,
        status == L"paused" ? L"conversation.paused" : L"conversation.cancelled",
        nlohmann::json{{"status", aicli::text::Utf8Text(status)}});
    NotifyState();
}

bool DesktopController::HasSession(const std::wstring& project_id) const {
    return terminal_.HasSession(project_id);
}

bool DesktopController::ActivateProject(const std::wstring& project_id, bool focus) {
    selected_project_id_ = project_id;
    store_.SetSetting(L"last_project_id", project_id);
    const bool activated = terminal_.Activate(project_id, focus);
    if (activated) terminal_.SetVisible(true);
    NotifyState();
    return activated;
}

void DesktopController::ResizeTerminal(int width, int height) {
    terminal_.Resize(RECT{0, 0, std::max(1, width), std::max(1, height)});
}

void DesktopController::SetTerminalVisible(bool visible) {
    terminal_.SetVisible(visible);
}

void DesktopController::SetSelectedProject(const std::wstring& project_id) {
    if (project_id.empty()) {
        selected_project_id_.clear();
        terminal_.SetVisible(false);
        NotifyState();
        return;
    }
    selected_project_id_ = project_id;
    store_.SetSetting(L"last_project_id", project_id);
    if (terminal_.HasSession(project_id)) {
        terminal_.Activate(project_id, false);
        terminal_.SetVisible(true);
    } else {
        terminal_.SetVisible(false);
    }
    NotifyState();
}

int DesktopController::ProjectIndex(const std::wstring& project_id) const {
    for (std::size_t i = 0; i < projects_.size(); ++i) {
        if (projects_[i].id == project_id) return static_cast<int>(i);
    }
    return -1;
}

const Project* DesktopController::FindProject(const std::wstring& project_id) const {
    const int index = ProjectIndex(project_id);
    return index < 0 ? nullptr : &projects_[static_cast<std::size_t>(index)];
}

Project* DesktopController::FindProject(const std::wstring& project_id) {
    const int index = ProjectIndex(project_id);
    return index < 0 ? nullptr : &projects_[static_cast<std::size_t>(index)];
}

std::vector<int> DesktopController::VisibleProjectIndices(const std::wstring& query) const {
    std::vector<int> result;
    const auto needle = Lower(query);
    for (std::size_t i = 0; i < projects_.size(); ++i) {
        const auto& project = projects_[i];
        const auto project_group = project.group.empty() ? L"默认分组" : project.group;
        if (group_name_ != kAllGroups && project_group != group_name_) continue;
        if (!needle.empty() && Lower(project.name).find(needle) == std::wstring::npos &&
            Lower(project.type).find(needle) == std::wstring::npos) continue;
        result.push_back(static_cast<int>(i));
    }
    return result;
}

std::size_t DesktopController::ProjectCount(const std::wstring& group) const {
    if (group == kAllGroups) return projects_.size();
    return static_cast<std::size_t>(std::count_if(
        projects_.begin(), projects_.end(), [&](const Project& project) {
            return (project.group.empty() ? L"默认分组" : project.group) == group;
        }));
}

void DesktopController::SetGroup(const std::wstring& group) {
    if (group != kAllGroups && !IsGroupName(groups_, group)) return;
    group_name_ = group;
    store_.SetSetting(L"last_group", group_name_);
    NotifyState();
}

void DesktopController::ReloadModels() {
    project_types_ = store_.LoadProjectTypes();
    groups_ = store_.LoadGroups();
    projects_ = store_.LoadProjects();
    if (groups_.empty()) {
        store_.AddGroup(L"默认分组");
        groups_ = store_.LoadGroups();
    }
    if (group_name_ != kAllGroups && !IsGroupName(groups_, group_name_)) {
        group_name_ = L"默认分组";
        store_.SetSetting(L"last_group", group_name_);
    }
    RefreshConversationCache();
    NotifyState();
}

bool DesktopController::SaveProject(Project project, std::wstring* error) {
    if (project.group.empty()) project.group = L"默认分组";
    if (project.command.empty()) project.command = DefaultCommandForType(project.type);
    if (!store_.SaveProject(project, error)) return false;
    ReloadModels();
    group_name_ = project.group;
    store_.SetSetting(L"last_group", group_name_);
    selected_project_id_ = project.id;
    store_.SetSetting(L"last_project_id", selected_project_id_);
    NotifyState();
    return true;
}

bool DesktopController::DeleteProject(const std::wstring& project_id, std::wstring* error) {
    if (!FindProject(project_id)) return false;
    terminal_.Stop(project_id);
    terminal_streams_.RemoveSession(project_id);
    if (!store_.DeleteProject(project_id, error)) return false;
    if (selected_project_id_ == project_id) selected_project_id_.clear();
    ReloadModels();
    NotifyState();
    return true;
}

bool DesktopController::MoveProjectToGroup(const std::wstring& project_id,
                                            const std::wstring& group,
                                            std::wstring* error) {
    auto* project = FindProject(project_id);
    if (!project || group.empty()) return false;
    project->group = group;
    if (!store_.SaveProject(*project, error)) return false;
    ReloadModels();
    NotifyState();
    return true;
}

bool DesktopController::ReorderVisibleProjects(
    const std::wstring& group,
    const std::vector<std::wstring>& project_ids,
    std::wstring* error) {
    if (group != kAllGroups) {
        if (!store_.ReorderProjects(group, project_ids, error)) return false;
    } else {
        for (const auto& item : groups_) {
            std::vector<std::wstring> ids;
            for (const auto& id : project_ids) {
                const auto* project = FindProject(id);
                if (project && (project->group.empty() ? L"默认分组" : project->group) == item.name) {
                    ids.push_back(id);
                }
            }
            if (!ids.empty() && !store_.ReorderProjects(item.name, ids, error)) return false;
        }
    }
    ReloadModels();
    return true;
}

void DesktopController::SetTheme(bool light) {
    store_.SetSetting(L"theme", light ? L"light" : L"dark");
    theme_manager_.LoadMode(light ? ui::ThemeMode::Light : ui::ThemeMode::Dark);
    terminal_.SetTheme(light);
    NotifyState();
}

void DesktopController::RefreshTerminalViewport() {
    terminal_.RefreshTerminalViewport();
}

std::size_t DesktopController::BoundDeviceCount() {
    return store_.LoadDevices().size();
}

aicli::git::RepositoryStatus DesktopController::GitStatus(const std::wstring& folder) const {
    return aicli::git::InspectRepository(folder);
}

std::size_t DesktopController::WorkingSetMegabytes() const {
    return aicli::WorkingSetMegabytes();
}

bool DesktopController::TakeTerminalDirty() noexcept {
    return terminal_dirty_.exchange(false) || terminal_snapshots_.TakeDirty();
}

void DesktopController::PublishDirtySnapshots() {
    terminal_snapshots_.Publish(terminal_, conversation_bridge_);
}

bool DesktopController::SelectProjectFromRemote(const std::wstring& project_id) {
    const auto* project = FindProject(project_id);
    if (!project) return false;
    if (group_name_ != kAllGroups) {
        group_name_ = project->group.empty() ? L"默认分组" : project->group;
        store_.SetSetting(L"last_group", group_name_);
    }
    SetSelectedProject(project_id);
    return true;
}

bool DesktopController::HandleRemoteInput(
    const std::shared_ptr<RemoteInputMessage>& request) {
    auto complete = [&](bool accepted, const std::wstring& error = L"") {
        request->accepted = accepted;
        request->error = error;
        SetEvent(request->completed);
    };
    if (request->cancelled) { complete(false, L"输入请求已超时"); return false; }
    if (!SelectProjectFromRemote(request->project_id)) {
        complete(false, L"项目或会话不存在"); return false;
    }
    if (!terminal_.HasSession(request->project_id)) StartProject(request->project_id);
    if (request->cancelled) { complete(false, L"输入请求已超时"); return false; }
    if (!terminal_.HasSession(request->project_id)) {
        complete(false, L"终端会话未就绪"); return false;
    }
    std::wstring text = request->input;
    bool submit = false;
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
        submit = true;
        text.pop_back();
    }
    const bool accepted = submit
        ? terminal_.SubmitInput(request->project_id, text)
        : terminal_.WriteInput(request->project_id, text);
    if (!accepted) { complete(false, L"终端输入失败"); return false; }
    conversation_bridge_.RecordAndBroadcast(
        request->project_id, L"terminal.input",
        nlohmann::json{{"input", aicli::text::Utf8Text(text)},
                       {"phoneId", aicli::text::Utf8Text(request->phone_id)}});
    complete(true);
    return true;
}

bool DesktopController::HandleRemoteKey(
    const std::shared_ptr<RemoteKeyMessage>& request) {
    auto complete = [&](bool accepted, const std::wstring& error = L"") {
        request->accepted = accepted;
        request->error = error;
        SetEvent(request->completed);
    };
    if (request->cancelled) { complete(false, L"按键请求已超时"); return false; }
    if (!SelectProjectFromRemote(request->project_id)) {
        complete(false, L"项目或会话不存在"); return false;
    }
    if (!terminal_.HasSession(request->project_id)) StartProject(request->project_id);
    if (!terminal_.HasSession(request->project_id)) {
        complete(false, L"终端会话未就绪"); return false;
    }
    UINT key = 0;
    if (request->key == L"left") key = VK_LEFT;
    else if (request->key == L"right") key = VK_RIGHT;
    else if (request->key == L"up") key = VK_UP;
    else if (request->key == L"down") key = VK_DOWN;
    else if (request->key == L"enter") key = VK_RETURN;
    if (!key || !terminal_.SendKey(request->project_id, key)) {
        complete(false, L"终端按键发送失败"); return false;
    }
    complete(true);
    return true;
}

void DesktopController::HandleRemoteControl(std::unique_ptr<RemoteControlMessage> request) {
    if (!request || !SelectProjectFromRemote(request->project_id)) return;
    if (request->action == L"restart") StartProject(request->project_id, true);
    else if (request->action == L"start" || request->action == L"continue") StartProject(request->project_id);
    else if (request->action == L"pause" || request->action == L"cancel") {
        StopProject(request->project_id, request->action == L"pause" ? L"paused" : L"cancelled");
    } else if (request->action == L"ctrl_c") {
        terminal_.WriteInput(request->project_id, std::wstring(1, L'\x03'));
    }
}

void DesktopController::HandlePairing(std::unique_ptr<PendingPairing> pairing) {
    if (pairing && pairing_pending_) pairing_pending_(*pairing);
}

void DesktopController::HandleAgentStatus(std::unique_ptr<AgentStatusMessage> status) {
    if (!status) return;
    if (agent_status_) agent_status_(status->online, status->text);
}

bool DesktopController::HandleNativeMessage(UINT message, WPARAM, LPARAM lparam) {
    if (message == kQtTerminalDirtyMessage) {
        terminal_snapshots_.MarkDirtyMessageHandled();
        terminal_dirty_ = true;
        return true;
    }
    if (message == kQtRemoteInputMessage) {
        std::unique_ptr<std::shared_ptr<RemoteInputMessage>> envelope(
            reinterpret_cast<std::shared_ptr<RemoteInputMessage>*>(lparam));
        if (envelope && *envelope) HandleRemoteInput(*envelope);
        return true;
    }
    if (message == kQtRemoteKeyMessage) {
        std::unique_ptr<std::shared_ptr<RemoteKeyMessage>> envelope(
            reinterpret_cast<std::shared_ptr<RemoteKeyMessage>*>(lparam));
        if (envelope && *envelope) HandleRemoteKey(*envelope);
        return true;
    }
    if (message == kQtRemoteControlMessage) {
        HandleRemoteControl(std::unique_ptr<RemoteControlMessage>(
            reinterpret_cast<RemoteControlMessage*>(lparam)));
        return true;
    }
    if (message == kQtPairingPendingMessage) {
        HandlePairing(std::unique_ptr<PendingPairing>(
            reinterpret_cast<PendingPairing*>(lparam)));
        return true;
    }
    if (message == kQtAgentStatusMessage) {
        HandleAgentStatus(std::unique_ptr<AgentStatusMessage>(
            reinterpret_cast<AgentStatusMessage*>(lparam)));
        return true;
    }
    return false;
}

} // namespace aicli::app
