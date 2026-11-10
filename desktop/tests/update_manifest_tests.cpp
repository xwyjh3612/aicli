#include "app/update_manifest.h"
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>

using namespace aicli::update;
void Require(bool value) { if (!value) throw std::runtime_error("Assertion failed"); }
template<class F> void Reject(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Require(rejected);
}
int main() {
    try {
        Require(IsNewerVersion("1.0.0.10", "1.0.0.9"));
        Require(!IsNewerVersion("1.0.0.4", "1.0.0.4"));
        Require(!IsNewerVersion("1.0.0.3", "1.0.0.4"));
        Require(!IsNewerVersion("1.2.0", "1.2.0.0"));
        for (const auto bad : {"", "1.2", "1.2.3.", "1.2.3.4.5", "1.0.-1", "1.2.3-beta", "1.2.65536", "01.2.3"})
            Reject([&] { IsNewerVersion(bad, "1.0.0.4"); });
        nlohmann::json manifest = {{"schemaVersion", 1}, {"windows", {
            {"version", "1.0.0.5"}, {"size", 1024}, {"sha256", std::string(64, 'a')},
            {"url", "https://github.com/xwyjh3612/aicli/releases/download/v1.3.0/AI-CLI-Setup-1.0.0.5-x64.exe"}}}};
        Require(ParseDesktopRelease(manifest.dump()).version == "1.0.0.5");
        for (const auto bad : {"http://github.com/xwyjh3612/aicli/releases/download/v1/AI-CLI-Setup-1.0.0.5-x64.exe",
                "https://evil.example/AI-CLI-Setup-1.0.0.5-x64.exe",
                "https://github.com/other/repo/releases/download/v1/AI-CLI-Setup-1.0.0.5-x64.exe",
                "https://github.com/xwyjh3612/aicli/releases/download/v1/AI-CLI-Setup-1.0.0.4-x64.exe"}) {
            auto changed = manifest;
            changed["windows"]["url"] = bad;
            Reject([&] { ParseDesktopRelease(changed.dump()); });
        }
        for (const auto bad : {-1LL, 0LL, 1073741825LL}) {
            auto changed = manifest; changed["windows"]["size"] = bad;
            Reject([&] { ParseDesktopRelease(changed.dump()); });
        }
        auto changed = manifest; changed["schemaVersion"] = 2;
        Reject([&] { ParseDesktopRelease(changed.dump()); });
        changed = manifest; changed["windows"]["sha256"] = "invalid";
        Reject([&] { ParseDesktopRelease(changed.dump()); });
        Reject([] { ParseDesktopRelease("{}"); });
        Reject([] { ParseDesktopRelease("not json"); });
        std::cout << "Update manifest tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
