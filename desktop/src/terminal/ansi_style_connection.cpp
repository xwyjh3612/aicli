#include "ansi_style_connection.h"

#include <string_view>

namespace aicli::terminal {
namespace {

constexpr std::wstring_view kMutedForeground = L"\x1b[38;2;102;102;102m";
constexpr std::wstring_view kDefaultForeground = L"\x1b[39m";
constexpr std::wstring_view kCodexInputBackground = L"\x1b[48;2;41;41;41m";
constexpr std::wstring_view kLightInputBackground = L"\x1b[48;2;232;235;239m";

void WrapMutedLiteral(std::wstring& value, std::wstring_view literal) {
    size_t position = 0;
    while ((position = value.find(literal, position)) != std::wstring::npos) {
        value.insert(position, kMutedForeground);
        position += kMutedForeground.size() + literal.size();
        value.insert(position, kDefaultForeground);
        position += kDefaultForeground.size();
    }
}

void WrapCodexVersion(std::wstring& value) {
    size_t position = 0;
    while ((position = value.find(L"(v", position)) != std::wstring::npos) {
        const auto end = value.find(L')', position + 2);
        if (end == std::wstring::npos || end - position > 32) {
            position += 2;
            continue;
        }
        value.insert(position, kMutedForeground);
        const auto after_version = end + 1 + kMutedForeground.size();
        value.insert(after_version, kDefaultForeground);
        position = after_version + kDefaultForeground.size();
    }
}

size_t SkipCsiSequence(std::wstring_view value, size_t position) {
    if (position + 1 >= value.size() || value[position] != L'\x1b' ||
        value[position + 1] != L'[') {
        return position;
    }
    position += 2;
    while (position < value.size()) {
        const auto current = value[position++];
        if (current >= 0x40 && current <= 0x7e) break;
    }
    return position;
}

void StyleCodexInputPlaceholder(std::wstring& value,
                                std::wstring& known_placeholder,
                                bool user_input_active,
                                bool light_theme) {
    const std::wstring_view input_background =
        light_theme ? kLightInputBackground : kCodexInputBackground;
    size_t block = 0;
    while ((block = value.find(input_background, block)) != std::wstring::npos) {
        size_t content = block + input_background.size();
        bool skipped_prompt_glyph = false;
        while (content < value.size()) {
            const auto after_sequence = SkipCsiSequence(value, content);
            if (after_sequence != content) {
                content = after_sequence;
                continue;
            }
            const auto current = value[content];
            if (current == L'\r' || current == L'\n' || current == L' ') {
                ++content;
                continue;
            }
            if (!skipped_prompt_glyph && current == L'›') {
                skipped_prompt_glyph = true;
                ++content;
                continue;
            }
            break;
        }

        size_t end = content;
        while (end < value.size() && value[end] != L'\r' && value[end] != L'\n') {
            if (end + 2 < value.size() && value[end] == L'\x1b' &&
                value[end + 1] == L'[' && value[end + 2] == L'K') {
                break;
            }
            ++end;
        }
        if (content < end) {
            const std::wstring visible_text{value.data() + content, end - content};
            if (skipped_prompt_glyph && !user_input_active) {
                known_placeholder = visible_text;
            }
            // Once the placeholder has been learned, matching text is the
            // placeholder even when the last user action was Backspace.  A
            // boolean "input active" flag cannot know that the editor became
            // empty again, which previously left this redraw bright until the
            // next Enter reset the flag.
            if (!known_placeholder.empty() && visible_text == known_placeholder) {
                value.insert(content, kMutedForeground);
                end += kMutedForeground.size();
                value.insert(end, kDefaultForeground);
                block = end + kDefaultForeground.size();
            } else {
                block = end;
            }
        } else {
            block = content;
        }
    }
}

bool ContainsSgrParameter(std::wstring_view parameters, std::wstring_view wanted) {
    size_t begin = 0;
    while (begin <= parameters.size()) {
        const auto end = parameters.find(L';', begin);
        const auto value = parameters.substr(
            begin, end == std::wstring_view::npos ? parameters.size() - begin : end - begin);
        if (value == wanted) return true;
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    }
    return false;
}

}

AnsiStyleConnection::AnsiStyleConnection(
    winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection inner,
    bool light_theme,
    std::function<void(std::wstring_view)> output_observer) :
    inner_(std::move(inner)), light_theme_(light_theme),
    output_observer_(std::move(output_observer)) {
    inner_output_token_ = inner_.TerminalOutput([this](const winrt::hstring& output) {
        const auto transformed = TransformOutput(output);
        if (!transformed.empty()) {
            // Keep the native terminal on the hot path. Recording/network
            // observers may touch SQLite and sockets, so invoking them first
            // makes one Codex repaint visibly pause at intermediate cursor
            // positions.
            output_handlers_(transformed);
            if (output_observer_) {
                try { output_observer_(std::wstring_view{transformed.c_str(), transformed.size()}); }
                catch (...) {}
            }
        }
    });
    inner_state_token_ = inner_.StateChanged(
        [this](const auto&, const auto&) { state_handlers_(*this, nullptr); });
}

AnsiStyleConnection::~AnsiStyleConnection() {
    try {
        if (inner_) {
            inner_.TerminalOutput(inner_output_token_);
            inner_.StateChanged(inner_state_token_);
        }
    } catch (...) {
    }
}

void AnsiStyleConnection::Initialize(
    const winrt::Windows::Foundation::Collections::ValueSet& settings) {
    inner_.Initialize(settings);
}

void AnsiStyleConnection::Start() {
    inner_.Start();
}

void AnsiStyleConnection::WriteInput(winrt::array_view<const char16_t> data) {
    if (!data.empty() && data.front() != u'\x1b') {
        for (const auto value : data) {
            if (value == u'\r' || value == u'\n') {
                user_input_active_ = false;
            } else if (value >= u' ' && value != u'\x7f') {
                user_input_active_ = true;
            }
        }
    }
    inner_.WriteInput(data);
}

void AnsiStyleConnection::Resize(uint32_t rows, uint32_t columns) {
    inner_.Resize(rows, columns);
}

void AnsiStyleConnection::Close() noexcept {
    try {
        if (inner_) inner_.Close();
    } catch (...) {
    }
}

void AnsiStyleConnection::SetLightTheme(bool light_theme) noexcept {
    light_theme_.store(light_theme, std::memory_order_relaxed);
}

winrt::event_token AnsiStyleConnection::TerminalOutput(
    const winrt::Microsoft::Terminal::TerminalConnection::TerminalOutputHandler& handler) {
    return output_handlers_.add(handler);
}

void AnsiStyleConnection::TerminalOutput(const winrt::event_token& token) noexcept {
    output_handlers_.remove(token);
}

winrt::event_token AnsiStyleConnection::StateChanged(
    const winrt::Windows::Foundation::TypedEventHandler<
        winrt::Microsoft::Terminal::TerminalConnection::ITerminalConnection,
        winrt::Windows::Foundation::IInspectable>& handler) {
    return state_handlers_.add(handler);
}

void AnsiStyleConnection::StateChanged(const winrt::event_token& token) noexcept {
    state_handlers_.remove(token);
}

winrt::guid AnsiStyleConnection::SessionId() const noexcept {
    try {
        return inner_ ? inner_.SessionId() : winrt::guid{};
    } catch (...) {
        return {};
    }
}

winrt::Microsoft::Terminal::TerminalConnection::ConnectionState
AnsiStyleConnection::State() const noexcept {
    try {
        return inner_ ? inner_.State() :
            winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Closed;
    } catch (...) {
        return winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Failed;
    }
}

winrt::hstring AnsiStyleConnection::TransformOutput(const winrt::hstring& output) {
    std::scoped_lock lock(transform_mutex_);
    const bool light_theme = light_theme_.load(std::memory_order_relaxed);
    pending_.append(output.c_str(), output.size());

    std::wstring result;
    result.reserve(pending_.size() + 32);
    size_t position = 0;
    while (position < pending_.size()) {
        const auto escape = pending_.find(L'\x1b', position);
        if (escape == std::wstring::npos) {
            result.append(pending_, position, std::wstring::npos);
            position = pending_.size();
            break;
        }
        result.append(pending_, position, escape - position);
        if (escape + 1 >= pending_.size()) {
            position = escape;
            break;
        }
        if (pending_[escape + 1] != L'[') {
            result.push_back(pending_[escape]);
            position = escape + 1;
            continue;
        }

        size_t final = escape + 2;
        while (final < pending_.size() &&
               !(pending_[final] >= 0x40 && pending_[final] <= 0x7e)) {
            ++final;
        }
        if (final >= pending_.size()) {
            position = escape;
            break;
        }

        const std::wstring_view sequence{pending_.data() + escape, final - escape + 1};
        if (pending_[final] != L'm') {
            result.append(sequence);
            position = final + 1;
            continue;
        }

        const std::wstring_view parameters{
            pending_.data() + escape + 2, final - escape - 2};
        const bool reset = parameters.empty() || ContainsSgrParameter(parameters, L"0");
        const bool normal_intensity = ContainsSgrParameter(parameters, L"22");
        const bool default_foreground = ContainsSgrParameter(parameters, L"39");

        // ConPTY normalizes Codex's faint text to SGR 90 in this hosting path.
        // TermControl's active color table renders that code too brightly, so
        // use the same explicit #666666 foreground visible in Windows Terminal.
        if (light_theme && parameters == L"48;2;41;41;41") {
            // Codex paints its composer with a fixed dark true-color surface.
            // Remap only that background in the application light theme while
            // keeping all input and rendering inside the native TermControl.
            result.append(kLightInputBackground);
        } else if (parameters == L"2" || parameters == L"90") {
            result.append(kMutedForeground);
            dim_active_ = true;
        } else {
            result.append(sequence);
            if (reset || normal_intensity || default_foreground) {
                dim_active_ = false;
            } else if (dim_active_) {
                // A later foreground-color sequence must not accidentally make
                // an active dim span bright again.
                result.append(kMutedForeground);
            }
        }
        position = final + 1;
    }

    if (position > 0) pending_.erase(0, position);
    if (result.find(L"OpenAI Codex") != std::wstring::npos) codex_ui_seen_ = true;
    if (codex_ui_seen_) {
        if (result.find(L"OpenAI Codex") != std::wstring::npos) {
            WrapCodexVersion(result);
            WrapMutedLiteral(result, L">_");
        }
        WrapMutedLiteral(result, L"model:");
        WrapMutedLiteral(result, L"directory:");
        if (result.find(L"/model") != std::wstring::npos) {
            WrapMutedLiteral(result, L"to change");
        }
        StyleCodexInputPlaceholder(result, placeholder_text_, user_input_active_.load(),
                                   light_theme);
    }
    return winrt::hstring{result};
}

}
