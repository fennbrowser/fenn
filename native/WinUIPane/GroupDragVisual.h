#pragma once

// Only WinUI objects live here; gesture and insertion decisions live in Rust.
struct GroupDragVisual {
    Controls::Canvas layer{nullptr};
    Controls::Border backing{nullptr}, placeholder{nullptr};
    Controls::Button button{nullptr};
    Windows::Foundation::Collections::IObservableMap<hstring,Windows::Foundation::IInspectable> model{nullptr};
    Windows::Foundation::IInspectable localDirection{nullptr};
    Thickness margin{};
    double grabX{}, top{};
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    void Create(Controls::Grid const& root) {
        if (layer) return;
        layer = Markup::XamlReader::Load(LR"(
<Canvas xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" IsHitTestVisible="False" FlowDirection="LeftToRight" AutomationProperties.AccessibilityView="Raw">
 <Border Background="{ThemeResource TabViewItemHeaderDragBackground}" CornerRadius="4"/>
</Canvas>)").as<Controls::Canvas>();
        backing = layer.Children().GetAt(0).as<Controls::Border>();
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
        layer.Children().Append(button);
        layer.Visibility(Visibility::Visible);
    }
    void Show(double x, double left, double width) {
        layer.Visibility(Visibility::Visible);
        if (button) {
            const auto buttonWidth=button.ActualWidth();
            const auto position=std::clamp(x-grabX,left,std::max(left,left+width-buttonWidth));
            Controls::Canvas::SetLeft(button,position);
            Controls::Canvas::SetTop(button,top);
            // SubtleButtonStyle is transparent at rest. Keep underlying tab
            // labels from showing through the actual button while it moves.
            backing.Width(buttonWidth); backing.Height(button.ActualHeight());
            Controls::Canvas::SetLeft(backing,position);
            Controls::Canvas::SetTop(backing,top);
        }
    }
    void Hide() {
        if (timer) timer.Stop();
        if (button) {
            uint32_t index;
            if (layer.Children().IndexOf(button,index)) layer.Children().RemoveAt(index);
            button.Margin(margin);
            if (localDirection==DependencyProperty::UnsetValue()) button.ClearValue(FrameworkElement::FlowDirectionProperty());
            else button.SetValue(FrameworkElement::FlowDirectionProperty(),localDirection);
            model.Insert(L"Header",button);
            button=nullptr; model=nullptr; placeholder=nullptr; localDirection=nullptr;
        }
        if (layer) layer.Visibility(Visibility::Collapsed);
    }
};
