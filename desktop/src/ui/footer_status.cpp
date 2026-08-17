#include "ui/footer_status.h"

#include <dwmapi.h>

#include <algorithm>
#include <sstream>

#include "ui/drawing_utils.h"

namespace {

constexpr UINT_PTR kGitHoverTimerId = 0xA1C31;
constexpr UINT kGitHoverDelayMs = 350;
constexpr wchar_t kGitHoverCardClass[] = L"AiCliGitHoverCard";

void AppendPart(std::wstring& text, std::wstring part) {
    if (part.empty()) return;
    if (!text.empty()) text += L" · ";
    text += std::move(part);
}

} // namespace

namespace aicli::ui {

FooterStatusController::~FooterStatusController() {
    Detach();
}

void FooterStatusController::Attach(HWND owner) {
    if (owner_ == owner) return;
    Detach();
    owner_ = owner;
}

void FooterStatusController::Detach() {
    if (owner_ && IsWindow(owner_)) KillTimer(owner_, kGitHoverTimerId);
    if (hover_card_ && IsWindow(hover_card_)) DestroyWindow(hover_card_);
    hover_card_ = nullptr;
    if (hover_font_) DeleteObject(hover_font_);
    if (hover_title_font_) DeleteObject(hover_title_font_);
    hover_font_ = nullptr;
    hover_title_font_ = nullptr;
    hover_font_dpi_ = 0;
    git_hovered_ = false;
    hover_card_visible_ = false;
    owner_ = nullptr;
}

bool FooterStatusController::OnMouseMove(
    POINT client_point,
    const ThemePalette& theme) {
    if (!owner_ || !IsWindow(owner_)) return false;
    const bool inside = PtInRect(&git_bounds_, client_point) != FALSE;
    if (!inside) {
        if (git_hovered_ || hover_card_visible_) HideHoverCard();
        return false;
    }

    hover_theme_ = theme;
    if (!git_hovered_) {
        git_hovered_ = true;
        // Anchor the card once when the pointer first enters the Git label.
        // Repositioning and invalidating the popup for every WM_MOUSEMOVE made
        // its text visibly flicker while the pointer was still inside the
        // same small footer region.
        hover_screen_point_ = client_point;
        ClientToScreen(owner_, &hover_screen_point_);
        SetTimer(owner_, kGitHoverTimerId, kGitHoverDelayMs, nullptr);
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, owner_, 0};
        TrackMouseEvent(&tracking);
    }
    return true;
}

void FooterStatusController::OnMouseLeave() {
    HideHoverCard();
}

bool FooterStatusController::HandleTimer(UINT_PTR timer_id) {
    if (timer_id != kGitHoverTimerId) return false;
    if (owner_ && IsWindow(owner_)) KillTimer(owner_, kGitHoverTimerId);
    if (!git_hovered_) return true;

    POINT cursor{};
    GetCursorPos(&cursor);
    POINT client = cursor;
    ScreenToClient(owner_, &client);
    if (!PtInRect(&git_bounds_, client)) {
        HideHoverCard();
        return true;
    }
    // Keep the original entry point as the stable card anchor. The cursor may
    // move anywhere inside the Git label without moving or repainting it.
    ShowHoverCard();
    return true;
}

void FooterStatusController::Layout(const RECT& client, int footer_height) {
    footer_height_ = std::max(1, footer_height);
    const UINT dpi = owner_ && IsWindow(owner_) ? GetDpiForWindow(owner_) : 96;
    const auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
    const int footer_top = std::max(
        0, static_cast<int>(client.bottom) - footer_height_);
    telemetry_left_ = std::max(
        scale(14), static_cast<int>(client.right) - scale(kTelemetryWidth) + scale(8));
    const int git_left = scale(14);
    const int git_right = std::max(
        git_left, std::min(telemetry_left_ - scale(12),
                           git_left + scale(300)));
    const RECT next{git_left, footer_top, git_right, client.bottom};
    if (!EqualRect(&git_bounds_, &next)) {
        git_bounds_ = next;
        UpdateTooltip();
    }
    phone_bounds_={telemetry_left_+scale(108),footer_top,
                   std::max(telemetry_left_+scale(108),
                            static_cast<int>(client.right)-scale(10)),
                   client.bottom};
}

bool FooterStatusController::IsPhonePoint(POINT client_point) const noexcept {
    return PtInRect(&phone_bounds_,client_point)!=FALSE;
}

bool FooterStatusController::RefreshRepository(
    std::wstring_view project_folder) {
    const std::wstring next_project_folder(project_folder);
    const auto next = aicli::git::InspectRepository(project_folder);
    const bool project_changed = next_project_folder != project_folder_;
    const bool changed = project_changed || repository_loading_ ||
                         next != repository_;
    project_folder_ = next_project_folder;
    repository_ = next;
    repository_loading_ = false;
    if (changed) {
        UpdateTooltip();
        if (project_changed && owner_ && IsWindow(owner_)) {
            RECT client{};
            GetClientRect(owner_, &client);
            RECT footer{
                0,
                std::max(0, static_cast<int>(client.bottom) - footer_height_),
                client.right,
                client.bottom};
            RedrawWindow(owner_, &footer, nullptr,
                         RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
        } else {
            InvalidateFooter();
        }
    }
    return changed;
}

void FooterStatusController::BeginRepositoryRefresh(
    std::wstring_view project_folder) {
    project_folder_ = std::wstring(project_folder);
    repository_ = {};
    repository_loading_ = true;
    repository_spinner_frame_ = 0;
    UpdateTooltip();
    InvalidateFooter(true);
}

void FooterStatusController::CompleteRepositoryRefresh(
    std::wstring_view project_folder,
    aicli::git::RepositoryStatus repository) {
    if (project_folder != project_folder_) return;
    repository_ = std::move(repository);
    repository_loading_ = false;
    UpdateTooltip();
    InvalidateFooter(true);
}

bool FooterStatusController::AdvanceRepositorySpinner() {
    if (!repository_loading_) return false;
    repository_spinner_frame_ = (repository_spinner_frame_ + 1) % 8;
    InvalidateFooter();
    return true;
}

bool FooterStatusController::UpdateTelemetry(bool mobile_access_enabled,
                                             bool agent_online,
                                             std::size_t memory_megabytes,
                                             std::size_t online_devices,
                                             std::size_t authorized_devices) {
    const bool changed = !telemetry_initialized_ ||
                         mobile_access_enabled_ != mobile_access_enabled ||
                         agent_online_ != agent_online ||
                         memory_megabytes_ != memory_megabytes ||
                         online_devices_ != online_devices ||
                         authorized_devices_ != authorized_devices;
    if (!changed) return false;
    telemetry_initialized_ = true;
    mobile_access_enabled_ = mobile_access_enabled;
    agent_online_ = agent_online;
    memory_megabytes_ = memory_megabytes;
    online_devices_ = std::min(online_devices,authorized_devices);
    authorized_devices_ = authorized_devices;
    InvalidateFooter();
    return true;
}

std::wstring FooterStatusController::GitLabel() const {
    std::wstring label = L"git";
    if (!repository_.branch.empty()) label += L"  " + repository_.branch;
    AppendPart(label, GitStateText());
    if (repository_.ahead_count > 0)
        AppendPart(label, L"↑" + std::to_wstring(repository_.ahead_count) +
                              L" 待推送");
    if (repository_.behind_count > 0)
        AppendPart(label, L"↓" + std::to_wstring(repository_.behind_count) +
                              L" 待拉取");
    return label;
}

std::wstring FooterStatusController::GitStateText() const {
    if (repository_loading_) {
        return std::wstring({wchar_t(0x6B63), wchar_t(0x5728),
                             wchar_t(0x8BFB), wchar_t(0x53D6)});
    }
    switch (repository_.state) {
    case aicli::git::RepositoryState::InvalidDirectory:
        return L"目录无效";
    case aicli::git::RepositoryState::GitUnavailable:
        return L"Git 不可用";
    case aicli::git::RepositoryState::HasCommits:
        if (repository_.conflicted_count > 0)
            return std::to_wstring(repository_.conflicted_count) + L" 个冲突";
        if (repository_.merging) return L"正在合并";
        if (repository_.rebasing) return L"正在变基";
        if (repository_.cherry_picking) return L"正在拣选提交";
        if (repository_.pending_change_count() > 0)
            return std::to_wstring(repository_.pending_change_count()) +
                   L" 项未提交";
        if (repository_.ahead_count == 0 && repository_.behind_count == 0 &&
            repository_.has_upstream)
            return L"已同步";
        if (!repository_.has_remote) return L"工作区干净 · 未设置远程";
        if (!repository_.has_upstream) return L"工作区干净 · 未设置上游";
        return L"工作区干净";
    case aicli::git::RepositoryState::NoCommits:
        return repository_.pending_change_count() > 0
            ? L"尚无首次提交 · " +
                  std::to_wstring(repository_.pending_change_count()) +
                  L" 项待提交"
            : L"尚无首次提交";
    case aicli::git::RepositoryState::NotInitialized:
        return L"未创建";
    case aicli::git::RepositoryState::Unavailable:
        break;
    }
    return L"○ 未选择项目";
}

COLORREF FooterStatusController::GitColor(
    const ThemePalette& theme) const {
    if (repository_loading_) return theme.accent;
    if (repository_.conflicted_count > 0)
        return theme.danger;
    if (repository_.pending_change_count() > 0 ||
        repository_.ahead_count > 0 || repository_.behind_count > 0)
        return theme.warning;
    switch (repository_.state) {
    case aicli::git::RepositoryState::InvalidDirectory:
    case aicli::git::RepositoryState::GitUnavailable:
        return theme.danger;
    case aicli::git::RepositoryState::HasCommits:
        return RGB(63, 194, 130);
    case aicli::git::RepositoryState::NoCommits:
        return RGB(241, 174, 65);
    case aicli::git::RepositoryState::NotInitialized:
        return RGB(226, 91, 105);
    case aicli::git::RepositoryState::Unavailable:
        break;
    }
    return theme.muted;
}

void FooterStatusController::UpdateTooltip() {
    if (!owner_ || !IsWindow(owner_)) return;
    const std::wstring root = !repository_.root.empty()
        ? repository_.root
        : (!project_folder_.empty() ? project_folder_
                                    : L"未选择有效项目目录");
    const std::wstring push = repository_.last_push.empty()
        ? L"未记录"
        : repository_.last_push;
    const std::wstring pull = repository_.last_pull.empty()
        ? L"未记录"
        : repository_.last_pull;
    const std::wstring branch = repository_.branch.empty()
        ? L"未识别"
        : repository_.branch;
    const std::wstring remote = repository_.remote_url.empty()
        ? L"未配置"
        : repository_.remote_url;
    const std::wstring commit = repository_.last_commit.empty()
        ? L"暂无提交"
        : repository_.last_commit;
    tooltip_text_ = L"目录：" + root + L"\n分支：" + branch +
                    L"\n状态：" + GitStateText() +
                    L"\n已暂存：" + std::to_wstring(repository_.staged_count) +
                    L"\n已修改：" + std::to_wstring(repository_.modified_count) +
                    L"\n未跟踪：" + std::to_wstring(repository_.untracked_count) +
                    L"\n冲突：" + std::to_wstring(repository_.conflicted_count) +
                    L"\n领先/落后：↑" +
                    std::to_wstring(repository_.ahead_count) + L" ↓" +
                    std::to_wstring(repository_.behind_count) +
                    L"\n远程：" + remote +
                    L"\n最近提交：" + commit +
                    L"\n最后推送：" + push + L"\n最后拉取：" + pull;
    if (hover_card_visible_ && hover_card_ && IsWindow(hover_card_)) {
        InvalidateRect(hover_card_, nullptr, FALSE);
    }
}

void FooterStatusController::EnsureHoverCard() {
    if (hover_card_ && IsWindow(hover_card_)) return;
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    window_class.lpfnWndProc = HoverCardProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kGitHoverCardClass;
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return;
    }
    hover_card_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kGitHoverCardClass, L"", WS_POPUP,
        0, 0, 1, 1, owner_, nullptr, GetModuleHandleW(nullptr), this);
    if (!hover_card_) return;
    BOOL dark = hover_theme_.light() ? FALSE : TRUE;
    DwmSetWindowAttribute(hover_card_, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &dark, sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(hover_card_, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corners, sizeof(corners));
}

void FooterStatusController::RecreateHoverCardFonts() {
    const int dpi = owner_ && IsWindow(owner_)
        ? static_cast<int>(GetDpiForWindow(owner_))
        : 96;
    if (dpi == hover_font_dpi_ && hover_font_ && hover_title_font_) return;
    if (hover_font_) DeleteObject(hover_font_);
    if (hover_title_font_) DeleteObject(hover_title_font_);
    hover_font_ = CreateFontW(
        -MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    hover_title_font_ = CreateFontW(
        -MulDiv(14, dpi, 96), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    hover_font_dpi_ = dpi;
}

void FooterStatusController::PositionHoverCard() {
    if (!hover_card_ || !IsWindow(hover_card_)) return;
    RecreateHoverCardFonts();
    const UINT dpi = owner_ && IsWindow(owner_) ? GetDpiForWindow(owner_) : 96;
    const auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
    const int width = scale(470);
    const int height = scale(286);
    const int gap = scale(14);
    HMONITOR monitor = MonitorFromPoint(hover_screen_point_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    int x = hover_screen_point_.x + gap;
    int y = hover_screen_point_.y - height - gap;
    if (x + width > info.rcWork.right) x = hover_screen_point_.x - width - gap;
    if (y < info.rcWork.top) y = hover_screen_point_.y + gap;
    x = std::clamp(x, static_cast<int>(info.rcWork.left),
                   std::max(static_cast<int>(info.rcWork.left),
                            static_cast<int>(info.rcWork.right) - width));
    y = std::clamp(y, static_cast<int>(info.rcWork.top),
                   std::max(static_cast<int>(info.rcWork.top),
                            static_cast<int>(info.rcWork.bottom) - height));
    ApplyRoundedRegion(hover_card_, width, height, scale(12));
    SetWindowPos(hover_card_, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                     (hover_card_visible_ ? SWP_SHOWWINDOW : 0));
}

void FooterStatusController::ShowHoverCard() {
    EnsureHoverCard();
    if (!hover_card_) return;
    hover_card_visible_ = true;
    PositionHoverCard();
    ShowWindow(hover_card_, SW_SHOWNOACTIVATE);
    RedrawWindow(hover_card_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
}

void FooterStatusController::HideHoverCard() {
    if (owner_ && IsWindow(owner_)) KillTimer(owner_, kGitHoverTimerId);
    git_hovered_ = false;
    hover_card_visible_ = false;
    if (hover_card_ && IsWindow(hover_card_)) ShowWindow(hover_card_, SW_HIDE);
}

void FooterStatusController::PaintHoverCard(HDC target) const {
    if (!hover_card_ || !target) return;
    RECT client{};
    GetClientRect(hover_card_, &client);
    const UINT dpi = owner_ && IsWindow(owner_) ? GetDpiForWindow(owner_) : 96;
    const auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
    DrawSmoothRoundedRect(target, client, static_cast<float>(scale(11)),
                          hover_theme_.surface, hover_theme_.border,
                          std::max(1.0f, static_cast<float>(dpi) / 96.0f));
    SetBkMode(target, TRANSPARENT);

    const int left = scale(18);
    const int right = client.right - scale(18);
    RECT title{left + scale(15), scale(13), right, scale(39)};
    HGDIOBJ previous_font = hover_title_font_
        ? SelectObject(target, hover_title_font_)
        : nullptr;
    SetTextColor(target, hover_theme_.text);
    DrawTextW(target, L"Git 仓库信息", -1, &title,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    HBRUSH status_brush = CreateSolidBrush(GitColor(hover_theme_));
    RECT status_dot{left, scale(21), left + scale(7), scale(28)};
    FillRect(target, &status_dot, status_brush);
    DeleteObject(status_brush);

    if (hover_font_) SelectObject(target, hover_font_);
    std::wistringstream lines(tooltip_text_);
    std::wstring line;
    int y = scale(47);
    const int row_height = scale(20);
    const int label_width = scale(76);
    while (std::getline(lines, line) && y + row_height <= client.bottom - scale(10)) {
        const auto separator = line.find(L'：');
        RECT row{left, y, right, y + row_height};
        if (separator == std::wstring::npos) {
            SetTextColor(target, hover_theme_.text);
            DrawTextW(target, line.c_str(), -1, &row,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            const std::wstring label = line.substr(0, separator + 1);
            const std::wstring value = line.substr(separator + 1);
            RECT label_rect = row;
            label_rect.right = std::min(row.right, row.left + label_width);
            SetTextColor(target, hover_theme_.muted);
            DrawTextW(target, label.c_str(), -1, &label_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT value_rect = row;
            value_rect.left = label_rect.right;
            SetTextColor(target, hover_theme_.text);
            DrawTextW(target, value.c_str(), -1, &value_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        y += row_height;
    }
    if (previous_font) SelectObject(target, previous_font);
}

LRESULT CALLBACK FooterStatusController::HoverCardProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    auto* self = reinterpret_cast<FooterStatusController*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<FooterStatusController*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (self) self->PaintHoverCard(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void FooterStatusController::InvalidateFooter(bool immediate) const {
    if (!owner_ || !IsWindow(owner_)) return;
    RECT client{};
    GetClientRect(owner_, &client);
    RECT footer{
        0,
        std::max(0, static_cast<int>(client.bottom) - footer_height_),
        client.right,
        client.bottom};
    if (immediate) {
        RedrawWindow(owner_, &footer, nullptr,
                     RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    } else {
        InvalidateRect(owner_, &footer, FALSE);
    }
}

void FooterStatusController::Draw(HDC dc,
                                  const RECT& client,
                                  int footer_height,
                                  HFONT font,
                                  HFONT git_font,
                                  const ThemePalette& theme) const {
    HGDIOBJ previous_font = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    const UINT dpi = owner_ && IsWindow(owner_) ? GetDpiForWindow(owner_) : 96;
    const auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };

    if (git_bounds_.right > git_bounds_.left) {
        if (git_font) SelectObject(dc, git_font);
        SetTextColor(dc, GitColor(theme));
        RECT git_rect = git_bounds_;
        git_rect.left += scale(4);
        git_rect.right -= scale(4);
        if (repository_loading_) {
            RECT prefix = git_rect;
            prefix.right = prefix.left + scale(24);
            constexpr wchar_t git_text[] = {wchar_t(0x67), wchar_t(0x69),
                                             wchar_t(0x74), wchar_t(0)};
            DrawTextW(dc, git_text, -1, &prefix,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            const int center_x = prefix.right + scale(10);
            const int center_y = (git_rect.top + git_rect.bottom) / 2;
            const POINT points[8] = {
                {0, -scale(6)}, {scale(4), -scale(4)}, {scale(6), 0}, {scale(4), scale(4)},
                {0, scale(6)}, {-scale(4), scale(4)}, {-scale(6), 0}, {-scale(4), -scale(4)}};
            const unsigned phase = repository_spinner_frame_ % 8;
            for (unsigned step = 0; step < 8; ++step) {
                const POINT point = points[(phase + step) % 8];
                const unsigned strength = 255u - step * 25u;
                const COLORREF color = RGB(
                    (GetRValue(theme.panel) * (255u - strength) +
                     GetRValue(theme.accent) * strength) / 255u,
                    (GetGValue(theme.panel) * (255u - strength) +
                     GetGValue(theme.accent) * strength) / 255u,
                    (GetBValue(theme.panel) * (255u - strength) +
                     GetBValue(theme.accent) * strength) / 255u);
                HBRUSH dot = CreateSolidBrush(color);
                RECT dot_rect{center_x + point.x - scale(1),
                              center_y + point.y - scale(1),
                              center_x + point.x + scale(2),
                              center_y + point.y + scale(2)};
                FillRect(dc, &dot_rect, dot);
                DeleteObject(dot);
            }

            RECT loading = git_rect;
            loading.left = center_x + scale(14);
            constexpr wchar_t loading_text[] = {
                wchar_t(0x6B63), wchar_t(0x5728), wchar_t(0x8BFB),
                wchar_t(0x53D6), wchar_t(0)};
            DrawTextW(dc, loading_text, -1, &loading,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                          DT_END_ELLIPSIS);
        } else {
            const auto label = GitLabel();
            DrawTextW(dc, label.c_str(), -1, &git_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                          DT_END_ELLIPSIS);
        }
    }

    // Use one 12 px footer font for Git, memory and mobile status.
    if (git_font) SelectObject(dc, git_font);
    else if (font) SelectObject(dc, font);

    const int telemetry_top = client.bottom - footer_height;
    const std::wstring memory_label =
        L"内存 " + std::to_wstring(memory_megabytes_) + L" MB";
    const std::wstring phone_label = !mobile_access_enabled_
        ? L"手机访问未开启"
        : (!agent_online_
            ? L"手机服务异常"
            : L"手机 " + std::to_wstring(online_devices_) + L"/" +
                  std::to_wstring(authorized_devices_) + L" 在线");
    RECT memory_rect{
        telemetry_left_, telemetry_top, telemetry_left_ + scale(108), client.bottom};
    RECT phone_rect=phone_bounds_;
    SetTextColor(dc, theme.muted);
    DrawTextW(dc, memory_label.c_str(), -1, &memory_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc,!mobile_access_enabled_
        ? theme.muted
        : (!agent_online_?theme.danger:
            (online_devices_>0?RGB(83,203,146):theme.muted)));
    DrawTextW(dc,phone_label.c_str(),-1,&phone_rect,
              DT_LEFT|DT_VCENTER|DT_SINGLELINE);

    if (previous_font) SelectObject(dc, previous_font);
}

} // namespace aicli::ui
