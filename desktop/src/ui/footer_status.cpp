#include "ui/footer_status.h"

#include <commctrl.h>

#include <algorithm>

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
    const int footer_top = std::max(
        0, static_cast<int>(client.bottom) - footer_height_);
    telemetry_left_ = std::max(
        14, static_cast<int>(client.right) - kTelemetryWidth + 8);
    const int git_right = std::max(14, telemetry_left_ - 8);
    const int git_left = std::max(14, git_right - 214);
    const RECT next{git_left, footer_top, git_right, client.bottom};
    if (!EqualRect(&git_bounds_, &next)) {
        git_bounds_ = next;
        UpdateTooltip();
    }
}

bool FooterStatusController::RefreshRepository(
    std::wstring_view project_folder) {
    const auto next = aicli::git::InspectRepository(project_folder);
    const bool changed = next.state != repository_.state ||
                         next.root != repository_.root ||
                         next.last_push != repository_.last_push ||
                         next.last_pull != repository_.last_pull;
    repository_ = next;
    if (changed) {
        InvalidateFooter();
        UpdateTooltip();
    }
    return changed;
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
    return L"git  " + GitStateText();
}

std::wstring FooterStatusController::GitStateText() const {
    switch (repository_.state) {
    case aicli::git::RepositoryState::HasCommits:
        return L"● 已提交";
    case aicli::git::RepositoryState::NoCommits:
        return L"● 尚无提交";
    case aicli::git::RepositoryState::NotInitialized:
        return L"○ 未初始化";
    case aicli::git::RepositoryState::Unavailable:
        break;
    }
    return L"○ 未选择项目";
}

COLORREF FooterStatusController::GitColor(
    const ThemePalette& theme) const {
    switch (repository_.state) {
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
    const std::wstring root = repository_.root.empty()
        ? L"未选择有效项目目录"
        : repository_.root;
    const std::wstring push = repository_.last_push.empty()
        ? L"未记录"
        : repository_.last_push;
    const std::wstring pull = repository_.last_pull.empty()
        ? L"未记录"
        : repository_.last_pull;
    tooltip_text_ = L"git\n目录：" + root + L"\n状态：" + GitStateText() +
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

void FooterStatusController::InvalidateFooter() const {
    if (!owner_ || !IsWindow(owner_)) return;
    RECT client{};
    GetClientRect(owner_, &client);
    RECT footer{
        0,
        std::max(0, static_cast<int>(client.bottom) - footer_height_),
        client.right,
        client.bottom};
    InvalidateRect(owner_, &footer, FALSE);
}

void FooterStatusController::Draw(HDC dc,
                                  const RECT& client,
                                  int footer_height,
                                  HFONT font,
                                  const ThemePalette& theme) const {
    HGDIOBJ previous_font = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);

    if (git_bounds_.right > git_bounds_.left) {
        SetTextColor(dc, GitColor(theme));
        RECT git_rect = git_bounds_;
        git_rect.left += 4;
        git_rect.right -= 4;
        const auto label = GitLabel();
        DrawTextW(dc, label.c_str(), -1, &git_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

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
        telemetry_left_, telemetry_top, telemetry_left_ + 116, client.bottom};
    RECT memory_rect{
        agent_rect.right, telemetry_top, agent_rect.right + 108, client.bottom};
    RECT relay_rect{
        memory_rect.right, telemetry_top, client.right - 10, client.bottom};
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
