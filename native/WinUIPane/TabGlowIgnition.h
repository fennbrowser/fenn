#pragma once
#include <list>
#include <winrt/Windows.UI.ViewManagement.h>

// Opt-in light-up motion for group glow (Firefox's groupGlowAnimations).
// Newly lit groups warm up from their dominant light pool outward, geometry
// changes let the bright spots trail the surface, color changes cross-fade and
// extinguished glow dims before release. Everything here is a Composition
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

    // Light-up timeline, in milliseconds from the start of one group's light.
    // The primary pool (the first of TabAmbient::Pools, lower left) lights first; the
    // weaker upper-right pool follows, so the glow stays uneven as it grows.
    static constexpr uint32_t PoolCount = 2;
    // Owner tuning (2026-09-30): slower and more pronounced than the first
    // 640 ms version; the whole light-up now takes about 1.4 s.
    static constexpr float PoolDuration = 600, SecondaryDelay = 170;
    static constexpr float PoolStartScale = .3f, PoolBloom = 1.3f;
    // The core starts near white and saturates into the group color, like a
    // warming filament; the bloom still carries a trace of that heat.
    static constexpr float HotTint = .5f, BloomTint = .25f, BloomAt = .55f;
    // The wash briefly overshoots its resting opacity with the pools' bloom.
    static constexpr float WashDelay = 150, SpillDelay = 260, WashDuration = 700, WashBloom = 1.25f;
    // Outline highlights catch the light last, nearest the primary pool
    // first. Index order matches TabGroupSurface: TL, TR, BL, BR.
    static constexpr float CornerDelay[4] = {450, 670, 340, 560};
    static constexpr float CornerDuration = 480, CornerGlint = 1.6f, CornerStartScale = .05f;
    // Halo rings bleed outward, inner ring first.
    static constexpr float HaloDelay = 420, HaloStep = 80, HaloDuration = 520;
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
    static auto Decelerate(Compositor const& compositor) {
        return compositor.CreateCubicBezierEasingFunction({.1f, .9f}, {.2f, 1.f});
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

    // Stops rise from dark through an optional bloom to their final color.
    static void Rise(Gradient const& brush, float delay, float duration, float bloom, float hot) {
        auto compositor = brush.Compositor();
        for (auto stop : brush.ColorStops()) {
            const auto final = stop.Color();
            if (!final.A) continue;
            auto animation = compositor.CreateColorKeyFrameAnimation();
            animation.InterpolationColorSpace(winrt::Microsoft::UI::Composition::CompositionColorSpace::Rgb);
            animation.InsertKeyFrame(0.f, Mix(final, hot, 0));
            if (bloom != 1.f) animation.InsertKeyFrame(BloomAt, Mix(final, hot * BloomTint / HotTint, final.A * bloom), Decelerate(compositor));
            animation.InsertKeyFrame(1.f, final, Decelerate(compositor));
            Time(animation, delay, duration);
            stop.StartAnimation(L"Color", animation);
        }
    }
    // Light spreads from its source: the gradient grows about its own center.
    // Scale is never set statically, so geometry updates cannot conflict.
    static void Grow(Gradient const& brush, float from, float delay, float duration) {
        auto compositor = brush.Compositor();
        if (auto radial = brush.try_as<winrt::Microsoft::UI::Composition::CompositionRadialGradientBrush>())
            radial.CenterPoint(radial.EllipseCenter());
        auto animation = compositor.CreateVector2KeyFrameAnimation();
        animation.InsertKeyFrame(0.f, {from, from});
        animation.InsertKeyFrame(1.f, {1.f, 1.f}, Decelerate(compositor));
        Time(animation, delay, duration);
        brush.StartAnimation(L"Scale", animation);
    }
    static void Fade(Visual const& visual, float delay, float duration) {
        if (!visual) return;
        auto compositor = visual.Compositor();
        auto animation = compositor.CreateScalarKeyFrameAnimation();
        const float final = visual.Opacity();
        animation.InsertKeyFrame(0.f, 0.f);
        animation.InsertKeyFrame(BloomAt, std::min(1.f, final * WashBloom), Decelerate(compositor));
        animation.InsertKeyFrame(1.f, final, Decelerate(compositor));
        Time(animation, delay, duration);
        visual.StartAnimation(L"Opacity", animation);
    }
    static Gradient BrushOf(ShapeVisual const& shapes, uint32_t index) {
        auto sprite = shapes.Shapes().GetAt(index).as<winrt::Microsoft::UI::Composition::CompositionSpriteShape>();
        auto brush = sprite.FillBrush() ? sprite.FillBrush() : sprite.StrokeBrush();
        return brush.try_as<Gradient>();
    }

    struct Targets {
        Visual wash{nullptr}, spill{nullptr};
        // TabAmbient::Halo rings, pool-major; null without the halo option.
        ShapeVisual halo{nullptr};
        // TabGroupSurface outline light: four corner strokes, then the pools.
        ShapeVisual outline{nullptr};
    };
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
            if (brush) Rise(brush, delay + HaloStep * (index % rings) + SecondaryDelay * (index / rings), HaloDuration, 1.f, 0.f);
        }
    }
    void Ignite(uint32_t id, Targets const& targets, float delay) {
        Visual any = targets.wash ? targets.wash : targets.spill ? targets.spill :
            targets.halo ? Visual(targets.halo) : Visual(targets.outline);
        if (!any) return;
        auto batch = any.Compositor().CreateScopedBatch(winrt::Microsoft::UI::Composition::CompositionBatchTypes::Animation);
        if (auto outline = targets.outline) {
            const uint32_t count = outline.Shapes().Size();
            for (uint32_t pool = 0; pool < PoolCount && 4 + pool < count; ++pool) {
                if (auto brush = BrushOf(outline, 4 + pool)) {
                    const float start = delay + SecondaryDelay * pool;
                    Rise(brush, start, PoolDuration, PoolBloom, HotTint);
                    Grow(brush, PoolStartScale, start, PoolDuration);
                }
            }
            for (uint32_t corner = 0; corner < 4 && corner < count; ++corner) {
                if (auto brush = BrushOf(outline, corner)) {
                    Rise(brush, delay + CornerDelay[corner], CornerDuration, CornerGlint, HotTint);
                    Grow(brush, CornerStartScale, delay + CornerDelay[corner], CornerDuration);
                }
            }
        }
        Fade(targets.wash, delay + WashDelay, WashDuration);
        Fade(targets.spill, delay + SpillDelay, WashDuration);
        if (targets.halo) RiseHalo(targets.halo, delay + HaloDelay);
        batch.End();
        Track(id, batch);
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
                animation.InsertKeyFrame(1.f, final, Decelerate(compositor));
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
            animation.InsertExpressionKeyFrame(1.f, L"this.FinalValue", Decelerate(compositor));
            animation.Duration(Ms(FollowDuration));
            follow.Insert(name, animation);
        }
        tint = compositor.CreateImplicitAnimationCollection();
        auto animation = compositor.CreateColorKeyFrameAnimation();
        animation.Target(L"Color");
        animation.InterpolationColorSpace(winrt::Microsoft::UI::Composition::CompositionColorSpace::Rgb);
        animation.InsertExpressionKeyFrame(1.f, L"this.FinalValue", Decelerate(compositor));
        animation.Duration(Ms(TintDuration));
        tint.Insert(L"Color", animation);
    }
    // Attach (or detach) implicit motion to a lit visual tree: every gradient
    // stop cross-fades its color, and the positioned light of shape brushes
    // (pools, corner highlights, halo rings) trails geometry changes. Attach
    // only after the first color and geometry, so creation never animates.
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
                    brush.ImplicitAnimations(on ? follow : nullptr);
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
