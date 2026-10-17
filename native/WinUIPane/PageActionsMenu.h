#pragma once
#include "JsonValidation.h"
#include "OmnibarIcons.h"
#include <array>

// WinUI presentation only. Firefox owns availability, security, commands and pins.
struct NativePageActionsMenu {
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Button = Microsoft::UI::Xaml::Controls::Button;
    struct Item { winrt::hstring key, id, label, reason; bool enabled{}, pinnable{}, pinned{}; };
    struct Model { winrt::hstring label, pinLabel, unpinLabel, context; std::vector<Item> items; };
    struct Row {
        Microsoft::UI::Xaml::Controls::ListViewItem host{nullptr};
        Button action{nullptr}, pin{nullptr};
        Microsoft::UI::Xaml::Controls::FontIcon icon{nullptr}, actionIcon{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock label{nullptr};
    };
    Button button{nullptr};
    Microsoft::UI::Xaml::Controls::Flyout flyout{nullptr};
    Microsoft::UI::Xaml::Controls::ListView list{nullptr};
    std::array<Row, 6> rows;
    Model model;
    bool enabled{}, open{};
    std::function<void(wchar_t const*, uint32_t, Button const&)> dispatch;
    std::function<void()> changed;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    ~NativePageActionsMenu() { lifetime.reset(); Close(); }
    void Close() {
        // Revoke actions synchronously; the stock closing animation/event can
        // finish after the selected document or address session has changed.
        open=false;
        if (flyout) flyout.Hide();
    }

    static Model Read(Object const& snapshot) {
        Model result;
        if (!snapshot.HasKey(L"navigation")) return result;
        auto navigation = snapshot.GetNamedObject(L"navigation");
        if (!navigation.HasKey(L"pageActions")) return result;
        auto menu = navigation.GetNamedObject(L"pageActions");
        result.label = menu.GetNamedString(L"label");
        result.pinLabel = menu.GetNamedString(L"pinLabel");
        result.unpinLabel = menu.GetNamedString(L"unpinLabel");
        result.context = menu.GetNamedString(L"context");
        for (auto const& text : {result.label, result.pinLabel, result.unpinLabel, result.context})
            NativeJson::RequireLength(text, 4096);
        auto items = menu.GetNamedArray(L"items");
        if (items.Size() != 6) winrt::throw_hresult(E_INVALIDARG);
        constexpr wchar_t const* keys[] = {L"connection", L"reader", L"pip", L"taskbar", L"translation", L"permissions"};
        std::set<winrt::hstring> ids;
        for (uint32_t i = 0; i < 6; ++i) {
            auto value = items.GetObjectAt(i);
            Item item{value.GetNamedString(L"key"), value.GetNamedString(L"id"), value.GetNamedString(L"label"),
                value.GetNamedString(L"reason"), value.GetNamedBoolean(L"enabled"),
                value.GetNamedBoolean(L"pinnable"), value.GetNamedBoolean(L"pinned")};
            if (item.key != keys[i] || item.id.empty() || item.id.size() > 64 || !ids.insert(item.id).second ||
                item.pinnable != (i != 0 && i != 5) || ((i == 0 || i == 5) && item.pinned)) winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(item.label, 4096);
            NativeJson::RequireLength(item.reason, 4096);
            result.items.push_back(std::move(item));
        }
        return result;
    }
    void Invoke(size_t index, bool pin) {
        if (!enabled || !open || index >= model.items.size() || !dispatch) return;
        auto item = model.items[index];
        if (pin ? !item.pinnable : !item.enabled) return;
        auto command = winrt::hstring(pin ? (item.pinned ? L"omni-unpin:" : L"omni-pin:") : L"omni:") + item.id;
        uint32_t modifiers = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) |
            ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) | ((GetKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u);
        // Anchor destination panels at the retained ellipsis, not a closing row.
        if (!pin) Close();
        dispatch(command.c_str(), modifiers, button);
    }
    void Create() {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        button = Markup::XamlReader::Load(LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" Width="28" Height="28" MinWidth="0" MinHeight="0" Padding="0"><FontIcon FontFamily="{ThemeResource SymbolThemeFontFamily}" Glyph="&#xE712;" FontSize="14"/></Button>)").as<Button>();
        button.Margin({0,0,4,0});
        button.VerticalAlignment(VerticalAlignment::Center);
        Automation::AutomationProperties::SetAutomationId(button,L"FennPageActions");
        flyout = Flyout();
        // A toolbar island is only one row high. Let ContentIsland.Popups host
        // the stock flyout outside that root, as native menus require.
        flyout.ShouldConstrainToRootBounds(false);
        flyout.Placement(Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
        // Match the product MenuFlyout's outer inset instead of the general
        // Flyout's dialog-like padding. Keep these resources live across themes.
        flyout.FlyoutPresenterStyle(Markup::XamlReader::Load(LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="FlyoutPresenter"><Setter Property="Padding" Value="{ThemeResource MenuFlyoutPresenterThemePadding}"/><Setter Property="MinWidth" Value="{ThemeResource FlyoutThemeMinWidth}"/><Setter Property="BorderBrush" Value="{ThemeResource MenuFlyoutPresenterBorderBrush}"/><Setter Property="BorderThickness" Value="{ThemeResource MenuFlyoutPresenterBorderThemeThickness}"/><Setter Property="CornerRadius" Value="{ThemeResource OverlayCornerRadius}"/></Style>)").as<Style>());
        list = ListView();
        list.SelectionMode(ListViewSelectionMode::None);
        list.IsItemClickEnabled(false);
        list.Padding({0,0,0,0});
        list.MaxWidth(480);
        flyout.Content(list);
        button.Flyout(flyout);
        auto alive = std::weak_ptr<int>(lifetime);
        flyout.Opened([this,alive](auto const&, auto const&) { if (!alive.expired()) { open=true; if (changed) changed(); } });
        flyout.Closed([this,alive](auto const&, auto const&) { if (!alive.expired()) { open=false; if (changed) changed(); } });
        for (size_t i=0; i<rows.size(); ++i) {
            auto& row = rows[i];
            row.host = ListViewItem();
            row.host.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            row.host.Padding({0,0,0,0});
            row.host.MinHeight(0);
            row.host.Margin({0,0,0,0});
            auto grid = Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Margin="{ThemeResource MenuFlyoutItemMargin}"><Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions><Button Style="{StaticResource SubtleButtonStyle}" MinWidth="0" MinHeight="{ThemeResource MenuFlyoutThemeMinHeight}" Padding="{ThemeResource MenuFlyoutItemThemePaddingNarrow}" HorizontalAlignment="Stretch" HorizontalContentAlignment="Left" IsTabStop="False"/><Button Grid.Column="1" Style="{StaticResource SubtleButtonStyle}" Width="32" Height="32" MinWidth="0" MinHeight="0" Padding="0" Opacity="0"><FontIcon FontFamily="{ThemeResource SymbolThemeFontFamily}" FontSize="14"/></Button></Grid>)").as<Grid>();
            row.action=grid.Children().GetAt(0).as<Button>();
            auto content=Markup::XamlReader::Load(LR"(<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Orientation="Horizontal" Spacing="12"><FontIcon FontFamily="{ThemeResource SymbolThemeFontFamily}" FontSize="16"/><TextBlock VerticalAlignment="Center"/></StackPanel>)").as<StackPanel>();
            row.actionIcon=content.Children().GetAt(0).as<FontIcon>();
            if (i == 2) {
                auto xaml=winrt::hstring(L"<PathIcon xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" Width=\"16\" Height=\"16\" Data=\"")+OmnibarIcons::Path(L"pip")+L"\"/>";
                content.Children().SetAt(0,Markup::XamlReader::Load(xaml).as<PathIcon>());
                row.actionIcon=nullptr;
            }
            row.label=content.Children().GetAt(1).as<TextBlock>();
            row.action.Content(content);
            row.pin=grid.Children().GetAt(1).as<Button>();
            row.icon=row.pin.Content().as<FontIcon>();
            row.host.Content(grid);
            auto show=[this,i,alive](auto const&,auto const&) { if (!alive.expired()) rows[i].pin.Opacity(1); };
            auto hide=[this,i,alive](auto const&,auto const&) {
                if (alive.expired()) return;
                auto root=rows[i].host.XamlRoot();
                if (!root) { rows[i].pin.Opacity(0); return; }
                auto focused=Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(root);
                if (focused != rows[i].host && focused != rows[i].pin && focused != rows[i].action) rows[i].pin.Opacity(0);
            };
            row.host.PointerEntered(show); row.host.GotFocus(show);
            row.host.PointerExited(hide);
            row.host.LostFocus([hide](auto const&, auto const&) {
                // LostFocus can run before WinUI installs the new focus target.
                auto queue=Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
                if (queue) queue.TryEnqueue([hide] { hide(nullptr,nullptr); });
            });
            row.action.Click([this,i,alive](auto const&,auto const&) { if (!alive.expired()) Invoke(i,false); });
            row.pin.Click([this,i,alive](auto const&,auto const&) { if (!alive.expired()) Invoke(i,true); });
            row.host.KeyDown([this,i,alive](auto const&,Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                if (alive.expired() || args.OriginalSource()==rows[i].pin || args.OriginalSource()==rows[i].action) return;
                if (args.Key()==winrt::Windows::System::VirtualKey::Enter || args.Key()==winrt::Windows::System::VirtualKey::Space) {
                    args.Handled(true); Invoke(i,false);
                }
            });
            list.Items().Append(row.host);
        }
    }
    void Apply(Model const& next, bool visible) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        enabled=visible && next.items.size()==6;
        if (!button) Create();
        if (!enabled || model.context!=next.context) Close();
        model=next;
        button.Visibility(enabled ? Visibility::Visible : Visibility::Collapsed);
        button.IsEnabled(enabled);
        Automation::AutomationProperties::SetName(button,next.label);
        ToolTipService::SetToolTip(button,winrt::box_value(next.label));
        for (size_t i=0; i<next.items.size(); ++i) {
            auto const& item=next.items[i]; auto& row=rows[i];
            row.label.Text(item.label); row.action.IsEnabled(item.enabled);
            if (row.actionIcon) row.actionIcon.Glyph(OmnibarIcons::Glyph(item.key));
            Automation::AutomationProperties::SetName(row.action,item.label);
            Automation::AutomationProperties::SetName(row.host,item.label);
            Automation::AutomationProperties::SetHelpText(row.host,item.reason);
            Automation::AutomationProperties::SetHelpText(row.action,item.reason);
            Automation::AutomationProperties::SetAutomationId(row.host,L"FennPageActionRow-"+item.key);
            Automation::AutomationProperties::SetAutomationId(row.action,L"FennPageAction-"+item.key);
            // The enabled row owns the tooltip so disabled actions still explain themselves.
            ToolTipService::SetToolTip(row.host,winrt::box_value(item.enabled ? item.label : item.reason));
            row.pin.Visibility(item.pinnable ? Visibility::Visible : Visibility::Collapsed);
            row.icon.Glyph(item.pinned ? L"\xE77A" : L"\xE718");
            auto pinLabel=item.pinned ? next.unpinLabel : next.pinLabel;
            Automation::AutomationProperties::SetName(row.pin,pinLabel+L" "+item.label);
            Automation::AutomationProperties::SetAutomationId(row.pin,L"FennPageActionPin-"+item.key);
            ToolTipService::SetToolTip(row.pin,winrt::box_value(pinLabel));
        }
    }
};
