#pragma once

// Only WinUI objects live here; gesture and insertion decisions live in Rust.
struct GroupDragVisual {
    Controls::Canvas layer{nullptr};
    // The moving button sits in `chip`, whose themed backing keeps underlying
    // tab labels from showing through; the tear-off snapshot captures it whole.
    Controls::Border chip{nullptr}, placeholder{nullptr};
    Controls::Button button{nullptr};
    Windows::Foundation::Collections::IObservableMap<hstring,Windows::Foundation::IInspectable> model{nullptr};
    Windows::Foundation::IInspectable localDirection{nullptr};
    Thickness margin{};
    double grabX{}, top{};
    bool detached{};
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    void Create(Controls::Grid const& root) {
        if (layer) return;
        layer = Markup::XamlReader::Load(LR"(
<Canvas xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" IsHitTestVisible="False" FlowDirection="LeftToRight" AutomationProperties.AccessibilityView="Raw">
 <Border Background="{ThemeResource TabViewItemHeaderDragBackground}" CornerRadius="4" Visibility="Collapsed"/>
</Canvas>)").as<Controls::Canvas>();
        chip = layer.Children().GetAt(0).as<Controls::Border>();
        layer.Visibility(Visibility::Collapsed);
        Controls::Grid::SetColumnSpan(layer,3);
        Controls::Canvas::SetZIndex(layer,20);
        root.Children().Append(layer);
    }
    void Arm(Controls::Button const& source, Windows::Foundation::Point point) {
        auto bounds=source.TransformToVisual(layer).TransformBounds({0,0,float(source.ActualWidth()),float(source.ActualHeight())});
        grabX=point.X-bounds.X;
    }
    void Begin(Controls::Button const& source,
               Windows::Foundation::Collections::IObservableMap<hstring,Windows::Foundation::IInspectable> const& header) {
        if (button) return;
        const auto bounds=source.TransformToVisual(layer).TransformBounds({0,0,float(source.ActualWidth()),float(source.ActualHeight())});
        top=bounds.Y;
        button=source; model=header; margin=source.Margin();
        localDirection=source.ReadLocalValue(FrameworkElement::FlowDirectionProperty());
        const auto direction=source.FlowDirection();
        placeholder=Controls::Border();
        placeholder.Width(source.ActualWidth()); placeholder.Height(source.ActualHeight());
        placeholder.Margin(margin);
        placeholder.IsHitTestVisible(false);
        Automation::AutomationProperties::SetAccessibilityView(placeholder,Automation::Peers::AccessibilityView::Raw);
        model.Insert(L"Header",placeholder);
        // Release the header presenter before putting the same Button in the
        // un-clipped overlay. HWND capture already owns the promoted gesture.
        layer.XamlRoot().Content().as<FrameworkElement>().UpdateLayout();
        button.Margin(Thickness{0,0,0,0});
        button.FlowDirection(direction);
        chip.Child(button);
        chip.Visibility(Visibility::Visible);
        layer.Visibility(Visibility::Visible);
    }
    void Show(double x, double left, double width) {
        layer.Visibility(Visibility::Visible);
        if (button) {
            const auto buttonWidth=button.ActualWidth();
            const auto position=std::clamp(x-grabX,left,std::max(left,left+width-buttonWidth));
            Controls::Canvas::SetLeft(chip,position);
            Controls::Canvas::SetTop(chip,top);
        }
    }
    // A torn-off group is drawn by the floating window instead. Opacity, not
    // Visibility: the chip must stay laid out for its snapshot, and capturing
    // a descendant excludes the layer's own opacity.
    void Detach(bool value) {
        if (detached==value || !layer) return;
        detached=value;
        layer.Opacity(value ? 0 : 1);
    }
    void Hide() {
        if (timer) timer.Stop();
        Detach(false);
        if (button) {
            if (chip.Child()==button) chip.Child(nullptr);
            chip.Visibility(Visibility::Collapsed);
            button.Margin(margin);
            if (localDirection==DependencyProperty::UnsetValue()) button.ClearValue(FrameworkElement::FlowDirectionProperty());
            else button.SetValue(FrameworkElement::FlowDirectionProperty(),localDirection);
            model.Insert(L"Header",button);
            button=nullptr; model=nullptr; placeholder=nullptr; localDirection=nullptr;
        }
        if (layer) layer.Visibility(Visibility::Collapsed);
    }
};
