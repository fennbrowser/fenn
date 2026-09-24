#pragma once
#include <chrono>
#include <map>
#include <set>
#include <functional>
#include <winrt/Windows.Data.Json.h>

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
        hstring name, label;
        bool collapsed{};
        uint32_t color{};
        Media::SolidColorBrush accent{nullptr};
        Media::Animation::Storyboard arrowMotion{nullptr};
        Run() { model.Insert(L"Items", items); model.Insert(L"Header", nullptr); }
        ~Run() { if (arrowMotion) { try { arrowMotion.Stop(); } catch (...) {} } }
    };
    Data::CollectionViewSource source;
    Vector data = single_threaded_observable_vector<Object>();
    std::map<uint64_t, Run> runs;
    std::vector<uint64_t> order;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    std::function<void(uint32_t, bool)> toggle;
    bool configured{}, active{};
    ~InlineTabGroups() { lifetime.reset(); }

    bool Configure(Controls::ListViewBase const& list) {
        if (configured) return true;
        // Localized template dependency. Callers retain the dropdown fallback
        // when the stock list part is unavailable on a different WinUI version.
        if (!list || list.Name() != L"TabListView") return false;
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
        configured = true;
        return true;
    }

    void Metadata(Windows::Data::Json::JsonArray const& groups, bool animate = false) {
        for (auto value : groups) {
            auto group = value.GetObject();
            const auto id = uint32_t(group.GetNamedNumber(L"id"));
            auto found = runs.find(id);
            if (found == runs.end() || !found->second.button) continue;
            auto& run = found->second;
            auto name = group.GetNamedString(L"name"), label = group.GetNamedString(L"label");
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
                Controls::ToolTipService::SetToolTip(run.button, box_value(label));
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
<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Style="{StaticResource SubtleButtonStyle}" Margin="4,0" Padding="0" MinWidth="0" MinHeight="0" Width="32" Height="32" VerticalAlignment="Center">
 <Button.Resources><ResourceDictionary><ResourceDictionary.ThemeDictionaries>
  <ResourceDictionary x:Key="Default"><SolidColorBrush x:Key="InlineGroupColorBrush" Color="Transparent"/></ResourceDictionary>
  <ResourceDictionary x:Key="HighContrast"><SolidColorBrush x:Key="InlineGroupColorBrush" Color="{ThemeResource SystemColorWindowTextColor}"/></ResourceDictionary>
 </ResourceDictionary.ThemeDictionaries></ResourceDictionary></Button.Resources>
 <Grid Width="24" Height="24" IsHitTestVisible="False" AutomationProperties.AccessibilityView="Raw">
  <Rectangle Width="16" Height="3" VerticalAlignment="Bottom" RadiusX="1" RadiusY="1" Fill="{ThemeResource InlineGroupColorBrush}" Visibility="Collapsed"/>
  <Grid RenderTransformOrigin="0.5,0.5">
   <Grid.RenderTransform><RotateTransform/></Grid.RenderTransform>
   <FontIcon FontSize="12" MirroredWhenRightToLeft="True"/>
  </Grid>
 </Grid>
</Button>)").as<Controls::Button>();
                run.accent = run.button.Resources().ThemeDictionaries().Lookup(box_value(L"Default")).as<ResourceDictionary>()
                    .Lookup(box_value(L"InlineGroupColorBrush")).as<Media::SolidColorBrush>();
                // One retained 32-DIP Button owns the entire input target. A future
                // group drag can arm from this opaque ID, use the existing drag
                // session, and suppress Click after crossing its drag threshold.
                // Do not enable CanDrag: this host deliberately avoids OLE.
                run.button.Tag(box_value(uint32_t(id)));
                Automation::AutomationProperties::SetAutomationId(run.button, L"FirefoxWinUIInlineGroup-" + to_hstring(id));
                auto alive = std::weak_ptr<int>(lifetime);
                run.button.Click([this, alive, id](auto&&, auto&&) {
                    if (alive.expired()) return;
                    auto found = runs.find(id);
                    if (found != runs.end() && toggle) toggle(uint32_t(id), !found->second.collapsed);
                });
                run.button.Unloaded([this, alive, id](auto&&, auto&&) {
                    if (alive.expired()) return;
                    auto found = runs.find(id);
                    if (found != runs.end() && found->second.arrowMotion) {
                        found->second.arrowMotion.Stop();
                        found->second.arrowMotion = nullptr;
                    }
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
        // Prune obsolete runs before positioning survivors. Removing/reinserting
        // a group merely because its preceding run vanished can leave its
        // retained button attached to WinUI's recycled header container.
        std::set<uint64_t> live(wanted.begin(), wanted.end());
        for (auto const& [id, run] : runs) {
            if (live.count(id)) continue;
            uint32_t old;
            if (data.IndexOf(run.model, old)) data.RemoveAt(old);
        }
        for (uint32_t i = 0; i < wanted.size(); ++i) {
            Object model = runs.at(wanted[i]).model;
            if (i < data.Size() && data.GetAt(i) == model) continue;
            uint32_t old;
            if (data.IndexOf(model, old)) data.RemoveAt(old);
            data.InsertAt(i, model);
        }
        while (data.Size() > wanted.size()) data.RemoveAtEnd();
        for (auto it = runs.begin(); it != runs.end();) {
            if (!live.count(it->first)) it = runs.erase(it); else ++it;
        }
        order = std::move(wanted);
        Metadata(groups);
        if (!active) {
            view.TabItems().Clear();
            view.TabItemsSource(source.View());
            active = true;
        }
    }
};
