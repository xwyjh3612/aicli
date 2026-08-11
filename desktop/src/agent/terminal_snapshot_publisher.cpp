#include "agent/terminal_snapshot_publisher.h"

#include <algorithm>

#include "agent/conversation_bridge.h"
#include "agent/terminal_stream_publisher.h"
#include "core/text_utils.h"
#include "terminal/xaml_island_adapter.h"

namespace aicli::agent {

void TerminalSnapshotPublisher::Attach(HWND owner, UINT dirty_message) noexcept {
    dirty_message_.store(dirty_message);
    owner_.store(owner);
}

void TerminalSnapshotPublisher::Detach() noexcept {
    owner_.store(nullptr);
    dirty_message_.store(0);
    message_pending_.store(false);
}

void TerminalSnapshotPublisher::Queue(const std::wstring& project_id) {
    const HWND owner = owner_.load();
    const UINT message = dirty_message_.load();
    if (project_id.empty() || !owner || !message) return;
    {
        std::scoped_lock lock(mutex_);
        if (std::find(pending_projects_.begin(), pending_projects_.end(),
                      project_id) == pending_projects_.end()) {
            pending_projects_.push_back(project_id);
        }
    }
    dirty_.store(true);
    if (!message_pending_.exchange(true) &&
        !PostMessageW(owner, message, 0, 0)) {
        message_pending_.store(false);
    }
}

void TerminalSnapshotPublisher::MarkDirtyMessageHandled() noexcept {
    message_pending_.store(false);
}

bool TerminalSnapshotPublisher::TakeDirty() noexcept {
    return dirty_.exchange(false);
}

void TerminalSnapshotPublisher::Publish(
    aicli::terminal::XamlIslandAdapter& terminals,
    ConversationBridge& conversations,
    TerminalStreamPublisher& streams) {
    std::vector<std::wstring> project_ids;
    {
        std::scoped_lock lock(mutex_);
        project_ids.swap(pending_projects_);
    }
    for (const auto& project_id : project_ids) {
        std::wstring raw;
        std::uint64_t snapshot_sequence{};
        // Avoid assigning output that raced after ReadEntireBuffer() to the
        // snapshot. A stable pass is normally obtained immediately; retrying
        // also keeps the bootstrap boundary exact during a busy TUI repaint.
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto before = streams.CurrentSequence(project_id);
            raw = terminals.ReadEntireBuffer(project_id);
            const auto after = streams.CurrentSequence(project_id);
            snapshot_sequence = before;
            if (before == after) {
                snapshot_sequence = after;
                break;
            }
        }
        auto history_snapshot =
            aicli::text::NormalizeTerminalHistorySnapshot(raw);
        if (!history_snapshot.empty()) {
            streams.PublishSnapshot(project_id, history_snapshot,
                                    snapshot_sequence);
        }
        auto snapshot = aicli::text::NormalizeTerminalSnapshot(std::move(raw));
        if (snapshot.empty()) continue;
        const auto previous = last_snapshots_.find(project_id);
        if (previous != last_snapshots_.end() &&
            previous->second == snapshot) {
            continue;
        }
        last_snapshots_[project_id] = snapshot;
        conversations.PublishTerminalSnapshot(project_id, snapshot);
    }
}

} // namespace aicli::agent
