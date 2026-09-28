#pragma once
#include <chrono>
#include <map>
#include <set>
#include <functional>
#include <cstdio>
#include <string>
#include <winrt/Windows.Data.Json.h>
#include "GroupTabsFlyout.h"

// WinUI projection only. Firefox supplies order/visibility; ShellCore validates
// the complete layout. All tab policy and group commands remain in Firefox.
struct InlineTabGroups {
    static constexpr auto MotionDuration = std::chrono::milliseconds(150);
    using Object = Windows::Foundation::IInspectable;
    using Vector = Windows::Foundation::Collections::IObservableVector<Object>;
    using Map = Windows::Foundation::Collections::IObservableMap<hstring, Object>;
    struct Run {
        Map model = single_threaded_observable_map<hstring, Object>();
        Vector items = single_threaded_observable_vector<Object>();
        Controls::Button button{nullptr};
        hstring name, label, headerText;
        Windows::Data::Json::JsonObject metadata;
        bool collapsed{};
        uint32_t color{};
        Media::SolidColorBrush accent{nullptr};
        Media::Animation::Storyboard arrowMotion{nullptr};
        Media::Animation::Storyboard indicatorMotion{nullptr};
        bool indicatorActive{};
        Run() { model.Insert(L"Items", items); model.Insert(L"Header", nullptr); }
        ~Run() {
            if (arrowMotion) { try { arrowMotion.Stop(); } catch (...) {} }
            if (indicatorMotion) { try { indicatorMotion.Stop(); } catch (...) {} }
        }
    };
    Data::CollectionViewSource source;
    Vector data = single_threaded_observable_vector<Object>();
    std::map<uint64_t, Run> runs;
    std::vector<uint64_t> order;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    std::function<void(uint32_t, bool)> toggle;
    std::function<void(uint32_t)> highlight;
    std::function<void(uint32_t, Windows::Foundation::Rect)> context;
    std::function<void(uint32_t, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&)> press;
    uint32_t suppressClick{};
    GroupTabsFlyout hover;
    bool configured{}, active{}, hasPreview{};
    Windows::Foundation::Point lastPointer{-1000000,-1000000};
    ~InlineTabGroups() { lifetime.reset(); }

    void ActiveGroup(uint32_t activeGroup, bool animate) {
        for (auto& [id, run] : runs) {
            if (!run.button) continue;
            const bool active = id == activeGroup;
            if (run.indicatorActive == active && (animate || !run.indicatorMotion)) continue;
            run.indicatorActive = active;
            auto marker = run.button.Content().as<Controls::Grid>().Children().GetAt(0).as<FrameworkElement>();
            const auto previous = marker.Width();
            if (run.indicatorMotion) { run.indicatorMotion.Stop(); run.indicatorMotion = nullptr; }
            const double target = active ? 16.0 : 6.0;
            marker.Width(target);
            if (!animate || !marker.IsLoaded()) continue;
            try {
                Media::Animation::DoubleAnimation motion;
                // Animate geometry rather than scaling the corner radii. The
                // existing 24-DIP minimum content width keeps the button stable.
                motion.EnableDependentAnimation(true);
                motion.From(previous);
                motion.To(target);
                motion.Duration(Duration{Windows::Foundation::TimeSpan{MotionDuration}, DurationType::TimeSpan});
                Media::Animation::CubicEase ease;
                ease.EasingMode(Media::Animation::EasingMode::EaseOut);
                motion.EasingFunction(ease);
                Media::Animation::Storyboard::SetTarget(motion, marker);
                Media::Animation::Storyboard::SetTargetProperty(motion, L"Width");
                run.indicatorMotion = Media::Animation::Storyboard();
                run.indicatorMotion.Children().Append(motion);
                run.indicatorMotion.Begin();
            } catch (...) {
                if (run.indicatorMotion) { try { run.indicatorMotion.Stop(); } catch (...) {} }
                run.indicatorMotion = nullptr;
                marker.Width(target);
            }
        }
    }

    // A transient popup can leave XAML's routed hover state stale after closing.
    // Observe native mouse moves through the host's existing message hook too.
    void PointerAt(Windows::Foundation::Point point, UIElement const& relativeTo) {
        if (point.X == lastPointer.X && point.Y == lastPointer.Y) return;
        lastPointer = point;
        auto surface = relativeTo.as<FrameworkElement>();
        if (point.X < 0 || point.Y < 0 || point.X >= surface.ActualWidth() || point.Y >= surface.ActualHeight()) {
            if (highlight) highlight(0);
            hover.Exit(hover.group);
            return;
        }
        for (auto const& [id, run] : runs) {
            if (!run.button || !run.button.IsLoaded() || run.button.Visibility() != Visibility::Visible || run.button.Opacity() == 0) continue;
            auto bounds = run.button.TransformToVisual(relativeTo).TransformBounds(
                {0,0,float(run.button.ActualWidth()),float(run.button.ActualHeight())});
            if (point.X >= bounds.X && point.X < bounds.X + bounds.Width &&
                point.Y >= bounds.Y && point.Y < bounds.Y + bounds.Height) {
                if (highlight) highlight(uint32_t(id));
                if (run.collapsed && (!hover.allowed || hover.allowed())) hover.Enter(uint32_t(id),run.button,run.metadata);
                else hover.Exit(hover.group);
                return;
            }
        }
        if (highlight) highlight(0);
        hover.Exit(hover.group);
    }

    bool Configure(Controls::ListViewBase const& list, Controls::TabView const& view) {
        if (configured) return true;
        // Localized template dependency. Callers retain the dropdown fallback
        // when the stock list part is unavailable on a different WinUI version.
        if (!list || list.Name() != L"TabListView") return false;
        if (!Media::VisualTreeHelper::GetChildrenCount(list)) return false;
        auto templateRoot = Media::VisualTreeHelper::GetChild(list, 0).try_as<FrameworkElement>();
        auto scroller = templateRoot ? templateRoot.FindName(L"ScrollViewer").try_as<Controls::ScrollViewer>() : nullptr;
        auto grid = Media::VisualTreeHelper::GetParent(list).try_as<Controls::Grid>();
        if (!scroller || !grid || grid.Name() != L"TabContainerGrid" || grid.ColumnDefinitions().Size() != 4) return false;
        if (!Media::VisualTreeHelper::GetChildrenCount(scroller)) return false;
        auto scrollRoot = Media::VisualTreeHelper::GetChild(scroller, 0).as<FrameworkElement>();
        auto decrease = scrollRoot.FindName(L"ScrollDecreaseButton").try_as<Controls::Primitives::RepeatButton>();
        auto increase = scrollRoot.FindName(L"ScrollIncreaseButton").try_as<Controls::Primitives::RepeatButton>();
        if (!decrease || !increase) return false;
        Controls::GroupStyle style;
        style.HidesIfEmpty(false);
        style.HeaderTemplate(Markup::XamlReader::Load(LR"(
<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
 <ContentPresenter Content="{Binding [Header]}" VerticalAlignment="Center"/>
</DataTemplate>)").as<DataTemplate>());
        // The stock header stacks a divider below its content, making a
        // 32-DIP capsule measure 41 DIP and displacing the entire tab row.
        // Keep the owner's 24-DIP empty-header gap without that extra height.
        style.HeaderContainerStyle(Markup::XamlReader::Load(LR"(
<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="ListViewHeaderItem">
 <Setter Property="IsTabStop" Value="False"/>
 <Setter Property="Margin" Value="0"/><Setter Property="Padding" Value="0"/>
 <Setter Property="MinHeight" Value="0"/><Setter Property="VerticalContentAlignment" Value="Center"/>
 <Setter Property="MinWidth" Value="24"/>
 <Setter Property="Template"><Setter.Value>
  <ControlTemplate TargetType="ListViewHeaderItem">
   <ContentPresenter Content="{TemplateBinding Content}" ContentTemplate="{TemplateBinding ContentTemplate}"
    HorizontalContentAlignment="{TemplateBinding HorizontalContentAlignment}"
    VerticalContentAlignment="{TemplateBinding VerticalContentAlignment}"/>
  </ControlTemplate>
 </Setter.Value></Setter>
</Style>)").as<Style>());
        list.GroupStyle().Append(style);
        list.ItemsPanel(Markup::XamlReader::Load(LR"(
<ItemsPanelTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
 <ItemsStackPanel Orientation="Horizontal" GroupHeaderPlacement="Left"/>
</ItemsPanelTemplate>)").as<Controls::ItemsPanelTemplate>());
        source.IsSourceGrouped(true);
        source.ItemsPath(PropertyPath(L"[Items]"));
        source.Source(data);
        // TabView's Equal-width calculation counts tabs but not grouped-source
        // headers (including the 24-DIP ungrouped gap). Its Auto tab column can
        // therefore consume the add-button space before minimum-width overflow
        // starts. Bound the actual list, and let its stock ScrollViewer detect
        // overflow from the real grouped extent instead of that tab-only estimate.
        scroller.HorizontalScrollBarVisibility(Controls::ScrollBarVisibility::Auto);
        // The same tab-only estimate also leaves stale arrow enablement when
        // it thinks everything fits. Reflect actual scroll endpoints; retain
        // the stock RepeatButtons and their native click/hold handlers.
        auto refreshArrows = [alive = std::weak_ptr<int>(lifetime), scroll = make_weak(scroller),
                              back = make_weak(decrease), forward = make_weak(increase)] {
            if (alive.expired()) return;
            auto scroller = scroll.get();
            auto decrease = back.get();
            auto increase = forward.get();
            if (!scroller || !decrease || !increase) return;
            const bool canBack = scroller.HorizontalOffset() > .1;
            const bool canForward = scroller.ScrollableWidth() - scroller.HorizontalOffset() > .1;
            if (decrease.IsEnabled() != canBack) decrease.IsEnabled(canBack);
            if (increase.IsEnabled() != canForward) increase.IsEnabled(canForward);
        };
        scroller.ViewChanged([refreshArrows](auto const&, auto const&) { refreshArrows(); });
        // Equal mode divides the full width among tabs alone, so in its fill
        // range the headers always overflow by their own width and show
        // arrows with nothing to reveal (typically after a collapse widens
        // the remaining tabs). Cap tabs to the space the headers leave;
        // TabView still writes Width, and the stock minimum keeps real overflow.
        auto themeWidth = [](wchar_t const* key, double fallback) {
            try {
                if (auto value = Application::Current().Resources().TryLookup(box_value(key)))
                    if (auto width = value.try_as<Windows::Foundation::IReference<double>>()) return width.Value();
            } catch (...) {}
            return fallback;
        };
        const double minTab = themeWidth(L"TabViewItemMinWidth", 100);
        const double maxTab = std::max(minTab, themeWidth(L"TabViewItemMaxWidth", 240));
        list.LayoutUpdated([this, alive = std::weak_ptr<int>(lifetime), weakList = make_weak(list),
                            weakView = make_weak(view), scroll = make_weak(scroller), minTab, maxTab,
                            back = make_weak(decrease), forward = make_weak(increase),
                            refreshArrows](auto const&, auto const&) {
            if (alive.expired()) return;
            auto list = weakList.get();
            auto view = weakView.get();
            if (!list || !view || view.ActualWidth() <= 0) return;
            auto grid = Media::VisualTreeHelper::GetParent(list).try_as<Controls::Grid>();
            if (!grid || grid.Name() != L"TabContainerGrid" || grid.ColumnDefinitions().Size() != 4) return;
            auto footer = grid.FindName(L"RightContentPresenter").try_as<FrameworkElement>();
            const auto available = std::max(0.0, view.ActualWidth() -
                grid.ColumnDefinitions().GetAt(0).ActualWidth() -
                grid.ColumnDefinitions().GetAt(2).ActualWidth() -
                (footer ? footer.DesiredSize().Width : 0));
            if (list.MaxWidth() != available) list.MaxWidth(available);
            if (auto scroller = scroll.get(); scroller && scroller.ActualWidth() > 0) {
                // Without arrows the viewport is the list less its fixed
                // inset (the scroller keeps 2 DIP); count visible arrows back
                // in, since fitting is exactly what hides them.
                double arrows = 0;
                for (auto const& weak : {back, forward}) {
                    auto arrow = weak.get();
                    if (!arrow || arrow.Visibility() != Visibility::Visible) continue;
                    arrows += arrow.ActualWidth() + arrow.Margin().Left + arrow.Margin().Right;
                }
                // Arrows can report Visible with a width while their slots
                // are still empty; subtract only space they actually occupy.
                const double gap = scroller.ActualWidth() - scroller.ViewportWidth();
                if (arrows > gap + .5) arrows = 0;
                const double inset = std::max(0.0, list.ActualWidth() - scroller.ViewportWidth() - arrows);
                CapTabWidths(available - inset, scroller.ExtentWidth(), minTab, maxTab);
                LogScroller(list, scroller, grid, available);
            }
            refreshArrows();
        });
        configured = true;
        return true;
    }

    // Opt-in (FIREFOX_WINUI_PANE_DIAGNOSTICS) record of what decides the stock
    // arrows, written only when it changes. Identifies whether the configured
    // ScrollViewer is still the templated one and still holds its local Auto.
    std::string lastScrollLog;
    void LogScroller(Controls::ListViewBase const& list, Controls::ScrollViewer const& scroller,
                     Controls::Grid const& grid, double available) {
        static const bool enabled = GetEnvironmentVariableW(L"FIREFOX_WINUI_PANE_DIAGNOSTICS", nullptr, 0) != 0;
        if (!enabled) return;
        Controls::ScrollViewer current{nullptr};
        if (Media::VisualTreeHelper::GetChildrenCount(list))
            if (auto root = Media::VisualTreeHelper::GetChild(list, 0).try_as<FrameworkElement>())
                current = root.FindName(L"ScrollViewer").try_as<Controls::ScrollViewer>();
        const auto local = scroller.ReadLocalValue(Controls::ScrollViewer::HorizontalScrollBarVisibilityProperty());
        const auto localAuto = local.try_as<Windows::Foundation::IReference<Controls::ScrollBarVisibility>>() &&
            unbox_value<Controls::ScrollBarVisibility>(local) == Controls::ScrollBarVisibility::Auto;
        const auto column = grid.ColumnDefinitions().GetAt(1);
        size_t tabs = 0, hidden = 0;
        double tabWidth = 0;
        for (auto const& [id, run] : runs) for (auto item : run.items) {
            auto row = item.as<Controls::TabViewItem>();
            if (row.Visibility() != Visibility::Visible) { ++hidden; continue; }
            ++tabs; tabWidth = row.ActualWidth();
        }
        char line[512];
        snprintf(line, sizeof line,
            "WinUIPane tab-scroll same=%d localAuto=%d mode=%d listMode=%d computed=%d extent=%.1f viewport=%.1f "
            "offset=%.1f scroller=%.1f list=%.1f listMax=%.1f available=%.1f column=%.1f/%d/%.1f "
            "runs=%zu tabs=%zu hidden=%zu tab=%.1f\n",
            current == scroller, localAuto, int(scroller.HorizontalScrollBarVisibility()),
            int(Controls::ScrollViewer::GetHorizontalScrollBarVisibility(list)),
            scroller.ComputedHorizontalScrollBarVisibility() == Visibility::Visible,
            scroller.ExtentWidth(), scroller.ViewportWidth(), scroller.HorizontalOffset(), scroller.ActualWidth(),
            list.ActualWidth(), list.MaxWidth(), available, column.Width().Value, int(column.Width().GridUnitType),
            column.ActualWidth(), runs.size(), tabs, hidden, tabWidth);
        if (lastScrollLog == line) return;
        lastScrollLog = line;
        fputs(line, stderr);
        fflush(stderr);
    }

    // `space` is the scroll viewport without arrows; `extent` is the grouped
    // content, so extent minus the tabs is every header, gap and padding.
    void CapTabWidths(double space, double extent, double minTab, double maxTab) {
        std::vector<Controls::TabViewItem> tabs;
        double measured = 0;
        size_t loaded = 0;
        for (auto const& [id, run] : runs) {
            for (auto item : run.items) {
                auto row = item.as<Controls::TabViewItem>();
                if (row.Visibility() != Visibility::Visible) continue;
                tabs.push_back(row);
                // Grouped-source rows can be measured and arranged while
                // IsLoaded stays false, so a nonzero measure marks realization.
                if (const auto width = row.DesiredSize().Width; width > 0) { measured += width; ++loaded; }
            }
        }
        if (!loaded || space <= 0) return;
        // Unrealized rows are estimated the same way ItemsStackPanel does.
        const double tabExtent = measured / loaded * tabs.size();
        const double slot = (space - std::max(0.0, extent - tabExtent)) / tabs.size();
        for (auto& row : tabs) {
            const auto margin = row.Margin();
            // Round down: layout rounding up by one pixel would reintroduce
            // the overflow this cap removes.
            const double cap = std::clamp(std::floor(slot - margin.Left - margin.Right), minTab, maxTab);
            if (std::abs(row.MaxWidth() - cap) > .5) row.MaxWidth(cap);
        }
    }

    void Metadata(Windows::Data::Json::JsonArray const& groups, bool animate = false) {
        hasPreview = false;
        for (auto value : groups) {
            auto group = value.GetObject();
            const auto id = uint32_t(group.GetNamedNumber(L"id"));
            auto found = runs.find(id);
            if (found == runs.end() || !found->second.button) continue;
            auto& run = found->second;
            run.metadata = group;
            if (group.GetNamedBoolean(L"collapsed") && group.HasKey(L"tabs") && group.GetNamedArray(L"tabs").Size()) hasPreview = true;
            hover.Refresh(id, group);
            auto name = group.GetNamedString(L"name"), label = group.GetNamedString(L"label");
            auto headerText = group.GetNamedString(L"headerText", L"");
            auto panel = run.button.Content().as<Controls::Grid>();
            auto rotor = panel.Children().GetAt(1).as<Controls::Grid>();
            auto text = panel.Children().GetAt(2).as<Controls::TextBlock>();
            const bool named = !headerText.empty();
            rotor.Visibility(named ? Visibility::Collapsed : Visibility::Visible);
            if (run.headerText != headerText) {
                run.headerText = headerText;
                text.Text(headerText);
                text.Visibility(named ? Visibility::Visible : Visibility::Collapsed);
                rotor.Margin(named ? Thickness{4,0,0,0} : Thickness{0,0,0,0});
                if (named) run.button.ClearValue(FrameworkElement::WidthProperty());
                else run.button.Width(32);
                if (run.arrowMotion) { run.arrowMotion.Stop(); run.arrowMotion = nullptr; }
            }
            const bool collapsed = group.GetNamedBoolean(L"collapsed");
            if (run.name.empty() || run.collapsed != collapsed) {
                const bool changed = run.collapsed != collapsed;
                run.collapsed = collapsed;
                auto panel = run.button.Content().as<Controls::Grid>();
                auto rotor = panel.Children().GetAt(1).as<Controls::Grid>();
                auto arrow = rotor.Children().GetAt(0).as<Controls::FontIcon>();
                // FontIcon owns its transform for native RTL mirroring. Rotate
                // a separate layout container so those transforms cannot clash.
                auto turn = rotor.RenderTransform().as<Media::RotateTransform>();
                const auto previous = turn.Angle();
                if (run.arrowMotion) { run.arrowMotion.Stop(); run.arrowMotion = nullptr; }
                arrow.Glyph(collapsed ? L"\uE70D" : L"\uE76C");
                turn.Angle(0);
                if (changed && animate && arrow.IsLoaded()) {
                    // Rotate the new glyph from the old direction. Preserve the
                    // current angle on reversal instead of jumping to an endpoint.
                    const auto direction = arrow.FlowDirection() == FlowDirection::RightToLeft ? -1.0 : 1.0;
                    Media::Animation::DoubleAnimation rotation;
                    rotation.From(previous + direction * (collapsed ? -90.0 : 90.0));
                    rotation.To(0.0);
                    rotation.Duration(Duration{Windows::Foundation::TimeSpan{MotionDuration}, DurationType::TimeSpan});
                    Media::Animation::CubicEase ease;
                    ease.EasingMode(Media::Animation::EasingMode::EaseOut);
                    rotation.EasingFunction(ease);
                    Media::Animation::Storyboard::SetTarget(rotation, turn);
                    Media::Animation::Storyboard::SetTargetProperty(rotation, L"Angle");
                    run.arrowMotion = Media::Animation::Storyboard();
                    run.arrowMotion.Children().Append(rotation);
                    run.arrowMotion.Begin();
                }
            }
            run.name = name;
            if (run.label != label) {
                run.label = label;
                Automation::AutomationProperties::SetName(run.button, label);
                Controls::ToolTipService::SetToolTip(run.button, collapsed ? nullptr : box_value(label));
            }
            const auto color = uint32_t(group.GetNamedNumber(L"color", 0));
            if (color != run.color) {
                run.color = color;
                run.accent.Color(Windows::UI::Color{uint8_t(color >> 24), uint8_t(color >> 16), uint8_t(color >> 8), uint8_t(color)});
                auto panel = run.button.Content().as<Controls::Grid>();
                panel.Children().GetAt(0).Visibility(color ? Visibility::Visible : Visibility::Collapsed);
            }
            Automation::AutomationProperties::SetItemStatus(run.button, label);
        }
    }

    void Sync(Controls::TabView const& view, std::vector<ShellStripSlot> const& layout,
              std::map<uint32_t, Controls::TabViewItem> const& rows,
              Windows::Data::Json::JsonArray const& groups) {
        std::vector<uint64_t> wanted;
        std::map<uint64_t, std::vector<Object>> members;
        uint64_t key = 0;
        bool ungrouped = false;
        for (auto slot : layout) {
            if (!slot.tab) { key = slot.group; wanted.push_back(key); members[key]; ungrouped = false; }
            else {
                if (!slot.group && !ungrouped) {
                    // An ungrouped run belongs to the boundary after the
                    // preceding group (zero at the start), not its first tab.
                    // Reordering that tab must not replace the whole group:
                    // WinUI can retain a stale selected index across replacement.
                    key = (uint64_t(1) << 32) | key;
                    wanted.push_back(key); ungrouped = true;
                }
                members[key].push_back(rows.at(slot.tab));
            }
        }
        // When the last visible member changes, WinUI can recycle the header
        // presenter while its UIElement content is still parented to the old
        // presenter. Release that binding before the empty/nonempty transition,
        // then reattach the same Button after the grouped source settles.
        std::vector<uint64_t> reattach;
        for (auto& [id, run] : runs) {
            auto desired = members.find(id);
            if (!run.button || desired == members.end()) continue;
            if ((run.items.Size()==0) != desired->second.empty()) {
                run.model.Insert(L"Header",nullptr);
                reattach.push_back(id);
            }
        }
        // Remove departing objects from their old parent before inserting them
        // in a different run. Surviving tabs and headers retain their identity.
        for (auto& [id, run] : runs) {
            auto desired = members.find(id);
            std::set<uint32_t> desiredIds;
            if (desired != members.end()) for (auto item : desired->second)
                desiredIds.insert(unbox_value<uint32_t>(item.as<Controls::TabViewItem>().Tag()));
            for (uint32_t i = run.items.Size(); i > 0; --i) {
                auto item = run.items.GetAt(i - 1);
                if (!desiredIds.count(unbox_value<uint32_t>(item.as<Controls::TabViewItem>().Tag())))
                    run.items.RemoveAt(i - 1);
            }
        }
        for (auto id : wanted) {
            auto [it, created] = runs.try_emplace(id);
            auto& run = it->second;
            if (created && id <= UINT32_MAX) {
                run.button = Markup::XamlReader::Load(LR"(
<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Style="{StaticResource SubtleButtonStyle}" Margin="4,0" Padding="4,0" MinWidth="32" MinHeight="0" Width="32" Height="32" VerticalAlignment="Center">
 <Button.Resources><ResourceDictionary><ResourceDictionary.ThemeDictionaries>
  <ResourceDictionary x:Key="Default"><SolidColorBrush x:Key="InlineGroupColorBrush" Color="Transparent"/></ResourceDictionary>
  <ResourceDictionary x:Key="HighContrast"><SolidColorBrush x:Key="InlineGroupColorBrush" Color="{ThemeResource SystemColorWindowTextColor}"/></ResourceDictionary>
 </ResourceDictionary.ThemeDictionaries></ResourceDictionary></Button.Resources>
 <Grid MinWidth="24" MaxWidth="144" Height="24" IsHitTestVisible="False" AutomationProperties.AccessibilityView="Raw">
  <Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions>
  <Rectangle Grid.ColumnSpan="2" Width="6" Height="3" HorizontalAlignment="Center" VerticalAlignment="Bottom" RadiusX="1.5" RadiusY="1.5" Fill="{ThemeResource InlineGroupColorBrush}" Visibility="Collapsed"/>
  <Grid Grid.Column="1" Width="24" RenderTransformOrigin="0.5,0.5">
   <Grid.RenderTransform><RotateTransform/></Grid.RenderTransform>
   <FontIcon FontSize="12" MirroredWhenRightToLeft="True"/>
  </Grid>
  <TextBlock FontSize="12" VerticalAlignment="Center" TextWrapping="NoWrap" TextTrimming="CharacterEllipsis" Visibility="Collapsed"/>
 </Grid>
</Button>)").as<Controls::Button>();
                run.accent = run.button.Resources().ThemeDictionaries().Lookup(box_value(L"Default")).as<ResourceDictionary>()
                    .Lookup(box_value(L"InlineGroupColorBrush")).as<Media::SolidColorBrush>();
                // One retained Button owns the entire input target. A future
                // group drag can arm from this opaque ID, use the existing drag
                // session, and suppress Click after crossing its drag threshold.
                // Do not enable CanDrag: this host deliberately avoids OLE.
                run.button.Tag(box_value(uint32_t(id)));
                Automation::AutomationProperties::SetAutomationId(run.button, L"FirefoxWinUIInlineGroup-" + to_hstring(id));
                auto alive = std::weak_ptr<int>(lifetime);
                run.button.AddHandler(UIElement::PointerPressedEvent(),
                    box_value(Microsoft::UI::Xaml::Input::PointerEventHandler([this, alive, id](auto&&, auto const& args) {
                        if (alive.expired()) return;
                        if (args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed()) {
                            suppressClick = 0;
                            if (press) press(uint32_t(id),args);
                        }
                    })), true);
                run.button.ContextRequested([this, alive, id](auto&&, auto&& args) {
                    if (alive.expired() || !context) return;
                    auto found = runs.find(id);
                    if (found == runs.end() || !found->second.button.IsLoaded()) return;
                    args.Handled(true);
                    hover.Dismiss();
                    auto button = found->second.button;
                    auto bounds = button.TransformToVisual(nullptr).TransformBounds(
                        {0,0,float(button.ActualWidth()),float(button.ActualHeight())});
                    auto screen = button.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(bounds);
                    context(uint32_t(id), {float(screen.X),float(screen.Y),float(screen.Width),float(screen.Height)});
                });
                run.button.Click([this, alive, id](auto&&, auto&&) {
                    if (alive.expired()) return;
                    if (suppressClick == id) { suppressClick = 0; return; }
                    hover.Hide();
                    auto found = runs.find(id);
                    if (found != runs.end() && toggle) toggle(uint32_t(id), !found->second.collapsed);
                });
                run.button.PointerEntered([this, alive, id](auto&&, auto&&) {
                    if (alive.expired()) return;
                    if (highlight) highlight(uint32_t(id));
                    auto found = runs.find(id);
                    if (found != runs.end()) hover.Enter(uint32_t(id), found->second.button, found->second.metadata);
                });
                run.button.PointerExited([this, alive, id](auto&&, auto&&) {
                    if (alive.expired()) return;
                    if (highlight) highlight(0);
                    hover.Exit(uint32_t(id));
                });
                run.button.KeyDown([this, alive, id](auto&&, auto&& args) {
                    if (alive.expired() || args.Key() != Windows::System::VirtualKey::Down) return;
                    auto found = runs.find(id);
                    if (found != runs.end() && found->second.collapsed) {
                        args.Handled(true);
                        hover.Enter(uint32_t(id), found->second.button, found->second.metadata, true);
                    }
                });
                run.button.Unloaded([this, alive, id](auto&&, auto&&) {
                    if (alive.expired()) return;
                    if (highlight) highlight(0);
                    if (hover.group == id) hover.Hide();
                    // Emptying a group briefly reparents this retained Button.
                    // Keep its finite rotation through that unload; the Run
                    // destructor stops it when the group actually goes away.
                });
                run.model.Insert(L"Header", run.button);
            }
            auto const& desired = members.at(id);
            for (uint32_t i = 0; i < desired.size(); ++i) {
                if (i < run.items.Size() && run.items.GetAt(i) == desired[i]) continue;
                uint32_t old;
                if (run.items.IndexOf(desired[i], old)) run.items.RemoveAt(old);
                run.items.InsertAt(i, desired[i]);
            }
        }
        // Group removal can synchronously select/measure through TabView while
        // ItemsStackPanel is updating its valid-element indices. Publish the
        // final topology with one reset, never a sequence of group removals.
        std::set<uint64_t> live(wanted.begin(), wanted.end());
        if (wanted != order || !reattach.empty()) {
            for (auto& [id, run] : runs) {
                if (run.button) {
                    run.model.Insert(L"Header",nullptr);
                    // Retired presenters can retain a binding to the old map
                    // in the panel's recycling section. Never put the Button
                    // back in that map: only the new presentation owns it.
                    run.model = single_threaded_observable_map<hstring, Object>();
                    run.model.Insert(L"Items",run.items);
                    run.model.Insert(L"Header",nullptr);
                    reattach.push_back(id);
                }
            }
            std::vector<Object> models;
            for (auto id : wanted) models.push_back(runs.at(id).model);
            data.ReplaceAll(models);
        }
        for (auto it = runs.begin(); it != runs.end();) {
            if (!live.count(it->first)) it = runs.erase(it); else ++it;
        }
        order = std::move(wanted);
        for (auto id : reattach) if (runs.count(id)) runs.at(id).model.Insert(L"Header",runs.at(id).button);
        Metadata(groups);
        if (!active) {
            view.TabItems().Clear();
            view.TabItemsSource(source.View());
            active = true;
        }
    }
};
