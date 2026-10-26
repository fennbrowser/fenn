#pragma once
#include <vector>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Composition.h>

// A row's compositor scale moves its edges without reflowing TabView. Keep
// the stock header columns and close control at their natural horizontal scale.
// Expressions follow the row on the compositor; no per-frame UI/layout work.
// Lifetime is the owning settle, including interrupted and unloaded rows.
struct TabContentScale {
    using Visual = winrt::Microsoft::UI::Composition::Visual;
    using Element = winrt::Microsoft::UI::Xaml::FrameworkElement;
    struct Part {
        Visual visual{nullptr};
        winrt::Windows::Foundation::Numerics::float3 scale{}, center{};
        winrt::Microsoft::UI::Composition::CompositionClip clip{nullptr};
    };
    std::vector<Part> parts;
    TabContentScale(TabContentScale const&) = delete;
    TabContentScale& operator=(TabContentScale const&) = delete;
    explicit TabContentScale(winrt::Microsoft::UI::Xaml::Controls::TabViewItem const& row) {
        try {
        namespace Xaml = winrt::Microsoft::UI::Xaml;
        auto owner = Xaml::Hosting::ElementCompositionPreview::GetElementVisual(row);
        auto attach = [&](Element const& element, bool text) {
            auto visual = Xaml::Hosting::ElementCompositionPreview::GetElementVisual(element);
            parts.push_back({visual, visual.Scale(), visual.CenterPoint(), visual.Clip()});
            auto inverse = visual.Compositor().CreateExpressionAnimation(L"natural / Max(0.01, row.Scale.X)");
            inverse.SetScalarParameter(L"natural", visual.Scale().x);
            inverse.SetReferenceParameter(L"row", owner);
            // Center fixed-size artwork in its moving slot. Text keeps its
            // leading edge and clips to the column's currently displayed width.
            const bool rtl = row.FlowDirection() == Xaml::FlowDirection::RightToLeft;
            auto center = visual.Compositor().CreateExpressionAnimation(
                text ? (rtl ? L"this.Target.Size.X" : L"0.0") : L"this.Target.Size.X * 0.5");
            visual.StartAnimation(L"CenterPoint.X", center);
            visual.StartAnimation(L"Scale.X", inverse);
            if (text && !visual.Clip()) {
                auto clip = visual.Compositor().CreateInsetClip();
                auto inset = visual.Compositor().CreateExpressionAnimation(L"Max(0, content.Size.X * (1 - row.Scale.X))");
                inset.SetReferenceParameter(L"content", visual);
                inset.SetReferenceParameter(L"row", owner);
                clip.StartAnimation(rtl ? L"LeftInset" : L"RightInset", inset);
                visual.Clip(clip);
            }
        };
        if (auto header = row.Header().try_as<Xaml::Controls::Grid>()) {
            for (auto child : header.Children())
                if (auto element = child.try_as<Element>())
                    attach(element, bool(element.try_as<Xaml::Controls::StackPanel>()));
        }
        if (Xaml::Media::VisualTreeHelper::GetChildrenCount(row)) {
            auto root = Xaml::Media::VisualTreeHelper::GetChild(row, 0).try_as<Element>();
            if (auto close = root ? root.FindName(L"CloseButton").try_as<Element>() : Element{nullptr}) attach(close, false);
        }
        } catch (...) { Reset(); throw; }
    }
    void Reset() noexcept {
        for (auto const& part : parts) {
            try {
                part.visual.StopAnimation(L"Scale.X");
                part.visual.StopAnimation(L"CenterPoint.X");
                part.visual.Scale(part.scale);
                part.visual.CenterPoint(part.center);
                part.visual.Clip(part.clip);
            } catch (...) {}
        }
        parts.clear();
    }
    ~TabContentScale() { Reset(); }
};
