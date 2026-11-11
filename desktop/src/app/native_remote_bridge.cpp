#include "app/native_remote_bridge.h"
#include "agent/remote_agent.h"
#include "agent/conversation_bridge.h"
#include "agent/terminal_stream_publisher.h"
#include "core/text_utils.h"
#include "data/data_store.h"

namespace aicli::app::native_remote
{

void ConfigureCallbacks(HWND owner, agent::RemoteAgent &agent,
                        agent::TerminalStreamPublisher &streams,
                        agent::ConversationBridge &conversations)
{

    streams.SetBroadcastCallback([&agent](const std::wstring &project, const std::string &event) {
        agent.BroadcastEvent(project, event);
    });
    agent.SetTerminalReplayCallback(
        [&streams](const std::wstring &project, const std::wstring &session, std::uint64_t after) {
            return streams.Replay(project, session, after);
        });
    agent.SetTerminalCompactCallback(
        [&streams](const std::wstring &project) { return streams.CompactReplay(project, 80); });
    agent.SetTerminalHistoryCallback(
        [&streams](const std::wstring &project, const std::wstring &token, std::size_t before,
                   std::size_t limit) { return streams.History(project, token, before, limit); });
    agent.SetConversationsCallback([&conversations] { return conversations.ConversationsJson(); });
    agent.SetEventsCallback([&conversations](const std::wstring &project, std::int64_t after) {
        return conversations.EventsJson(project, after);
    });
    agent.SetCreateProjectCallback([owner](const aicli::mobile_projects::CreateRequest &value) {
        auto request = std::make_shared<RemoteCreateProjectMessage>(value);
        if (!request->completed)
        {
            request->result.http_status = 500;
            request->result.error = L"无法创建项目同步事件";
            return request->result;
        }
        auto *envelope = new (std::nothrow) std::shared_ptr<RemoteCreateProjectMessage>(request);
        if (!envelope || !owner ||
            !PostMessageW(owner, WM_REMOTE_CREATE_PROJECT, 0, reinterpret_cast<LPARAM>(envelope)))
        {
            delete envelope;
            request->result.http_status = 503;
            request->result.error = L"桌面窗口未就绪";
            return request->result;
        }
        if (WaitForSingleObject(request->completed, 30000) != WAIT_OBJECT_0)
        {
            request->cancelled = true;
            aicli::mobile_projects::CreateResult timeout;
            timeout.http_status = 504;
            timeout.error = L"电脑端创建项目超时";
            return timeout;
        }
        return request->result;
    });
    agent.SetInputCallback([owner](const std::wstring &project, const std::wstring &phone,
                                   const std::wstring &input, std::wstring *error) {
        auto request = std::make_shared<RemoteInputMessage>(project, phone, input);
        if (!request->completed)
        {
            if (error)
                *error = L"无法创建输入同步事件";
            return false;
        }
        auto *envelope = new (std::nothrow) std::shared_ptr<RemoteInputMessage>(request);
        if (!envelope || !owner ||
            !PostMessageW(owner, WM_REMOTE_INPUT, 0, reinterpret_cast<LPARAM>(envelope)))
        {
            delete envelope;
            if (error)
                *error = L"桌面窗口未就绪";
            return false;
        }
        if (WaitForSingleObject(request->completed, 30000) != WAIT_OBJECT_0)
        {
            request->cancelled = true;
            if (error)
                *error = L"电脑端处理输入超时";
            return false;
        }
        if (!request->accepted && error)
            *error = request->error.empty() ? L"终端未接受输入" : request->error;
        return request->accepted;
    });
    agent.SetKeyCallback([owner](const std::wstring &project, const std::wstring &phone,
                                 const std::wstring &key, std::wstring *error) {
        auto request = std::make_shared<RemoteKeyMessage>(project, phone, key);
        if (!request->completed)
        {
            if (error)
                *error = L"无法创建按键同步事件";
            return false;
        }
        auto *envelope = new (std::nothrow) std::shared_ptr<RemoteKeyMessage>(request);
        if (!envelope || !owner ||
            !PostMessageW(owner, WM_REMOTE_KEY, 0, reinterpret_cast<LPARAM>(envelope)))
        {
            delete envelope;
            if (error)
                *error = L"桌面窗口未就绪";
            return false;
        }
        if (WaitForSingleObject(request->completed, 30000) != WAIT_OBJECT_0)
        {
            request->cancelled = true;
            if (error)
                *error = L"电脑端处理按键超时";
            return false;
        }
        if (!request->accepted && error)
            *error = request->error.empty() ? L"终端未接受按键" : request->error;
        return request->accepted;
    });
    agent.SetControlCallback([owner](const std::wstring &project, const std::wstring &phone,
                                     const std::wstring &action, std::wstring *error) {
        auto *message = new (std::nothrow) RemoteControlMessage{project, phone, action};
        if (!message || !owner ||
            !PostMessageW(owner, WM_REMOTE_CONTROL, 0, reinterpret_cast<LPARAM>(message)))
        {
            delete message;
            if (error)
                *error = L"桌面窗口未就绪";
            return false;
        }
        return true;
    });
    agent.SetPairingCallback([owner](const aicli::agent::PendingPairing &pairing) {
        auto *message = new (std::nothrow) aicli::agent::PendingPairing(pairing);
        if (!message || !owner ||
            !PostMessageW(owner, WM_PAIRING_PENDING, 0, reinterpret_cast<LPARAM>(message)))
            delete message;
    });
    agent.SetStatusCallback([owner](bool online, const std::wstring &text) {
        auto *message = new (std::nothrow) AgentStatusMessage{online, text};
        if (!message || !owner ||
            !PostMessageW(owner, WM_AGENT_STATUS, 0, reinterpret_cast<LPARAM>(message)))
            delete message;
    });
}

bool HandleMessage(UINT message, LPARAM payload, const UiCallbacks &ui, DataStore &store,
                   agent::ConversationBridge &conversations)
{
    switch (message)
    {
    case WM_REMOTE_INPUT: {
        std::unique_ptr<std::shared_ptr<RemoteInputMessage>> envelope(
            reinterpret_cast<std::shared_ptr<RemoteInputMessage> *>(payload));
        if (!envelope || !*envelope)
            return true;
        const auto request = *envelope;
        auto complete = [&](bool accepted, const std::wstring &error = {}) {
            request->accepted = accepted;
            request->error = error;
            SetEvent(request->completed);
        };
        if (request->cancelled)
        {
            complete(false, L"输入请求已超时");
            return true;
        }
        if (!ui.select_project(request->project_id))
        {
            complete(false, L"项目或会话不存在");
            return true;
        }
        if (!ui.has_session(request->project_id))
            ui.start_session(false);
        if (request->cancelled)
        {
            complete(false, L"输入请求已超时");
            return true;
        }
        if (!ui.has_session(request->project_id))
        {
            complete(false, L"终端会话未就绪");
            return true;
        }
        std::wstring text = request->input;
        bool submit = false;
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
        {
            submit = true;
            text.pop_back();
        }
        const bool accepted = submit ? ui.submit_input(request->project_id, text)
                                     : ui.write_input(request->project_id, text);
        if (accepted)
        {
            conversations.RecordAndBroadcast(
                request->project_id, L"terminal.input",
                nlohmann::json{{"input", aicli::text::Utf8Text(text)},
                               {"phoneId", aicli::text::Utf8Text(request->phone_id)}});
            complete(true);
        }
        else
            complete(false, L"终端输入失败");
        return true;
    }
    case WM_REMOTE_KEY: {
        std::unique_ptr<std::shared_ptr<RemoteKeyMessage>> envelope(
            reinterpret_cast<std::shared_ptr<RemoteKeyMessage> *>(payload));
        if (!envelope || !*envelope)
            return true;
        const auto request = *envelope;
        auto complete = [&](bool accepted, const std::wstring &error = {}) {
            request->accepted = accepted;
            request->error = error;
            SetEvent(request->completed);
        };
        if (request->cancelled)
        {
            complete(false, L"按键请求已超时");
            return true;
        }
        if (!ui.select_project(request->project_id))
        {
            complete(false, L"项目或会话不存在");
            return true;
        }
        if (!ui.has_session(request->project_id))
            ui.start_session(false);
        if (request->cancelled)
        {
            complete(false, L"按键请求已超时");
            return true;
        }
        if (!ui.has_session(request->project_id))
        {
            complete(false, L"终端会话未就绪");
            return true;
        }
        UINT virtualKey = 0;
        if (request->key == L"left")
            virtualKey = VK_LEFT;
        else if (request->key == L"right")
            virtualKey = VK_RIGHT;
        else if (request->key == L"up")
            virtualKey = VK_UP;
        else if (request->key == L"down")
            virtualKey = VK_DOWN;
        else if (request->key == L"enter")
            virtualKey = VK_RETURN;
        if (!virtualKey)
        {
            complete(false, L"不支持的终端按键");
            return true;
        }
        if (ui.send_key(request->project_id, virtualKey))
            complete(true);
        else
            complete(false, L"终端按键发送失败");
        return true;
    }
    case WM_REMOTE_CONTROL: {
        std::unique_ptr<RemoteControlMessage> request(
            reinterpret_cast<RemoteControlMessage *>(payload));
        if (request)
            ui.control(*request);
        return true;
    }
    case WM_REMOTE_CREATE_PROJECT: {
        std::unique_ptr<std::shared_ptr<RemoteCreateProjectMessage>> envelope(
            reinterpret_cast<std::shared_ptr<RemoteCreateProjectMessage> *>(payload));
        if (!envelope || !*envelope)
            return true;
        const auto request = *envelope;
        if (request->cancelled)
        {
            request->result.http_status = 504;
            request->result.error = L"创建项目请求已超时";
            SetEvent(request->completed);
            return true;
        }
        request->result = aicli::mobile_projects::CreateProject(store, request->request);
        if (request->result.success)
            ui.project_created(request->result);
        SetEvent(request->completed);
        return true;
    }
    case WM_PAIRING_PENDING: {
        std::unique_ptr<agent::PendingPairing> request(
            reinterpret_cast<agent::PendingPairing *>(payload));
        if (request)
            ui.pairing(*request);
        return true;
    }
    case WM_AGENT_STATUS: {
        std::unique_ptr<AgentStatusMessage> request(
            reinterpret_cast<AgentStatusMessage *>(payload));
        if (request)
            ui.status(request->online, request->text);
        else
            ui.status(true, L"");
        return true;
    }
    default:
        return false;
    }
}

} // namespace aicli::app::native_remote
