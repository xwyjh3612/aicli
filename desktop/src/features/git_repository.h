#pragma once

#include <string>
#include <string_view>

namespace aicli::git {

enum class RepositoryState {
    Unavailable,
    NotInitialized,
    NoCommits,
    HasCommits,
};

struct RepositoryStatus {
    RepositoryState state{RepositoryState::Unavailable};
    std::wstring root;
    std::wstring last_push;
    std::wstring last_pull;
};

RepositoryStatus InspectRepository(std::wstring_view project_folder);

} // namespace aicli::git
