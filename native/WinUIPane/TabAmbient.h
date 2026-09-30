#pragma once
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include "TabGlowDiffusion.h"
#include "TabAmbientClip.h"
#include "TabLightContour.h"
#include "TabGlowIgnition.h"

// Decorative Composition children cannot take focus, enter UIA's control tree,
// or affect TabView measurement. Both fields use ordinary source-over alpha.
struct TabAmbient {
    using Rect = winrt::Windows::Foundation::Rect;
    using Sprite = winrt::Microsoft::UI::Composition::SpriteVisual;
    using Gradient = winrt::Microsoft::UI::Composition::CompositionGradientBrush;
    struct Field {
        Sprite visual{nullptr}, left{nullptr}, middle{nullptr}, right{nullptr};
        Gradient brush{nullptr}, leftBrush{nullptr}, rightBrush{nullptr};
        winrt::Microsoft::UI::Composition::ShapeVisual rimShape{nullptr};
        winrt::Microsoft::UI::Composition::CompositionPathGeometry rimGeometry{nullptr};
        winrt::Microsoft::UI::Composition::CompositionRoundedRectangleGeometry roundedClip{nullptr};
        uint32_t color{}; Rect bounds{}; bool capped{};
        float cornerLeft{-1}, cornerRight{-1};
        // Implicit glow motion is attached (TabGlowIgnition::Attach).
        bool animated{};
        // The visual that holds this field in its layer.
        winrt::Microsoft::UI::Composition::Visual Root() const {
            return rimShape && !visual ? winrt::Microsoft::UI::Composition::Visual(rimShape) : winrt::Microsoft::UI::Composition::Visual(visual);
        }
    };
    winrt::Microsoft::UI::Xaml::Controls::Canvas layer{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Canvas rimLayer{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual rims{nullptr};
    Field upperRim;
    winrt::Microsoft::UI::Composition::ContainerVisual bottom{nullptr}, top{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual spill{nullptr}, halo{nullptr};
    winrt::Microsoft::UI::Composition::LayerVisual diffusion{nullptr}, haloDiffusion{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual lit{nullptr};
    winrt::Microsoft::UI::Composition::CompositionVisualSurface diffusedSurface{nullptr};
    winrt::Microsoft::UI::Composition::CompositionLinearGradientBrush edgeFade{nullptr};
    Sprite output{nullptr};
    TabLightContour::Mask faceMask;
    Sprite occluded{nullptr};
    winrt::Microsoft::UI::Composition::CompositionVisualSurface occludedSurface{nullptr};
    winrt::com_ptr<ID2D1Factory> clipFactory;
    Rect clippedViewport{}, clippedActive{};
    std::map<uint32_t, Field> groups, containers;
    std::map<uint32_t, Field> groupSpill, containerSpill, groupHalo;
    // Unequal material light pools across a group, in its relative
    // coordinates. TabGroupSurface paints them inside the outline; the halo
    // continues the same pools outside it, so the glow stays uneven.
    struct Pool { float x, y; int peak; };
    static constexpr Pool Pools[] = {{.22f, .9f, 48}, {.82f, .18f, 30}};
    static_assert(ARRAYSIZE(Pools) == TabGlowIgnition::PoolCount);
    // Opt-in light-up motion; the strip owns the option.
    TabGlowIgnition motion;
    // Release a field, dimming it first when its group's light went out
    // (not when it was only scrolled away) and motion is enabled.
    void Release(winrt::Microsoft::UI::Composition::ContainerVisual const& owner, Field const& field, bool dim, float delay, float duration) {
        auto visual = field.Root();
        if (!visual) return;
        if (!dim || !motion.Enabled()) { owner.Children().Remove(visual); return; }
        motion.Dim(visual, delay, duration, [owner, visual] { owner.Children().Remove(visual); });
    }
    static constexpr float PoolRadiusX = .95f, PoolRadiusY = 1.2f;
    static constexpr std::pair<float, float> PoolStops[] = {{0.f, 1.f}, {.55f, .5f}, {1.f, 0.f}};
    // Outer halo: concentric rounded rings outside the group outline, each
    // carrying the pools at a falling fraction of their interior strength.
    // The shared Gaussian diffusion merges the rings into one falloff.
    static constexpr float HaloPitch = 2.f;
    static constexpr float HaloFade[] = {.75f, .42f, .18f, .05f};
    static constexpr uint32_t HaloRings = ARRAYSIZE(HaloFade);
    static constexpr float HaloReach = HaloPitch * HaloRings;
    winrt::Windows::UI::ViewManagement::AccessibilitySettings accessibility;
    winrt::Windows::UI::ViewManagement::UISettings settings;
    winrt::event_token contrastToken{}, layoutToken{}, renderingToken{};
    std::shared_ptr<int> life = std::make_shared<int>(0);
    std::function<void()> refresh;
    bool observing{}, updating{}, reportedFailure{}, queued{};
    // Rendered frames observed while following motion; bounds waits in
    // frames, so a throttled window cannot end them before it draws.
    uint64_t frames{};
    // Assigning a stop's color first ends any light-up running on it
    // (TabGlowIgnition), so a recolor is not overridden by its keyframes.
    static void Tint(winrt::Microsoft::UI::Composition::CompositionColorGradientStop const& stop,
                     uint8_t r, uint8_t g, uint8_t b) {
        auto value = stop.Color();
        value.R = r; value.G = g; value.B = b;
        stop.StopAnimation(L"Color");
        stop.Color(value);
    }
    static void Geometry(Sprite const& visual, winrt::Windows::Foundation::Numerics::float3 offset,
                         winrt::Windows::Foundation::Numerics::float2 size) {
        if (visual.Offset() != offset) visual.Offset(offset);
        if (visual.Size() != size) visual.Size(size);
    }
    void ScheduleUpdate() {
        if (queued) return;
        queued = true;
        if (!layer.DispatcherQueue().TryEnqueue([this, weak = std::weak_ptr<int>(life)] {
            if (weak.expired()) return;
            queued = false;
            Update();
        })) queued = false;
    }

    void Create(winrt::Microsoft::UI::Xaml::Controls::Grid const& root) {
        using namespace winrt::Microsoft::UI::Xaml;
        layer = Markup::XamlReader::Load(LR"(<Canvas xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Grid.ColumnSpan="3" IsHitTestVisible="False" AutomationProperties.AccessibilityView="Raw" FlowDirection="LeftToRight"/>)").as<Controls::Canvas>();
        root.Children().Append(layer);
        rimLayer = Markup::XamlReader::Load(LR"(<Canvas xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" IsHitTestVisible="False" AutomationProperties.AccessibilityView="Raw"/>)").as<Controls::Canvas>();
        layer.Children().Append(rimLayer);
        auto weak = std::weak_ptr<int>(life);
        layer.Loaded([this, weak](auto&&, auto&&) {
            if (weak.expired()) return;
            if (!layoutToken.value) layoutToken = layer.LayoutUpdated([this, weak](auto&&, auto&&) { if (!weak.expired()) Update(); });
            Update();
        });
        contrastToken = settings.ColorValuesChanged([this, weak, queue = root.DispatcherQueue()](auto&&, auto&&) {
            queue.TryEnqueue([this, weak] { if (!weak.expired()) Update(); });
        });

    }
    void Update() {
        if (updating || !refresh || !layer.IsLoaded()) return;
        updating = true;
        try {
            if (!bottom) {
                auto compositor = winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(layer).Compositor();
                rims = compositor.CreateContainerVisual();
                winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::SetElementChildVisual(rimLayer, rims);
                diffusion = compositor.CreateLayerVisual();
                diffusion.Effect(compositor.CreateEffectFactory(winrt::make<TabGlowDiffusion>()).CreateBrush());
                diffusion.Opacity(.9f);
                auto parent = diffusion;
                bottom = compositor.CreateContainerVisual(); top = compositor.CreateContainerVisual();
                parent.Children().InsertAtTop(bottom); parent.Children().InsertAtTop(top);
                spill = compositor.CreateContainerVisual();
                spill.Comment(L"ambient-color-spill");
                parent.Children().InsertAtTop(spill);
                // Halos get their own diffusion layer without the active tab's
                // notch: the tab sits in front of an unbroken halo, while other
                // light keeps the hard notch. Both share the masks below.
                haloDiffusion = compositor.CreateLayerVisual();
                haloDiffusion.Comment(L"group-halo-diffusion");
                haloDiffusion.Effect(compositor.CreateEffectFactory(winrt::make<TabGlowDiffusion>()).CreateBrush());
                haloDiffusion.Opacity(.9f);
                halo = compositor.CreateContainerVisual();
                halo.Comment(L"group-halo");
                haloDiffusion.Children().InsertAtTop(halo);
                lit = compositor.CreateContainerVisual();
                lit.Comment(L"ambient-lit");
                lit.Children().InsertAtTop(diffusion); lit.Children().InsertAtTop(haloDiffusion);
                // Mask the already-diffused light, so blur cannot refill the
                // transparent tail before the new-tab button.
                diffusedSurface = compositor.CreateVisualSurface();
                diffusedSurface.SourceVisual(lit);
                auto source = compositor.CreateSurfaceBrush(diffusedSurface);
                faceMask.Create(compositor);
                auto occlusion = compositor.CreateMaskBrush();
                occlusion.Source(source); occlusion.Mask(compositor.CreateSurfaceBrush(faceMask.surface));
                occluded = compositor.CreateSpriteVisual(); occluded.Brush(occlusion);
                occluded.Comment(L"occluded-tab-light");
                occludedSurface = compositor.CreateVisualSurface(); occludedSurface.SourceVisual(occluded);
                edgeFade = compositor.CreateLinearGradientBrush();
                edgeFade.MappingMode(winrt::Microsoft::UI::Composition::CompositionMappingMode::Absolute);
                for (int i = 0; i < 4; ++i)
                    edgeFade.ColorStops().Append(compositor.CreateColorGradientStop(float(i) / 3, {255, 255, 255, 255}));
                auto mask = compositor.CreateMaskBrush();
                mask.Source(compositor.CreateSurfaceBrush(occludedSurface)); mask.Mask(edgeFade);
                output = compositor.CreateSpriteVisual(); output.Brush(mask);
                winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::SetElementChildVisual(layer, output);
            }
            refresh();
        } catch (...) {
#ifdef WINUI_PANE_TESTING
            if (!reportedFailure) {
                reportedFailure = true;
                try { throw; }
                catch (winrt::hresult_error const& error) { fwprintf(stderr, L"Ambient refresh: %08x %s\n", unsigned(error.code()), error.message().c_str()); }
                catch (...) { fprintf(stderr, "Ambient refresh failed\n"); }
            }
#endif
            if (bottom) bottom.IsVisible(false);
            if (top) top.IsVisible(false);
            if (rims) rims.IsVisible(false);
            if (spill) spill.IsVisible(false);
            if (halo) halo.IsVisible(false);
        }
        updating = false;
    }
    // Only layout/scroll invalidation runs at rest. Follow existing tab motion
    // for its bounded lifetime; never keep an ambient render loop alive.
    void Motion(bool moving) {
        using namespace winrt::Microsoft::UI::Xaml;
        if (moving == observing) return;
        observing = moving;
        if (!moving) { Media::CompositionTarget::Rendering(renderingToken); return; }
        renderingToken = Media::CompositionTarget::Rendering([this, weak = std::weak_ptr<int>(life)](auto&&, auto&&) {
            if (weak.expired()) return;
            ++frames;
            Update();
        });
    }
    // fadeLeft/fadeRight are the DIP widths, at each end of the viewport, in
    // which light falls to zero; zero keeps full light to that edge.
    void Clip(Rect viewport, bool visible, Rect active = {}, float fadeLeft = 0, float fadeRight = 0) {
        visible = visible && !accessibility.HighContrast();
        bottom.IsVisible(visible); top.IsVisible(visible);
        rims.IsVisible(visible);
        spill.IsVisible(visible);
        halo.IsVisible(visible);
        const winrt::Windows::Foundation::Numerics::float2 size{
            std::max(0.f, viewport.X + viewport.Width), std::max(0.f, viewport.Y + viewport.Height)};
        if (diffusion.Size() != size) { diffusion.Size(size); haloDiffusion.Size(size); lit.Size(size); }
        if (output.Size() != size) {
            output.Size(size); diffusedSurface.SourceSize(size);
            occluded.Size(size); occludedSurface.SourceSize(size);
        }
        const float width = std::max(1.f, viewport.Width);
        const float leftTail = std::clamp(fadeLeft, 0.f, width / 2) / width;
        const float rightTail = std::clamp(fadeRight, 0.f, width / 2) / width;
        edgeFade.StartPoint({viewport.X, 0}); edgeFade.EndPoint({viewport.X + width, 0});
        auto stops = edgeFade.ColorStops();
        stops.GetAt(0).Color({uint8_t(fadeLeft > 0 ? 0 : 255), 255, 255, 255});
        stops.GetAt(1).Offset(leftTail); stops.GetAt(2).Offset(1 - rightTail);
        stops.GetAt(3).Color({uint8_t(fadeRight > 0 ? 0 : 255), 255, 255, 255});
        // Cut only the area above the active tab, after Gaussian diffusion.
        // Shared container fields keep their geometry beneath the notch.
        const float left = std::max(viewport.X, active.X);
        const float right = std::min(viewport.X + viewport.Width, active.X + active.Width);
        if (active.Height <= 0 || right <= left || active.Y <= viewport.Y) {
            diffusion.Clip(nullptr);
            clippedActive = {};
        } else if (!diffusion.Clip() || clippedViewport != viewport || clippedActive != active) {
            if (!clipFactory) winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, clipFactory.put()));
            winrt::com_ptr<ID2D1PathGeometry> path;
            winrt::check_hresult(clipFactory->CreatePathGeometry(path.put()));
            winrt::com_ptr<ID2D1GeometrySink> sink;
            winrt::check_hresult(path->Open(sink.put()));
            sink->BeginFigure({viewport.X, viewport.Y}, D2D1_FIGURE_BEGIN_FILLED);
            const float edge = std::min(active.Y, viewport.Y + viewport.Height);
            const D2D1_POINT_2F points[] = {{left, viewport.Y}, {left, edge}, {right, edge},
                {right, viewport.Y}, {viewport.X + viewport.Width, viewport.Y},
                {viewport.X + viewport.Width, viewport.Y + viewport.Height}, {viewport.X, viewport.Y + viewport.Height}};
            sink->AddLines(points, ARRAYSIZE(points));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            winrt::check_hresult(sink->Close());
            auto compositor = diffusion.Compositor();
            auto geometry = compositor.CreatePathGeometry(winrt::Microsoft::UI::Composition::CompositionPath(
                winrt::make<TabAmbientClip>(std::move(path))));
            diffusion.Clip(compositor.CreateGeometricClip(geometry));
            clippedViewport = viewport; clippedActive = active;
        }
        // Avoid invalidating XAML layout from LayoutUpdated.
        auto old = layer.Clip();
        if (!old) {
            old = winrt::Microsoft::UI::Xaml::Media::RectangleGeometry();
            old.Rect(viewport);
            layer.Clip(old);
        } else if (old.Rect() != viewport) old.Rect(viewport);
    }
    void Faces(Rect viewport, std::vector<TabLightContour::Face> const& faces) {
        if (!clipFactory) winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, clipFactory.put()));
        faceMask.Update(clipFactory.get(), viewport, faces);
    }
    void Spill(std::map<uint32_t, Field>& fields, uint32_t id, uint32_t color, Rect bounds, bool upper) {
        auto& field = fields[id];
        if (!field.visual) {
            auto compositor = spill.Compositor();
            field.visual = compositor.CreateSpriteVisual();
            field.visual.Comment((upper ? L"container-spill:" : L"group-spill:") + winrt::to_hstring(id));
            auto lobe = [&](float y, int peak, Gradient& target, Sprite& sprite) {
                auto brush = compositor.CreateRadialGradientBrush();
                brush.EllipseCenter({.5f, y}); brush.EllipseRadius({.5f, 1.65f});
                for (auto [offset, alpha] : {std::pair{0.f, peak}, {.4f, peak * 2 / 3}, {.75f, 1}, {1.f, 0}})
                    brush.ColorStops().Append(compositor.CreateColorGradientStop(offset, {uint8_t(alpha), 0, 0, 0}));
                target = brush;
                sprite = compositor.CreateSpriteVisual(); sprite.Brush(brush);
                field.visual.Children().InsertAtTop(sprite);
            };
            // Peaks are only 2–3% alpha; long vertical tails let independent
            // colors overlap through the body without setting a tab background.
            lobe(upper ? -.05f : 1.05f, upper ? 8 : 6, field.brush, field.middle);
            if (!upper) lobe(1.2f, 5, field.leftBrush, field.left);
            spill.Children().InsertAtTop(field.visual);
        }
        if (field.color != color) {
            field.color = color;
            for (auto brush : {field.brush, field.leftBrush}) {
                if (!brush) continue;
                for (auto stop : brush.ColorStops()) Tint(stop, uint8_t(color >> 16), uint8_t(color >> 8), uint8_t(color));
            }
        }
        if (field.bounds != bounds) {
            field.bounds = bounds;
            Geometry(field.visual, {bounds.X, bounds.Y, 0}, {bounds.Width, bounds.Height});
            Geometry(field.middle, {}, {bounds.Width * (upper ? 1.f : .72f), bounds.Height});
            if (!upper) {
                Geometry(field.left, {bounds.Width * .28f, 0, 0}, {bounds.Width * .72f, bounds.Height});
            }
        }
    }
    void PruneSpill(std::map<uint32_t, Field>& fields, std::set<uint32_t> const& live, std::set<uint32_t> const& dim = {}) {
        for (auto it = fields.begin(); it != fields.end();) {
            if (live.count(it->first)) { ++it; continue; }
            Release(spill, it->second, dim.count(it->first) != 0, TabGlowIgnition::DimLag, TabGlowIgnition::DimDuration);
            it = fields.erase(it);
        }
    }
    // Soft light just beyond the rounded group edge, so the group reads as
    // glowing instead of ending at a hard boundary. Rings start outside the
    // outline and leave the group interior unchanged. The post-diffusion face
    // mask and viewport clips still apply, but the active tab's notch does
    // not, so the halo surrounds the selected tab unbroken. Shapes are
    // pool-major: index p * HaloRings + i, with both pools sharing ring i's
    // geometry.
    void Halo(uint32_t id, uint32_t color, Rect bounds, float radius) {
        using namespace winrt::Microsoft::UI::Composition;
        auto& field = groupHalo[id];
        auto compositor = halo.Compositor();
        if (!field.rimShape) {
            field.rimShape = compositor.CreateShapeVisual();
            field.rimShape.Comment(L"group-halo:" + winrt::to_hstring(id));
            std::vector<CompositionRoundedRectangleGeometry> rings;
            for (uint32_t i = 0; i < HaloRings; ++i) rings.push_back(compositor.CreateRoundedRectangleGeometry());
            for (auto const& pool : Pools) {
                for (uint32_t i = 0; i < HaloRings; ++i) {
                    auto brush = compositor.CreateRadialGradientBrush();
                    brush.MappingMode(CompositionMappingMode::Absolute);
                    for (auto [offset, level] : PoolStops)
                        brush.ColorStops().Append(compositor.CreateColorGradientStop(offset,
                            {uint8_t(pool.peak * level * HaloFade[i] + .5f), 0, 0, 0}));
                    auto ring = compositor.CreateSpriteShape(rings[i]);
                    ring.StrokeBrush(brush);
                    // Slight overlap avoids antialiased seams between rings.
                    ring.StrokeThickness(HaloPitch + .25f);
                    field.rimShape.Shapes().Append(ring);
                }
            }
            halo.Children().InsertAtTop(field.rimShape);
        }
        auto brushAt = [&](uint32_t index) {
            return field.rimShape.Shapes().GetAt(index).as<CompositionSpriteShape>().StrokeBrush()
                .as<CompositionRadialGradientBrush>();
        };
        const uint32_t count = field.rimShape.Shapes().Size();
        if (field.color != color) {
            field.color = color;
            for (uint32_t index = 0; index < count; ++index) for (auto stop : brushAt(index).ColorStops())
                Tint(stop, uint8_t(color >> 16), uint8_t(color >> 8), uint8_t(color));
        }
        if (field.bounds == bounds && field.cornerLeft == radius) return;
        field.bounds = bounds; field.cornerLeft = radius;
        field.rimShape.Offset({bounds.X - HaloReach, bounds.Y - HaloReach, 0});
        field.rimShape.Size({bounds.Width + 2 * HaloReach, bounds.Height + 2 * HaloReach});
        for (uint32_t i = 0; i < HaloRings; ++i) {
            // Each stroke is centered on its ring, covering [d - pitch/2, d + pitch/2].
            const float d = HaloPitch * (i + .5f);
            auto geometry = field.rimShape.Shapes().GetAt(i).as<CompositionSpriteShape>().Geometry()
                .as<CompositionRoundedRectangleGeometry>();
            geometry.Offset({HaloReach - d, HaloReach - d});
            geometry.Size({bounds.Width + 2 * d, bounds.Height + 2 * d});
            geometry.CornerRadius({radius + d, radius + d});
        }
        // Pools stay anchored in group coordinates, matching the interior.
        for (uint32_t index = 0; index < count; ++index) {
            auto const& pool = Pools[index / HaloRings];
            auto brush = brushAt(index);
            brush.EllipseCenter({HaloReach + pool.x * bounds.Width, HaloReach + pool.y * bounds.Height});
            brush.EllipseRadius({std::max(1.f, PoolRadiusX * bounds.Width), std::max(1.f, PoolRadiusY * bounds.Height)});
        }
    }
    void PruneHalo(std::set<uint32_t> const& live, std::set<uint32_t> const& dim = {}) {
        for (auto it = groupHalo.begin(); it != groupHalo.end();) {
            if (live.count(it->first)) { ++it; continue; }
            Release(halo, it->second, dim.count(it->first) != 0, 0, TabGlowIgnition::DimEdge);
            it = groupHalo.erase(it);
        }
    }
    void Corners(Field& field, uint32_t color, Rect bounds, float leftRadius, float rightRadius, bool reflection = false) {
        if (!color || bounds.Width <= 0 || bounds.Height <= 0) {
            if (field.visual) field.visual.IsVisible(false);
            return;
        }
        auto compositor = top.Compositor();
        if (!field.visual) {
            field.visual = compositor.CreateSpriteVisual();
            field.visual.Comment(reflection ? L"active-container-reflection" : L"container-corners");
            field.rimShape = compositor.CreateShapeVisual();
            field.rimShape.Offset({-4, -4, 0});
            for (int side = 0; side < 2; ++side) {
                auto brush = compositor.CreateRadialGradientBrush();
                brush.MappingMode(winrt::Microsoft::UI::Composition::CompositionMappingMode::Absolute);
                for (auto [offset, alpha] : {std::pair{0.f, 180}, {.3f, 145}, {.65f, 55}, {1.f, 0}})
                    brush.ColorStops().Append(compositor.CreateColorGradientStop(offset,
                        {uint8_t(reflection ? alpha / 2 : alpha), 0, 0, 0}));
                auto stroke = compositor.CreateSpriteShape(compositor.CreatePathGeometry());
                stroke.Offset({4, 4}); stroke.StrokeBrush(brush); stroke.StrokeThickness(reflection ? 1.f : 3.f);
                field.rimShape.Shapes().Append(stroke);
                (side ? field.rightBrush : field.leftBrush) = brush;
            }
            field.visual.Children().InsertAtTop(field.rimShape);
            (reflection ? rims : top).Children().InsertAtTop(field.visual);
        }
        field.visual.IsVisible(true);
        if (field.color != color) {
            field.color = color;
            for (auto brush : {field.leftBrush, field.rightBrush}) for (auto stop : brush.ColorStops()) {
                auto value = stop.Color();
                auto channel = [&](uint8_t c) { return reflection ? uint8_t(c + (255 - c) * .3f) : c; };
                value.R = channel(uint8_t(color >> 16)); value.G = channel(uint8_t(color >> 8)); value.B = channel(uint8_t(color));
                stop.Color(value);
            }
        }
        if (field.bounds == bounds && field.cornerLeft == leftRadius && field.cornerRight == rightRadius) return;
        field.bounds = bounds; field.cornerLeft = leftRadius; field.cornerRight = rightRadius;
        Geometry(field.visual, {bounds.X, bounds.Y, 0}, {bounds.Width, bounds.Height});
        field.rimShape.Size({bounds.Width + 8, bounds.Height + 8});
        if (!clipFactory) winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, clipFactory.put()));
        const float reach = std::min(28.f, bounds.Width / 4), depth = std::min(16.f, bounds.Height / 2);
        for (int side = 0; side < 2; ++side) {
            const float r = std::clamp(side ? rightRadius : leftRadius, 0.f, std::min(reach, depth) / 2);
            auto x = [&](float value) { return side ? bounds.Width - value : value; };
            winrt::com_ptr<ID2D1PathGeometry> path;
            winrt::check_hresult(clipFactory->CreatePathGeometry(path.put()));
            winrt::com_ptr<ID2D1GeometrySink> sink;
            winrt::check_hresult(path->Open(sink.put()));
            sink->BeginFigure({x(0), depth}, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine({x(0), r});
            sink->AddBezier({{x(0), r * TabLightContour::Arc}, {x(r * TabLightContour::Arc), 0}, {x(r), 0}});
            sink->AddLine({x(reach), 0}); sink->EndFigure(D2D1_FIGURE_END_OPEN);
            winrt::check_hresult(sink->Close());
            auto stroke = field.rimShape.Shapes().GetAt(side).as<winrt::Microsoft::UI::Composition::CompositionSpriteShape>();
            stroke.Geometry().as<winrt::Microsoft::UI::Composition::CompositionPathGeometry>().Path(
                winrt::Microsoft::UI::Composition::CompositionPath(winrt::make<TabAmbientClip>(std::move(path))));
            auto brush = stroke.StrokeBrush().as<winrt::Microsoft::UI::Composition::CompositionRadialGradientBrush>();
            brush.EllipseCenter({x(r), r}); brush.EllipseRadius({std::max(1.f, reach - r), std::max(1.f, depth - r)});
        }
    }
    void Put(std::map<uint32_t, Field>& fields, uint32_t id, uint32_t color, Rect bounds, bool upper, bool connected = false) {
        const bool capped = !upper || connected;
        if (upper) {
            // Expose the upper half of the same light source above the tab.
            // Keep its peak at the original tab edge and its existing falloff
            // below that edge; only the enclosing strip clips the spill.
            bounds.Y -= .8f * bounds.Height;
            bounds.Height *= 1.8f;
        }
        auto& field = fields[id];
        if (field.visual && field.capped != capped) {
            (upper ? top : bottom).Children().Remove(field.visual);
            field = Field{};
        }
        if (!field.visual) {
            field.capped = capped;
            auto owner = upper ? top : bottom;
            auto compositor = owner.Compositor();
            field.visual = compositor.CreateSpriteVisual();
            field.visual.Comment((upper ? L"container:" : L"group:") + winrt::to_hstring(id));
            auto radial = [&](float x, float y, float radius) {
                auto brush = compositor.CreateRadialGradientBrush();
                brush.EllipseCenter({x, y});
                brush.EllipseRadius({radius, upper ? .8f / 1.8f : .8f});
                return brush;
            };
            if (!capped) {
                auto brush = radial(.5f, .8f / 1.8f, .5f);
                brush.EllipseRadius({.5f, .8f / 1.8f});
                field.brush = brush;
                field.visual.Brush(field.brush);
            } else {
                // A shallow elliptical crown avoids a flat colored band. Its
                // edge has radial distance 1/2; the wider end caps meet at that
                // same distance, so their alpha matches at every height. Keep
                // the caps bounded rather than dimming entire outer tabs as
                // one ellipse stretched across a large group would do.
                const float y = upper ? .8f / 1.8f : 1.f;
                auto center = radial(.5f, y, 1.f);
                field.brush = center;
                field.leftBrush = radial(2.f, y, 2.f);
                field.rightBrush = radial(-1.f, y, 2.f);
                field.left = compositor.CreateSpriteVisual();
                field.middle = compositor.CreateSpriteVisual();
                field.right = compositor.CreateSpriteVisual();
                field.left.Brush(field.leftBrush); field.middle.Brush(center); field.right.Brush(field.rightBrush);
                field.middle.Comment(upper ? L"container-center" : L"group-center");
                field.visual.Children().InsertAtTop(field.left);
                field.visual.Children().InsertAtTop(field.middle);
                field.visual.Children().InsertAtTop(field.right);
            }
            // A broad luminous shoulder makes both fields legible on light
            // tabs. Keep the horizontal radius inside the bounds so the ends
            // still disappear completely, rather than clipping a wider ellipse.
            // Radial alpha: 36%, 35%, 24%, 9%, 2%, then transparent.
            for (auto brush : {field.brush, field.leftBrush, field.rightBrush}) {
                if (!brush) continue;
                for (auto [offset, alpha] : {std::pair{0.f, 92}, {.12f, 88}, {.4f, 61}, {.65f, 23}, {.85f, 5}, {1.f, 0}})
                    brush.ColorStops().Append(compositor.CreateColorGradientStop(offset, {uint8_t(alpha), 0, 0, 0}));
            }
            owner.Children().InsertAtTop(field.visual);
        }
        if (field.color != color) {
            field.color = color;
            for (auto brush : {field.brush, field.leftBrush, field.rightBrush}) {
                if (!brush) continue;
                for (auto stop : brush.ColorStops()) Tint(stop, uint8_t(color >> 16), uint8_t(color >> 8), uint8_t(color));
            }
        }
        if (field.bounds != bounds) {
            field.bounds = bounds;
            Geometry(field.visual, {bounds.X, bounds.Y, 0}, {bounds.Width, bounds.Height});
            if (capped) {
                const auto cap = std::min(upper ? 16.f : 24.f, bounds.Width / 4);
                Geometry(field.left, {}, {cap, bounds.Height});
                Geometry(field.middle, {cap, 0, 0}, {bounds.Width - 2 * cap, bounds.Height});
                Geometry(field.right, {bounds.Width - cap, 0, 0}, {cap, bounds.Height});
            }
        }
    }
    void Prune(std::map<uint32_t, Field>& fields, std::set<uint32_t> const& live, bool upper, std::set<uint32_t> const& dim = {}) {
        for (auto it = fields.begin(); it != fields.end();) {
            if (live.count(it->first)) { ++it; continue; }
            Release(upper ? top : bottom, it->second, dim.count(it->first) != 0, TabGlowIgnition::DimLag, TabGlowIgnition::DimDuration);
            it = fields.erase(it);
        }
    }
    ~TabAmbient() {
        life.reset();
        settings.ColorValuesChanged(contrastToken);
        if (layer && layoutToken.value) layer.LayoutUpdated(layoutToken);
        if (observing) winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(renderingToken);
    }
};
