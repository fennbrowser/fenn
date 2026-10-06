#pragma once
#include <array>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <functional>
#include "JsonValidation.h"
#include "shell_core.h"
#include "PinnedExtensionButtons.h"
#include "ToolbarCustomization.h"
#include "ToolbarDragPreview.h"

// WinUI presentation only. Firefox owns eligibility, labels and commands.
struct NativeNavigationToolbar {
    using JObject = winrt::Windows::Data::Json::JsonObject;
    using Button = Microsoft::UI::Xaml::Controls::Button;
    struct State { bool enabled{}, stopping{}, required{}; winrt::hstring label, tooltip; };
    using States = std::array<State, 7>;
    States commandState{};
    std::vector<std::pair<winrt::hstring,Microsoft::UI::Xaml::FrameworkElement>> dropSlots;
    static constexpr wchar_t const* commands[] = {L"back", L"forward", L"reload", L"profile", L"extensions", L"appmenu", L"overflow"};
    Microsoft::UI::Xaml::Controls::Grid host{nullptr}, surface{nullptr};
    Microsoft::UI::Xaml::FrameworkElement address{nullptr};
    std::map<winrt::hstring,Microsoft::UI::Xaml::Controls::Grid> spacers, dragHosts;
    std::vector<Microsoft::UI::Xaml::UIElement> ordered;
    bool visible{};
    std::array<Button, 7> buttons;
    std::array<Microsoft::UI::Xaml::Controls::SymbolIcon, 7> icons;
    PinnedExtensionButtons pinned;
    NativeToolbarCustomization customization;
    ToolbarDragPreview preview;
    bool stopping{}, hasOverflow{}, requiredOverflow{}, overflowQueued{}, rootObserved{};
    NativeToolbarCustomization::State layoutState;
    std::wstring pendingOverflow, sentOverflow;
    std::array<Microsoft::UI::Dispatching::DispatcherQueueTimer, 2> historyTimers{nullptr, nullptr};
    std::array<bool, 2> historyHeld{};
    std::array<uint32_t, 7> inputModifiers{};
    std::function<void(wchar_t const*, uint32_t, Button const&)> activate;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    static Button CreateButton(size_t i,Microsoft::UI::Xaml::Controls::Symbol symbol) {
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        auto button = Markup::XamlReader::Load(
            LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" Width="32" Height="32" MinWidth="0" MinHeight="0" Padding="0"/>)").as<Button>();
        SymbolIcon icon(symbol);
        // Firefox-like density with stock icons and generous button targets.
        Viewbox iconHost;
        iconHost.Width(16);
        iconHost.Height(16);
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
        for(size_t i=0;i<5;i++)if(item.control==commands[i])return CreateButton(i,i==4?Symbol::Library:icons[i].Symbol());
        for(auto const& state:paletteState)if(state.id==item.palette) {
            auto renderer=std::make_shared<PinnedExtensionButtons>();renderer->externalLayout=true;renderer->Apply({state},true);
            auto button=renderer->rows.at(state.id).button;renderer->panel.Children().Clear();button.IsEnabled(true);
            paletteRenderers[item.id]=renderer;return button;
        }
        if(item.control==L"address") {
            TextBox field;field.Text(item.label);field.IsReadOnly(true);field.Width(160);field.Height(32);return field;
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
        const Symbol symbols[] = {Symbol::Back, Symbol::Forward, Symbol::Refresh, Symbol::Contact, Symbol::Library, Symbol::More, Symbol::Forward};
        const wchar_t* names[] = {L"Back", L"Forward", L"Reload", L"Account", L"Extensions", L"Application menu", L"More tools"};
        const wchar_t* ids[] = {L"FirefoxWinUIBack", L"FirefoxWinUIForward", L"FirefoxWinUIReload", L"FirefoxWinUIProfile", L"FirefoxWinUIExtensions", L"FirefoxWinUIAppMenu", L"FirefoxWinUIOverflow"};
        for (size_t i = 0; i < buttons.size(); ++i) {
            auto button = CreateButton(i,symbols[i]);
            icons[i]=button.Content().as<Viewbox>().Child().try_as<SymbolIcon>();
            button.IsEnabled(false);
            Automation::AutomationProperties::SetAutomationId(button, ids[i]);
            Automation::AutomationProperties::SetName(button, names[i]);
            ToolTipService::SetToolTip(button, winrt::box_value(winrt::hstring(names[i])));
            // Click has no modifier data. Preserve the originating WinUI event
            // instead of sampling keys after an asynchronously delivered click.
            button.AddHandler(UIElement::PointerPressedEvent(), winrt::box_value(
                Microsoft::UI::Xaml::Input::PointerEventHandler([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& args) {
                    if (alive.expired()) return;
                    inputModifiers[i] = uint32_t(args.KeyModifiers()) & 15u;
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
            button.PreviewKeyDown([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (alive.expired()) return;
                if (i < 2) { historyHeld[i] = false; historyTimers[i].Stop(); }
                inputModifiers[i] = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) |
                    ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) |
                    ((GetKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u) |
                    ((GetKeyState(VK_LWIN) & 0x8000) ? 8u : 0u);
            });
            button.Click([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (alive.expired() || !activate || !buttons[i].IsEnabled() || !visible) return;
                if (i < 2 && historyHeld[i]) { historyHeld[i] = false; return; }
                const uint32_t modifiers = inputModifiers[i];
                inputModifiers[i] = 0;
                activate(i == 2 && stopping ? L"stop" : commands[i], modifiers, buttons[i]);
            });
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
        customization.editorChanged=[this]{Layout(layoutState);};
        customization.dragEnded=[this]{preview.Reset();};
        preview.arrange=[this](auto const& order,bool extra){Arrange(order,extra);};
        host.AllowDrop(true);
        auto updatePreview=[this](auto const& args) {
            if(!customization.Accept(args)){preview.Reset();return false;}
            preview.Begin(host,ordered,dropSlots,customization.dragId,customization.dragRevision);
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
        const double availableWidth=host.XamlRoot()?host.XamlRoot().Size().Width:0;
        std::set<winrt::hstring> overflowed;
        if(visible && state.revision && availableWidth>0) {
            std::vector<ShellToolbarSlot> slots;
            std::vector<NativeToolbarCustomization::Item const*> items;
            for(auto const& item:state.items)if(item.placed) {
                slots.push_back({item.spring?2u:item.control==L"address"?1u:0u,item.canAutoOverflow?(item.extension?2u:1u):0u});
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
        dropSlots.clear();
        std::vector<UIElement> desired;
        std::set<winrt::hstring> keep, keepDrag;
        if(!visible || !state.revision) {
            for(auto const& [id,grid]:dragHosts){if(grid.Children().Size())grid.Children().GetAt(0).IsHitTestVisible(true);grid.Children().Clear();}
            dragHosts.clear();
        }
        if(visible && state.revision) {
            for(auto const& item:state.items) {
                if(!item.placed || overflowed.contains(item.id))continue;
                UIElement control{nullptr};
                if(item.control==L"address")control=address;
                else if(item.spring) {
                    keep.insert(item.id);
                    auto [it,inserted]=spacers.try_emplace(item.id);
                    if(inserted)it->second.IsHitTestVisible(false);
                    control=it->second;
                } else {
                    for(size_t i=0;i<5;i++)if(item.control==commands[i])control=buttons[i];
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
            desired.push_back(buttons[5]);
        } else if(visible) {
            for(size_t i=0;i<3;i++)desired.push_back(buttons[i]);
            desired.push_back(address);desired.push_back(buttons[3]);desired.push_back(buttons[4]);
            desired.push_back(pinned.panel);if(hasOverflow)desired.push_back(buttons[6]);desired.push_back(buttons[5]);
        } else desired.push_back(address);
        for(auto it=dragHosts.begin();it!=dragHosts.end();) {
            if(!keepDrag.contains(it->first)){if(it->second.Children().Size())it->second.Children().GetAt(0).IsHitTestVisible(true);it->second.Children().Clear();it=dragHosts.erase(it);}else ++it;
        }
        if(desired==ordered)return;
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
    void Arrange(std::vector<Microsoft::UI::Xaml::UIElement> const& desired,bool extra) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        host.ColumnDefinitions().Clear();
        auto column=[&](GridLength width,double min=0,double max=INFINITY) {
            ColumnDefinition c;c.Width(width);c.MinWidth(min);c.MaxWidth(max);host.ColumnDefinitions().Append(c);
        };
        if(visible)column(GridLengthHelper::FromPixels(8));
        for(size_t i=0;i<desired.size();i++) {
            auto child=desired[i];bool spring=false;
            for(auto const& [id,grid]:spacers)if(child==grid){spring=true;break;}
            Grid::SetColumn(child.as<FrameworkElement>(),int(host.ColumnDefinitions().Size()));
            if(child==address)column(GridLengthHelper::FromValueAndType(5,GridUnitType::Star),visible?(extra?220:256):0);
            else if(spring)column(GridLengthHelper::FromValueAndType(1,GridUnitType::Star),1,112);
            else column(GridLengthHelper::Auto());
            if(visible && i+1<desired.size() && child!=address && desired[i+1]!=address && !spring) {
                bool nextSpring=false;
                for(auto const& [id,grid]:spacers)if(desired[i+1]==grid){nextSpring=true;break;}
                if(!nextSpring)column(GridLengthHelper::FromPixels(4));
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
            item.required = entry.GetNamedBoolean(L"required",item.enabled);
            if (i == 2) item.stopping = entry.GetNamedBoolean(L"stopping", false);
            item.label = entry.GetNamedString(L"label", L"");
            item.tooltip = entry.GetNamedString(L"tooltip", L"");
            NativeJson::RequireLength(item.label, NativeJson::kMaxShortLabel);
            NativeJson::RequireLength(item.tooltip, NativeJson::kMaxShortLabel);
        }
        return result;
    }
    void Apply(States const& state, bool visible) {
        using namespace Microsoft::UI::Xaml;
        this->visible=visible;commandState=state;
        if (!visible) { sentOverflow.clear();pendingOverflow.clear();pinned.Apply({}, false); customization.Apply({}, false); Layout({}); }
        hasOverflow = visible && state[6].enabled;
        requiredOverflow = visible && state[6].required;
        stopping = visible && state[2].stopping;
        icons[2].Symbol(stopping ? Controls::Symbol::Cancel : Controls::Symbol::Refresh);
        for (size_t i = 0; i < buttons.size(); ++i) {
            buttons[i].IsEnabled(visible && state[i].enabled);
            if (!state[i].label.empty()) Automation::AutomationProperties::SetName(buttons[i], state[i].label);
            if (!state[i].tooltip.empty()) Controls::ToolTipService::SetToolTip(buttons[i], winrt::box_value(state[i].tooltip));
        }
    }
};
