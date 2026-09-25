#pragma once
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include "TabGlowDiffusion.h"
#include "TabAmbientClip.h"

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
        uint32_t color{}; Rect bounds{}; bool capped{};
    };
    winrt::Microsoft::UI::Xaml::Controls::Canvas layer{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Canvas rimLayer{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual rims{nullptr};
    Field upperRim, lowerRim;
    winrt::Microsoft::UI::Composition::ContainerVisual bottom{nullptr}, top{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual spill{nullptr};
    winrt::Microsoft::UI::Composition::LayerVisual diffusion{nullptr};
    winrt::Microsoft::UI::Composition::CompositionVisualSurface diffusedSurface{nullptr};
    winrt::Microsoft::UI::Composition::CompositionLinearGradientBrush edgeFade{nullptr};
    Sprite output{nullptr};
    winrt::com_ptr<ID2D1Factory> clipFactory;
    Rect clippedViewport{}, clippedActive{};
    std::map<uint32_t, Field> groups, containers;
    std::map<uint32_t, Field> groupSpill, containerSpill;
    winrt::Windows::UI::ViewManagement::AccessibilitySettings accessibility;
    winrt::Windows::UI::ViewManagement::UISettings settings;
    winrt::event_token contrastToken{}, layoutToken{}, renderingToken{};
    std::shared_ptr<int> life = std::make_shared<int>(0);
    std::function<void()> refresh;
    bool observing{}, updating{}, reportedFailure{}, queued{};
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
                // Mask the already-diffused light, so blur cannot refill the
                // transparent tail before the new-tab button.
                diffusedSurface = compositor.CreateVisualSurface();
                diffusedSurface.SourceVisual(diffusion);
                auto source = compositor.CreateSurfaceBrush(diffusedSurface);
                edgeFade = compositor.CreateLinearGradientBrush();
                edgeFade.MappingMode(winrt::Microsoft::UI::Composition::CompositionMappingMode::Absolute);
                for (int i = 0; i < 4; ++i)
                    edgeFade.ColorStops().Append(compositor.CreateColorGradientStop(float(i) / 3, {255, 255, 255, 255}));
                auto mask = compositor.CreateMaskBrush(); mask.Source(source); mask.Mask(edgeFade);
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
        bottom.IsVisible(visible); top.IsVisible(visible);
        rims.IsVisible(visible);
        spill.IsVisible(visible);
        const winrt::Windows::Foundation::Numerics::float2 size{
            std::max(0.f, viewport.X + viewport.Width), std::max(0.f, viewport.Y + viewport.Height)};
        if (diffusion.Size() != size) diffusion.Size(size);
        if (output.Size() != size) {
            output.Size(size); diffusedSurface.SourceSize(size);
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
                const float r = std::min(4.f, std::min(bounds.Width / 2, y));
                const float start = std::max(0.f, y - 12.f);
                sink->BeginFigure({r, start}, D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine({r, y - r});
                sink->AddBezier({{r, y - r * .447715f}, {r * .447715f, y}, {0, y}});
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->BeginFigure({bounds.Width - r, start}, D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine({bounds.Width - r, y - r});
                sink->AddBezier({{bounds.Width - r, y - r * .447715f}, {bounds.Width - r * .447715f, y}, {bounds.Width, y}});
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
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
            field.bounds = bounds;
            field.visual.Offset({bounds.X, bounds.Y, 0}); field.visual.Size({bounds.Width, bounds.Height});
            field.middle.Size({bounds.Width * (upper ? 1.f : .72f), bounds.Height});
            if (!upper) {
                field.left.Size({bounds.Width * .72f, bounds.Height});
                field.left.Offset({bounds.Width * .28f, 0, 0});
            }
        }
    }
    void PruneSpill(std::map<uint32_t, Field>& fields, std::set<uint32_t> const& live) {
        for (auto it = fields.begin(); it != fields.end();) {
            if (live.count(it->first)) { ++it; continue; }
            spill.Children().Remove(it->second.visual); it = fields.erase(it);
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
                for (auto stop : brush.ColorStops()) {
                    auto value = stop.Color();
                    value.R = uint8_t(color >> 16); value.G = uint8_t(color >> 8); value.B = uint8_t(color);
                    stop.Color(value);
                }
            }
        }
        if (field.bounds != bounds) {
            field.bounds = bounds;
            field.visual.Offset({bounds.X, bounds.Y, 0});
            field.visual.Size({bounds.Width, bounds.Height});
            if (capped) {
                const auto cap = std::min(upper ? 16.f : 24.f, bounds.Width / 4);
                field.left.Size({cap, bounds.Height});
                field.middle.Offset({cap, 0, 0});
                field.middle.Size({bounds.Width - 2 * cap, bounds.Height});
                field.right.Offset({bounds.Width - cap, 0, 0});
                field.right.Size({cap, bounds.Height});
            }
        }
    }
    void Prune(std::map<uint32_t, Field>& fields, std::set<uint32_t> const& live, bool upper) {
        for (auto it = fields.begin(); it != fields.end();) {
            if (live.count(it->first)) { ++it; continue; }
            (upper ? top : bottom).Children().Remove(it->second.visual);
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
