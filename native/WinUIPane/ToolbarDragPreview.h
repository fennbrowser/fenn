#pragma once
#include "shell_core.h"
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>

// WinUI layout and animation only; ShellCore supplies the logical permutation.
struct ToolbarDragPreview {
    using Element=winrt::Microsoft::UI::Xaml::FrameworkElement;
    using UIElement=winrt::Microsoft::UI::Xaml::UIElement;
    using Grid=winrt::Microsoft::UI::Xaml::Controls::Grid;
    Grid host{nullptr},gap{nullptr};
    Element source{nullptr};
    std::vector<UIElement> original;
    std::vector<std::pair<winrt::hstring,Element>> slots;
    std::vector<ShellToolbarDropSlot> geometry;
    std::function<void(std::vector<UIElement> const&,bool)> arrange;
    bool active{},changing{};
    size_t sourceIndex{};
    uint32_t target=UINT32_MAX;
    uint64_t revision{};
    double width{};
    using Positions=std::vector<std::pair<Element,float>>;
    Positions Capture() {
        Positions result;
        for(auto const& child:original){auto e=child.as<Element>();result.emplace_back(e,e.ActualOffset().x);}
        return result;
    }
    void Animate(Positions const& before,bool animate) {
        using winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview;
        animate=animate && winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled();
        for(auto const& [element,x]:before) {
            ElementCompositionPreview::SetIsTranslationEnabled(element,true);
            auto visual=ElementCompositionPreview::GetElementVisual(element);
            if(!animate){visual.Properties().StopAnimation(L"Translation.X");visual.Properties().InsertVector3(L"Translation",{0,0,0});continue;}
            auto shift=visual.Compositor().CreateScalarKeyFrameAnimation();
            // StartingValue includes any in-flight preview, avoiding jumps on rapid reversals.
            shift.SetScalarParameter(L"delta",x-element.ActualOffset().x);
            shift.InsertExpressionKeyFrame(0,L"this.StartingValue + delta");
            shift.InsertKeyFrame(1,0,visual.Compositor().CreateCubicBezierEasingFunction({0.2f,0.7f},{0.3f,1.f}));
            shift.Duration(std::chrono::milliseconds(160));
            visual.Properties().StartAnimation(L"Translation.X",shift);
        }
    }
    void Begin(Grid const& root,std::vector<UIElement> const& order,
        std::vector<std::pair<winrt::hstring,Element>> const& items,winrt::hstring const& id,uint64_t rev,double paletteWidth=32) {
        if(active)return;
        host=root;original=order;slots=items;revision=rev;width=host.XamlRoot().Size().Width;
        geometry.clear();sourceIndex=slots.size();
        for(size_t i=0;i<slots.size();i++){
            auto e=slots[i].second;geometry.push_back({e.ActualOffset().x,e.ActualWidth()});
            if(slots[i].first==id){sourceIndex=i;source=e;}
        }
        if(!source){gap=Grid();gap.Width(paletteWidth);gap.IsHitTestVisible(false);host.Children().Append(gap);}
        active=true;target=UINT32_MAX;
    }
    bool Hit(double x,bool rtl,uint32_t& index) {
        return active && shell_core_toolbar_drop_target(geometry.data(),geometry.size(),x,rtl,&index)==SHELL_STATUS_OK;
    }
    void Show(uint32_t index) {
        if(!active || target==index)return;
        ShellToolbarPreview plan{};
        if(shell_core_toolbar_preview(slots.size(),sourceIndex,index,&plan)!=SHELL_STATUS_OK)return;
        auto before=Capture();std::vector<UIElement> desired;
        for(uint32_t i=0;i<plan.len;i++)desired.push_back(plan.order[i]<slots.size()?slots[plan.order[i]].second.as<UIElement>():gap.as<UIElement>());
        for(size_t i=slots.size();i<original.size();i++)desired.push_back(original[i]);
        changing=true;if(source)source.Opacity(0);
        arrange(desired,sourceIndex==slots.size());host.UpdateLayout();changing=false;
        Animate(before,true);target=index;
    }
    void Reset(bool animate=true) {
        if(!active)return;
        auto before=Capture();changing=true;active=false;
        arrange(original,false);
        if(source)source.Opacity(1);
        if(gap){uint32_t index;if(host.Children().IndexOf(gap,index))host.Children().RemoveAt(index);}
        host.UpdateLayout();changing=false;Animate(before,animate);
        gap=nullptr;source=nullptr;slots.clear();geometry.clear();original.clear();target=UINT32_MAX;
    }
};
