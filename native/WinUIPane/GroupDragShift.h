#pragma once

// WinUI-only easing; ShellCore supplies the insertion offsets for each block.
struct GroupDragShift {
    struct Entry {
        FrameworkElement element{nullptr};
        Media::Transform previous{nullptr};
        Media::TranslateTransform shift;
        Media::Animation::Storyboard animation{nullptr};
        double target{};
    };
    std::map<uint64_t,Entry> entries;
    double Offset(uint64_t id) const {
        auto found=entries.find(id);
        return found==entries.end() ? 0 : found->second.shift.X();
    }
    void Apply(uint64_t id, FrameworkElement const& element, double target, bool animate) {
        auto found=entries.find(id);
        if (found==entries.end()) {
            if (!target) return;
            auto& entry=entries[id];
            entry.element=element; entry.previous=element.RenderTransform();
            Media::TransformGroup transforms;
            if (entry.previous) transforms.Children().Append(entry.previous);
            transforms.Children().Append(entry.shift);
            element.RenderTransform(transforms);
            found=entries.find(id);
        }
        auto& entry=found->second;
        if (entry.target==target) return;
        const auto from=entry.shift.X();
        if (entry.animation) entry.animation.Stop();
        entry.target=target; entry.shift.X(target);
        if (!animate || !element.IsLoaded()) { entry.animation=nullptr; return; }
        Media::Animation::DoubleAnimation motion;
        motion.From(from); motion.To(target);
        motion.Duration(Duration{Windows::Foundation::TimeSpan{std::chrono::milliseconds(135)},DurationType::TimeSpan});
        Media::Animation::CubicEase ease;
        ease.EasingMode(Media::Animation::EasingMode::EaseOut); motion.EasingFunction(ease);
        Media::Animation::Storyboard::SetTarget(motion,entry.shift);
        Media::Animation::Storyboard::SetTargetProperty(motion,L"X");
        entry.animation=Media::Animation::Storyboard();
        entry.animation.Children().Append(motion); entry.animation.Begin();
    }
    void Reset() noexcept {
        for (auto& [id,entry] : entries) {
            try {
                if (entry.animation) entry.animation.Stop();
                entry.element.RenderTransform(entry.previous);
            } catch (...) {}
        }
        entries.clear();
    }
    ~GroupDragShift() { Reset(); }
};
