#pragma once
#ifdef AICLI_UPDATE_TESTING
#include "app/auto_update.h"
#include "app/update_manifest.h"
#include <atomic>
#include <filesystem>

namespace aicli::update::testing {
struct Services {
    std::function<Release()> check;
    std::function<std::filesystem::path(std::atomic<bool>&, std::atomic<int>&)> download;
    std::function<bool(const std::filesystem::path&, const std::wstring&)> launch;
};
void SetServices(Services services);
void BeginAutomaticCheck();
void Poll();
}
#endif
