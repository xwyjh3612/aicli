#include "ui/taskbar_working_indicator.h"

#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace aicli::ui {
namespace {

constexpr int kOverlaySize = 16;
constexpr int kOverlayDotCount = 8;
constexpr UINT kOverlayFrameIntervalMs = 120;
constexpr UINT_PTR kOverlayTimerId = 0xA1C14;
constexpr wchar_t kOverlayIndicatorProperty[] = L"AiCli.TaskbarWorkingIndicator";

void BlendPixel(std::uint32_t& destination, BYTE red, BYTE green, BYTE blue,
                double alpha) {
    alpha = std::clamp(alpha, 0.0, 1.0);
    if (alpha <= 0.0) return;
    const double destination_alpha = static_cast<double>((destination >> 24) & 0xff) / 255.0;
    const double output_alpha = alpha + destination_alpha * (1.0 - alpha);
    if (output_alpha <= 0.0) return;
    const auto channel = [&](int shift, BYTE source) {
        const double existing = static_cast<double>((destination >> shift) & 0xff);
        const double value = (static_cast<double>(source) * alpha +
                              existing * destination_alpha * (1.0 - alpha)) /
                             output_alpha;
        return static_cast<std::uint32_t>(std::clamp(value, 0.0, 255.0) + 0.5);
    };
    const std::uint32_t out_red = channel(16, red);
    const std::uint32_t out_green = channel(8, green);
    const std::uint32_t out_blue = channel(0, blue);
    const std::uint32_t out_alpha = static_cast<std::uint32_t>(
        std::clamp(output_alpha * 255.0, 0.0, 255.0) + 0.5);
    destination = (out_alpha << 24) | (out_red << 16) | (out_green << 8) | out_blue;
}

HICON CreateOverlaySpinnerFrame(std::size_t frame) {
    BITMAPV5HEADER bitmap_info{};
    bitmap_info.bV5Size = sizeof(bitmap_info);
    bitmap_info.bV5Width = kOverlaySize;
    bitmap_info.bV5Height = -kOverlaySize;
    bitmap_info.bV5Planes = 1;
    bitmap_info.bV5BitCount = 32;
    bitmap_info.bV5Compression = BI_BITFIELDS;
    bitmap_info.bV5RedMask = 0x00ff0000;
    bitmap_info.bV5GreenMask = 0x0000ff00;
    bitmap_info.bV5BlueMask = 0x000000ff;
    bitmap_info.bV5AlphaMask = 0xff000000;

    void* pixels_raw = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(
        screen, reinterpret_cast<BITMAPINFO*>(&bitmap_info), DIB_RGB_COLORS,
        &pixels_raw, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !pixels_raw) {
        if (color) DeleteObject(color);
        return nullptr;
    }

    auto* pixels = static_cast<std::uint32_t*>(pixels_raw);
    std::fill(pixels, pixels + kOverlaySize * kOverlaySize, 0u);
    constexpr double pi = 3.14159265358979323846;
    constexpr double center = (kOverlaySize - 1) / 2.0;
    constexpr double ring_radius = 5.25;
    constexpr double dot_radius = 1.35;
    constexpr int samples = 4;
    for (int dot = 0; dot < kOverlayDotCount; ++dot) {
        const int phase = (dot - static_cast<int>(frame) + kOverlayDotCount * 2) %
                          kOverlayDotCount;
        const double opacity = 0.18 + 0.82 *
            (1.0 - static_cast<double>(phase) / (kOverlayDotCount - 1));
        const double angle = -pi / 2.0 + 2.0 * pi * dot / kOverlayDotCount;
        const double dot_x = center + std::cos(angle) * ring_radius;
        const double dot_y = center + std::sin(angle) * ring_radius;
        const int left = std::max(0, static_cast<int>(std::floor(dot_x - dot_radius - 1.0)));
        const int right = std::min(kOverlaySize - 1,
                                   static_cast<int>(std::ceil(dot_x + dot_radius + 1.0)));
        const int top = std::max(0, static_cast<int>(std::floor(dot_y - dot_radius - 1.0)));
        const int bottom = std::min(kOverlaySize - 1,
                                    static_cast<int>(std::ceil(dot_y + dot_radius + 1.0)));
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                int covered = 0;
                for (int sample_y = 0; sample_y < samples; ++sample_y) {
                    for (int sample_x = 0; sample_x < samples; ++sample_x) {
                        const double px = x + (sample_x + 0.5) / samples;
                        const double py = y + (sample_y + 0.5) / samples;
                        const double dx = px - dot_x;
                        const double dy = py - dot_y;
                        if (dx * dx + dy * dy <= dot_radius * dot_radius) ++covered;
                    }
                }
                const double coverage = static_cast<double>(covered) /
                                        (samples * samples);
                BlendPixel(pixels[y * kOverlaySize + x], 83, 156, 255,
                           opacity * coverage);
            }
        }
    }

    std::array<BYTE, kOverlaySize * 2> mask_bits{};
    HBITMAP mask = CreateBitmap(kOverlaySize, kOverlaySize, 1, 1,
                                mask_bits.data());
    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color;
    icon_info.hbmMask = mask;
    HICON icon = mask ? CreateIconIndirect(&icon_info) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

} // namespace

TaskbarWorkingIndicator::~TaskbarWorkingIndicator() {
    Detach();
}

void TaskbarWorkingIndicator::Attach(HWND window) {
    if (window_ == window) {
        Apply();
        return;
    }
    Detach();
    window_ = window;
    SetPropW(window_, kOverlayIndicatorProperty, this);
    Apply();
}

void TaskbarWorkingIndicator::SetStyle(TaskbarWorkingStyle style) {
    if (style_ == style) return;
    style_ = style;
    Apply();
}

void TaskbarWorkingIndicator::SetWorking(bool working) {
    if (working_ == working) return;
    working_ = working;
    Apply();
}

void TaskbarWorkingIndicator::OnTaskbarButtonCreated() {
    // Explorer can restart while AI CLI stays open. Recreate the COM object
    // and restore the current state when the new taskbar button appears.
    if (taskbar_) {
        taskbar_->Release();
        taskbar_ = nullptr;
    }
    Apply();
}

void TaskbarWorkingIndicator::Detach() {
    StopOverlayAnimation();
    ClearTaskbarVisuals();
    if (taskbar_) {
        taskbar_->Release();
        taskbar_ = nullptr;
    }
    if (window_) RemovePropW(window_, kOverlayIndicatorProperty);
    window_ = nullptr;
    working_ = false;
    overlay_frame_ = 0;
    DestroyOverlayFrames();
}

void CALLBACK TaskbarWorkingIndicator::OverlayTimerProc(
    HWND window, UINT, UINT_PTR timer_id, DWORD) {
    auto* indicator = reinterpret_cast<TaskbarWorkingIndicator*>(
        GetPropW(window, kOverlayIndicatorProperty));
    if (indicator && indicator->overlay_timer_id_ == timer_id) {
        indicator->AdvanceOverlayFrame();
    }
}

bool TaskbarWorkingIndicator::EnsureInterface() {
    if (taskbar_) return true;
    ITaskbarList3* taskbar = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&taskbar))) ||
        !taskbar) {
        return false;
    }
    if (FAILED(taskbar->HrInit())) {
        taskbar->Release();
        return false;
    }
    taskbar_ = taskbar;
    return true;
}

bool TaskbarWorkingIndicator::EnsureOverlayFrames() {
    if (overlay_frames_.front()) return true;
    for (std::size_t index = 0; index < overlay_frames_.size(); ++index) {
        overlay_frames_[index] = CreateOverlaySpinnerFrame(index);
        if (!overlay_frames_[index]) {
            DestroyOverlayFrames();
            return false;
        }
    }
    return true;
}

void TaskbarWorkingIndicator::StartOverlayAnimation() {
    if (!window_ || overlay_timer_id_) return;
    overlay_timer_id_ = SetTimer(
        window_, kOverlayTimerId, kOverlayFrameIntervalMs, OverlayTimerProc);
}

void TaskbarWorkingIndicator::StopOverlayAnimation() {
    if (window_ && overlay_timer_id_) KillTimer(window_, overlay_timer_id_);
    overlay_timer_id_ = 0;
}

void TaskbarWorkingIndicator::AdvanceOverlayFrame() {
    if (!working_ || style_ != TaskbarWorkingStyle::OverlaySpinner ||
        !window_ || !EnsureInterface() || !EnsureOverlayFrames()) {
        StopOverlayAnimation();
        return;
    }
    overlay_frame_ = (overlay_frame_ + 1) % overlay_frames_.size();
    taskbar_->SetOverlayIcon(window_, overlay_frames_[overlay_frame_],
                             L"AI CLI 正在处理任务");
}

void TaskbarWorkingIndicator::ClearTaskbarVisuals() {
    if (!taskbar_ || !window_) return;
    taskbar_->SetProgressState(window_, TBPF_NOPROGRESS);
    taskbar_->SetOverlayIcon(window_, nullptr, L"");
}

void TaskbarWorkingIndicator::DestroyOverlayFrames() {
    for (HICON& icon : overlay_frames_) {
        if (icon) DestroyIcon(icon);
        icon = nullptr;
    }
}

void TaskbarWorkingIndicator::Apply() {
    if (!window_ || !EnsureInterface()) return;
    if (!working_) {
        StopOverlayAnimation();
        ClearTaskbarVisuals();
        return;
    }
    if (style_ == TaskbarWorkingStyle::ProgressBar) {
        StopOverlayAnimation();
        taskbar_->SetOverlayIcon(window_, nullptr, L"");
        taskbar_->SetProgressState(window_, TBPF_INDETERMINATE);
        return;
    }
    taskbar_->SetProgressState(window_, TBPF_NOPROGRESS);
    if (!EnsureOverlayFrames()) return;
    taskbar_->SetOverlayIcon(window_, overlay_frames_[overlay_frame_],
                             L"AI CLI 正在处理任务");
    StartOverlayAnimation();
}

} // namespace aicli::ui
