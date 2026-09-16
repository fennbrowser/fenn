#pragma once
#include <functional>
#include <winrt/Microsoft.UI.Input.h>
#include "JsonValidation.h"

// Strip-only experiment: TabView owns presentation, Firefox owns all mutations.
// No content pages, template internals, drag reordering or tear-out are used.
struct NativeTabStrip {
    Controls::Grid root{nullptr};
    Controls::TabView view{nullptr};
    NativeTabMenu& menu;
    std::function<void(uint32_t)> select, close;
    std::function<void()> add;
    std::map<uint32_t, Controls::TabViewItem> rows;
    struct Presentation { hstring detail; uint32_t flags{}; hstring status; };
    std::map<uint32_t, Presentation> presentation;
    Controls::DropDownButton groupsButton{nullptr};
    uint32_t presentationRevision{};
    hstring presentationFingerprint, groupFingerprint, pendingGroup;
    bool syncing{};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    NativeTabStrip(NativeTabMenu& model) : menu(model) {
        root = Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource SolidBackgroundFillColorBaseBrush}"><TabView TabWidthMode="Equal" CanDragTabs="False" CanReorderTabs="False" AllowDrop="False" VerticalAlignment="Top" AutomationProperties.AutomationId="FirefoxWinUITabStrip" AutomationProperties.Name="Tabs in this window" /></Grid>)").as<Controls::Grid>();
        view = root.Children().GetAt(0).as<Controls::TabView>();
        groupsButton = Controls::DropDownButton();
        groupsButton.Visibility(Visibility::Collapsed);
        groupsButton.Margin({4, 0, 4, 0});
        groupsButton.VerticalAlignment(VerticalAlignment::Center);
        Automation::AutomationProperties::SetAutomationId(groupsButton, L"FirefoxWinUITabGroups");
        view.TabStripHeader(groupsButton);
        auto alive = std::weak_ptr<int>(lifetime);
        view.SelectionChanged([this, alive](auto const&, auto const&) {
            if (alive.expired() || syncing || !view.SelectedItem()) return;
            // Modified gestures are observed on the exact tab below. Never let
            // TabView's single selection replace Firefox's multiselected set.
            if (GetKeyState(VK_CONTROL) & 0x8000 || GetKeyState(VK_SHIFT) & 0x8000) return;
            auto row = view.SelectedItem().as<Controls::TabViewItem>();
            select(unbox_value<uint32_t>(row.Tag()));
        });
        view.TabCloseRequested([this, alive](auto const&, Controls::TabViewTabCloseRequestedEventArgs const& args) {
            if (!alive.expired() && !syncing) close(unbox_value<uint32_t>(args.Tab().Tag()));
        });
        view.AddTabButtonClick([this, alive](auto const&, auto const&) { if (!alive.expired() && !syncing) add(); });
    }
    ~NativeTabStrip() { lifetime.reset(); }

    // Parse a complete bounded copy before changing XAML or accepting any IDs.
    void Publish(NativeTabMenu::Object const& data) {
        using Object = NativeTabMenu::Object;
        using Value = NativeTabMenu::Value;
        if (data.GetNamedNumber(L"version") != 1) throw hresult_invalid_argument();
        auto revision = NativeJson::RequireId(data, L"revision");
        auto fingerprint = data.Stringify();
        if (revision < presentationRevision || (revision == presentationRevision && fingerprint != presentationFingerprint)) throw hresult_invalid_argument();
        if (revision == presentationRevision) return;
        auto label = data.GetNamedString(L"groupsLabel");
        const bool rtl = data.GetNamedBoolean(L"rtl");
        auto entries = data.GetNamedArray(L"rows"), groups = data.GetNamedArray(L"groups");
        if (label.empty() || label.size() > 1024 || entries.Size() > 10000 || groups.Size() > 10000) throw hresult_invalid_argument();
        std::map<uint32_t, Presentation> next;
        for (auto value : entries) {
            auto entry = value.GetObject();
            auto id = NativeJson::RequireId(entry, L"id");
            auto flags = entry.GetNamedNumber(L"flags");
            auto detail = entry.GetNamedString(L"detail");
            auto status = entry.GetNamedString(L"status", L"");
            if (!std::isfinite(flags) || flags < 0 || flags > 15 || std::floor(flags) != flags || detail.size() > 2048 || status.size() > 2048 || !next.emplace(id, Presentation{detail, uint32_t(flags), status}).second) throw hresult_invalid_argument();
        }
        std::set<uint32_t> ids;
        for (auto value : groups) {
            auto group = value.GetObject();
            auto id = NativeJson::RequireId(group, L"id");
            auto name = group.GetNamedString(L"name"), text = group.GetNamedString(L"label");
            group.GetNamedBoolean(L"collapsed");
            if (!ids.insert(id).second || name.empty() || name.size() > 1024 || text.empty() || text.size() > 2048) throw hresult_invalid_argument();
        }
        // Group-only changes replace the flyout, never mutate a closing presenter.
        // Other state changes preserve the flyout but revoke any queued action.
        const auto groupState = groups.Stringify();
        if (groupState != groupFingerprint) {
            Controls::MenuFlyout flyout;
            for (auto value : groups) {
                auto group = value.GetObject();
                const auto id = NativeJson::RequireId(group, L"id");
                const bool collapsed = group.GetNamedBoolean(L"collapsed");
                Controls::ToggleMenuFlyoutItem item;
                item.Text(group.GetNamedString(L"name"));
                item.IsChecked(!collapsed);
                Automation::AutomationProperties::SetName(item, group.GetNamedString(L"label"));
                Automation::AutomationProperties::SetAutomationId(item, L"FirefoxWinUIGroup-" + to_hstring(id));
                item.Click([this, alive = std::weak_ptr<int>(lifetime), id, collapsed, groupState](auto const& sender, auto const&) {
                    if (alive.expired() || groupState != groupFingerprint) return;
                    sender.template as<Controls::ToggleMenuFlyoutItem>().IsChecked(!collapsed);
                    Object action;
                    action.SetNamedValue(L"kind", Value::CreateStringValue(L"strip-group"));
                    action.SetNamedValue(L"revision", Value::CreateNumberValue(presentationRevision));
                    action.SetNamedValue(L"group", Value::CreateNumberValue(id));
                    action.SetNamedValue(L"collapsed", Value::CreateBooleanValue(!collapsed));
                    pendingGroup = action.Stringify();
                });
                flyout.Items().Append(item);
            }
            if (auto old = groupsButton.Flyout()) old.Hide();
            groupsButton.Flyout(flyout);
            groupFingerprint = groupState;
        }
        groupsButton.Content(box_value(label));
        groupsButton.Visibility(groups.Size() ? Visibility::Visible : Visibility::Collapsed);
        root.FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
        presentation.swap(next);
        presentationRevision = revision;
        presentationFingerprint = fingerprint;
        pendingGroup = L"";
    }

    void UpdateHeader(Controls::TabViewItem const& row, uint32_t id, hstring const& title) {
        auto header = row.Header().as<Controls::Grid>();
        auto pin = header.Children().GetAt(0).as<Controls::SymbolIcon>();
        auto busy = header.Children().GetAt(1).as<Controls::ProgressRing>();
        auto text = header.Children().GetAt(2).as<Controls::StackPanel>();
        auto name = text.Children().GetAt(0).as<Controls::TextBlock>();
        auto detail = text.Children().GetAt(1).as<Controls::TextBlock>();
        auto audio = header.Children().GetAt(3).as<Controls::SymbolIcon>();
        auto meta = presentation.count(id) ? presentation.at(id) : Presentation{};
        if (name.Text() != title) name.Text(title);
        if (detail.Text() != meta.detail) detail.Text(meta.detail);
        detail.Visibility(meta.detail.empty() ? Visibility::Collapsed : Visibility::Visible);
        pin.Visibility(meta.flags & 1 ? Visibility::Visible : Visibility::Collapsed);
        busy.Visibility(meta.flags & 2 ? Visibility::Visible : Visibility::Collapsed);
        busy.IsActive((meta.flags & 2) != 0);
        audio.Symbol(meta.flags & 4 ? Controls::Symbol::Mute : Controls::Symbol::Volume);
        audio.Visibility(meta.flags & 12 ? Visibility::Visible : Visibility::Collapsed);
        Controls::ToolTipService::SetToolTip(row, box_value(meta.detail.empty() ? title : title + L"\n" + meta.detail));
        Automation::AutomationProperties::SetHelpText(row, meta.detail + (meta.detail.empty() || meta.status.empty() ? L"" : L" · ") + meta.status);
    }

    void AttachMenu(Controls::TabViewItem const& row, uint32_t id) {
        Controls::MenuFlyout flyout;
        flyout.Items().Append(NativeTabMenu::Placeholder());
        auto alive = std::weak_ptr<int>(lifetime);
        flyout.Opening([this, alive, id, weak = make_weak(flyout)](auto const&, auto const&) {
            if (!alive.expired() && rows.count(id)) menu.Open(weak.get(), id);
        });
        flyout.Opened([this, alive, weak = make_weak(flyout)](auto const&, auto const&) {
            if (!alive.expired() && weak.get() == menu.active) menu.FocusFirst();
        });
        flyout.Closed([this, alive, id, weak = make_weak(flyout), target = make_weak(row)](auto const&, auto const&) {
            if (alive.expired()) return;
            if (weak.get() == menu.active) menu.Dismiss();
            if (auto row = target.get(); row && rows.count(id) && row.ContextFlyout() == weak.get()) AttachMenu(row, id);
        });
        row.ContextFlyout(flyout);
    }

    void Sync(const WinUITab* tabs, uint32_t count, uint32_t active) {
        syncing = true;
        try {
            std::set<uint32_t> current;
            for (uint32_t i = 0; i < count; ++i) {
                const auto id = tabs[i].id;
                current.insert(id);
                if (!rows.count(id)) {
                    Controls::TabViewItem row;
                    row.Tag(box_value(id));
                    row.IsClosable(true);
                    row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
                    row.Header(Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" ColumnSpacing="6"><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition Width="Auto"/><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions><SymbolIcon Symbol="Pin" Width="16"/><ProgressRing Grid.Column="1" Width="16" Height="16" IsActive="False"/><StackPanel Grid.Column="2" MaxWidth="220" VerticalAlignment="Center"><TextBlock TextTrimming="CharacterEllipsis"/><TextBlock FontSize="10" Foreground="{ThemeResource TextFillColorSecondaryBrush}" TextTrimming="CharacterEllipsis"/></StackPanel><SymbolIcon Grid.Column="3" Symbol="Volume" Width="16"/></Grid>)"));
                    Automation::AutomationProperties::SetAutomationId(row, L"FirefoxWinUITab-" + to_hstring(id));
                    auto alive = std::weak_ptr<int>(lifetime);
                    row.AddHandler(UIElement::PointerPressedEvent(), box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                        [this, alive, id](auto const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                            if (alive.expired() || syncing || !rows.count(id) || !args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed()) return;
                            if (!(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000)) return;
                            // Ignore the stock close button; retain its normal input.
                            auto node = args.OriginalSource().try_as<DependencyObject>();
                            while (node && node != rows.at(id)) {
                                if (node.try_as<Controls::Primitives::ButtonBase>()) return;
                                node = Media::VisualTreeHelper::GetParent(node);
                            }
                            select(id);
                        })), true);
                    row.PreviewKeyDown([this, alive, id](auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                        if (alive.expired() || syncing || !rows.count(id)) return;
                        if (args.Key() == Windows::System::VirtualKey::Space || args.Key() == Windows::System::VirtualKey::Enter) {
                            if (args.OriginalSource() != rows.at(id)) return;
                            select(id);
                            args.Handled(true);
                        }
                    });
                    rows.emplace(id, row);
                    AttachMenu(row, id);
                }
                auto row = rows.at(id);
                UpdateHeader(row, id, tabs[i].title);
                Automation::AutomationProperties::SetName(row, tabs[i].title);
                // Secondary selection is explicit and theme-aware; SelectedItem
                // remains solely the Firefox active tab.
                bool selected = menu.selectedTabs.count(id) != 0;
                row.FontWeight(Windows::UI::Text::FontWeight{uint16_t(selected ? 600 : 400)});
                Automation::AutomationProperties::SetItemStatus(row, id == active ? L"Active tab" : selected ? L"Selected tab" : L"");
                uint32_t old;
                if (!view.TabItems().IndexOf(row, old) || old != i) {
                    if (view.TabItems().IndexOf(row, old)) view.TabItems().RemoveAt(old);
                    view.TabItems().InsertAt(i, row);
                }
            }
            while (view.TabItems().Size() > count) view.TabItems().RemoveAtEnd();
            for (auto it = rows.begin(); it != rows.end();) {
                if (!current.count(it->first)) it = rows.erase(it); else ++it;
            }
            view.SelectedItem(rows.count(active) ? rows.at(active) : nullptr);
        } catch (...) { syncing = false; throw; }
        syncing = false;
    }
};
