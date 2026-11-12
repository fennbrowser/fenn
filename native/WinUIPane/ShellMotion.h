#pragma once
#include <chrono>
#include <utility>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include "../../bridge/v2/shell_core.h"

// WinUI adapters for shared structural recipes. Keep stock control feedback
// and decorative lighting independent. Never animate layout on every frame.
struct ShellMotion {
    enum class Kind { Reposition, Enter, Exit, Fade };
    static bool Enabled() {
        return winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled();
    }
    static auto Duration(Kind kind) {
        return std::chrono::milliseconds(kind == Kind::Enter ? SHELL_MOTION_ENTER_MS :
            kind == Kind::Exit ? SHELL_MOTION_EXIT_MS : kind == Kind::Fade ? SHELL_MOTION_FADE_MS : SHELL_MOTION_REPOSITION_MS);
    }
    static auto Points(Kind kind) {
        using Point = winrt::Windows::Foundation::Point;
        if (kind == Kind::Reposition) return std::pair{Point{float(SHELL_MOTION_MOVE_X1), float(SHELL_MOTION_MOVE_Y1)},
            Point{float(SHELL_MOTION_MOVE_X2), float(SHELL_MOTION_MOVE_Y2)}};
        // Begin departing promptly instead of lingering at full opacity.
        if (kind == Kind::Exit) return std::pair{Point{.4f, 0}, Point{1, 1}};
        if (kind == Kind::Fade) return std::pair{Point{0, 0}, Point{1, 1}};
        return std::pair{Point{0, 0}, Point{0, 1}};
    }
    static auto Ease(winrt::Microsoft::UI::Composition::Compositor const& compositor, Kind kind = Kind::Reposition) {
        auto [a, b] = Points(kind);
        return compositor.CreateCubicBezierEasingFunction({a.X, a.Y}, {b.X, b.Y});
    }
    static void Append(winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard const& storyboard,
        winrt::Microsoft::UI::Xaml::DependencyObject const& target, wchar_t const* property,
        double from, double to, Kind kind) {
        namespace A = winrt::Microsoft::UI::Xaml::Media::Animation;
        A::DoubleAnimationUsingKeyFrames animation;
        A::DiscreteDoubleKeyFrame first;
        first.KeyTime({std::chrono::milliseconds(0)}); first.Value(from);
        A::SplineDoubleKeyFrame last;
        last.KeyTime({Duration(kind)}); last.Value(to);
        A::KeySpline spline;
        auto [a, b] = Points(kind);
        spline.ControlPoint1(a); spline.ControlPoint2(b); last.KeySpline(spline);
        animation.KeyFrames().Append(first); animation.KeyFrames().Append(last);
        A::Storyboard::SetTarget(animation, target);
        A::Storyboard::SetTargetProperty(animation, property);
        storyboard.Children().Append(animation);
    }
};
