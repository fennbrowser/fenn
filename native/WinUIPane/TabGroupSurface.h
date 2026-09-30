#pragma once

// XAML-only decoration under the stock TabView. It never participates in
// hit testing, focus, accessibility or measurement of the grouped source.
struct TabGroupSurface {
    Controls::Canvas layer{nullptr};
    std::map<uint32_t, Controls::Border> surfaces;
    struct Light {
        Microsoft::UI::Composition::ShapeVisual visual{nullptr};
        Microsoft::UI::Composition::CompositionRoundedRectangleGeometry geometry{nullptr};
        uint32_t color{};
        bool animated{};
        // While revealed (glow motion), the outline shows `reveal`: the light
        // captured in a surface and masked by a growing front. A recolor also
        // keeps `previous`, the light in its old color, beneath it
        // (`revealRoot` holds both).
        Microsoft::UI::Composition::SpriteVisual reveal{nullptr};
        Microsoft::UI::Composition::CompositionVisualSurface revealSurface{nullptr};
        Microsoft::UI::Composition::ContainerVisual revealRoot{nullptr};
        std::shared_ptr<Light> previous;
        // The old light shows only outside the front: masked by its complement,
        // since the new light composites over it rather than replacing it.
        Microsoft::UI::Composition::CompositionVisualSurface previousSurface{nullptr};
        Microsoft::UI::Composition::CompositionRadialGradientBrush previousFront{nullptr};
    };
    std::map<uint32_t, Light> lights;

    // The outline light: four corner strokes, then the material pools.
    static void Build(Light& light, Microsoft::UI::Composition::Compositor const& compositor) {
        light.visual = compositor.CreateShapeVisual();
        light.visual.Comment(L"group-outline-reflection");
        light.geometry = compositor.CreateRoundedRectangleGeometry();
        light.geometry.Offset({.5f, .5f});
        for (int corner = 0; corner < 4; ++corner) {
            auto brush = compositor.CreateRadialGradientBrush();
            brush.MappingMode(Microsoft::UI::Composition::CompositionMappingMode::Absolute);
            const int peak[] = {84, 36, 36, 68};
            for (auto [offset, alpha] : {std::pair{0.f, peak[corner]}, {.45f, peak[corner] / 2}, {1.f, 0}})
                brush.ColorStops().Append(compositor.CreateColorGradientStop(offset, {uint8_t(alpha), 0, 0, 0}));
            auto stroke = compositor.CreateSpriteShape(light.geometry);
            stroke.StrokeThickness(1.f);
            stroke.StrokeBrush(brush);
            light.visual.Shapes().Append(stroke);
        }
        // Reflected color across the material, confined to the existing
        // outline. Unequal light pools avoid a uniform colored fill;
        // TabAmbient's group halo continues them past the outline.
        // Absolute mapping, placed with the size below, lets glow
        // motion trail the pools behind geometry changes.
        for (auto const& pool : TabAmbient::Pools) {
            auto brush = compositor.CreateRadialGradientBrush();
            brush.MappingMode(Microsoft::UI::Composition::CompositionMappingMode::Absolute);
            for (auto [offset, level] : TabAmbient::PoolStops)
                brush.ColorStops().Append(compositor.CreateColorGradientStop(offset, {uint8_t(pool.peak * level), 0, 0, 0}));
            auto body = compositor.CreateSpriteShape(light.geometry);
            body.FillBrush(brush);
            light.visual.Shapes().Append(body);
        }
    }
    static void Shape(Light& light, float width, float height, float radius) {
        light.visual.Size({width, height});
        light.geometry.Size({std::max(0.f, width - 1), std::max(0.f, height - 1)});
        light.geometry.CornerRadius({std::max(0.f, radius - .5f), std::max(0.f, radius - .5f)});
        for (uint32_t corner = 0; corner < 4; ++corner) {
            auto brush = light.visual.Shapes().GetAt(corner).as<Microsoft::UI::Composition::CompositionSpriteShape>()
                .StrokeBrush().as<Microsoft::UI::Composition::CompositionRadialGradientBrush>();
            brush.EllipseCenter({corner % 2 ? width - radius : radius, corner / 2 ? height - radius : radius});
            brush.EllipseRadius({std::max(1.f, std::min(48.f, width / 3)), std::max(1.f, height * .6f)});
        }
        for (uint32_t index = 0; index < ARRAYSIZE(TabAmbient::Pools); ++index) {
            auto const& pool = TabAmbient::Pools[index];
            auto brush = light.visual.Shapes().GetAt(4 + index).as<Microsoft::UI::Composition::CompositionSpriteShape>()
                .FillBrush().as<Microsoft::UI::Composition::CompositionRadialGradientBrush>();
            brush.EllipseCenter({pool.x * width, pool.y * height});
            brush.EllipseRadius({std::max(1.f, TabAmbient::PoolRadiusX * width), std::max(1.f, TabAmbient::PoolRadiusY * height)});
        }
    }
    static void Paint(Light& light, uint32_t color) {
        light.color = color;
        auto shine = [](uint8_t channel) { return uint8_t(channel + (255 - channel) * .10f); };
        for (auto shape : light.visual.Shapes()) {
            auto sprite = shape.as<Microsoft::UI::Composition::CompositionSpriteShape>();
            const bool body = bool(sprite.FillBrush());
            auto brush = (body ? sprite.FillBrush() : sprite.StrokeBrush())
                .as<Microsoft::UI::Composition::CompositionRadialGradientBrush>();
            for (auto stop : brush.ColorStops())
                TabAmbient::Tint(stop, body ? uint8_t(color >> 16) : shine(uint8_t(color >> 16)),
                    body ? uint8_t(color >> 8) : shine(uint8_t(color >> 8)),
                    body ? uint8_t(color) : shine(uint8_t(color)));
        }
    }

    // Reveal a group's outline light from its primary pool; the same front
    // shape as the blurred light, so highlights catch as it arrives. With
    // `under`, that visual stays beneath (a recolor's old light).
    Microsoft::UI::Composition::CompositionRadialGradientBrush BeginReveal(uint32_t id,
            std::shared_ptr<Light> under = nullptr) {
        auto light = lights.find(id);
        auto surface = surfaces.find(id);
        if (light == lights.end() || surface == surfaces.end() || !light->second.visual) return nullptr;
        EndReveal(id);
        auto& entry = light->second;
        auto compositor = entry.visual.Compositor();
        const auto size = entry.visual.Size();
        entry.revealSurface = compositor.CreateVisualSurface();
        entry.revealSurface.SourceVisual(entry.visual);
        entry.revealSurface.SourceSize(size);
        const Windows::Foundation::Numerics::float2 center{TabGlowIgnition::OriginX * size.x, TabGlowIgnition::OriginY * size.y};
        auto front = TabGlowIgnition::Front(compositor, center, TabGlowIgnition::Reach(center, size));
        auto mask = compositor.CreateMaskBrush();
        mask.Source(compositor.CreateSurfaceBrush(entry.revealSurface)); mask.Mask(front);
        entry.reveal = compositor.CreateSpriteVisual();
        entry.reveal.Comment(L"group-outline-reveal");
        entry.reveal.Size(size); entry.reveal.Brush(mask);
        if (under) {
            entry.previous = std::move(under);
            entry.previousSurface = compositor.CreateVisualSurface();
            entry.previousSurface.SourceVisual(entry.previous->visual);
            entry.previousSurface.SourceSize(size);
            entry.previousFront = TabGlowIgnition::Front(compositor, center, TabGlowIgnition::Reach(center, size), true);
            auto previousMask = compositor.CreateMaskBrush();
            previousMask.Source(compositor.CreateSurfaceBrush(entry.previousSurface)); previousMask.Mask(entry.previousFront);
            auto previous = compositor.CreateSpriteVisual();
            previous.Comment(L"group-outline-previous");
            previous.Size(size); previous.Brush(previousMask);
            entry.revealRoot = compositor.CreateContainerVisual();
            entry.revealRoot.Comment(L"group-outline-recolor");
            entry.revealRoot.Size(size);
            entry.revealRoot.Children().InsertAtBottom(previous);
            entry.revealRoot.Children().InsertAtTop(entry.reveal);
            Hosting::ElementCompositionPreview::SetElementChildVisual(surface->second, entry.revealRoot);
        } else {
            Hosting::ElementCompositionPreview::SetElementChildVisual(surface->second, entry.reveal);
        }
        return front;
    }
    // Recolor: the old light gives way outside-in as the new color is
    // revealed; returns both fronts.
    std::vector<Microsoft::UI::Composition::CompositionRadialGradientBrush> BeginRecolor(uint32_t id, uint32_t oldColor) {
        auto light = lights.find(id);
        auto surface = surfaces.find(id);
        if (light == lights.end() || surface == surfaces.end() || !light->second.visual) return {};
        auto previous = std::make_shared<Light>();
        Build(*previous, light->second.visual.Compositor());
        previous->visual.Comment(L"group-outline-previous");
        auto const& border = surface->second;
        Shape(*previous, float(border.Width()), float(border.Height()), float(border.CornerRadius().TopLeft));
        Paint(*previous, oldColor);
        auto front = BeginReveal(id, std::move(previous));
        if (!front) return {};
        return {front, light->second.previousFront};
    }
    void EndReveal(uint32_t id) {
        auto light = lights.find(id);
        if (light == lights.end() || !light->second.reveal) return;
        auto& entry = light->second;
        if (auto surface = surfaces.find(id); surface != surfaces.end()) {
            auto child = Hosting::ElementCompositionPreview::GetElementChildVisual(surface->second);
            if (child && (child == entry.reveal || child == entry.revealRoot))
                Hosting::ElementCompositionPreview::SetElementChildVisual(surface->second, entry.visual);
        }
        entry.reveal = nullptr; entry.revealSurface = nullptr; entry.revealRoot = nullptr; entry.previous = nullptr;
        entry.previousSurface = nullptr; entry.previousFront = nullptr;
    }
    void EndReveals() { for (auto& [id, light] : lights) EndReveal(id); }

    // Reflections belong to the actual outline, below stock tab contents and
    // selection. Retain four soft corner lobes rather than lighting tab seams.
    // With glow motion, a group whose light went out (`dim`) fades first.
    void Lighting(std::map<uint32_t, uint32_t> const& colors, TabGlowIgnition* motion = nullptr,
                  std::set<uint32_t> const& dim = {}) {
        for (auto const& [id, border] : surfaces) {
            auto color = colors.find(id);
            if (color == colors.end()) continue;
            auto& light = lights[id];
            if (!light.visual) {
                Build(light, Hosting::ElementCompositionPreview::GetElementVisual(border).Compositor());
                Hosting::ElementCompositionPreview::SetElementChildVisual(border, light.visual);
            }
            const float width = float(border.Width()), height = float(border.Height());
            const float radius = float(border.CornerRadius().TopLeft);
            const Windows::Foundation::Numerics::float2 size{width, height};
            if (light.visual.Size() != size) {
                Shape(light, width, height, radius);
                if (light.reveal) { light.reveal.Size(size); light.revealSurface.SourceSize(size); }
                if (light.revealRoot) {
                    light.revealRoot.Size(size); light.previousSurface.SourceSize(size);
                    for (auto child : light.revealRoot.Children()) child.Size(size);
                }
                if (light.previous) Shape(*light.previous, width, height, radius);
            }
            if (light.color != color->second) Paint(light, color->second);
        }
        for (auto it = lights.begin(); it != lights.end();) {
            if (colors.count(it->first) && surfaces.count(it->first)) { ++it; continue; }
            EndReveal(it->first);
            if (surfaces.count(it->first)) {
                auto border = surfaces.at(it->first);
                if (motion && dim.count(it->first) && motion->Dims()) {
                    // Only clear the outline if a relit group has not replaced it.
                    motion->Dim(it->second.visual, 0, TabGlowIgnition::DimEdge + TabGlowIgnition::DimLag,
                        [border, visual = it->second.visual] {
                            if (Hosting::ElementCompositionPreview::GetElementChildVisual(border) == visual)
                                Hosting::ElementCompositionPreview::SetElementChildVisual(border, nullptr);
                        });
                } else {
                    Hosting::ElementCompositionPreview::SetElementChildVisual(border, nullptr);
                }
            }
            it = lights.erase(it);
        }
    }

    // The header's pointer-over state washes the whole group, not only the
    // button. The fill is a live theme brush inside the outline, under the tabs.
    uint32_t hovered{};
    void ApplyHover(uint32_t id, Controls::Border const& border) {
        auto fill = border.Child().as<Controls::Border>();
        const double opacity = id == hovered ? 1.0 : 0.0;
        if (fill.Opacity() != opacity) fill.Opacity(opacity);
    }
    void Hover(uint32_t id) {
        if (hovered == id) return;
        hovered = id;
        for (auto const& [group, border] : surfaces) ApplyHover(group, border);
    }

    void Create(Controls::Grid const& root) {
        layer = Markup::XamlReader::Load(LR"(<Canvas xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Grid.ColumnSpan="3" Canvas.ZIndex="-1" FlowDirection="LeftToRight" IsHitTestVisible="False" AutomationProperties.AccessibilityView="Raw"/>)").as<Controls::Canvas>();
        root.Children().Append(layer);
    }

    void Update(std::map<uint32_t, Windows::Foundation::Rect> const& bounds,
                Windows::Foundation::Rect viewport) {
        auto clip = layer.Clip().try_as<Media::RectangleGeometry>();
        if (!clip) { clip = Media::RectangleGeometry(); layer.Clip(clip); }
        if (clip.Rect() != viewport) clip.Rect(viewport);
        for (auto const& [id, rect] : bounds) {
            auto [entry, inserted] = surfaces.try_emplace(id, nullptr);
            auto& border = entry->second;
            if (!border) {
                border = Markup::XamlReader::Load(LR"(
<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
 BorderBrush="{ThemeResource ControlStrokeColorDefaultBrush}" BorderThickness="1" CornerRadius="8"
 IsHitTestVisible="False" AutomationProperties.AccessibilityView="Raw">
 <Border Background="{ThemeResource TabViewItemHeaderBackgroundPointerOver}" CornerRadius="7" Opacity="0"
  IsHitTestVisible="False" AutomationProperties.AccessibilityView="Raw"/>
</Border>)").as<Controls::Border>();
                Automation::AutomationProperties::SetAutomationId(border, L"FirefoxWinUIGroupSurface-" + to_hstring(id));
                layer.Children().Append(border);
                ApplyHover(id, border);
            }
            if (Controls::Canvas::GetLeft(border) != rect.X) Controls::Canvas::SetLeft(border, rect.X);
            if (Controls::Canvas::GetTop(border) != rect.Y) Controls::Canvas::SetTop(border, rect.Y);
            if (border.Width() != rect.Width) border.Width(rect.Width);
            if (border.Height() != rect.Height) border.Height(rect.Height);
        }
        // A surface whose group left stays in the canvas until the strip has
        // decided whether its light went out (Depart): the outline hosts the
        // group's color pools, so removing it here would cut them off.
        for (auto it = surfaces.begin(); it != surfaces.end();) {
            if (bounds.count(it->first)) { ++it; continue; }
            if (auto old = departing.find(it->first); old != departing.end()) { Remove(old->second); departing.erase(old); }
            departing.emplace(it->first, it->second);
            it = surfaces.erase(it);
        }
    }

    std::map<uint32_t, Controls::Border> departing;
    void Remove(Controls::Border const& border) {
        uint32_t index;
        if (border && layer.Children().IndexOf(border, index)) layer.Children().RemoveAt(index);
    }
    // Release this pass's departing surfaces. A lit group whose light went out
    // fades its whole outline, color pools and highlights together, then goes;
    // everything else (culling, unlit groups, motion off) goes at once.
    void Depart(TabGlowIgnition* motion, std::set<uint32_t> const& gone) {
        auto leaving = std::move(departing);
        departing.clear();
        for (auto const& [id, border] : leaving) {
            if (motion && motion->Dims() && gone.count(id) && Hosting::ElementCompositionPreview::GetElementChildVisual(border)) {
                motion->Dim(Hosting::ElementCompositionPreview::GetElementVisual(border), 0,
                    TabGlowIgnition::DimEdge + TabGlowIgnition::DimLag, [this, border] { Remove(border); });
            } else {
                Remove(border);
            }
        }
    }
};
