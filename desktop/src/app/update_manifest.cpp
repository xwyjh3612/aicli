#include "app/update_manifest.h"

#include <array>
#include <charconv>
#include <regex>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace aicli::update {
namespace {
std::array<unsigned, 4> Version(std::string_view text) {
    std::array<unsigned, 4> parts{};
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto dot = text.find('.');
        const auto part = text.substr(0, dot);
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), parts[index]);
        if (part.empty() || error != std::errc{} || end != part.data() + part.size() ||
            parts[index] > 65535 || (part.size() > 1 && part.front() == '0'))
            throw std::runtime_error("Invalid version");
        if (dot == std::string_view::npos) {
            if (index < 2) throw std::runtime_error("Incomplete version");
            return parts;
        }
        text.remove_prefix(dot + 1);
    }
    throw std::runtime_error("Invalid version component count");
}
}

bool IsNewerVersion(std::string_view candidate, std::string_view installed) {
    return Version(candidate) > Version(installed);
}

Release ParseDesktopRelease(std::string_view text) {
    const auto manifest = nlohmann::json::parse(text);
    if (manifest.at("schemaVersion") != 1) throw std::runtime_error("Unsupported manifest");
    const auto& value = manifest.at("windows");
    Release release{value.at("version").get<std::string>(), value.at("url").get<std::string>(),
                    value.at("sha256").get<std::string>(), 0};
    Version(release.version);
    if (!value.at("size").is_number_unsigned()) throw std::runtime_error("Invalid size");
    release.size = value.at("size").get<std::uint64_t>();
    const std::regex url(R"(^https://github\.com/xwyjh3612/aicli/releases/download/[A-Za-z0-9_-][A-Za-z0-9._-]*/AI-CLI-Setup-[0-9.]+-x64\.exe$)");
    if (!std::regex_match(release.url, url) ||
        !release.url.ends_with("/AI-CLI-Setup-" + release.version + "-x64.exe") ||
        !std::regex_match(release.sha256, std::regex("[a-f0-9]{64}")) ||
        release.size == 0 || release.size > 1024ULL * 1024 * 1024)
        throw std::runtime_error("Invalid release asset");
    return release;
}
}
