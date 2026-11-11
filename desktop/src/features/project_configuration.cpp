#include "features/project_configuration.h"
#include "data/default_commands.h"

#include <algorithm>

namespace aicli::project_configuration
{
std::wstring DefaultCommand(const std::vector<ProjectType> &types, const std::wstring &type)
{
    const auto found = std::find_if(types.begin(), types.end(), [&](const ProjectType &value) {
        return _wcsicmp(value.name.c_str(), type.c_str()) == 0;
    });
    if (found != types.end())
        return found->default_command;
    return type == L"Claude" ? L"claude --dangerously-skip-permissions --continue; if "
                               L"($LASTEXITCODE -ne 0) { claude --dangerously-skip-permissions }"
                             : kDefaultCodexCommand;
}
} // namespace aicli::project_configuration
