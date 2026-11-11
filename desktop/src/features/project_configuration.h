#pragma once

#include <string>
#include <vector>
#include "core/models.h"

namespace aicli::project_configuration
{
std::wstring DefaultCommand(const std::vector<ProjectType> &types, const std::wstring &type);
}
