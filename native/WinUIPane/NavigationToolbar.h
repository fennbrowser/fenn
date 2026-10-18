#pragma once
#include <array>
#include <cmath>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <functional>
#include "JsonValidation.h"
#include "shell_core.h"
#include "PinnedExtensionButtons.h"
#include "ToolbarCustomization.h"
#include "ToolbarDragPreview.h"
#include "HistoryMenu.h"
#include "ProductMenu.h"
#include "ToolbarMetrics.h"
#include "StockToolbar.h"

// WinUI presentation only. Firefox owns eligibility, labels and commands.
struct NativeNavigationToolbar {
    using JObject = winrt::Windows::Data::Json::JsonObject;
    using Button = Microsoft::UI::Xaml::Controls::Button;
    struct State { bool enabled{}, stopping{}, required{}, shown{true}; double progress{-1}; winrt::hstring label, tooltip, attention; };
    struct States : std::array<State, 12> {
        std::array<State, 2> indicators{};
        States() { for(auto& item:indicators)item.shown=false; }
    };
    std::array<Button, 2> indicatorButtons;
    Microsoft::UI::Xaml::Controls::TextBlock privateLabel;
    static constexpr wchar_t const* indicatorCommands[] = {L"indicator:private", L"indicator:analysis"};
    States commandState{};
    std::vector<std::pair<winrt::hstring,Microsoft::UI::Xaml::FrameworkElement>> dropSlots;
    static constexpr wchar_t const* commands[] = {L"back", L"forward", L"reload", L"profile", L"extensions", L"appmenu", L"overflow", L"home", L"downloads", L"history", L"bookmarks", L"reset-pbm"};
    Microsoft::UI::Xaml::Controls::Grid host{nullptr}, surface{nullptr};
    Microsoft::UI::Xaml::FrameworkElement address{nullptr};
    std::map<winrt::hstring,Microsoft::UI::Xaml::Controls::Grid> spacers, dragHosts;
    std::vector<Microsoft::UI::Xaml::UIElement> ordered;
    bool visible{};
    std::array<Button, 12> buttons;
    std::array<Microsoft::UI::Xaml::Controls::SymbolIcon, 12> icons;
    Microsoft::UI::Xaml::Controls::Grid downloadProgress{nullptr};
    Microsoft::UI::Xaml::Shapes::Path downloadPie{nullptr};
    std::array<Microsoft::UI::Xaml::Media::ArcSegment,2> downloadArcs{nullptr,nullptr};
    Microsoft::UI::Xaml::Controls::Viewbox downloadIcon{nullptr};
    std::array<Microsoft::UI::Xaml::Controls::Border,3> downloadAttention;
    PinnedExtensionButtons pinned;
    NativeToolbarCustomization customization;
    NativeStockToolbar stock;
    NativeHistoryMenu historyMenu;
    NativeProductMenu productMenu;
    ToolbarDragPreview preview;
    bool keyboardActivation{};
    bool stopping{}, hasOverflow{}, requiredOverflow{}, overflowQueued{}, rootObserved{};
    NativeToolbarCustomization::State layoutState;
    std::wstring pendingOverflow, sentOverflow;
    std::array<Microsoft::UI::Dispatching::DispatcherQueueTimer, 2> historyTimers{nullptr, nullptr};
    std::array<bool, 2> historyHeld{};
    std::array<uint32_t, 12> inputModifiers{};
    std::function<void(wchar_t const*, uint32_t, Button const&)> activate;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    static Button CreateButton(size_t i,Microsoft::UI::Xaml::Controls::Symbol symbol) {
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        auto button = Markup::XamlReader::Load(
            LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" MinWidth="0" MinHeight="0" Padding="0"/>)").as<Button>();
        button.Width(ToolbarMetrics::kButton);
        button.Height(ToolbarMetrics::kButton);
        SymbolIcon icon(symbol);
        // Firefox-like density with stock icons and generous button targets.
        Viewbox iconHost;
        iconHost.Width(ToolbarMetrics::kIcon);
        iconHost.Height(ToolbarMetrics::kIcon);
        iconHost.Child(icon);
        if (i == 6) {
            iconHost.Child(Markup::XamlReader::Load(
                LR"(<FontIcon xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" FontFamily="{ThemeResource SymbolThemeFontFamily}" Glyph="&#xE974;"/>)").as<FontIcon>());
        }
        if (i == 4) {
            iconHost.Child(Markup::XamlReader::Load(
                LR"(<FontIcon xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" FontFamily="{ThemeResource SymbolThemeFontFamily}" Glyph="&#xEA86;"/>)").as<FontIcon>());
        }
        button.Content(iconHost);
        return button;
    }
    std::vector<PinnedExtensionButtons::State> paletteState;
    std::map<winrt::hstring,std::shared_ptr<PinnedExtensionButtons>> paletteRenderers;
    Microsoft::UI::Xaml::FrameworkElement PaletteControl(NativeToolbarCustomization::Item const& item) {
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        if(!item.stock.empty())return stock.Create(item,true).host;
        for(size_t i=0;i<buttons.size();i++)if(i!=5 && i!=6 && item.control==commands[i])return CreateButton(i,i==4?Symbol::Library:icons[i].Symbol());
        for(auto const& state:paletteState)if(state.id==item.palette) {
            auto renderer=std::make_shared<PinnedExtensionButtons>();renderer->externalLayout=true;renderer->Apply({state},true);
            auto button=renderer->rows.at(state.id).button;renderer->panel.Children().Clear();button.IsEnabled(true);
            paletteRenderers[item.id]=renderer;return button;
        }
        auto spacer=Markup::XamlReader::Load(LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Width="32" Height="32" BorderThickness="1" BorderBrush="{ThemeResource ControlStrokeColorDefaultBrush}" CornerRadius="4"><SymbolIcon Symbol="More"/></Border>)").as<Border>();return spacer;
    }
    void UpdatePalette(std::vector<PinnedExtensionButtons::State> const& next) {
        paletteState=next;
        for(auto const& item:customization.state.items) {
            auto r=paletteRenderers.find(item.id);if(r==paletteRenderers.end())continue;
            for(auto const& entry:next)if(entry.id==item.palette){r->second->Apply({entry},true);r->second->panel.Children().Clear();r->second->rows.at(entry.id).button.IsEnabled(true);}
        }
    }
    NativeNavigationToolbar() {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        customization.createPreview=[this](auto const& item){return PaletteControl(item);};
        customization.clearPreviews=[this]{paletteRenderers.clear();};
        customization.activate = [this](auto command, auto modifiers, auto const& button) { if (activate) activate(command, modifiers, button); };
        pinned.activate = [this](auto command, auto modifiers, auto const& button) { if (activate) activate(command, modifiers, button); };
        productMenu.dispatch = [this](auto command, auto modifiers, auto const& button) { if (activate) activate(command, modifiers, button); };
        productMenu.customize = [this] { if (visible && !customization.editing) customization.Open(buttons[0]); };
        // With the application menu button gone, the product button also closes the editor.
        productMenu.closeEditor = [this] {
            if (!customization.editing) return false;
            customization.Hide();
            return true;
        };
        const Symbol symbols[] = {Symbol::Back, Symbol::Forward, Symbol::Refresh, Symbol::Contact, Symbol::Library, Symbol::More, Symbol::Forward, Symbol::Home, Symbol::Download, Symbol::Clock, Symbol::OutlineStar, Symbol::Delete};
        const wchar_t* names[] = {L"Back", L"Forward", L"Reload", L"Account", L"Extensions", L"Application menu", L"More tools", L"Home", L"Downloads", L"History", L"Bookmarks", L"Clear private session"};
        const wchar_t* ids[] = {L"FennBack", L"FennForward", L"FennReload", L"FennProfile", L"FennExtensions", L"FennAppMenu", L"FennOverflow", L"FennHome", L"FennDownloads", L"FennHistory", L"FennBookmarks", L"FennResetPrivate"};
        for (size_t i = 0; i < buttons.size(); ++i) {
            auto button = CreateButton(i,symbols[i]);
            icons[i]=button.Content().as<Viewbox>().Child().try_as<SymbolIcon>();
            if(i==8) {
                // Firefox owns the percentage; XAML supplies a persistent outline and a filled pie.
                // Badge placement is relative to the button interior, independent of the progress graphic.
                Grid contents;contents.Width(ToolbarMetrics::kButton);contents.Height(ToolbarMetrics::kButton);downloadIcon=button.Content().as<Viewbox>();button.Content(nullptr);downloadIcon.HorizontalAlignment(HorizontalAlignment::Center);downloadIcon.VerticalAlignment(VerticalAlignment::Center);contents.Children().Append(downloadIcon);
                downloadProgress=Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Width="16" Height="16" HorizontalAlignment="Center" VerticalAlignment="Center" IsHitTestVisible="False" Visibility="Collapsed" AutomationProperties.AccessibilityView="Raw"><Path Fill="{ThemeResource TextFillColorPrimaryBrush}"><Path.Data><PathGeometry><PathFigure StartPoint="8,8" IsClosed="True"><LineSegment Point="8,1.5"/><ArcSegment Point="8,1.5" Size="6.5,6.5" SweepDirection="Clockwise"/><ArcSegment Point="8,1.5" Size="6.5,6.5" SweepDirection="Clockwise"/></PathFigure></PathGeometry></Path.Data></Path><Ellipse Width="16" Height="16" StrokeThickness="1" Stroke="{ThemeResource TextFillColorPrimaryBrush}"/></Grid>)").as<Grid>();contents.Children().Append(downloadProgress);
                downloadPie=downloadProgress.Children().GetAt(0).as<Shapes::Path>();
                // The ring geometry is drawn for a 16-DIP icon; scale it to the toolbar icon size.
                downloadProgress.RenderTransformOrigin({0.5f,0.5f});
                Media::ScaleTransform ringScale;
                ringScale.ScaleX(ToolbarMetrics::kIcon/16.0);ringScale.ScaleY(ToolbarMetrics::kIcon/16.0);
                downloadProgress.RenderTransform(ringScale);
                auto segments=downloadPie.Data().as<Media::PathGeometry>().Figures().GetAt(0).Segments();
                for(size_t j=0;j<downloadArcs.size();j++)downloadArcs[j]=segments.GetAt(uint32_t(j+1)).as<Media::ArcSegment>();
                const wchar_t* brushes[]={L"SystemFillColorAttentionBrush",L"SystemFillColorCautionBrush",L"SystemFillColorCriticalBrush"};
                for(size_t j=0;j<downloadAttention.size();j++) {
                    auto xaml=std::wstring(L"<Border xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" Width=\"5\" Height=\"5\" CornerRadius=\"2.5\" HorizontalAlignment=\"Right\" VerticalAlignment=\"Top\" Margin=\"0,4,4,0\" IsHitTestVisible=\"False\" Background=\"{ThemeResource ")+brushes[j]+L"}\"/>";
                    downloadAttention[j]=Markup::XamlReader::Load(xaml).as<Border>();downloadAttention[j].Visibility(Visibility::Collapsed);contents.Children().Append(downloadAttention[j]);
                }
                button.Content(contents);
            }
            if(i==7)button.AddHandler(UIElement::PointerReleasedEvent(),winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler([this,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const& args){
                if(alive.expired() || !visible || customization.editing || !activate)return;
                if(args.GetCurrentPoint(buttons[7]).Properties().PointerUpdateKind()==winrt::Microsoft::UI::Input::PointerUpdateKind::MiddleButtonReleased)
                    activate(L"home-middle",uint32_t(args.KeyModifiers())&15u,buttons[7]);
            })),true);
            button.IsEnabled(false);
            Automation::AutomationProperties::SetAutomationId(button, ids[i]);
            Automation::AutomationProperties::SetName(button, names[i]);
            ToolTipService::SetToolTip(button, winrt::box_value(winrt::hstring(names[i])));
            // Click has no modifier data. Preserve the originating WinUI event
            // instead of sampling keys after an asynchronously delivered click.
            button.AddHandler(UIElement::PointerPressedEvent(), winrt::box_value(
                Microsoft::UI::Xaml::Input::PointerEventHandler([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& args) {
                    if (alive.expired()) return;
                    keyboardActivation=false;inputModifiers[i] = uint32_t(args.KeyModifiers()) & 15u;
                    if (i < 2) {
                        historyHeld[i] = false;
                        if (args.GetCurrentPoint(buttons[i]).Properties().IsLeftButtonPressed()) historyTimers[i].Start();
                    }
                })), true);
            for (auto event : {UIElement::PointerReleasedEvent(), UIElement::PointerCanceledEvent(), UIElement::PointerCaptureLostEvent()}) {
                button.AddHandler(event, winrt::box_value(
                    Microsoft::UI::Xaml::Input::PointerEventHandler([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                        if (!alive.expired()) {
                            inputModifiers[i] = 0;
                            if (i < 2) historyTimers[i].Stop();
                        }
                    })), true);
            }
            button.PreviewKeyDown([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& args) {
                if (alive.expired()) return;
                if(i==5 && customization.editing && args.Key()==winrt::Windows::System::VirtualKey::Tab && !(GetKeyState(VK_SHIFT)&0x8000)) {
                    customization.available.Focus(FocusState::Keyboard);args.Handled(true);return;
                }
                if (i < 2) { historyHeld[i] = false; historyTimers[i].Stop(); }
                keyboardActivation=true;inputModifiers[i] = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) |
                    ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) |
                    ((GetKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u) |
                    ((GetKeyState(VK_LWIN) & 0x8000) ? 8u : 0u);
            });
            button.Click([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (alive.expired() || !activate || !buttons[i].IsEnabled() || !visible) return;
                if(i==5 && customization.editing){customization.Hide();return;}
                if (i < 2 && historyHeld[i]) { historyHeld[i] = false; return; }
                const uint32_t modifiers = inputModifiers[i];
                inputModifiers[i] = 0;
                if(i==9 && historyMenu.enabled && !customization.editing){historyMenu.Open(buttons[i]);return;}
                activate(i == 2 && stopping ? L"stop" : commands[i], modifiers, buttons[i]);
            });
            if(i==7 || i==8)button.ContextRequested([this,i,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const& args){if(alive.expired() || !visible || customization.editing || !activate)return;args.Handled(true);activate(i==7?L"widget-menu:home":L"widget-menu:downloads",0,buttons[i]);});
            button.VerticalAlignment(VerticalAlignment::Center);
            buttons[i] = button;
            if (i == 5) button.ContextRequested([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& args) { if (!alive.expired()) {args.Handled(true);customization.Open(buttons[5]);} });
            if (i < 2) {
                historyTimers[i] = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
                historyTimers[i].IsRepeating(false);
                historyTimers[i].Interval(std::chrono::milliseconds(500));
                auto history = [this, i, alive = std::weak_ptr<int>(lifetime)] {
                    if (!alive.expired() && activate && buttons[i].IsEnabled() && visible)
                        activate(i == 0 ? L"back-history" : L"forward-history", 0, buttons[i]);
                };
                historyTimers[i].Tick([this, i, history, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                    if (alive.expired() || !(GetKeyState(VK_LBUTTON) & 0x8000)) return;
                    historyHeld[i] = true;
                    history();
                });
                button.ContextRequested([history](auto const&, auto const& args) { args.Handled(true); history(); });
                button.PointerExited([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                    if (!alive.expired()) historyTimers[i].Stop();
                });
            }

        }
    }
    void Attach(Microsoft::UI::Xaml::Controls::Grid const& grid,
                Microsoft::UI::Xaml::FrameworkElement const& editor,
                Microsoft::UI::Xaml::Controls::Grid const& background) {
        host=grid;address=editor;surface=background;
        for(size_t i=0;i<indicatorButtons.size();i++) {
            auto button=CreateButton(0,Controls::Symbol::Permissions);
            if(i==0) { button.Width(NAN); button.Padding(Thickness{8,0,8,0}); }
            Automation::AutomationProperties::SetAutomationId(button,i==0?L"FennPrivateIndicator":L"FennContentAnalysisIndicator");
            button.AddHandler(UIElement::PointerPressedEvent(),winrt::box_value(
                Microsoft::UI::Xaml::Input::PointerEventHandler([this,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const&){if(!alive.expired())keyboardActivation=false;})),true);
            button.PreviewKeyDown([this,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const&){if(!alive.expired())keyboardActivation=true;});
            button.Click([this,i,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const&) {
                if(!alive.expired() && visible && !customization.editing && activate &&
                   commandState.indicators[i].shown && commandState.indicators[i].enabled)
                    activate(indicatorCommands[i],0,indicatorButtons[i]);
            });
            button.VerticalAlignment(VerticalAlignment::Center);
            indicatorButtons[i]=button;
        }
        privateLabel.VerticalAlignment(VerticalAlignment::Center);
        privateLabel.Margin(Thickness{8,0,8,0});
        Automation::AutomationProperties::SetAutomationId(privateLabel,L"FennPrivateIndicatorLabel");
        customization.editorChanged=[this]{Layout(layoutState);};
        customization.dragEnded=[this]{preview.Reset();};
        preview.arrange=[this](auto const& order,bool extra){Arrange(order,extra);};
        host.AllowDrop(true);
        // The editor is not light-dismissed, so a press on anything in the row
        // other than a placed control (which can be dragged or edited) closes it.
        host.AddHandler(UIElement::PointerPressedEvent(), winrt::box_value(
            Microsoft::UI::Xaml::Input::PointerEventHandler([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& args) {
                if (alive.expired() || !visible || !customization.editing || customization.dragging) return;
                auto node = args.OriginalSource().try_as<Microsoft::UI::Xaml::DependencyObject>();
                while (node) {
                    for (auto const& element : ordered)
                        if (node == element && element != address) return;
                    if (node == host) break;
                    node = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(node);
                }
                customization.Hide();
            })), true);
        // Buttons without a menu of their own offer to hide themselves, and every
        // such request offers the editor, as items in a native context menu.
        host.ContextRequested([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& args) {
            if (alive.expired() || args.Handled() || !visible || customization.editing ||
                !customization.state.revision || !customization.state.labels.contains(L"open")) return;
            args.Handled(true);
            winrt::hstring sourceId;
            auto node = args.OriginalSource().try_as<Microsoft::UI::Xaml::DependencyObject>();
            while (node && node != host && sourceId.empty()) {
                for (auto const& [id, control] : customization.placedControls)
                    if (node == control) { sourceId = id; break; }
                node = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(node);
            }
            contextMenu = Controls::MenuFlyout();
            if (!sourceId.empty() && customization.state.labels.contains(L"remove")) {
                auto found = std::find_if(customization.state.items.begin(), customization.state.items.end(),
                    [&](auto const& item) { return item.id == sourceId; });
                if (found != customization.state.items.end() && found->placed && found->removable && found->control != L"address") {
                    Controls::MenuFlyoutItem hide;
                    hide.Text(customization.state.labels.at(L"remove"));
                    Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(hide, L"FennToolbarRemove");
                    const auto revision = customization.state.revision;
                    hide.Click([this, alive, sourceId, revision](auto const&, auto const&) {
                        if (alive.expired() || !visible || customization.state.revision != revision) return;
                        if (!customization.anchor) customization.anchor = buttons[0];
                        customization.Send(sourceId, L"remove", {}, revision);
                    });
                    contextMenu.Items().Append(hide);
                }
            }
            Controls::MenuFlyoutItem open;
            open.Text(customization.state.labels.at(L"open"));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(open, L"FennCustomizeOpen");
            open.Click([this, alive](auto const&, auto const&) {
                if (!alive.expired() && visible && !customization.editing) customization.Open(buttons[0]);
            });
            contextMenu.Items().Append(open);
            Windows::Foundation::Point point;
            Controls::Primitives::FlyoutShowOptions options;
            if (args.TryGetPosition(host, point)) options.Position(point);
            contextMenu.ShowAt(host, options);
        });
        auto updatePreview=[this](auto const& args) {
            if(!customization.Accept(args)){preview.Reset();return false;}
            auto palette=customization.previews.find(customization.dragId);
            const double paletteWidth=palette!=customization.previews.end() && palette->second.ActualWidth()>0
                ? palette->second.ActualWidth():ToolbarMetrics::kButton;
            preview.Begin(host,ordered,dropSlots,customization.dragId,customization.dragRevision,paletteWidth);
            uint32_t index;
            if(!preview.Hit(args.GetPosition(host).X,host.FlowDirection()==Microsoft::UI::Xaml::FlowDirection::RightToLeft,index))return false;
            preview.Show(index);return true;
        };
        host.DragOver([this,updatePreview,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const& args){if(!alive.expired() && updatePreview(args))args.Handled(true);});
        host.DragLeave([this,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const& args){
            if(alive.expired())return;auto p=args.GetPosition(host);
            if(p.X<0 || p.Y<0 || p.X>=host.ActualWidth() || p.Y>=host.ActualHeight())preview.Reset();
        });
        host.Drop([this,updatePreview,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const& args){
            if(alive.expired() || !updatePreview(args))return;
            auto target=preview.target<preview.slots.size()?preview.slots[preview.target].first:winrt::hstring{};
            preview.Reset(false);customization.DropAt(args,target);
        });
        host.SizeChanged([this,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const&){if(!alive.expired())Layout(layoutState);});
        host.Loaded([this,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const&) {
            if(alive.expired() || rootObserved || !host.XamlRoot())return;
            rootObserved=true;
            host.XamlRoot().Changed([this,alive](auto const&,auto const&){if(!alive.expired())Layout(layoutState);});
            Layout(layoutState);
        });
    }
    void Layout(NativeToolbarCustomization::State const& state) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        if(!host || preview.changing)return;
        if(preview.active) {
            if(visible && customization.dragging && customization.editing && state.revision==preview.revision &&
                host.XamlRoot().Size().Width==preview.width)return;
            preview.Reset(false);
        }
        if(layoutState.revision!=state.revision)layoutState=state;
        double indicatorWidth=0;
        if(commandState.indicators[0].shown) {
            auto element=commandState.indicators[0].enabled?indicatorButtons[0].as<UIElement>():privateLabel.as<UIElement>();
            element.Measure({INFINITY,float(ToolbarMetrics::kButton)});
            indicatorWidth+=element.DesiredSize().Width+ToolbarMetrics::kGap;
        }
        if(commandState.indicators[1].shown)indicatorWidth+=ToolbarMetrics::kButton+ToolbarMetrics::kGap;
        const double availableWidth=host.XamlRoot()?std::max(0.0,double(host.XamlRoot().Size().Width)-indicatorWidth):0;
        std::set<winrt::hstring> overflowed;
        if(visible && state.revision && availableWidth>0) {
            std::vector<ShellToolbarSlot> slots;
            std::vector<NativeToolbarCustomization::Item const*> items;
            for(auto const& item:state.items)if(item.placed && (item.control!=L"downloads" || commandState[8].shown || customization.editing)) {
                slots.push_back({item.spring?2u:item.control==L"address"?1u:item.stock.size()==3?3u:0u,item.canAutoOverflow?(item.extension?2u:1u):0u});
                items.push_back(&item);
            }
            uint64_t mask=0;
            if(shell_core_toolbar_overflow(slots.data(),slots.size(),availableWidth,requiredOverflow,&mask)==SHELL_STATUS_OK) {
                std::wstring ids;
                for(size_t i=0;i<items.size();i++)if(mask&(uint64_t(1)<<i)) {
                    overflowed.insert(items[i]->id);
                    if(!ids.empty())ids+=L",";ids+=items[i]->id.c_str();
                }
                pendingOverflow=L"customize:"+std::to_wstring(state.revision)+L":overflow-layout:"+(ids.empty()?L"none":ids);
                if(!overflowQueued && pendingOverflow!=sentOverflow) {
                    overflowQueued=true;
                    if(!host.DispatcherQueue().TryEnqueue([this,alive=std::weak_ptr<int>(lifetime)] {
                        if(alive.expired())return;
                        overflowQueued=false;
                        if(!visible || pendingOverflow==sentOverflow || !activate)return;
                        sentOverflow=pendingOverflow;
                        activate(sentOverflow.c_str(),0,buttons[5]);
                    }))overflowQueued=false;
                }
            }
        }
        for(size_t i=0;i<buttons.size();i++)buttons[i].IsEnabled(visible && (customization.editing || commandState[i].enabled));
        for(size_t i=0;i<indicatorButtons.size();i++)indicatorButtons[i].IsEnabled(
            visible && commandState.indicators[i].shown && commandState.indicators[i].enabled && !customization.editing);
        dropSlots.clear();customization.placedControls.clear();
        std::vector<UIElement> desired;
        std::set<winrt::hstring> keep, keepDrag;
        if(!visible || !state.revision) {
            for(auto const& [id,grid]:dragHosts){if(grid.Children().Size())grid.Children().GetAt(0).IsHitTestVisible(true);grid.Children().Clear();}
            dragHosts.clear();
        }
        if(visible && state.revision) {
            for(auto const& item:state.items) {
                if(!item.placed || overflowed.contains(item.id) || (item.control==L"downloads" && !commandState[8].shown && !customization.editing))continue;
                UIElement control{nullptr};
                if(item.control==L"address")control=address;
                else if(!item.stock.empty())control=stock.Control(item,visible,customization.editing);
                else if(item.spring) {
                    keep.insert(item.id);
                    auto [it,inserted]=spacers.try_emplace(item.id);
                    if(inserted) {
                        it->second.IsHitTestVisible(false);
                        auto outline=Markup::XamlReader::Load(LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Height="24" Margin="4,0,4,0" VerticalAlignment="Center" BorderThickness="1" BorderBrush="{ThemeResource TextFillColorSecondaryBrush}" IsHitTestVisible="False"/>)").as<Border>();
                        it->second.Children().Append(outline);
                        auto keyboardTarget=Markup::XamlReader::Load(LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" Height="24" MinWidth="0" MinHeight="0" Padding="0" Margin="4,0,4,0" HorizontalAlignment="Stretch" VerticalAlignment="Center" IsHitTestVisible="False"/>)").as<Button>();
                        it->second.Children().Append(keyboardTarget);
                    }
                    it->second.Children().GetAt(0).Visibility(customization.editing?Visibility::Visible:Visibility::Collapsed);
                    auto keyboardTarget=it->second.Children().GetAt(1).as<Button>();
                    keyboardTarget.Visibility(customization.editing?Visibility::Visible:Visibility::Collapsed);
                    keyboardTarget.IsTabStop(customization.editing);
                    Automation::AutomationProperties::SetName(keyboardTarget,item.label);
                    Automation::AutomationProperties::SetAutomationId(keyboardTarget,L"FennCustomizePlaced-"+item.control);
                    control=it->second;
                } else {
                    for(size_t i=0;i<buttons.size();i++)if(i!=5 && i!=6 && item.control==commands[i])control=buttons[i];
                    if(auto it=pinned.rows.find(item.control);it!=pinned.rows.end()){control=it->second.button;it->second.button.IsEnabled(visible && (customization.editing || it->second.enabled));}
                }
                if(control) {
                    if(item.control!=L"address" && !item.spring) {
                        keepDrag.insert(item.id);
                        auto [it,inserted]=dragHosts.try_emplace(item.id);
                        auto grid=it->second;
                        if(inserted) {
                            grid.Background(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
                            grid.VerticalAlignment(VerticalAlignment::Center);
                            auto element=control.as<FrameworkElement>();
                            if(auto parent=element.Parent().try_as<Panel>()){uint32_t index;if(parent.Children().IndexOf(control,index))parent.Children().RemoveAt(index);}
                            Grid::SetColumn(element,0);grid.Children().Append(control);
                        }
                        control.IsHitTestVisible(!customization.editing);
                        control=grid;
                    }
                    auto element=control.as<FrameworkElement>();
                    customization.BindToolbarControl(element,item);dropSlots.emplace_back(item.id,element);
                    desired.push_back(control);
                }
            }
            if(hasOverflow)desired.push_back(buttons[6]);
            if(!productMenu.enabled)desired.push_back(buttons[5]);
        } else if(visible) {
            for(size_t i=0;i<3;i++)desired.push_back(buttons[i]);
            desired.push_back(address);desired.push_back(buttons[3]);desired.push_back(buttons[4]);
            desired.push_back(pinned.panel);if(hasOverflow)desired.push_back(buttons[6]);if(!productMenu.enabled)desired.push_back(buttons[5]);
        } else desired.push_back(address);
        // Status indicators are fixed, never palette entries or overflow candidates.
        if(visible) {
            if(commandState.indicators[0].shown)
                desired.push_back(commandState.indicators[0].enabled
                    ? indicatorButtons[0].as<UIElement>() : privateLabel.as<UIElement>());
            if(commandState.indicators[1].shown)desired.push_back(indicatorButtons[1]);
        }
        for(auto it=dragHosts.begin();it!=dragHosts.end();) {
            if(!keepDrag.contains(it->first)){if(it->second.Children().Size())it->second.Children().GetAt(0).IsHitTestVisible(true);it->second.Children().Clear();it=dragHosts.erase(it);}else ++it;
        }
        for(auto it=stock.rows.begin();it!=stock.rows.end();) {
            if(!keepDrag.contains(it->first))it=stock.rows.erase(it);else ++it;
        }
        if(desired==ordered && arrangedCollapsed==!customization.editing)return;
        // Retain the editor and flyout anchor while changing placement columns.
        for(uint32_t i=host.Children().Size();i>0;i--) {
            auto child=host.Children().GetAt(i-1);
            if(child!=surface && std::find(desired.begin(),desired.end(),child)==desired.end())host.Children().RemoveAt(i-1);
        }
        for(auto const& child:desired) {uint32_t index;if(!host.Children().IndexOf(child,index))host.Children().Append(child);}
        for(uint32_t i=0;i<desired.size();i++) {
            auto child=desired[i];
            if(child==address || child==buttons[5])continue;
            uint32_t index;host.Children().IndexOf(child,index);
            if(index!=i+1){host.Children().RemoveAt(index);host.Children().InsertAt(i+1,child);}
        }
        for(auto it=spacers.begin();it!=spacers.end();) {if(!keep.contains(it->first))it=spacers.erase(it);else ++it;}
        Arrange(desired,false);
        ordered=std::move(desired);
    }
    // Flexible spaces take no width outside customization: the default layout
    // is the controls and the address field with fixed gaps.
    bool arrangedCollapsed{true};
    Microsoft::UI::Xaml::Controls::MenuFlyout contextMenu{nullptr};
    // Firefox's Customize Toolbar command arrives as a counter in the snapshot.
    double editorRequest{-1};
    void RequestEditor(double value) {
        if (editorRequest < 0) { editorRequest = value; return; }
        if (value <= editorRequest) return;
        editorRequest = value;
        if (visible && !customization.editing && customization.state.revision) customization.Open(buttons[0]);
    }
    void Arrange(std::vector<Microsoft::UI::Xaml::UIElement> const& desired,bool extra) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        const bool collapsed=!customization.editing;
        arrangedCollapsed=collapsed;
        auto isSpring=[&](UIElement const& element){
            for(auto const& [id,grid]:spacers)if(element==grid)return true;
            return false;
        };
        host.ColumnDefinitions().Clear();
        auto column=[&](GridLength width,double min=0,double max=INFINITY) {
            ColumnDefinition c;c.Width(width);c.MinWidth(min);c.MaxWidth(max);host.ColumnDefinitions().Append(c);
        };
        if(visible)column(GridLengthHelper::FromPixels(8));
        for(size_t i=0;i<desired.size();i++) {
            auto child=desired[i];const bool spring=isSpring(child);
            Grid::SetColumn(child.as<FrameworkElement>(),int(host.ColumnDefinitions().Size()));
            if(child==address)column(GridLengthHelper::FromValueAndType(5,GridUnitType::Star),visible?(extra?220:256):0);
            else if(spring&&collapsed)column(GridLengthHelper::FromPixels(0));
            else if(spring)column(GridLengthHelper::FromValueAndType(1,GridUnitType::Star),1,112);
            else column(GridLengthHelper::Auto());
            if(spring&&collapsed)continue;
            // The next control, skipping flexible spaces that take no width.
            size_t next=i+1;
            while(collapsed && next<desired.size() && isSpring(desired[next]))next++;
            if(visible && next<desired.size() && child!=address && desired[next]!=address && !spring) {
                if(!isSpring(desired[next]))column(GridLengthHelper::FromPixels(ToolbarMetrics::kGap));
            }
        }
        if(visible)column(GridLengthHelper::FromPixels(8));
        Grid::SetColumn(surface,0);Grid::SetColumnSpan(surface,int(host.ColumnDefinitions().Size()));
    }
    ~NativeNavigationToolbar() { lifetime.reset(); for (auto timer : historyTimers) if (timer) timer.Stop(); }
    static States Read(JObject const& snapshot) {
        States result{};
        if (!snapshot.HasKey(L"navigation")) return result;
        auto navigation = snapshot.GetNamedObject(L"navigation");
        for (size_t i = 0; i < result.size(); ++i) {
            if (i >= 3 && !navigation.HasKey(commands[i])) continue;
            auto entry = navigation.GetNamedObject(commands[i]);
            auto& item = result[i];
            item.enabled = entry.GetNamedBoolean(L"enabled");
            item.shown=entry.GetNamedBoolean(L"shown",true);
            if(i==8){item.progress=entry.GetNamedNumber(L"progress",-1);if(!std::isfinite(item.progress)||item.progress< -1||item.progress>100)throw winrt::hresult_invalid_argument();item.attention=entry.GetNamedString(L"attention",L"");NativeJson::RequireLength(item.attention,32);}
            item.required = entry.GetNamedBoolean(L"required",item.enabled);
            if (i == 2) item.stopping = entry.GetNamedBoolean(L"stopping", false);
            item.label = entry.GetNamedString(L"label", L"");
            item.tooltip = entry.GetNamedString(L"tooltip", L"");
            NativeJson::RequireLength(item.label, NativeJson::kMaxShortLabel);
            NativeJson::RequireLength(item.tooltip, NativeJson::kMaxShortLabel);
        }
        if(navigation.HasKey(L"indicators")) {
            auto indicators=navigation.GetNamedObject(L"indicators");
            for(size_t i=0;i<result.indicators.size();i++) {
                auto entry=indicators.GetNamedObject(i==0?L"private":L"analysis");
                auto& item=result.indicators[i];
                item.shown=entry.GetNamedBoolean(L"shown");
                item.enabled=entry.GetNamedBoolean(L"enabled");
                item.label=entry.GetNamedString(L"label");
                NativeJson::RequireLength(item.label,NativeJson::kMaxShortLabel);
            }
        } else for(auto& item:result.indicators)item.shown=false;
        return result;
    }
    void Apply(States const& state, bool visible) {
        using namespace Microsoft::UI::Xaml;
        this->visible=visible;commandState=state;
        for(size_t i=0;i<indicatorButtons.size();i++) {
            auto const& item=state.indicators[i];
            indicatorButtons[i].IsEnabled(visible && item.shown && item.enabled && !customization.editing);
            Automation::AutomationProperties::SetName(indicatorButtons[i],item.label);
            Controls::ToolTipService::SetToolTip(indicatorButtons[i],winrt::box_value(item.label));
        }
        indicatorButtons[0].Content(winrt::box_value(state.indicators[0].label));
        privateLabel.Text(state.indicators[0].label);
        Automation::AutomationProperties::SetName(privateLabel,state.indicators[0].label);
        if (!visible) { historyMenu.Close(); productMenu.Close(); sentOverflow.clear();pendingOverflow.clear();pinned.Apply({}, false); customization.Apply({}, false); Layout({}); }
        hasOverflow = visible && state[6].enabled;
        requiredOverflow = visible && state[6].required;
        const bool downloading=visible && state[8].progress>=0 && !customization.editing;
        downloadProgress.Visibility(downloading?Visibility::Visible:Visibility::Collapsed);
        downloadIcon.Visibility(downloading?Visibility::Collapsed:Visibility::Visible);
        downloadPie.Visibility(state[8].progress>0?Visibility::Visible:Visibility::Collapsed);
        // Two arcs also represent exactly 100% without a coincident-endpoint full-circle ambiguity.
        const double sweep=std::clamp(state[8].progress,0.0,100.0)*6.283185307179586/100.0;
        for(size_t j=0;j<downloadArcs.size();j++) {
            const double angle=sweep*double(j+1)/2;
            downloadArcs[j].Point({float(8+6.5*std::sin(angle)),float(8-6.5*std::cos(angle))});
        }
        size_t attention=state[8].attention==L"severe"?2:state[8].attention==L"warning"?1:0;
        for(size_t j=0;j<downloadAttention.size();j++)downloadAttention[j].Visibility(visible && !state[8].attention.empty() && !customization.editing && j==attention?Visibility::Visible:Visibility::Collapsed);
        stopping = visible && state[2].stopping;
        icons[2].Symbol(stopping ? Controls::Symbol::Cancel : Controls::Symbol::Refresh);
        for (size_t i = 0; i < buttons.size(); ++i) {
            buttons[i].IsEnabled(visible && state[i].enabled);
            if (!state[i].label.empty()) Automation::AutomationProperties::SetName(buttons[i], state[i].label);
            if (!state[i].tooltip.empty()) Controls::ToolTipService::SetToolTip(buttons[i], winrt::box_value(state[i].tooltip));
        }
    }
};
