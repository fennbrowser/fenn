#pragma once
#include <list>
#include <winrt/Windows.UI.ViewManagement.h>

// Opt-in light-up motion for group glow (Firefox's groupGlowAnimations).
// Newly lit groups are revealed spatially from their dominant light pool
// outward, geometry changes let the bright spots trail the surface, color
// changes cross-fade and extinguished glow dims before release. Everything here is a Composition
// keyframe or implicit animation: the compositor runs it, no UI-thread render
// loop starts, and static properties always hold their final values, so later
// geometry updates and inspection read the settled state.
struct TabGlowIgnition {
    using Compositor = winrt::Microsoft::UI::Composition::Compositor;
    using Visual = winrt::Microsoft::UI::Composition::Visual;
    using ShapeVisual = winrt::Microsoft::UI::Composition::ShapeVisual;
    using Gradient = winrt::Microsoft::UI::Composition::CompositionGradientBrush;
    using Stop = winrt::Microsoft::UI::Composition::CompositionColorGradientStop;
    using Batch = winrt::Microsoft::UI::Composition::CompositionScopedBatch;
    using Color = winrt::Windows::UI::Color;

    static constexpr uint32_t PoolCount = 2;
    // Spatial light-up (owner request, 2026-09-30). A slow intensity fade of
    // faint translucent light shimmers: Composition advances its transparency
    // in steps while color changes continuously, so each pixel follows a small
    // sawtooth for as long as the fade lasts. Instead, the light is already at
    // full strength and a soft-edged front grows from the primary pool
    // (lower left) until it covers the group, its halo and outline highlights.
    // Each pixel brightens only while the soft edge passes, about RevealEdge
    // of the travel, while the whole light-up still takes RevealDuration.
    static constexpr float RevealDuration = 1100, RevealEdge = .35f;
    // Where the front starts, in group coordinates (TabAmbient::Pools[0]).
    static constexpr float OriginX = .22f, OriginY = .9f;
    // Outline highlights re-catching after growth, and the halo option
    // turning on alone, still use short intensity ramps.
    static constexpr float CornerDuration = 260, SecondaryDelay = 90;
    static constexpr float HaloStep = 45, HaloDuration = 280;
    // Several groups lighting together start left to right, briefly.
    static constexpr float Stagger = 70; static constexpr size_t StaggerCap = 5;
    static constexpr float FollowDuration = 180, TintDuration = 240;
    static constexpr float RecatchDip = .4f;
    // Extinguishing is quick: highlights and halo first, then the body.
    // Owner tuning (2026-09-30): a slower 750 ms dim did not suit the slower
    // light-up, so it stays about 220 ms.
    static constexpr float DimEdge = 140, DimLag = 40, DimDuration = 180;
    // A settled group that grew at least this much re-catches its moved edge.
    static constexpr float RecatchGrowth = 8, RecatchEdge = 4;

    bool option{};
    // Firefox's groupGlowFadeOut, opt-in: with it on, extinguished glow
    // dims out; otherwise it goes at once while the rest of motion remains.
    bool fadeOut{};
#ifdef WINUI_PANE_TESTING
    std::optional<bool> animationsForTest;
#endif
    winrt::Windows::UI::ViewManagement::UISettings settings;
    bool Enabled() const {
        if (!option) return false;
#ifdef WINUI_PANE_TESTING
        if (animationsForTest) return *animationsForTest;
#endif
        return settings.AnimationsEnabled();
    }

    // Extinguished glow dims before release.
    bool Dims() const { return fadeOut && Enabled(); }

    // Groups whose light-up is still running.
    std::map<uint32_t, Batch> active;
    // Visuals dimming out, released when their batch completes.
    struct Retained { Batch batch{nullptr}; std::function<void()> release; };
    std::map<uint64_t, Retained> dimming;
    uint64_t nextDim{};
    uint64_t recatches{};
    winrt::Microsoft::UI::Composition::ImplicitAnimationCollection follow{nullptr}, tint{nullptr};
    std::shared_ptr<int> life = std::make_shared<int>(0);

    bool Active(uint32_t id) const { return active.count(id) != 0; }

    // FIREFOX_WINUI_PANE_DIAGNOSTICS: reveal timing on stderr, with QPC
    // milliseconds comparable to frame capture times.
    static bool Diagnostics() {
        static const bool enabled = GetEnvironmentVariableW(L"FIREFOX_WINUI_PANE_DIAGNOSTICS", nullptr, 0) != 0;
        return enabled;
    }
    static double Now() {
        LARGE_INTEGER counter{}, frequency{};
        QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
        return counter.QuadPart * 1000.0 / frequency.QuadPart;
    }

    // Which groups are lit, as the owner defines new lighting: a group lights
    // up when it first becomes eligible and shown, not when scrolling or
    // realization merely shows an already-lit group again. `seed` accepts
    // everything eligible as already lit (first snapshot, option turned on).
    struct Lit {
        std::set<uint32_t> ids;
        std::vector<uint32_t> Advance(std::set<uint32_t> const& eligible, std::set<uint32_t> const& shown,
                                      bool seed, std::set<uint32_t>& gone) {
            for (auto it = ids.begin(); it != ids.end();) {
                if (eligible.count(*it)) { ++it; continue; }
                gone.insert(*it); it = ids.erase(it);
            }
            std::vector<uint32_t> lit;
            if (seed) { ids.insert(eligible.begin(), eligible.end()); return lit; }
            for (auto id : shown) if (eligible.count(id) && ids.insert(id).second) lit.push_back(id);
            return lit;
        }
    };

    static winrt::Windows::Foundation::TimeSpan Ms(float value) {
        return std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
            std::chrono::duration<float, std::milli>(std::max(1.f, value)));
    }
    // Light builds and settles gradually. A steep decelerate curve made
    // each ramp jump most of the way within two frames and then crawl, and
    // restarted just as steeply after a bloom's peak. Symmetric ease-in-out
    // segments meet a peak with zero velocity from both sides.
    static auto Smooth(Compositor const& compositor) {
        return compositor.CreateCubicBezierEasingFunction({.45f, 0.f}, {.55f, 1.f});
    }
    // Single-segment settles (growth, re-catch, trailing): starts gently and
    // eases out, without a front-loaded jump.
    static auto Gentle(Compositor const& compositor) {
        return compositor.CreateCubicBezierEasingFunction({.3f, 0.f}, {.2f, 1.f});
    }
    static auto Accelerate(Compositor const& compositor) {
        return compositor.CreateCubicBezierEasingFunction({.7f, 0.f}, {1.f, .5f});
    }
    template <class Animation> static void Time(Animation const& animation, float delay, float duration) {
        animation.DelayTime(Ms(delay));
        animation.Duration(Ms(duration));
        animation.DelayBehavior(winrt::Microsoft::UI::Composition::AnimationDelayBehavior::SetInitialValueBeforeDelay);
    }
    static Color Mix(Color color, float white, float alpha) {
        auto channel = [&](uint8_t c) { return uint8_t(c + (255 - c) * white + .5f); };
        return {uint8_t(std::clamp(alpha, 0.f, 255.f) + .5f), channel(color.R), channel(color.G), channel(color.B)};
    }

    // Stops rise from dark to their final color (the halo option alone).
    static void Rise(Gradient const& brush, float delay, float duration) {
        auto compositor = brush.Compositor();
        for (auto stop : brush.ColorStops()) {
            const auto final = stop.Color();
            if (!final.A) continue;
            auto animation = compositor.CreateColorKeyFrameAnimation();
            animation.InterpolationColorSpace(winrt::Microsoft::UI::Composition::CompositionColorSpace::Rgb);
            animation.InsertKeyFrame(0.f, Mix(final, 0, 0));
            animation.InsertKeyFrame(1.f, final, Smooth(compositor));
            Time(animation, delay, duration);
            stop.StartAnimation(L"Color", animation);
        }
    }
    static Gradient BrushOf(ShapeVisual const& shapes, uint32_t index) {
        auto sprite = shapes.Shapes().GetAt(index).as<winrt::Microsoft::UI::Composition::CompositionSpriteShape>();
        auto brush = sprite.FillBrush() ? sprite.FillBrush() : sprite.StrokeBrush();
        return brush.try_as<Gradient>();
    }

    void Track(uint32_t id, Batch const& batch) {
        active.insert_or_assign(id, batch);
        batch.Completed([this, weak = std::weak_ptr<int>(life), id, batch](auto&&, auto&&) {
            if (weak.expired()) return;
            auto it = active.find(id);
            if (it != active.end() && it->second == batch) active.erase(it);
        });
    }
    static void RiseHalo(ShapeVisual const& halo, float delay) {
        const uint32_t count = halo.Shapes().Size(), rings = count / PoolCount;
        for (uint32_t index = 0; rings && index < count; ++index) {
            auto brush = BrushOf(halo, index);
            if (brush) Rise(brush, delay + HaloStep * (index % rings) + SecondaryDelay * (index / rings), HaloDuration);
        }
    }
    // A reveal front: a white radial mask, opaque out to 1 - RevealEdge of its
    // radius and clear at the radius. Its static radius is the final one, so
    // without the animation the light is simply fully revealed.
    static winrt::Microsoft::UI::Composition::CompositionRadialGradientBrush Front(
        Compositor const& compositor, winrt::Windows::Foundation::Numerics::float2 center, float radius) {
        auto brush = compositor.CreateRadialGradientBrush();
        brush.MappingMode(winrt::Microsoft::UI::Composition::CompositionMappingMode::Absolute);
        brush.EllipseCenter(center);
        brush.EllipseRadius({radius, radius});
        for (auto [offset, alpha] : {std::pair{0.f, 255}, {1.f - RevealEdge, 255}, {1.f, 0}})
            brush.ColorStops().Append(compositor.CreateColorGradientStop(offset, {uint8_t(alpha), 255, 255, 255}));
        return brush;
    }
    // The radius that reaches every corner of `size` from `center` with the
    // soft edge fully past it.
    static float Reach(winrt::Windows::Foundation::Numerics::float2 center, winrt::Windows::Foundation::Numerics::float2 size) {
        const float dx = std::max(center.x, size.x - center.x), dy = std::max(center.y, size.y - center.y);
        return std::max(1.f, std::sqrt(dx * dx + dy * dy) / (1.f - RevealEdge));
    }
    // Grow the fronts together; `done` releases the reveal masks.
    void Reveal(uint32_t id, std::vector<winrt::Microsoft::UI::Composition::CompositionRadialGradientBrush> const& fronts,
                float delay, std::function<void()> done) {
        if (fronts.empty()) { if (done) done(); return; }
        auto compositor = fronts.front().Compositor();
        auto batch = compositor.CreateScopedBatch(winrt::Microsoft::UI::Composition::CompositionBatchTypes::Animation);
        for (auto const& front : fronts) {
            auto animation = compositor.CreateVector2KeyFrameAnimation();
            animation.InsertKeyFrame(0.f, {1.f, 1.f});
            animation.InsertKeyFrame(1.f, front.EllipseRadius(), Gentle(compositor));
            Time(animation, delay, RevealDuration);
            front.StartAnimation(L"EllipseRadius", animation);
        }
        batch.End();
        Track(id, batch);
        if (Diagnostics()) fprintf(stderr, "WinUIPane glow reveal begin group=%u delay=%.0f at=%.1f\n", id, delay, Now());
        batch.Completed([weak = std::weak_ptr<int>(life), done = std::move(done), id](auto&&, auto&&) {
            if (Diagnostics()) fprintf(stderr, "WinUIPane glow reveal end group=%u at=%.1f\n", id, Now());
            if (!weak.expired() && done) try { done(); } catch (...) {}
        });
    }
    // The halo option turned on for a group that is already lit.
    void IgniteHalo(uint32_t id, ShapeVisual const& halo) {
        if (!halo) return;
        auto batch = halo.Compositor().CreateScopedBatch(winrt::Microsoft::UI::Composition::CompositionBatchTypes::Animation);
        RiseHalo(halo, 0);
        batch.End();
        if (!Active(id)) Track(id, batch);
    }
    // The group settled wider: highlights on the edge that moved dip and catch
    // the light again as it arrives there.
    void Recatch(ShapeVisual const& outline, bool left, bool right) {
        if (!outline || (!left && !right)) return;
        auto compositor = outline.Compositor();
        for (uint32_t corner = 0; corner < 4 && corner < outline.Shapes().Size(); ++corner) {
            if (corner % 2 ? !right : !left) continue;
            auto brush = BrushOf(outline, corner);
            if (!brush) continue;
            for (auto stop : brush.ColorStops()) {
                const auto final = stop.Color();
                if (!final.A) continue;
                auto animation = compositor.CreateColorKeyFrameAnimation();
                animation.InsertKeyFrame(0.f, Mix(final, 0, final.A * RecatchDip));
                animation.InsertKeyFrame(1.f, final, Gentle(compositor));
                Time(animation, 0, CornerDuration);
                stop.StartAnimation(L"Color", animation);
            }
        }
        ++recatches;
    }
    // Fade a visual out, then release it. `release` must tolerate the owner
    // having created a replacement in the meantime.
    void Dim(Visual const& visual, float delay, float duration, std::function<void()> release) {
        auto compositor = visual.Compositor();
        auto batch = compositor.CreateScopedBatch(winrt::Microsoft::UI::Composition::CompositionBatchTypes::Animation);
        auto animation = compositor.CreateScalarKeyFrameAnimation();
        animation.InsertExpressionKeyFrame(0.f, L"this.StartingValue");
        animation.InsertKeyFrame(1.f, 0.f, Accelerate(compositor));
        animation.DelayTime(Ms(delay)); animation.Duration(Ms(duration));
        visual.StartAnimation(L"Opacity", animation);
        batch.End();
        const auto key = ++nextDim;
        dimming.emplace(key, Retained{batch, std::move(release)});
        batch.Completed([this, weak = std::weak_ptr<int>(life), key](auto&&, auto&&) {
            if (weak.expired()) return;
            auto it = dimming.find(key);
            if (it == dimming.end()) return;
            auto release = std::move(it->second.release);
            dimming.erase(it);
            if (release) try { release(); } catch (...) {}
        });
    }
    // Option or system animations turned off: release every dimming visual now.
    void Cancel() {
        auto pending = std::move(dimming);
        dimming.clear();
        for (auto& [key, retained] : pending) if (retained.release) try { retained.release(); } catch (...) {}
        active.clear();
    }

    void Collections(Compositor const& compositor) {
        if (follow) return;
        follow = compositor.CreateImplicitAnimationCollection();
        for (auto name : {L"EllipseCenter", L"EllipseRadius"}) {
            auto animation = compositor.CreateVector2KeyFrameAnimation();
            animation.Target(name);
            animation.InsertExpressionKeyFrame(1.f, L"this.FinalValue", Gentle(compositor));
            animation.Duration(Ms(FollowDuration));
            follow.Insert(name, animation);
        }
        tint = compositor.CreateImplicitAnimationCollection();
        auto animation = compositor.CreateColorKeyFrameAnimation();
        animation.Target(L"Color");
        animation.InterpolationColorSpace(winrt::Microsoft::UI::Composition::CompositionColorSpace::Rgb);
        animation.InsertExpressionKeyFrame(1.f, L"this.FinalValue", Smooth(compositor));
        animation.Duration(Ms(TintDuration));
        tint.Insert(L"Color", animation);
    }
    // Attach (or detach) implicit motion to a lit visual tree: every gradient
    // stop cross-fades its color, and the positioned light of shape brushes
    // (pools, corner highlights, halo rings) trails geometry changes. Attach
    // only after the first color and geometry, so creation never animates.
    // Whether positioned light trails geometry changes. Off while the strip
    // follows continuous motion (drags, slides, joins): the strip then updates
    // geometry every frame, and restarting a trailing animation from each
    // update made the light jitter at the UI thread's uneven frame times.
    bool following{true};
    // Switch trailing for one lit shape tree. Turning it off also stops any
    // trailing in flight; the caller then reassigns the current geometry.
    void Follow(Visual const& visual, bool on) {
        auto shapes = visual ? visual.try_as<ShapeVisual>() : ShapeVisual{nullptr};
        if (!shapes) return;
        if (on) Collections(shapes.Compositor());
        for (uint32_t index = 0; index < shapes.Shapes().Size(); ++index) {
            auto brush = BrushOf(shapes, index);
            if (!brush || !brush.try_as<winrt::Microsoft::UI::Composition::CompositionRadialGradientBrush>()) continue;
            brush.ImplicitAnimations(on ? follow : nullptr);
            if (!on) { brush.StopAnimation(L"EllipseCenter"); brush.StopAnimation(L"EllipseRadius"); }
        }
    }
    void Attach(Visual const& visual, bool on) {
        if (!visual) return;
        if (on) Collections(visual.Compositor());
        auto stops = [&](Gradient const& brush) {
            if (!brush) return;
            for (auto stop : brush.ColorStops()) stop.ImplicitAnimations(on ? tint : nullptr);
        };
        if (auto shapes = visual.try_as<ShapeVisual>()) {
            for (uint32_t index = 0; index < shapes.Shapes().Size(); ++index) {
                auto brush = BrushOf(shapes, index);
                if (!brush) continue;
                stops(brush);
                if (brush.try_as<winrt::Microsoft::UI::Composition::CompositionRadialGradientBrush>())
                    brush.ImplicitAnimations(on && following ? follow : nullptr);
            }
            return;
        }
        if (auto sprite = visual.try_as<winrt::Microsoft::UI::Composition::SpriteVisual>())
            if (auto brush = sprite.Brush()) stops(brush.try_as<Gradient>());
        if (auto container = visual.try_as<winrt::Microsoft::UI::Composition::ContainerVisual>())
            for (auto child : container.Children()) Attach(child, on);
    }

    ~TabGlowIgnition() { life.reset(); }
};
