#pragma once

// Only WinUI objects live here; gesture and insertion decisions live in Rust.
struct GroupDragVisual {
    Controls::Canvas layer{nullptr};
    Controls::Border chip{nullptr}, marker{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    void Create(Controls::Grid const& root) {
        if (layer) return;
        layer = Markup::XamlReader::Load(LR"(
<Canvas xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" IsHitTestVisible="False" FlowDirection="LeftToRight" AutomationProperties.AccessibilityView="Raw">
 <Border Background="{ThemeResource SolidBackgroundFillColorBaseBrush}" BorderBrush="{ThemeResource ControlStrokeColorDefaultBrush}" BorderThickness="1" CornerRadius="4" Padding="8,4" MaxWidth="160" Height="30">
  <TextBlock FontSize="12" TextTrimming="CharacterEllipsis" VerticalAlignment="Center"/>
 </Border>
 <Border Width="3" Height="30" Background="{ThemeResource SystemControlHighlightAccentBrush}" CornerRadius="1"/>
</Canvas>)").as<Controls::Canvas>();
        chip = layer.Children().GetAt(0).as<Controls::Border>();
        marker = layer.Children().GetAt(1).as<Controls::Border>();
        layer.Visibility(Visibility::Collapsed);
        root.Children().Append(layer);
    }
    void Show(hstring const& name, double x, double markerX, bool valid, double left, double width) {
        chip.Child().as<Controls::TextBlock>().Text(name);
        layer.Visibility(Visibility::Visible);
        Controls::Canvas::SetLeft(chip,std::clamp(x+12.0,left,std::max(left,left+width-160.0)));
        Controls::Canvas::SetTop(chip,3);
        marker.Visibility(valid ? Visibility::Visible : Visibility::Collapsed);
        Controls::Canvas::SetLeft(marker,std::clamp(markerX-1.5,left,std::max(left,left+width-3.0)));
        Controls::Canvas::SetTop(marker,3);
    }
    void Hide() { if (timer) timer.Stop(); if (layer) layer.Visibility(Visibility::Collapsed); }
};
