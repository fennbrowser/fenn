#pragma once
#include "ToolbarMetrics.h"

// The "List all tabs" button, hosted in the strip header beside the product
// menu button. It is a stock subtle Button in the same size and style; Firefox
// owns the panel it opens and this only names where it was pressed.
struct AllTabsButton {
    static constexpr double kWidth = ToolbarMetrics::kButton;
    Controls::Button button{nullptr};

    // Kept for the window-activation hook; the button no longer mimics the
    // system caption glyphs, so there is nothing to dim.
    void SetWindowActive(bool) {}

    AllTabsButton() {
        button = Markup::XamlReader::Load(
            LR"x(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" MinWidth="0" MinHeight="0" Padding="0" VerticalAlignment="Center" ToolTipService.ToolTip="List all tabs" AutomationProperties.Name="List all tabs" AutomationProperties.AutomationId="FennAllTabs">
  <FontIcon Glyph="&#xE70D;" FontSize="12" HorizontalAlignment="Center" VerticalAlignment="Center"/>
</Button>)x").as<Controls::Button>();
        button.Width(ToolbarMetrics::kButton);
        button.Height(ToolbarMetrics::kButton);
    }
};
