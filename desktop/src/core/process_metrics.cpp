#include "core/process_metrics.h"

#include <windows.h>
#include <psapi.h>

namespace aicli {

std::size_t WorkingSetMegabytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return 0;
    }
    return static_cast<std::size_t>(counters.WorkingSetSize / (1024ull * 1024ull));
}

} // namespace aicli
