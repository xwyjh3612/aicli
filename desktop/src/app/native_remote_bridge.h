#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "features/mobile_project_creation.h"

namespace aicli
{
class DataStore;
}
namespace aicli::agent
{
class RemoteAgent;
class TerminalStreamPublisher;
class ConversationBridge;
struct PendingPairing;
} // namespace aicli::agent

namespace aicli::app::native_remote
{

inline constexpr UINT WM_REMOTE_INPUT = WM_APP + 20;
inline constexpr UINT WM_REMOTE_CONTROL = WM_APP + 21;
inline constexpr UINT WM_PAIRING_PENDING = WM_APP + 22;
inline constexpr UINT WM_AGENT_STATUS = WM_APP + 23;
inline constexpr UINT WM_REMOTE_KEY = WM_APP + 27;
inline constexpr UINT WM_REMOTE_CREATE_PROJECT = WM_APP + 30;

struct RemoteInputMessage
{
    std::wstring project_id, phone_id, input;
    HANDLE completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_bool cancelled{};
    bool accepted{};
    std::wstring error;
    RemoteInputMessage(std::wstring project, std::wstring phone, std::wstring value)
        : project_id(std::move(project)), phone_id(std::move(phone)), input(std::move(value))
    {
    }
    ~RemoteInputMessage()
    {
        if (completed)
            CloseHandle(completed);
    }
};

struct RemoteControlMessage
{
    std::wstring project_id, phone_id, action;
};

struct RemoteKeyMessage
{
    std::wstring project_id, phone_id, key;
    HANDLE completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_bool cancelled{};
    bool accepted{};
    std::wstring error;
    RemoteKeyMessage(std::wstring project, std::wstring phone, std::wstring value)
        : project_id(std::move(project)), phone_id(std::move(phone)), key(std::move(value))
    {
    }
    ~RemoteKeyMessage()
    {
        if (completed)
            CloseHandle(completed);
    }
};

struct RemoteCreateProjectMessage
{
    mobile_projects::CreateRequest request;
    mobile_projects::CreateResult result;
    HANDLE completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_bool cancelled{};
    explicit RemoteCreateProjectMessage(const mobile_projects::CreateRequest &value)
        : request(value)
    {
    }
    ~RemoteCreateProjectMessage()
    {
        if (completed)
            CloseHandle(completed);
    }
};

struct AgentStatusMessage
{
    bool online{};
    std::wstring text;
};

// The native window supplies UI actions; the bridge owns payload lifetimes,
// cancellation, synchronization and dispatch from the agent's worker threads.
struct UiCallbacks
{
    std::function<bool(const std::wstring &)> select_project;
    std::function<void(bool restart)> start_session;
    std::function<bool(const std::wstring &)> has_session;
    std::function<bool(const std::wstring &, const std::wstring &)> write_input;
    std::function<bool(const std::wstring &, const std::wstring &)> submit_input;
    std::function<bool(const std::wstring &, UINT)> send_key;
    std::function<void(const RemoteControlMessage &)> control;
    std::function<void(const mobile_projects::CreateResult &)> project_created;
    std::function<void(const agent::PendingPairing &)> pairing;
    std::function<void(bool online, const std::wstring &)> status;
};

void ConfigureCallbacks(HWND owner, agent::RemoteAgent &agent,
                        agent::TerminalStreamPublisher &streams,
                        agent::ConversationBridge &conversations);
bool HandleMessage(UINT message, LPARAM payload, const UiCallbacks &ui, DataStore &store,
                   agent::ConversationBridge &conversations);

} // namespace aicli::app::native_remote
