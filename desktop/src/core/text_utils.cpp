#include "core/text_utils.h"

#include <windows.h>

namespace aicli::text {

std::string Utf8Text(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count,
                        nullptr, nullptr);
    return result;
}

std::wstring WideText(std::string_view value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::wstring NormalizeTerminalSnapshot(std::wstring value) {
    std::wstring normalized;
    normalized.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        const wchar_t character = value[index];
        if (character == L'\r') {
            if (index + 1 < value.size() && value[index + 1] == L'\n') ++index;
            normalized.push_back(L'\n');
        } else {
            normalized.push_back(character);
        }
    }

    size_t line_start = 0;
    for (size_t index = 0; index <= normalized.size(); ++index) {
        if (index == normalized.size() || normalized[index] == L'\n') {
            size_t end = index;
            while (end > line_start &&
                   (normalized[end - 1] == L' ' || normalized[end - 1] == L'\t')) {
                --end;
            }
            if (end < index) normalized.erase(end, index - end);
            index = end;
            line_start = index + 1;
        }
    }

    while (!normalized.empty() &&
           (normalized.back() == L'\n' || normalized.back() == L' ' ||
            normalized.back() == L'\t')) {
        normalized.pop_back();
    }

    constexpr size_t max_characters = 32 * 1024;
    constexpr size_t max_lines = 300;
    size_t start = normalized.size() > max_characters
        ? normalized.size() - max_characters
        : 0;
    size_t lines = 0;
    for (size_t index = normalized.size(); index > start; --index) {
        if (normalized[index - 1] == L'\n' && ++lines >= max_lines) {
            start = index;
            break;
        }
    }
    if (start > 0) {
        const auto next = normalized.find(L'\n', start);
        start = next == std::wstring::npos ? normalized.size() : next + 1;
        normalized.erase(0, start);
    }
    return normalized;
}

} // namespace aicli::text
