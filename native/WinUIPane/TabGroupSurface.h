#pragma once

// XAML-only decoration under the stock TabView. It never participates in
// hit testing, focus, accessibility or measurement of the grouped source.
struct TabGroupSurface {
    Controls::Canvas layer{nullptr};
    std::map<uint32_t, Controls::Border> surfaces;
    uint32_t hovered{};
    Style normalStyle{nullptr}, hoverStyle{nullptr};

    void Create(Controls::Grid const& root) {
        normalStyle = Markup::XamlReader::Load(LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="Border"><Setter Property="Background" Value="{ThemeResource ControlAltFillColorTertiaryBrush}"/></Style>)").as<Style>();
        hoverStyle = Markup::XamlReader::Load(LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="Border"><Setter Property="Background" Value="{ThemeResource ControlAltFillColorQuarternaryBrush}"/></Style>)").as<Style>();
        layer = Markup::XamlReader::Load(LR"(<Canvas xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Grid.ColumnSpan="3" Canvas.ZIndex="-1" FlowDirection="LeftToRight" IsHitTestVisible="False" AutomationProperties.AccessibilityView="Raw"/>)").as<Controls::Canvas>();
        root.Children().Append(layer);
    }

    void Hover(uint32_t id) {
        if (hovered == id) return;
        hovered = id;
        for (auto const& [key, border] : surfaces) border.Style(key == hovered ? hoverStyle : normalStyle);
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
 IsHitTestVisible="False" AutomationProperties.AccessibilityView="Raw"/>)").as<Controls::Border>();
                Automation::AutomationProperties::SetAutomationId(border, L"FirefoxWinUIGroupSurface-" + to_hstring(id));
                layer.Children().Append(border);
            }
            if (Controls::Canvas::GetLeft(border) != rect.X) Controls::Canvas::SetLeft(border, rect.X);
            if (Controls::Canvas::GetTop(border) != rect.Y) Controls::Canvas::SetTop(border, rect.Y);
            if (border.Width() != rect.Width) border.Width(rect.Width);
            if (border.Height() != rect.Height) border.Height(rect.Height);
            const auto style = id == hovered ? hoverStyle : normalStyle;
            if (border.Style() != style) border.Style(style);
        }
        for (auto it = surfaces.begin(); it != surfaces.end();) {
            if (bounds.count(it->first)) { ++it; continue; }
            uint32_t index;
            if (layer.Children().IndexOf(it->second, index)) layer.Children().RemoveAt(index);
            it = surfaces.erase(it);
        }
    }
};
