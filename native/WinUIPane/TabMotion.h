#pragma once
#include <chrono>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Composition.h>
#include "../../bridge/v2/shell_core.h"

// WinUI adapters for ShellCore's structural motion vocabulary. Keep decorative
// glow timings independent; group disclosure has a slightly longer duration.
struct TabMotion {
    static constexpr auto Duration = std::chrono::milliseconds(TAB_MOTION_MS);
    static constexpr auto GroupDuration = std::chrono::milliseconds(GROUP_MOTION_MS);
    static auto Ease() {
        winrt::Microsoft::UI::Xaml::Media::Animation::CubicEase ease;
        ease.EasingMode(winrt::Microsoft::UI::Xaml::Media::Animation::EasingMode::EaseOut);
        return ease;
    }
    static auto CompositionEase(winrt::Microsoft::UI::Composition::Compositor const& compositor) {
        return compositor.CreateCubicBezierEasingFunction({1.f / 3.f, 1.f}, {2.f / 3.f, 1.f});
    }
};
