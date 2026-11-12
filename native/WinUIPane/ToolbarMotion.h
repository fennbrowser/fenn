#pragma once
#include "ShellMotion.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Dispatching.h>

// Visual lifetime only. Firefox's placement is installed immediately; removed
// controls are temporary, non-interactive exit faces outside the live layout.
struct ToolbarMotion {
    using Element = winrt::Microsoft::UI::Xaml::FrameworkElement;
    using UIElement = winrt::Microsoft::UI::Xaml::UIElement;
    using Grid = winrt::Microsoft::UI::Xaml::Controls::Grid;
    using Visual = winrt::Microsoft::UI::Composition::Visual;
    using Preview = winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview;
    struct Position { Element element{nullptr}; float x{}, y{}, width{}, height{}, opacity{1}; };
    using Positions = std::vector<Position>;
    struct Moving { Element element{nullptr}; float dx{}, opacity{1}; bool entering{}; };
    struct Input { UIElement element{nullptr}; bool hit{}; winrt::Microsoft::UI::Xaml::Automation::Peers::AccessibilityView accessibility{}; winrt::hstring id; bool tabStop{}; };
    struct Leaving { Element element{nullptr}; double width{}, height{}; float from{1}, opacity{1}; };
    Grid host{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Canvas overlay{nullptr};
    std::vector<Moving> moving;
    std::vector<Leaving> leaving;
    std::vector<Input> inputs;
    winrt::Microsoft::UI::Composition::CompositionScopedBatch batch{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer watchdog{nullptr};
    std::shared_ptr<int> life = std::make_shared<int>(0);
    uint64_t generation{};
    winrt::event_token layoutToken{};
    bool waiting{};
    std::vector<std::pair<Visual,float>> pendingFaces;
    enum class Phase { Idle, Exit, Move, Enter };
    Phase phase{Phase::Idle};
    std::function<void()> finished;

    static Positions Capture(Grid const& host, std::vector<UIElement> const& order) {
        Positions result;
        if (!host || !host.IsLoaded()) return result;
        for (auto child : order) {
            auto element = child.as<Element>();
            auto at = element.TransformToVisual(host).TransformPoint({0,0});
            result.push_back({element,at.X,at.Y,float(element.ActualWidth()),float(element.ActualHeight()),Preview::GetElementVisual(element).Opacity()});
        }
        return result;
    }
    void VisualOnly(winrt::Microsoft::UI::Xaml::DependencyObject const& node) {
        namespace X = winrt::Microsoft::UI::Xaml;
        if (auto element = node.try_as<UIElement>()) {
            auto control = element.try_as<X::Controls::Control>();
            inputs.push_back({element,element.IsHitTestVisible(),X::Automation::AutomationProperties::GetAccessibilityView(element),
                X::Automation::AutomationProperties::GetAutomationId(element),control && control.IsTabStop()});
            element.IsHitTestVisible(false);
            X::Automation::AutomationProperties::SetAccessibilityView(element,X::Automation::Peers::AccessibilityView::Raw);
            X::Automation::AutomationProperties::SetAutomationId(element,L"");
            if (control) control.IsTabStop(false);
        }
        for (int i=0;i<X::Media::VisualTreeHelper::GetChildrenCount(node);++i)
            VisualOnly(X::Media::VisualTreeHelper::GetChild(node,i));
    }
    void Cancel() noexcept {
        const bool wasActive=waiting || phase!=Phase::Idle;
        ++generation; phase=Phase::Idle; batch=nullptr;
        if (waiting) { try { host.LayoutUpdated(layoutToken); } catch (...) {} waiting=false; }
        for (auto const& [visual,opacity] : pendingFaces) try { visual.Opacity(opacity); } catch (...) {}
        pendingFaces.clear();
        if (overlay) try { overlay.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed); } catch (...) {}
        if (watchdog) { try { watchdog.Stop(); } catch (...) {} watchdog=nullptr; }
        for (auto const& item : moving) try {
            auto visual=Preview::GetElementVisual(item.element);
            visual.StopAnimation(L"Opacity"); visual.Properties().StopAnimation(L"Translation.X"); visual.Properties().StopAnimation(L"Translation.Y");
            visual.Opacity(item.opacity); visual.Properties().InsertVector3(L"Translation",{0,0,0});
        } catch (...) {}
        for (auto const& item : leaving) try {
            auto visual=Preview::GetElementVisual(item.element);
            visual.StopAnimation(L"Opacity"); visual.Properties().StopAnimation(L"Translation.Y");
            visual.Opacity(item.opacity); visual.Properties().InsertVector3(L"Translation",{0,0,0});
            item.element.Width(item.width); item.element.Height(item.height);
        } catch (...) {}
        for (auto const& item : inputs) try {
            namespace X = winrt::Microsoft::UI::Xaml;
            item.element.IsHitTestVisible(item.hit);
            X::Automation::AutomationProperties::SetAccessibilityView(item.element,item.accessibility);
            X::Automation::AutomationProperties::SetAutomationId(item.element,item.id);
            if (auto control=item.element.try_as<X::Controls::Control>()) control.IsTabStop(item.tabStop);
        } catch (...) {}
        inputs.clear(); moving.clear(); leaving.clear();
        if (overlay) try {
            overlay.Children().Clear(); uint32_t index{};
            if (host && host.Children().IndexOf(overlay,index)) host.Children().RemoveAt(index);
        } catch (...) {}
        overlay=nullptr;
        if (wasActive && finished) try { finished(); } catch (...) {}
    }
    ~ToolbarMotion() { life.reset(); Cancel(); }
    static void Animate(Visual const& visual, wchar_t const* property, float from, float to, ShellMotion::Kind kind, bool translation=false) {
        auto animation=visual.Compositor().CreateScalarKeyFrameAnimation();
        animation.InsertKeyFrame(0,from); animation.InsertKeyFrame(1,to,ShellMotion::Ease(visual.Compositor(),kind));
        animation.Duration(ShellMotion::Duration(kind));
        if (translation) visual.Properties().StartAnimation(property,animation); else visual.StartAnimation(property,animation);
    }
    void Run(Phase next) {
        if (!ShellMotion::Enabled()) { Cancel(); return; }
        phase=next;
        auto compositor=Preview::GetElementVisual(host).Compositor();
        batch=compositor.CreateScopedBatch(winrt::Microsoft::UI::Composition::CompositionBatchTypes::Animation);
        bool any=false;
        if (next==Phase::Exit) for (auto const& item : leaving) {
            auto visual=Preview::GetElementVisual(item.element); any=true;
            Animate(visual,L"Opacity",item.from,0,ShellMotion::Kind::Exit);
            Animate(visual,L"Translation.Y",0,SHELL_MOTION_TRAVEL_DIP,ShellMotion::Kind::Exit,true);
        }
        // A property write can be replaced by a late XAML arrangement. An
        // explicit constant animation owns the held offset throughout exit.
        if (next==Phase::Exit && !leaving.empty()) for (auto const& item : moving)
            if (!item.entering && std::abs(item.dx)>=.5f)
                Animate(Preview::GetElementVisual(item.element),L"Translation.X",item.dx,item.dx,ShellMotion::Kind::Exit,true);
        if (next==Phase::Move) for (auto const& item : moving) if (!item.entering && std::abs(item.dx)>=.5f) {
            any=true; Animate(Preview::GetElementVisual(item.element),L"Translation.X",item.dx,0,ShellMotion::Kind::Reposition,true);
        }
        if (next==Phase::Enter) for (auto const& item : moving) if (item.entering) {
            auto visual=Preview::GetElementVisual(item.element); any=true;
            Animate(visual,L"Opacity",0,item.opacity,ShellMotion::Kind::Fade);
            Animate(visual,L"Translation.Y",SHELL_MOTION_TRAVEL_DIP,0,ShellMotion::Kind::Enter,true);
        }
        auto advance=[this,next] {
            if (next==Phase::Exit) { if (overlay) overlay.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed); Run(Phase::Move); }
            else if (next==Phase::Move) Run(Phase::Enter);
            else Cancel();
        };
        if (!any) { batch.End(); batch=nullptr; advance(); return; }
        batch.Completed([this,weak=std::weak_ptr<int>(life),serial=generation,advance](auto&&,auto&&) {
            if (!weak.expired() && generation==serial) try { advance(); } catch (...) { Cancel(); }
        });
        batch.End();
    }
    void Watch() {
        if (watchdog) watchdog.Stop();
        watchdog=host.DispatcherQueue().CreateTimer(); watchdog.IsRepeating(false); watchdog.Interval(std::chrono::milliseconds(1000));
        watchdog.Tick([this,weak=std::weak_ptr<int>(life)](auto&&,auto&&){if(!weak.expired())Cancel();}); watchdog.Start();
    }
    void Schedule(Grid const& root, Positions before, std::vector<UIElement> after) {
        host=root;
        if (before.empty() || !host.IsLoaded() || !ShellMotion::Enabled()) return;
        for (auto child : after) if (std::none_of(before.begin(),before.end(),[&](auto const& p){return p.element==child;})) {
            auto visual=Preview::GetElementVisual(child);
            pendingFaces.emplace_back(visual,visual.Opacity()); visual.Opacity(0);
        }
        waiting=true;
        layoutToken=host.LayoutUpdated([this,weak=std::weak_ptr<int>(life),serial=generation,before=std::move(before),after=std::move(after)](auto&&,auto&&) {
            if (weak.expired() || !waiting || generation!=serial) return;
            waiting=false; host.LayoutUpdated(layoutToken);
            for (auto const& [visual,opacity] : pendingFaces) visual.Opacity(opacity);
            pendingFaces.clear();
            try { Begin(host,before,after); } catch (...) { Cancel(); }
        });
        Watch();
    }
    void Begin(Grid const& root, Positions const& before, std::vector<UIElement> const& after) {
        namespace X = winrt::Microsoft::UI::Xaml;
        host=root;
        if (before.empty() || !host.IsLoaded() || !ShellMotion::Enabled()) return;
        for (auto child : after) {
            auto element=child.as<Element>();
            auto old=std::find_if(before.begin(),before.end(),[&](auto const& p){return p.element==element;});
            auto visual=Preview::GetElementVisual(element);
            Preview::SetIsTranslationEnabled(element,true);
            const bool entering=old==before.end() && element.ActualWidth()>0;
            const float dx=old==before.end()?0:old->x-element.TransformToVisual(host).TransformPoint({0,0}).X;
            moving.push_back({element,dx,visual.Opacity(),entering});
            visual.Properties().InsertVector3(L"Translation",{dx,entering?float(SHELL_MOTION_TRAVEL_DIP):0,0});
            if (entering) visual.Opacity(0);
        }
        for (auto const& old : before) if (old.width>0 && old.opacity>0 && std::find(after.begin(),after.end(),old.element)==after.end()) {
            if (!overlay) {
                overlay=X::Controls::Canvas(); overlay.IsHitTestVisible(false);
                Grid::SetColumnSpan(overlay,int(host.ColumnDefinitions().Size()));
                X::Controls::Canvas::SetZIndex(overlay,10); host.Children().Append(overlay);
            }
            leaving.push_back({old.element,old.element.Width(),old.element.Height(),old.opacity,Preview::GetElementVisual(old.element).Opacity()});
            old.element.Width(old.width); old.element.Height(old.height);
            X::Controls::Canvas::SetLeft(old.element,old.x); X::Controls::Canvas::SetTop(old.element,old.y);
            overlay.Children().Append(old.element); Preview::SetIsTranslationEnabled(old.element,true);
            VisualOnly(old.element);
        }
        Watch();
        Run(Phase::Exit);
    }
};
