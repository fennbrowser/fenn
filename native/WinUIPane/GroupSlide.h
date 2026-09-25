#pragma once
#include <winrt/Windows.UI.ViewManagement.h>

// WinUI-only lifetime and geometry. ShellCore plans arrivals/departures;
// Firefox's final visible list is installed before any visual animation runs.
struct GroupSlide {
    using Element = FrameworkElement;
    using Rect = Windows::Foundation::Rect;
    struct Frame { Element element{nullptr}; Rect bounds{}; };
    struct Moving { Element element{nullptr}; Media::Transform previous{nullptr}; };
    struct Ghost { Controls::Border clipper{nullptr}; Rect bounds{}; };
    struct Borrowed { Controls::TabViewItem source{nullptr}, ghost{nullptr}; Windows::Foundation::IInspectable content{nullptr}; };
    struct InputState { UIElement element{nullptr}; bool hitTest{}; Automation::Peers::AccessibilityView accessibility{}; hstring id; };
    std::vector<Borrowed> borrowed;
    std::vector<InputState> inputStates;
    std::map<uint64_t, Frame> before;
    std::vector<ShellGroupMotionRow> plan;
    std::vector<Moving> moving;
    std::vector<Ghost> departing;
    Controls::Canvas overlay{nullptr};
    Controls::Grid root{nullptr};
    Controls::ListViewBase viewport{nullptr};
    double oldViewportWidth{}, savedMinWidth{};
    bool holdingViewport{};
    Element active{nullptr};
    bool rtl{}, pending{};
    Media::Animation::Storyboard storyboard{nullptr};
    winrt::event_token rendering{};
    bool observing{};
    std::shared_ptr<int> life = std::make_shared<int>(0);
    static constexpr uint64_t AddKey = UINT64_MAX;
    static uint64_t GroupKey(uint32_t id) { return (uint64_t(1) << 32) | id; }
#ifdef WINUI_PANE_TESTING
    bool disableForTest{};
#endif
    bool Enabled() const {
#ifdef WINUI_PANE_TESTING
        if (disableForTest) return false;
#endif
        return Windows::UI::ViewManagement::UISettings().AnimationsEnabled();
    }
    static Rect Bounds(Element const& element, Element const& relative) {
        return element.TransformToVisual(relative).TransformBounds({0,0,float(element.ActualWidth()),float(element.ActualHeight())});
    }
    ~GroupSlide() { life.reset(); try { Cancel(); } catch (...) {} }
    void Cancel() noexcept {
        pending = false;
        if (observing) { try { Media::CompositionTarget::Rendering(rendering); } catch (...) {} observing = false; }
        if (storyboard) { try { storyboard.Stop(); } catch (...) {} storyboard = nullptr; }
        if (holdingViewport) {
            holdingViewport = false;
            try { viewport.MinWidth(savedMinWidth); } catch (...) {}
        }
        for (auto& item : moving) { try { item.element.RenderTransform(item.previous); } catch (...) {} }
        for (auto& item : inputStates) {
            try {
                item.element.IsHitTestVisible(item.hitTest);
                Automation::AutomationProperties::SetAccessibilityView(item.element,item.accessibility);
                Automation::AutomationProperties::SetAutomationId(item.element,item.id);
            } catch (...) {}
        }
        inputStates.clear();
        for (auto& item : borrowed) {
            try {
                item.ghost.Header(nullptr);
                if (!item.source.Header()) item.source.Header(item.content);
            } catch (...) {}
        }
        borrowed.clear();
        moving.clear(); departing.clear(); active = nullptr;
        if (overlay) { try { overlay.Children().Clear(); } catch (...) {} }
        before.clear(); plan.clear();
    }
    void Remember(uint64_t key, Element const& element) {
        if (!element || !element.IsLoaded() || before.size() >= 128) return;
        auto bounds = Bounds(element, root);
        if (bounds.Width <= 0 || bounds.X + bounds.Width <= 0 || bounds.X >= root.ActualWidth()) return;
        before.emplace(key, Frame{element, bounds});
    }
    void Prepare(Controls::Grid const& owner, Controls::TabView const& view,
                 Controls::ListViewBase const& list,
                 std::map<uint32_t, Controls::TabViewItem> const& rows,
                 InlineTabGroups const& groups, Controls::Button const& add,
                 std::vector<ShellGroupMotionRow> changes) {
        Cancel();
        if (changes.empty() || !Enabled() || !owner.IsLoaded()) return;
        root = owner;
        viewport = list;
        oldViewportWidth = list ? list.ActualWidth() : 0;
        rtl = root.FlowDirection() == FlowDirection::RightToLeft;
        for (auto const& [id, row] : rows) Remember(id, row);
        for (auto const& [id, run] : groups.runs) if (run.button) Remember(GroupKey(uint32_t(id)), run.button);
        Remember(AddKey, add);
        plan = std::move(changes);
        pending = true;
        if (!overlay) {
            overlay = Controls::Canvas();
            overlay.IsHitTestVisible(false);
            Automation::AutomationProperties::SetAccessibilityView(overlay, Automation::Peers::AccessibilityView::Raw);
            Controls::Grid::SetColumnSpan(overlay, 3);
            Controls::Canvas::SetZIndex(overlay, 10);
            root.Children().Append(overlay);
        }
        Media::RectangleGeometry clip;
        clip.Rect(Bounds(view, root));
        overlay.Clip(clip);
    }
    // Departing controls have left the visible tab layout. They are visual-only,
    // including their close buttons and any other descendants in the UIA tree.
    static void MakeVisualOnly(DependencyObject const& node, std::vector<InputState>* restore = nullptr) {
        if (auto element = node.try_as<UIElement>()) {
            if (restore) restore->push_back({element,element.IsHitTestVisible(),
                Automation::AutomationProperties::GetAccessibilityView(element),
                Automation::AutomationProperties::GetAutomationId(element)});
            element.IsHitTestVisible(false);
            Automation::AutomationProperties::SetAccessibilityView(element, Automation::Peers::AccessibilityView::Raw);
            Automation::AutomationProperties::SetAutomationId(element, L"");
        }
        for (int i = 0; i < Media::VisualTreeHelper::GetChildrenCount(node); ++i)
            MakeVisualOnly(Media::VisualTreeHelper::GetChild(node, i),restore);
    }
    void Slide(Element const& element, double from, double to) {
        if (std::abs(from - to) < .5) return;
        auto transform = Media::TranslateTransform();
        moving.push_back({element, element.RenderTransform()});
        element.RenderTransform(transform);
        transform.X(to);
        Media::Animation::DoubleAnimation animation;
        animation.From(from); animation.To(to);
        animation.Duration(Duration{Windows::Foundation::TimeSpan{InlineTabGroups::MotionDuration}, DurationType::TimeSpan});
        Media::Animation::CubicEase ease;
        ease.EasingMode(Media::Animation::EasingMode::EaseOut);
        animation.EasingFunction(ease);
        Media::Animation::Storyboard::SetTarget(animation, transform);
        Media::Animation::Storyboard::SetTargetProperty(animation, L"X");
        storyboard.Children().Append(animation);
    }
    void ClipActive() {
        if (!active) return;
        for (auto& ghost : departing) {
            // Transform into each clipper's local coordinates. Subtracting
            // root-space X positions is incorrect when RTL mirrors that space.
            const auto a = Bounds(active, ghost.clipper);
            Rect clip{0,0,ghost.bounds.Width,ghost.bounds.Height};
            if (a.X < clip.Width && a.X + a.Width > 0) {
                const auto left = std::clamp(a.X, 0.0f, clip.Width);
                const auto rightEdge = std::clamp(a.X + a.Width, 0.0f, clip.Width);
                if (left >= clip.Width - rightEdge) clip.Width = left;
                else { clip.X = rightEdge; clip.Width -= rightEdge; }
            }
            ghost.clipper.Clip().as<Media::RectangleGeometry>().Rect(clip);
        }
    }
    void Commit(std::map<uint32_t, Controls::TabViewItem> const& rows,
                InlineTabGroups const& groups, Controls::Button const& add, uint32_t activeId,
                bool retainDeparting = false) {
        if (!pending) return;
        pending = false;
        if (!Enabled()) { Cancel(); return; }
        root.UpdateLayout();
        const auto finalAdd = add ? Bounds(add, root).X : 0;
        if (viewport && viewport.ActualWidth() < oldViewportWidth) {
            savedMinWidth = viewport.MinWidth();
            holdingViewport = true;
            viewport.MinWidth(oldViewportWidth);
            root.UpdateLayout();
        }
        storyboard = Media::Animation::Storyboard();
        if (rows.count(activeId) && before.count(activeId)) {
            active = rows.at(activeId);
        }
        for (auto const& change : plan) {
            auto group = groups.runs.find(change.group);
            if (group == groups.runs.end() || !group->second.button) continue;
            auto anchor = Bounds(group->second.button, root);
            const auto edge = rtl ? anchor.X : anchor.X + anchor.Width;
            if (change.entering) {
                if (!rows.count(change.tab)) continue;
                auto row = rows.at(change.tab);
                if (!row.IsLoaded()) continue;
                const auto now = Bounds(row, root);
                if (now.X + now.Width <= 0 || now.X >= root.ActualWidth()) continue;
                Slide(row, edge - now.X - (rtl ? 0 : now.Width), 0);
            } else if (before.count(change.tab)) {
                auto old = before.at(change.tab);
                // WinUI can keep removed containers parented in its recycle
                // cache. Transfer only our header content into a visual-only
                // stock tab, never reparent that cached item or delay removal.
                auto source = old.element.as<Controls::TabViewItem>();
                auto content = source.Header();
                source.Header(nullptr);
                Controls::TabViewItem ghost;
                if (retainDeparting) borrowed.push_back({source,ghost,content});
                ghost.Header(content);
                ghost.IsSelected(source.IsSelected());
                ghost.IsClosable(source.IsClosable());
                ghost.FontWeight(source.FontWeight());
                ghost.HorizontalContentAlignment(source.HorizontalContentAlignment());
                old.element = ghost;
                old.element.Width(old.bounds.Width); old.element.Height(old.bounds.Height);
                Controls::Border wrapper;
                wrapper.Width(old.bounds.Width); wrapper.Height(old.bounds.Height);
                Media::RectangleGeometry clip;
                clip.Rect({0,0,old.bounds.Width,old.bounds.Height}); wrapper.Clip(clip);
                wrapper.Child(old.element);
                Controls::Canvas::SetLeft(wrapper, old.bounds.X);
                Controls::Canvas::SetTop(wrapper, old.bounds.Y);
                overlay.Children().Append(wrapper);
                ghost.UpdateLayout();
                MakeVisualOnly(ghost,retainDeparting ? &inputStates : nullptr);
                departing.push_back({wrapper, old.bounds});
                Slide(old.element, 0, edge - old.bounds.X - (rtl ? 0 : old.bounds.Width));
            }
        }
        auto move = [&](uint64_t key, Element const& element) {
            if (element && element.IsLoaded() && before.count(key)) Slide(element, before.at(key).bounds.X - Bounds(element, root).X, 0);
        };
        for (auto const& [id, row] : rows) move(id, row);
        for (auto const& [id, run] : groups.runs) if (run.button) move(GroupKey(uint32_t(id)), run.button);
        if (add && before.count(AddKey)) {
            const auto x = Bounds(add, root).X;
            Slide(add, before.at(AddKey).bounds.X - x, finalAdd - x);
        }
        auto weak = std::weak_ptr<int>(life);
        storyboard.Completed([this, weak](auto const& sender, auto const&) {
            if (!weak.expired() && storyboard == sender) Cancel();
        });
        if (active && !departing.empty()) {
            rendering = Media::CompositionTarget::Rendering([this, weak](auto&&, auto&&) {
                if (!weak.expired()) { try { ClipActive(); } catch (...) { Cancel(); } }
            });
            observing = true;
        }
        storyboard.Begin();
        ClipActive();
    }
};
