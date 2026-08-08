#include "features/git_repository.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "core/text_utils.h"

namespace aicli::git {
namespace {

std::string ReadFirstLine(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::string line;
    if (stream) std::getline(stream, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

std::wstring FormatFileTime(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return {};
    const auto file_time = std::filesystem::last_write_time(path, error);
    if (error) return {};
    const auto system_time =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            file_time - decltype(file_time)::clock::now() +
            std::chrono::system_clock::now());
    const std::time_t value = std::chrono::system_clock::to_time_t(system_time);
    tm local{};
    if (localtime_s(&local, &value) != 0) return {};
    wchar_t buffer[64]{};
    if (!std::wcsftime(buffer, std::size(buffer), L"%Y-%m-%d %H:%M", &local)) {
        return {};
    }
    return buffer;
}

std::wstring LatestLogTime(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return {};
    std::filesystem::file_time_type latest{};
    bool found = false;
    std::filesystem::recursive_directory_iterator iterator(directory, error), end;
    for (; !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error)) continue;
        const auto value = iterator->last_write_time(error);
        if (error) continue;
        if (!found || value > latest) {
            latest = value;
            found = true;
        }
    }
    if (!found) return {};
    const auto system_time =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            latest - decltype(latest)::clock::now() +
            std::chrono::system_clock::now());
    const std::time_t value = std::chrono::system_clock::to_time_t(system_time);
    tm local{};
    if (localtime_s(&local, &value) != 0) return {};
    wchar_t buffer[64]{};
    if (!std::wcsftime(buffer, std::size(buffer), L"%Y-%m-%d %H:%M", &local)) {
        return {};
    }
    return buffer;
}

std::filesystem::path ResolveGitDirectory(
    const std::filesystem::path& project_folder) {
    std::error_code error;
    auto current = std::filesystem::weakly_canonical(project_folder, error);
    if (error || !std::filesystem::is_directory(current, error)) return {};
    for (;;) {
        const auto marker = current / L".git";
        if (std::filesystem::is_directory(marker, error)) return marker;
        error.clear();
        if (std::filesystem::is_regular_file(marker, error)) {
            const auto line = ReadFirstLine(marker);
            constexpr std::string_view prefix = "gitdir:";
            if (line.rfind(prefix, 0) == 0) {
                std::string value = line.substr(prefix.size());
                while (!value.empty() &&
                       (value.front() == ' ' || value.front() == '\t')) {
                    value.erase(value.begin());
                }
                std::filesystem::path git_directory(aicli::text::WideText(value));
                if (git_directory.is_relative()) git_directory = current / git_directory;
                auto resolved = std::filesystem::weakly_canonical(git_directory, error);
                if (!error && std::filesystem::is_directory(resolved, error)) {
                    return resolved;
                }
            }
        }
        const auto parent = current.parent_path();
        if (parent.empty() || parent == current) break;
        current = parent;
        error.clear();
    }
    return {};
}

std::filesystem::path ResolveCommonDirectory(
    const std::filesystem::path& git_directory) {
    const auto value = ReadFirstLine(git_directory / L"commondir");
    if (value.empty()) return git_directory;
    std::error_code error;
    std::filesystem::path common(aicli::text::WideText(value));
    if (common.is_relative()) common = git_directory / common;
    auto resolved = std::filesystem::weakly_canonical(common, error);
    return error ? git_directory : resolved;
}

bool IsObjectId(std::string_view value) {
    if (value.size() < 7) return false;
    bool nonzero = false;
    for (char character : value) {
        const bool hexadecimal =
            (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f') ||
            (character >= 'A' && character <= 'F');
        if (!hexadecimal) return false;
        if (character != '0') nonzero = true;
    }
    return nonzero;
}

bool HasCommit(const std::filesystem::path& git_directory) {
    const auto head = ReadFirstLine(git_directory / L"HEAD");
    if (head.empty()) return false;
    constexpr std::string_view reference_prefix = "ref: ";
    if (head.rfind(reference_prefix, 0) != 0) return IsObjectId(head);

    const std::string reference = head.substr(reference_prefix.size());
    const auto common = ResolveCommonDirectory(git_directory);
    for (const auto& root : {git_directory, common}) {
        const auto value = ReadFirstLine(
            root / std::filesystem::path(aicli::text::WideText(reference)));
        if (IsObjectId(value)) return true;
    }

    std::ifstream packed(common / L"packed-refs", std::ios::binary);
    std::string line;
    while (std::getline(packed, line)) {
        if (line.empty() || line.front() == '#' || line.front() == '^') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto separator = line.find(' ');
        if (separator != std::string::npos &&
            line.substr(separator + 1) == reference &&
            IsObjectId(std::string_view(line).substr(0, separator))) {
            return true;
        }
    }
    return false;
}

} // namespace

RepositoryStatus InspectRepository(std::wstring_view project_folder) {
    RepositoryStatus status;
    if (project_folder.empty()) return status;

    std::error_code error;
    const std::filesystem::path folder(project_folder);
    if (!std::filesystem::is_directory(folder, error)) return status;

    const auto git_directory = ResolveGitDirectory(folder);
    if (git_directory.empty()) {
        status.state = RepositoryState::NotInitialized;
        return status;
    }

    status.state = HasCommit(git_directory)
        ? RepositoryState::HasCommits
        : RepositoryState::NoCommits;
    const auto common = ResolveCommonDirectory(git_directory);
    status.root = (common.filename() == L".git"
                       ? common.parent_path()
                       : git_directory.parent_path())
                      .wstring();
    status.last_pull = FormatFileTime(common / L"FETCH_HEAD");
    if (status.last_pull.empty()) {
        status.last_pull = LatestLogTime(common / L"logs" / L"refs" / L"remotes");
    }
    // Git has no reliable local push timestamp.  In-app pushes may update this
    // marker without inventing data for external pushes.
    status.last_push = FormatFileTime(common / L"aicli-last-push");
    return status;
}

} // namespace aicli::git
