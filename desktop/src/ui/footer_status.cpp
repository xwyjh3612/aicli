#include "ui/footer_status.h"

#include <commctrl.h>

#include <algorithm>

namespace {

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
    if (tooltip_ && IsWindow(tooltip_)) DestroyWindow(tooltip_);
    tooltip_ = nullptr;
    owner_ = nullptr;
}

void FooterStatusController::Layout(const RECT& client, int footer_height) {
    footer_height_ = std::max(1, footer_height);
    const UINT dpi = owner_ && IsWindow(owner_) ? GetDpiForWindow(owner_) : 96;
    const auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
    const int footer_top = std::max(
        0, static_cast<int>(client.bottom) - footer_height_);
    telemetry_left_ = std::max(
        scale(14), static_cast<int>(client.right) - scale(kTelemetryWidth) + scale(8));
    const int git_right = std::max(scale(14), telemetry_left_ - scale(8));
    const int git_left = std::max(scale(14), git_right - scale(214));
    const RECT next{git_left, footer_top, git_right, client.bottom};
    if (!EqualRect(&git_bounds_, &next)) {
        git_bounds_ = next;
        UpdateTooltip();
    }
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

bool FooterStatusController::UpdateTelemetry(bool agent_online,
                                             std::size_t memory_megabytes,
                                             bool remote_configured) {
    const bool changed = !telemetry_initialized_ ||
                         agent_online_ != agent_online ||
                         memory_megabytes_ != memory_megabytes ||
                         remote_configured_ != remote_configured;
    if (!changed) return false;
    telemetry_initialized_ = true;
    agent_online_ = agent_online;
    memory_megabytes_ = memory_megabytes;
    remote_configured_ = remote_configured;
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
    tooltip_text_ = L"git\n目录：" + root + L"\n分支：" + branch +
                    L"\n状态：" + GitStateText() +
                    L"\n已暂存：" + std::to_wstring(repository_.staged_count) +
                    L"\n已修改：" + std::to_wstring(repository_.modified_count) +
                    L"\n未跟踪：" + std::to_wstring(repository_.untracked_count) +
                    L"\n冲突：" + std::to_wstring(repository_.conflicted_count) +
                    L"\n领先/落后：↑" +
                    std::to_wstring(repository_.ahead_count) + L" ↓" +
                    std::to_wstring(repository_.behind_count) +
                    L"\n最后推送：" + push + L"\n最后拉取：" + pull;
    if (!tooltip_) {
        tooltip_ = CreateWindowExW(
            WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            owner_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!tooltip_) return;
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0,
                     static_cast<LPARAM>(420));
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND;
        tool.hwnd = owner_;
        tool.uId = reinterpret_cast<UINT_PTR>(owner_);
        tool.rect = git_bounds_;
        tool.lpszText = const_cast<LPWSTR>(tooltip_text_.c_str());
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0,
                     reinterpret_cast<LPARAM>(&tool));
        return;
    }

    TOOLINFOW tool{sizeof(tool)};
    tool.uFlags = TTF_IDISHWND;
    tool.hwnd = owner_;
    tool.uId = reinterpret_cast<UINT_PTR>(owner_);
    tool.rect = git_bounds_;
    tool.lpszText = const_cast<LPWSTR>(tooltip_text_.c_str());
    SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0,
                 reinterpret_cast<LPARAM>(&tool));
    SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0,
                 reinterpret_cast<LPARAM>(&tool));
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

    // Use one 12 px footer font for Git, Agent, memory and network status.
    if (git_font) SelectObject(dc, git_font);
    else if (font) SelectObject(dc, font);

    const int telemetry_top = client.bottom - footer_height;
    const std::wstring agent_label = agent_online_
        ? L"● Agent 在线"
        : L"○ Agent 离线";
    const std::wstring memory_label =
        L"内存 " + std::to_wstring(memory_megabytes_) + L" MB";
    const std::wstring relay_label = remote_configured_
        ? L"公网已配置"
        : L"公网未配置";
    RECT agent_rect{
        telemetry_left_, telemetry_top, telemetry_left_ + scale(116), client.bottom};
    RECT memory_rect{
        agent_rect.right, telemetry_top, agent_rect.right + scale(108), client.bottom};
    RECT relay_rect{
        memory_rect.right, telemetry_top, client.right - scale(10), client.bottom};
    SetTextColor(dc, agent_online_
        ? RGB(83, 203, 146)
        : RGB(225, 101, 112));
    DrawTextW(dc, agent_label.c_str(), -1, &agent_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(dc, theme.muted);
    DrawTextW(dc, memory_label.c_str(), -1, &memory_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(dc, relay_label.c_str(), -1, &relay_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (previous_font) SelectObject(dc, previous_font);
}

} // namespace aicli::ui
