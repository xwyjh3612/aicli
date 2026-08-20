#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <windows.h>

namespace aicli::terminal {

// Owns the XAML Island and one native Windows Terminal TermControl per
// project.  The public surface deliberately contains only operations the main
// window needs; WinRT controls, scrollbar discovery and ConPTY details stay
// private to the implementation file.
class XamlIslandAdapter {
public:
    XamlIslandAdapter();
    ~XamlIslandAdapter();

    XamlIslandAdapter(const XamlIslandAdapter&) = delete;
    XamlIslandAdapter& operator=(const XamlIslandAdapter&) = delete;
    XamlIslandAdapter(XamlIslandAdapter&&) noexcept;
    XamlIslandAdapter& operator=(XamlIslandAdapter&&) noexcept;

    bool Attach(HWND parent, RECT rc);
    bool Start(const std::wstring& project_id,
               const std::wstring& command,
               const std::wstring& cwd,
               const std::wstring& title,
               HWND owner,
               HWND status,
               std::function<void(std::wstring_view)> output_observer = {},
               std::function<void(std::uint32_t, std::uint32_t)> resize_observer = {},
               std::function<void(std::wstring_view)> input_observer = {});
    bool HasSession(std::wstring_view project_id) const;
    bool Activate(std::wstring_view project_id, bool focus = true);
    void Stop(std::wstring_view project_id);
    void StopAll();
    void Shutdown() noexcept;
    void SetVisible(bool visible);
    void SetTheme(bool light);
    void RefreshTerminalViewport();
    void Resize(RECT rc);
    HWND Window() const;
    bool Active() const;
    bool Active(std::wstring_view project_id) const;
    std::wstring ReadEntireBuffer(std::wstring_view project_id = {}) const;
    bool WriteInput(const std::wstring& value);
    bool WriteInput(std::wstring_view project_id, const std::wstring& value);
    bool SubmitInput(std::wstring_view project_id, const std::wstring& value);
    bool SendKey(std::wstring_view project_id, UINT virtual_key);
    // Called by the legacy Win32 message loop before TranslateMessage. XAML
    // keyboard focus lives below the island host HWND, so terminal clipboard
    // shortcuts must be intercepted at the thread-message boundary.
    bool PreTranslateMessage(const MSG& message) noexcept;
    void UpdateNativeScrollbarHover();
    void CollapseNativeScrollbar();
    const std::wstring& LastError() const;

    // Opaque implementation type used by the XAML island subclass callback.
    // It is public only to keep the Win32 callback out of the public API while
    // allowing the implementation translation unit to name the type.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace aicli::terminal
