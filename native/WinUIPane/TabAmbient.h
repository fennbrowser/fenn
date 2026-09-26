#pragma once
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include "TabGlowDiffusion.h"
#include "TabAmbientClip.h"
#include "TabLightContour.h"

// Decorative Composition children cannot take focus, enter UIA's control tree,
// or affect TabView measurement. Both fields use ordinary source-over alpha.
struct TabAmbient {
    using Rect = winrt::Windows::Foundation::Rect;
    using Sprite = winrt::Microsoft::UI::Composition::SpriteVisual;
    using Gradient = winrt::Microsoft::UI::Composition::CompositionGradientBrush;
    struct PartLayout {
        Sprite visual{nullptr};
        winrt::Microsoft::UI::Composition::ExpressionAnimation offset{nullptr}, size{nullptr};
    };
    struct Field {
        Sprite visual{nullptr}, left{nullptr}, middle{nullptr}, right{nullptr};
        Gradient brush{nullptr}, leftBrush{nullptr}, rightBrush{nullptr};
        winrt::Microsoft::UI::Composition::ShapeVisual rimShape{nullptr};
        winrt::Microsoft::UI::Composition::CompositionPathGeometry rimGeometry{nullptr};
        uint32_t color{}; Rect bounds{}; bool capped{}, animated{};
        float cornerLeft{-1}, cornerRight{-1};
        std::vector<PartLayout> layouts;
    };
    winrt::Microsoft::UI::Xaml::Controls::Canvas layer{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Canvas rimLayer{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual rims{nullptr};
    Field upperRim, lowerRim;
    std::map<uint64_t, Field> outerEdges;
    std::map<uint64_t, Field> sharedEdges;
    winrt::Microsoft::UI::Composition::ContainerVisual bottom{nullptr}, top{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual spill{nullptr};
    winrt::Microsoft::UI::Composition::LayerVisual diffusion{nullptr};
    winrt::Microsoft::UI::Composition::CompositionVisualSurface diffusedSurface{nullptr};
    winrt::Microsoft::UI::Composition::CompositionLinearGradientBrush edgeFade{nullptr};
    Sprite output{nullptr};
    TabLightContour::Mask faceMask;
    Sprite occluded{nullptr};
    winrt::Microsoft::UI::Composition::CompositionVisualSurface occludedSurface{nullptr};
    winrt::com_ptr<ID2D1Factory> clipFactory;
    Rect clippedViewport{}, clippedActive{};
    std::map<uint32_t, Field> groups, containers;
    std::map<uint32_t, Field> groupSpill, containerSpill;
    winrt::Windows::UI::ViewManagement::AccessibilitySettings accessibility;
    winrt::Windows::UI::ViewManagement::UISettings settings;
    winrt::event_token contrastToken{}, animationToken{}, layoutToken{}, renderingToken{};
    winrt::Microsoft::UI::Composition::ImplicitAnimationCollection geometryAnimations{nullptr};
    bool animateGeometry{}, frameReady{}, lastDirect{};
    float hostWidth{}, hostHeight{};
    double hostScale{}, scrollOffset{};
    bool lastRtl{};
#ifdef WINUI_PANE_TESTING
    bool disableMotionForTest{};
    uint64_t motionStarts{};
#endif
    std::shared_ptr<int> life = std::make_shared<int>(0);
    std::function<void()> refresh;
    bool observing{}, updating{}, reportedFailure{}, queued{};
    static void Snap(Field& field) {
        if (!field.animated) return;
        for (auto visual : {field.visual, field.left, field.middle, field.right}) {
            if (!visual) continue;
            visual.ImplicitAnimations(nullptr);
            // Implicit animation assignments retain their destination as the
            // API value. Direct writes disconnect animation and settle there.
            visual.Offset(visual.Offset());
            visual.Size(visual.Size());
        }
        field.animated = false;
    }
    void SettleMotion() {
        animateGeometry = false;
        for (auto fields : {&groups, &containers, &groupSpill, &containerSpill})
            for (auto& [id, field] : *fields) Snap(field);
    }
    void ResetMotion() {
        SettleMotion();
        frameReady = false;
    }
    void BeginFrame(bool direct, float width, float height, double scale, double scroll, bool rtl) {
        bool enabled = settings.AnimationsEnabled() && !accessibility.HighContrast();
#ifdef WINUI_PANE_TESTING
        enabled = enabled && !disableMotionForTest;
#endif
        animateGeometry = enabled && frameReady && !direct && !lastDirect &&
            width == hostWidth && height == hostHeight && scale == hostScale &&
            scroll == scrollOffset && rtl == lastRtl;
        if (!animateGeometry) SettleMotion();
        frameReady = true; lastDirect = direct;
        hostWidth = width; hostHeight = height; hostScale = scale;
        scrollOffset = scroll; lastRtl = rtl;
    }
    void PrepareGeometry(Field& field, Rect const& bounds) {
        const bool animate = animateGeometry && field.bounds.Width > 0 && field.bounds.Height > 0 &&
            // Large discontinuities should not sweep across unrelated tabs.
            std::abs(bounds.X - field.bounds.X) < std::max(240.f, hostWidth * .6f);
        if (!animate) { Snap(field); return; }
        if (!geometryAnimations) {
            auto compositor = field.visual.Compositor();
            auto ease = compositor.CreateCubicBezierEasingFunction({.2f, 0}, {.2f, 1});
            auto offset = compositor.CreateVector3KeyFrameAnimation();
            offset.Target(L"Offset"); offset.Duration(std::chrono::milliseconds(1200));
            offset.InsertExpressionKeyFrame(0, L"this.StartingValue");
            offset.InsertExpressionKeyFrame(1, L"this.FinalValue", ease);
            auto size = compositor.CreateVector2KeyFrameAnimation();
            size.Target(L"Size"); size.Duration(offset.Duration());
            size.InsertExpressionKeyFrame(0, L"this.StartingValue");
            size.InsertExpressionKeyFrame(1, L"this.FinalValue", ease);
            geometryAnimations = compositor.CreateImplicitAnimationCollection();
            geometryAnimations.Insert(L"Offset", offset);
            geometryAnimations.Insert(L"Size", size);
        }
        // Eligibility is explicit. Only these geometry writes trigger the
        // finite templates; unchanged snapshots do not restart the animation.
        field.visual.ImplicitAnimations(geometryAnimations);
        field.animated = true;
#ifdef WINUI_PANE_TESTING
        ++motionStarts;
#endif
    }
    static void Geometry(Sprite const& visual, winrt::Windows::Foundation::Numerics::float3 offset,
                         winrt::Windows::Foundation::Numerics::float2 size) {
        if (visual.Offset() != offset) visual.Offset(offset);
        if (visual.Size() != size) visual.Size(size);
    }
    static void BindParts(Field& field, bool upper, bool isSpill) {
        if (!field.animated || !field.middle) return;
        if (field.layouts.empty()) {
            auto compositor = field.visual.Compositor();
            auto bind = [&](Sprite const& visual, winrt::hstring offset, winrt::hstring size) {
                auto position = compositor.CreateExpressionAnimation(offset);
                auto dimensions = compositor.CreateExpressionAnimation(size);
                position.SetReferenceParameter(L"field", field.visual);
                dimensions.SetReferenceParameter(L"field", field.visual);
                field.layouts.push_back({visual, position, dimensions});
            };
            if (isSpill) {
                bind(field.middle, L"Vector3(0,0,0)", upper ? L"field.Size" : L"Vector2(field.Size.X * .72, field.Size.Y)");
                if (!upper) bind(field.left, L"Vector3(field.Size.X * .28,0,0)", L"Vector2(field.Size.X * .72, field.Size.Y)");
            } else {
                const winrt::hstring cap = upper ? L"Min(16.0, field.Size.X / 4.0)" : L"Min(24.0, field.Size.X / 4.0)";
                bind(field.left, L"Vector3(0,0,0)", L"Vector2(" + cap + L",field.Size.Y)");
                bind(field.middle, L"Vector3(" + cap + L",0,0)", L"Vector2(field.Size.X-2*" + cap + L",field.Size.Y)");
                bind(field.right, L"Vector3(field.Size.X-" + cap + L",0,0)", L"Vector2(" + cap + L",field.Size.Y)");
            }
        }
        // Derive pieces from the compositor's displayed size, including during
        // retargeting across the cap-width threshold. Independent child easing
        // can otherwise open seams when only some destinations change.
        for (auto const& layout : field.layouts) {
            layout.visual.StartAnimation(L"Offset", layout.offset);
            layout.visual.StartAnimation(L"Size", layout.size);
        }
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
        animationToken = settings.AnimationsEnabledChanged([this, weak, queue = root.DispatcherQueue()](auto&&, auto&&) {
            queue.TryEnqueue([this, weak] { if (!weak.expired()) { ResetMotion(); Update(); } });
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
                // Mask the already-diffused light, so blur cannot refill the
                // transparent tail before the new-tab button.
                diffusedSurface = compositor.CreateVisualSurface();
                diffusedSurface.SourceVisual(diffusion);
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
            if (!weak.expired()) Update();
        });
    }
    void Clip(Rect viewport, bool visible, Rect active = {}, bool fadeLeft = false, bool fadeRight = false) {
        visible = visible && !accessibility.HighContrast();
        if (!visible) ResetMotion();
        bottom.IsVisible(visible); top.IsVisible(visible);
        rims.IsVisible(visible);
        spill.IsVisible(visible);
        const winrt::Windows::Foundation::Numerics::float2 size{
            std::max(0.f, viewport.X + viewport.Width), std::max(0.f, viewport.Y + viewport.Height)};
        if (diffusion.Size() != size) diffusion.Size(size);
        if (output.Size() != size) {
            output.Size(size); diffusedSurface.SourceSize(size);
            occluded.Size(size); occludedSurface.SourceSize(size);
        }
        const float width = std::max(1.f, viewport.Width);
        const float tail = std::min(16.f, width / 2) / width;
        edgeFade.StartPoint({viewport.X, 0}); edgeFade.EndPoint({viewport.X + width, 0});
        auto stops = edgeFade.ColorStops();
        stops.GetAt(0).Color({uint8_t(fadeLeft ? 0 : 255), 255, 255, 255});
        stops.GetAt(1).Offset(tail); stops.GetAt(2).Offset(1 - tail);
        stops.GetAt(3).Color({uint8_t(fadeRight ? 0 : 255), 255, 255, 255});
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
    // One retained reflection per physical seam; two independent source lobes
    // leave a quiet middle rather than outlining every tab like a glass panel.
    void Edge(Field& field, uint32_t group, uint32_t left, uint32_t right, Rect bounds, bool outer) {
        auto compositor = rims.Compositor();
        if (!field.visual) {
            field.visual = compositor.CreateSpriteVisual();
            field.visual.Comment(outer ? L"group-outer-reflection" : L"group-shared-reflection");
            auto brush = compositor.CreateLinearGradientBrush();
            brush.StartPoint({0, 0}); brush.EndPoint({0, 1});
            for (float offset : {0.f, .16f, .42f, .68f, .88f, 1.f})
                brush.ColorStops().Append(compositor.CreateColorGradientStop(offset, {}));
            field.brush = brush; field.visual.Brush(brush);
            rims.Children().InsertAtTop(field.visual);
        }
        auto tint = [](uint32_t color, int alpha) {
            auto shine = [](uint8_t c) { return uint8_t(c + (255 - c) * .48f); };
            return winrt::Windows::UI::Color{uint8_t(alpha), shine(uint8_t(color >> 16)),
                shine(uint8_t(color >> 8)), shine(uint8_t(color))};
        };
        uint32_t container = left ? left : right;
        if (left && right) {
            container = 0;
            for (int shift : {0, 8, 16})
                container |= (((left >> shift & 255) + (right >> shift & 255)) / 2) << shift;
        }
        const int strength = outer ? 104 : 46;
        const winrt::Windows::UI::Color colors[] = {
            tint(container, 0), tint(container, container ? (outer ? 32 : 20) : 0),
            tint(group, 0), tint(group, strength / 5), tint(group, strength), tint(group, 0)};
        for (uint32_t i = 0; i < ARRAYSIZE(colors); ++i) {
            auto stop = field.brush.ColorStops().GetAt(i);
            if (stop.Color() != colors[i]) stop.Color(colors[i]);
        }
        field.bounds = bounds;
        Geometry(field.visual, {bounds.X, bounds.Y, 0}, {bounds.Width, bounds.Height});
    }
    void PruneEdges(std::map<uint64_t, Field>& fields, std::set<uint64_t> const& live) {
        for (auto it = fields.begin(); it != fields.end();) {
            if (live.count(it->first)) { ++it; continue; }
            rims.Children().Remove(it->second.visual); it = fields.erase(it);
        }
    }
    // Crisp surface reflections are outside the diffusion layer. Tint each
    // independently toward white; never mix the group and container colors.
    void Rim(Field& field, uint32_t color, Rect bounds, bool upper) {
        const bool visible = color && bounds.Width > 0 && bounds.Height > 0;
        if (!visible) { if (field.visual) field.visual.IsVisible(false); return; }
        if (!field.visual) {
            auto compositor = rims.Compositor();
            field.visual = compositor.CreateSpriteVisual();
            field.visual.Comment(upper ? L"active-container-reflection" : L"active-group-reflection");
            auto brush = compositor.CreateLinearGradientBrush();
            brush.StartPoint({0, .5f}); brush.EndPoint({1, .5f});
            for (auto [offset, alpha] : {std::pair{0.f, 0}, {.25f, 45}, {.5f, upper ? 155 : 100}, {.75f, upper ? 45 : 145}, {1.f, upper ? 0 : 185}})
                brush.ColorStops().Append(compositor.CreateColorGradientStop(offset, {uint8_t(alpha), 0, 0, 0}));
            field.brush = brush;
            if (upper) field.visual.Brush(brush);
            else {
                // The attached tab flares outward into the strip. It has no
                // closed bottom stroke (which would suggest a floating pill).
                brush.MappingMode(winrt::Microsoft::UI::Composition::CompositionMappingMode::Absolute);
                field.rimGeometry = compositor.CreatePathGeometry();
                auto stroke = compositor.CreateSpriteShape(field.rimGeometry);
                stroke.StrokeBrush(brush); stroke.StrokeThickness(1.25f);
                field.rimShape = compositor.CreateShapeVisual();
                field.rimShape.Shapes().Append(stroke);
                field.visual.Children().InsertAtTop(field.rimShape);
            }
            rims.Children().InsertAtTop(field.visual);
        }
        field.visual.IsVisible(true);
        if (field.color != color) {
            field.color = color;
            auto shine = [](uint8_t channel) { return uint8_t(channel + (255 - channel) * .55f); };
            for (auto stop : field.brush.ColorStops()) {
                auto value = stop.Color();
                value.R = shine(uint8_t(color >> 16)); value.G = shine(uint8_t(color >> 8)); value.B = shine(uint8_t(color));
                stop.Color(value);
            }
        }
        if (field.bounds != bounds) {
            field.bounds = bounds;
            field.visual.Offset({bounds.X, bounds.Y, 0});
            field.visual.Size({bounds.Width, bounds.Height});
            if (field.rimShape) {
                field.rimShape.Size({bounds.Width, bounds.Height});
                if (!clipFactory) winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, clipFactory.put()));
                winrt::com_ptr<ID2D1PathGeometry> path;
                winrt::check_hresult(clipFactory->CreatePathGeometry(path.put()));
                winrt::com_ptr<ID2D1GeometrySink> sink;
                winrt::check_hresult(path->Open(sink.put()));
                // Match the installed TabViewItem template's 4-DIP outward
                // radius. Separate open figures keep the content edge open.
                const float y = std::max(0.f, bounds.Height - .625f);
                const float r = std::min(TabLightContour::Flare, std::min(bounds.Width / 2, y));
                const float start = std::max(0.f, y - 12.f);
                TabLightContour::LowerEdges(sink.get(), bounds.Width, y, r, start);
                winrt::check_hresult(sink->Close());
                field.rimGeometry.Path(winrt::Microsoft::UI::Composition::CompositionPath(winrt::make<TabAmbientClip>(std::move(path))));
                auto brush = field.brush.as<winrt::Microsoft::UI::Composition::CompositionLinearGradientBrush>();
                brush.StartPoint({0, std::max(0.f, bounds.Height - 12)});
                brush.EndPoint({0, bounds.Height});
            }
        }
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
                for (auto stop : brush.ColorStops()) {
                    auto value = stop.Color();
                    value.R = uint8_t(color >> 16); value.G = uint8_t(color >> 8); value.B = uint8_t(color);
                    stop.Color(value);
                }
            }
        }
        if (field.bounds != bounds) {
            PrepareGeometry(field, bounds);
            field.bounds = bounds;
            Geometry(field.visual, {bounds.X, bounds.Y, 0}, {bounds.Width, bounds.Height});
            Geometry(field.middle, {}, {bounds.Width * (upper ? 1.f : .72f), bounds.Height});
            if (!upper) {
                Geometry(field.left, {bounds.Width * .28f, 0, 0}, {bounds.Width * .72f, bounds.Height});
            }
            BindParts(field, upper, true);
        }
    }
    void PruneSpill(std::map<uint32_t, Field>& fields, std::set<uint32_t> const& live) {
        for (auto it = fields.begin(); it != fields.end();) {
            if (live.count(it->first)) { ++it; continue; }
            Snap(it->second);
            spill.Children().Remove(it->second.visual); it = fields.erase(it);
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
        if (!reflection) {
            // Paths follow the control's current size. Only translate retained
            // contours; tweening their parent size would detach the right edge.
            if (field.bounds.Width == bounds.Width && field.bounds.Height == bounds.Height &&
                field.cornerLeft == leftRadius && field.cornerRight == rightRadius) PrepareGeometry(field, bounds);
            else Snap(field);
        }
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
            Snap(field);
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
                for (auto stop : brush.ColorStops()) {
                    auto value = stop.Color();
                    value.R = uint8_t(color >> 16); value.G = uint8_t(color >> 8); value.B = uint8_t(color);
                    stop.Color(value);
                }
            }
        }
        if (field.bounds != bounds) {
            PrepareGeometry(field, bounds);
            field.bounds = bounds;
            Geometry(field.visual, {bounds.X, bounds.Y, 0}, {bounds.Width, bounds.Height});
            if (capped) {
                const auto cap = std::min(upper ? 16.f : 24.f, bounds.Width / 4);
                Geometry(field.left, {}, {cap, bounds.Height});
                Geometry(field.middle, {cap, 0, 0}, {bounds.Width - 2 * cap, bounds.Height});
                Geometry(field.right, {bounds.Width - cap, 0, 0}, {cap, bounds.Height});
            }
            BindParts(field, upper, false);
        }
    }
    void Prune(std::map<uint32_t, Field>& fields, std::set<uint32_t> const& live, bool upper) {
        for (auto it = fields.begin(); it != fields.end();) {
            if (live.count(it->first)) { ++it; continue; }
            Snap(it->second);
            (upper ? top : bottom).Children().Remove(it->second.visual);
            it = fields.erase(it);
        }
    }
    ~TabAmbient() {
        life.reset();
        try { ResetMotion(); } catch (...) {}
        settings.ColorValuesChanged(contrastToken);
        settings.AnimationsEnabledChanged(animationToken);
        if (layer && layoutToken.value) layer.LayoutUpdated(layoutToken);
        if (observing) winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(renderingToken);
    }
};
