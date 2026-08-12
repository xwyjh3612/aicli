#pragma once

#include <string>
#include <string_view>

namespace aicli::git {

enum class RepositoryState {
    Unavailable,
    InvalidDirectory,
    GitUnavailable,
    NotInitialized,
    NoCommits,
    HasCommits,
};

struct RepositoryStatus {
    RepositoryState state{RepositoryState::Unavailable};
    std::wstring root;
    std::wstring branch;
    std::wstring upstream;
    std::wstring last_push;
    std::wstring last_pull;
    int staged_count{};
    int modified_count{};
    int tracked_change_count{};
    int untracked_count{};
    int conflicted_count{};
    int ahead_count{};
    int behind_count{};
    bool has_remote{};
    bool has_upstream{};
    bool detached_head{};
    bool merging{};
    bool rebasing{};
    bool cherry_picking{};

    bool operator==(const RepositoryStatus&) const = default;

    int pending_change_count() const noexcept {
        return tracked_change_count + untracked_count +
               conflicted_count;
    }
};

RepositoryStatus InspectRepository(std::wstring_view project_folder);

} // namespace aicli::git
