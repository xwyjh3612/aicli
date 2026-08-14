#pragma once

#include <string>

namespace aicli::app {

// Opens an independent Command Prompt whose working directory is the project
// folder. The new console does not share input or lifetime with an AI session.
bool OpenCommandPromptAt(const std::wstring& working_directory,
                         std::wstring* error_message = nullptr);

} // namespace aicli::app
