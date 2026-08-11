#include "core/text_utils.h"

#include <windows.h>

#include <iterator>
#include <utility>

namespace aicli::text {
namespace {

std::wstring NormalizeTerminalText(std::wstring value,
                                   size_t max_characters,
                                   size_t max_lines) {
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

    size_t start = normalized.size() > max_characters
        ? normalized.size() - max_characters
        : 0;
    size_t lines = normalized.empty() ? 0 : 1;
    for (size_t index = normalized.size(); index > start; --index) {
        if (normalized[index - 1] == L'\n' && ++lines > max_lines) {
            start = index;
            break;
        }
    }
    if (start > 0) {
        if (normalized[start - 1] != L'\n') {
            const auto next = normalized.find(L'\n', start);
            start = next == std::wstring::npos ? normalized.size() : next + 1;
        }
        normalized.erase(0, start);
    }
    return normalized;
}

} // namespace

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

std::wstring LocalComputerName() {
    wchar_t value[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = static_cast<DWORD>(std::size(value));
    if (GetComputerNameW(value, &size) && size > 0) {
        return std::wstring(value, size);
    }
    return L"AI CLI 电脑";
}

std::wstring NormalizeTerminalSnapshot(std::wstring value) {
    return NormalizeTerminalText(std::move(value), 32 * 1024, 300);
}

std::wstring NormalizeTerminalHistorySnapshot(std::wstring value) {
    // The phone keeps at most 1000 logical terminal rows. This larger copy is
    // held in memory for paging; SQLite continues to receive the compact
    // 300-row recovery snapshot above.
    return NormalizeTerminalText(std::move(value), 1024 * 1024, 1000);
}

} // namespace aicli::text
