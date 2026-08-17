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
    static constexpr int kTelemetryWidth = 276;

    FooterStatusController() = default;
    ~FooterStatusController();

    FooterStatusController(const FooterStatusController&) = delete;
    FooterStatusController& operator=(const FooterStatusController&) = delete;

    void Attach(HWND owner);
    void Detach();
    void Layout(const RECT& client, int footer_height);

    const RECT& git_bounds() const noexcept { return git_bounds_; }
    const RECT& phone_bounds() const noexcept { return phone_bounds_; }
    int telemetry_left() const noexcept { return telemetry_left_; }
    bool IsPhonePoint(POINT client_point) const noexcept;

    bool OnMouseMove(POINT client_point, const ThemePalette& theme);
    void OnMouseLeave();
    bool HandleTimer(UINT_PTR timer_id);

    bool RefreshRepository(std::wstring_view project_folder);
    void BeginRepositoryRefresh(std::wstring_view project_folder);
    void CompleteRepositoryRefresh(
        std::wstring_view project_folder,
        aicli::git::RepositoryStatus repository);
    bool AdvanceRepositorySpinner();
    bool repository_loading() const noexcept { return repository_loading_; }
    bool UpdateTelemetry(bool mobile_access_enabled,
                         bool agent_online,
                         std::size_t memory_megabytes,
                         std::size_t online_devices,
                         std::size_t authorized_devices);

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
    void EnsureHoverCard();
    void ShowHoverCard();
    void HideHoverCard();
    void PositionHoverCard();
    void PaintHoverCard(HDC dc) const;
    void RecreateHoverCardFonts();
    static LRESULT CALLBACK HoverCardProc(HWND window,
                                         UINT message,
                                         WPARAM wparam,
                                         LPARAM lparam);
    void InvalidateFooter(bool immediate = false) const;

    HWND owner_{};
    HWND hover_card_{};
    RECT git_bounds_{};
    RECT phone_area_{};
    mutable RECT phone_bounds_{};
    int telemetry_left_{14};
    int footer_height_{30};
    // RepositoryStatus::root is empty for non-Git folders. Keep the selected
    // project folder separately so switching between projects is still a
    // state change even when their displayed Git state happens to match.
    std::wstring project_folder_;
    std::wstring tooltip_text_;
    POINT hover_screen_point_{};
    ThemePalette hover_theme_{};
    HFONT hover_font_{};
    HFONT hover_title_font_{};
    int hover_font_dpi_{};
    bool git_hovered_{};
    bool phone_hovered_{};
    bool hover_card_visible_{};
    aicli::git::RepositoryStatus repository_;
    bool repository_loading_{};
    unsigned repository_spinner_frame_{};
    bool telemetry_initialized_{};
    bool mobile_access_enabled_{};
    bool agent_online_{};
    std::size_t memory_megabytes_{};
    std::size_t online_devices_{};
    std::size_t authorized_devices_{};
};

} // namespace aicli::ui
