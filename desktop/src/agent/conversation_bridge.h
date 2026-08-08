#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/models.h"

namespace aicli {
class DataStore;
}

namespace aicli::agent {
class RemoteAgent;

// Owns the persistent/event-facing side of desktop conversations.  UI code
// supplies project models and terminal snapshots; serialization, SQLite event
// writes and WSS broadcasts stay in one place.
class ConversationBridge {
public:
    ConversationBridge(DataStore& store, RemoteAgent& agent);

    void RecordAndBroadcast(const std::wstring& project_id,
                            const std::wstring& event_type,
                            const nlohmann::json& payload);
    void PublishTerminalSnapshot(const std::wstring& project_id,
                                 const std::wstring& snapshot);
    void RefreshConversations(const std::vector<Project>& projects);

    std::string ConversationsJson() const;
    std::string EventsJson(const std::wstring& project_id,
                           std::int64_t after_event_id) const;

private:
    DataStore& store_;
    RemoteAgent& agent_;
    mutable std::mutex conversations_mutex_;
    std::string conversations_cache_;
};

} // namespace aicli::agent
