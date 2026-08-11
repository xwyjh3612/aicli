#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aicli::agent {
class ConversationBridge;
class TerminalStreamPublisher;
}

namespace aicli::terminal {
class XamlIslandAdapter;
}

namespace aicli::agent {

// Coalesces high-frequency terminal output notifications on worker threads
// and publishes normalized snapshots from the UI thread.
class TerminalSnapshotPublisher {
public:
    void Attach(HWND owner, UINT dirty_message) noexcept;
    void Detach() noexcept;

    void Queue(const std::wstring& project_id);
    void MarkDirtyMessageHandled() noexcept;
    bool TakeDirty() noexcept;
    void Publish(aicli::terminal::XamlIslandAdapter& terminals,
                 ConversationBridge& conversations,
                 TerminalStreamPublisher& streams);

private:
    std::atomic<HWND> owner_{};
    std::atomic<UINT> dirty_message_{};
    std::mutex mutex_;
    std::vector<std::wstring> pending_projects_;
    std::unordered_map<std::wstring, std::wstring> last_snapshots_;
    std::atomic_bool dirty_{};
    std::atomic_bool message_pending_{};
};

} // namespace aicli::agent
