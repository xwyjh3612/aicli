#include "agent/conversation_bridge.h"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "agent/remote_agent.h"
#include "core/text_utils.h"
#include "data/data_store.h"

namespace aicli::agent {

ConversationBridge::ConversationBridge(DataStore& store, RemoteAgent& agent)
    : store_(store), agent_(agent) {}

void ConversationBridge::RecordAndBroadcast(
    const std::wstring& project_id,
    const std::wstring& event_type,
    const nlohmann::json& payload) {
    if (project_id.empty()) return;
    const std::string payload_json = payload.dump(
        -1, ' ', false, nlohmann::json::error_handler_t::replace);
    const auto id = store_.AppendEvent(project_id, event_type, payload_json);
    nlohmann::json event{
        {"id", id},
        {"eventId", id},
        {"conversationId", aicli::text::Utf8Text(project_id)},
        {"event_type", aicli::text::Utf8Text(event_type)},
        {"type", aicli::text::Utf8Text(event_type)},
        {"payload", payload},
    };
    agent_.BroadcastEvent(
        project_id,
        event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

void ConversationBridge::PublishTerminalSnapshot(
    const std::wstring& project_id, const std::wstring& snapshot) {
    if (project_id.empty() || snapshot.empty()) return;
    store_.SaveTerminalSnapshot(project_id, snapshot);
    // SQLite keeps a bounded plain-text recovery snapshot, but live phones
    // receive the original VT stream. Broadcasting the complete flattened
    // buffer on every TUI repaint caused duplicate content, large frames and
    // Android layout flashes.
}

void ConversationBridge::RefreshConversations(
    const std::vector<Project>& projects) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& project : projects) {
        std::string provider = aicli::text::Utf8Text(project.type);
        std::transform(provider.begin(), provider.end(), provider.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        items.push_back({
            {"id", aicli::text::Utf8Text(project.id)},
            {"name", aicli::text::Utf8Text(project.name)},
            {"provider", provider},
            {"type", aicli::text::Utf8Text(project.type)},
            {"group", aicli::text::Utf8Text(project.group)},
            {"folder", aicli::text::Utf8Text(project.folder)},
            {"command", aicli::text::Utf8Text(project.command)},
            {"status", aicli::text::Utf8Text(project.status)},
        });
    }
    std::scoped_lock lock(conversations_mutex_);
    conversations_cache_ = nlohmann::json{{"items", items}}.dump();
}

std::string ConversationBridge::ConversationsJson() const {
    std::scoped_lock lock(conversations_mutex_);
    return conversations_cache_.empty() ? "{\"items\":[]}" : conversations_cache_;
}

std::string ConversationBridge::EventsJson(
    const std::wstring& project_id, std::int64_t after_event_id) const {
    nlohmann::json items = nlohmann::json::array();
    const auto snapshot = store_.LoadTerminalSnapshot(project_id);
    if (snapshot) {
        items.push_back({
            {"conversationId", aicli::text::Utf8Text(project_id)},
            {"event_type", "terminal.snapshot"},
            {"type", "terminal.snapshot"},
            {"payload", {{"text", aicli::text::Utf8Text(*snapshot)}, {"replace", true}}},
        });
    }
    for (const auto& event :
         store_.LoadEvents(project_id, after_event_id, 1000, !snapshot.has_value())) {
        nlohmann::json payload;
        try {
            payload = nlohmann::json::parse(event.payload_json);
        } catch (...) {
            payload = {{"text", event.payload_json}};
        }
        items.push_back({
            {"id", event.id},
            {"eventId", event.id},
            {"conversationId", aicli::text::Utf8Text(event.project_id)},
            {"event_type", aicli::text::Utf8Text(event.event_type)},
            {"type", aicli::text::Utf8Text(event.event_type)},
            {"payload", payload},
            {"createdAt", event.created_at},
        });
    }
    return nlohmann::json{{"items", items}}.dump();
}

} // namespace aicli::agent
