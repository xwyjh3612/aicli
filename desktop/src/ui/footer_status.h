#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "features/git_repository.h"
#include "ui/theme.h"

namespace aicli::ui {

// Owns the bottom bar's Git state, telemetry values and Git tooltip.  It only
// invalidates the footer when displayed data changes, preventing timer-driven
// repaints from flashing the rest of the main window.
class FooterStatusController {
public:
    static constexpr int kTelemetryWidth = 344;

    FooterStatusController() = default;
    ~FooterStatusController();

    FooterStatusController(const FooterStatusController&) = delete;
    FooterStatusController& operator=(const FooterStatusController&) = delete;

    void Attach(HWND owner);
    void Detach();
    void Layout(const RECT& client, int footer_height);

    const RECT& git_bounds() const noexcept { return git_bounds_; }

    bool RefreshRepository(std::wstring_view project_folder);
    void BeginRepositoryRefresh(std::wstring_view project_folder);
    void CompleteRepositoryRefresh(
        std::wstring_view project_folder,
        aicli::git::RepositoryStatus repository);
    bool AdvanceRepositorySpinner();
    bool repository_loading() const noexcept { return repository_loading_; }
    bool UpdateTelemetry(bool agent_online,
                         std::size_t memory_megabytes,
                         bool remote_configured);

    void Draw(HDC dc,
              const RECT& client,
              int footer_height,
              HFONT font,
              HFONT git_font,
              const ThemePalette& theme) const;

private:
    std::wstring GitLabel() const;
    std::wstring GitStateText() const;
    COLORREF GitColor(const ThemePalette& theme) const;
    void UpdateTooltip();
    void InvalidateFooter(bool immediate = false) const;

    HWND owner_{};
    HWND tooltip_{};
    RECT git_bounds_{};
    int telemetry_left_{14};
    int footer_height_{30};
    // RepositoryStatus::root is empty for non-Git folders. Keep the selected
    // project folder separately so switching between projects is still a
    // state change even when their displayed Git state happens to match.
    std::wstring project_folder_;
    std::wstring tooltip_text_;
    aicli::git::RepositoryStatus repository_;
    bool repository_loading_{};
    unsigned repository_spinner_frame_{};
    bool telemetry_initialized_{};
    bool agent_online_{};
    std::size_t memory_megabytes_{};
    bool remote_configured_{};
};

} // namespace aicli::ui
