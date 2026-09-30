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
    };
    std::map<uint32_t, Light> lights;

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
                auto compositor = Hosting::ElementCompositionPreview::GetElementVisual(border).Compositor();
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
                Hosting::ElementCompositionPreview::SetElementChildVisual(border, light.visual);
            }
            const float width = float(border.Width()), height = float(border.Height());
            const float radius = float(border.CornerRadius().TopLeft);
            const Windows::Foundation::Numerics::float2 size{width, height};
            if (light.visual.Size() != size) {
                light.visual.Size(size);
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
            if (light.color != color->second) {
                light.color = color->second;
                auto shine = [](uint8_t channel) { return uint8_t(channel + (255 - channel) * .10f); };
                for (auto shape : light.visual.Shapes()) {
                    auto sprite = shape.as<Microsoft::UI::Composition::CompositionSpriteShape>();
                    const bool body = bool(sprite.FillBrush());
                    auto brush = (body ? sprite.FillBrush() : sprite.StrokeBrush())
                        .as<Microsoft::UI::Composition::CompositionRadialGradientBrush>();
                    for (auto stop : brush.ColorStops())
                        TabAmbient::Tint(stop, body ? uint8_t(light.color >> 16) : shine(uint8_t(light.color >> 16)),
                            body ? uint8_t(light.color >> 8) : shine(uint8_t(light.color >> 8)),
                            body ? uint8_t(light.color) : shine(uint8_t(light.color)));
                }
            }
        }
        for (auto it = lights.begin(); it != lights.end();) {
            if (colors.count(it->first) && surfaces.count(it->first)) { ++it; continue; }
            if (surfaces.count(it->first)) {
                auto border = surfaces.at(it->first);
                if (motion && dim.count(it->first) && motion->Enabled()) {
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
            if (motion && motion->Enabled() && gone.count(id) && Hosting::ElementCompositionPreview::GetElementChildVisual(border)) {
                motion->Dim(Hosting::ElementCompositionPreview::GetElementVisual(border), 0,
                    TabGlowIgnition::DimEdge + TabGlowIgnition::DimLag, [this, border] { Remove(border); });
            } else {
                Remove(border);
            }
        }
    }
};
