#pragma once

#include <string>
#include <string_view>

namespace aicli::text {

std::string Utf8Text(std::wstring_view value);
std::wstring WideText(std::string_view value);

// Produces the bounded plain-text snapshot used by SQLite/mobile clients.
// The native TermControl keeps its own larger scrollback independently.
std::wstring NormalizeTerminalSnapshot(std::wstring value);

} // namespace aicli::text
