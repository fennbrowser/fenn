#pragma once

// The "List all tabs" button beside the system caption buttons. It is a stock
// Button; Firefox owns the panel it opens and this only names where it was
// pressed. Sized like a caption button so the pair reads as one group.
struct AllTabsButton {
    static constexpr double kWidth = 46;
    Controls::Button button{nullptr};

    // System caption glyphs dim while the window is inactive but return to full
    // strength under the pointer; mirror that on the glyph only so the hover
    // wash keeps its normal strength.
    bool windowActive = true, pointerOver = false;
    void SetWindowActive(bool active) {
        windowActive = active;
        UpdateGlyph();
    }
    void UpdateGlyph() {
        if (auto glyph = button.Content().try_as<UIElement>()) glyph.Opacity(windowActive || pointerOver ? 1.0 : 0.6);
    }

    AllTabsButton() {
        button = Markup::XamlReader::Load(
            LR"x(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Width="46" Height="32" MinWidth="0" Padding="0" CornerRadius="0" BorderThickness="0" VerticalAlignment="Top" HorizontalAlignment="Left" Visibility="Collapsed" ToolTipService.ToolTip="List all tabs" AutomationProperties.Name="List all tabs" AutomationProperties.AutomationId="FennAllTabs">
  <Button.Resources>
    <ResourceDictionary>
      <ResourceDictionary.ThemeDictionaries>
        <ResourceDictionary x:Key="Default">
          <SolidColorBrush x:Key="ButtonBackground" Color="Transparent"/>
          <StaticResource x:Key="ButtonBackgroundPointerOver" ResourceKey="SubtleFillColorSecondaryBrush"/>
          <StaticResource x:Key="ButtonBackgroundPressed" ResourceKey="SubtleFillColorTertiaryBrush"/>
          <SolidColorBrush x:Key="ButtonBorderBrush" Color="Transparent"/>
          <SolidColorBrush x:Key="ButtonBorderBrushPointerOver" Color="Transparent"/>
          <SolidColorBrush x:Key="ButtonBorderBrushPressed" Color="Transparent"/>
        </ResourceDictionary>
        <ResourceDictionary x:Key="Light">
          <SolidColorBrush x:Key="ButtonBackground" Color="Transparent"/>
          <StaticResource x:Key="ButtonBackgroundPointerOver" ResourceKey="SubtleFillColorSecondaryBrush"/>
          <StaticResource x:Key="ButtonBackgroundPressed" ResourceKey="SubtleFillColorTertiaryBrush"/>
          <SolidColorBrush x:Key="ButtonBorderBrush" Color="Transparent"/>
          <SolidColorBrush x:Key="ButtonBorderBrushPointerOver" Color="Transparent"/>
          <SolidColorBrush x:Key="ButtonBorderBrushPressed" Color="Transparent"/>
        </ResourceDictionary>
        <ResourceDictionary x:Key="HighContrast">
          <SolidColorBrush x:Key="ButtonBackground" Color="Transparent"/>
          <StaticResource x:Key="ButtonBackgroundPointerOver" ResourceKey="SystemColorHighlightColorBrush"/>
          <StaticResource x:Key="ButtonBackgroundPressed" ResourceKey="SystemColorHighlightColorBrush"/>
          <StaticResource x:Key="ButtonForegroundPointerOver" ResourceKey="SystemColorHighlightTextColorBrush"/>
          <StaticResource x:Key="ButtonForegroundPressed" ResourceKey="SystemColorHighlightTextColorBrush"/>
          <SolidColorBrush x:Key="ButtonBorderBrush" Color="Transparent"/>
          <SolidColorBrush x:Key="ButtonBorderBrushPointerOver" Color="Transparent"/>
          <SolidColorBrush x:Key="ButtonBorderBrushPressed" Color="Transparent"/>
        </ResourceDictionary>
      </ResourceDictionary.ThemeDictionaries>
    </ResourceDictionary>
  </Button.Resources>
  <FontIcon Glyph="&#xE70D;" FontSize="12" HorizontalAlignment="Center" VerticalAlignment="Center"/>
</Button>)x").as<Controls::Button>();
        button.PointerEntered([this](auto const&, auto const&) { pointerOver = true; UpdateGlyph(); });
        button.PointerExited([this](auto const&, auto const&) { pointerOver = false; UpdateGlyph(); });
        button.PointerCanceled([this](auto const&, auto const&) { pointerOver = false; UpdateGlyph(); });
        button.PointerCaptureLost([this](auto const&, auto const&) { pointerOver = false; UpdateGlyph(); });
    }
};
