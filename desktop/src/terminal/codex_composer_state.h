#pragma once

#include <algorithm>
#include <cwctype>
#include <string_view>

namespace aicli::terminal {

// Codex permits a custom status line, so a live composer cannot be recognized
// solely by the English default "context left" footer. Only inspect the final
// rendered footer; a former composer in scrollback must not qualify a shell.
inline bool HasLiveCodexComposer(std::wstring_view buffer) noexcept {
    const auto last = buffer.find_last_not_of(L" \t\r\n");
    if (last == std::wstring_view::npos) return false;
    buffer = buffer.substr(0, last + 1);
    const auto footer_start = buffer.rfind(L'\n');
    if (footer_start == std::wstring_view::npos) return false;
    const auto footer = buffer.substr(footer_start + 1);
    const bool recognized_footer = footer.find(L"context left") != std::wstring_view::npos ||
        footer.find(L"? for shortcuts") != std::wstring_view::npos ||
        footer.find(L" \x00b7 ") != std::wstring_view::npos;
    if (!recognized_footer || footer.find(L"PS ") != std::wstring_view::npos) return false;
    for (const auto mode : {L"NORMAL", L"INSERT", L"VISUAL", L"REPLACE"})
        if (footer.find(mode) != std::wstring_view::npos) return false;
    // Question/settings views have their own cancellation semantics. Their
    // keyboard legend must not be treated as the ordinary task composer.
    for (const std::wstring_view legend : {L"enter to confirm", L"enter to select", L"enter confirm",
        L"enter select", L"esc to cancel", L"esc cancel", L"esc to close", L"esc close",
        L"esc to dismiss", L"esc dismiss", L"esc back", L"esc skip", L"switch questions"}) {
        if (std::search(footer.begin(), footer.end(), legend.begin(), legend.end(),
            [](wchar_t left, wchar_t right) { return std::towlower(left) == std::towlower(right); }) != footer.end())
            return false;
    }
    const auto prompt = buffer.rfind(L'\x203a', footer_start);
    if (prompt == std::wstring_view::npos || footer_start - prompt >= 8192) return false;
    const auto composer = buffer.substr(prompt, footer_start - prompt);
    return std::count(composer.begin(), composer.end(), L'\n') <= 32;
}

} // namespace aicli::terminal
