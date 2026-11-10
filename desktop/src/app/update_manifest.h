#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aicli::update {
inline constexpr wchar_t kManifestUrl[] =
    L"https://github.com/xwyjh3612/aicli/releases/latest/download/update-manifest.json";

struct Release {
    std::string version;
    std::string url;
    std::string sha256;
    std::uint64_t size{};
};

// Invalid versions/manifests throw; never turn malformed metadata into an update.
bool IsNewerVersion(std::string_view candidate, std::string_view installed);
Release ParseDesktopRelease(std::string_view json);
}
