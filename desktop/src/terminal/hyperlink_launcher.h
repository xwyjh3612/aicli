#pragma once

#include <string_view>
#include <windows.h>
#include <shellapi.h>
#include <winrt/Windows.Foundation.h>

namespace aicli::terminal {

// TermControl only raises an event for Ctrl+click. Its host must hand the
// original URL to the shell, preserving encoded routes, queries and fragments.
inline bool OpenTerminalWebLink(
    HWND owner, std::wstring_view value,
    decltype(&ShellExecuteW) execute = &ShellExecuteW) noexcept {
    try {
        if (value.empty() || !execute) return false;
        for (const wchar_t character : value) {
            if (character <= L'\x20' || character == L'\x7f') return false;
        }
        const winrt::hstring target{value};
        const winrt::Windows::Foundation::Uri uri{target};
        const auto scheme = uri.SchemeName();
        // CLI output can contain arbitrary OSC 8 links. Only launch web URLs,
        // never shell commands, executables or unrelated protocol handlers.
        if ((scheme != L"http" && scheme != L"https") || uri.Host().empty())
            return false;
        return reinterpret_cast<INT_PTR>(execute(
            owner, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    } catch (...) {
        return false;
    }
}

} // namespace aicli::terminal
