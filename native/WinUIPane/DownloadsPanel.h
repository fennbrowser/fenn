#pragma once
#include "JsonValidation.h"
#include "shell_core.h"

// WinUI controls and object lifetime only. Firefox owns all download policy;
// ShellCore owns independent opening/revision/action acceptance.
struct NativeDownloadsPanel {
    using Object=winrt::Windows::Data::Json::JsonObject;
    using Button=Microsoft::UI::Xaml::Controls::Button;
    struct Command { uint32_t id{}; winrt::hstring label,icon; bool close{},pending{}; };
    struct Item { uint32_t id{},main{},primary{}; winrt::hstring name,status; double progress{-1}; bool progressVisible{},paused{},warning{}; std::vector<Command> commands; };
    struct Model { bool enabled{}; uint32_t session{},revision{},actionRevision{},closed{}; winrt::hstring title,empty,all,more,fingerprint; std::vector<Item> items; std::vector<uint32_t> actions; };
    struct Row {
        Microsoft::UI::Xaml::Controls::ListViewItem host{nullptr};
        Button main{nullptr},primary{nullptr},more{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock name{nullptr},status{nullptr};
        Microsoft::UI::Xaml::Controls::FontIcon state{nullptr},primaryIcon{nullptr};
        Microsoft::UI::Xaml::Controls::ProgressBar progress{nullptr};
        winrt::hstring menuKey;
    };
    ShellCore* core{};
    bool enabled{};
    uint32_t opening{};
    Model model;
    std::shared_ptr<int> openingLifetime;
    std::map<uint32_t,Row> rows;
    Button anchor{nullptr},all{nullptr};
    Microsoft::UI::Xaml::Controls::Flyout flyout{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel content{nullptr};
    Microsoft::UI::Xaml::Controls::ListView list{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock title{nullptr},empty{nullptr};
    Microsoft::UI::Xaml::Controls::ProgressRing loading{nullptr};
    std::function<void(wchar_t const*,uint32_t,Button const&)> dispatch;
    std::function<void()> afterClose;
    std::shared_ptr<int> lifetime=std::make_shared<int>(0);
    ~NativeDownloadsPanel(){lifetime.reset();Close();}
    void ReleaseViews(){rows.clear();model={};content=nullptr;list=nullptr;title=nullptr;empty=nullptr;loading=nullptr;all=nullptr;anchor=nullptr;flyout=nullptr;}
    static uint32_t Id(Object const& object,wchar_t const* key,bool zero=false) {
        auto n=NativeJson::RequireRevision(object,key);
        if(n>UINT32_MAX || (!zero && !n))winrt::throw_hresult(E_INVALIDARG);
        return uint32_t(n);
    }
    static wchar_t const* Glyph(winrt::hstring const& icon) {
        if(icon==L"file")return L"\xE8A5";
        if(icon==L"cancel")return L"\xE711";
        if(icon==L"retry")return L"\xE72C";
        if(icon==L"folder")return L"\xE838";
        if(icon==L"review")return L"\xE7BA";
        if(icon==L"pause")return L"\xE769";
        if(icon==L"resume")return L"\xE768";
        if(icon==L"remove")return L"\xE74D";
        if(icon==L"copy")return L"\xE8C8";
        return nullptr;
    }
    static Model Read(Object const& snapshot) {
        Model result;
        auto navigation=snapshot.GetNamedObject(L"navigation",Object());
        if(!navigation.HasKey(L"downloadsPanel"))return result;
        auto value=navigation.GetNamedObject(L"downloadsPanel");
        result.enabled=value.GetNamedBoolean(L"enabled",false);
        if(value.HasKey(L"closed"))result.closed=Id(value,L"closed",true);
        if(!value.HasKey(L"session"))return result;
        result.session=Id(value,L"session");result.revision=Id(value,L"revision");result.actionRevision=Id(value,L"actionRevision");
        result.title=value.GetNamedString(L"title");result.empty=value.GetNamedString(L"empty");result.all=value.GetNamedString(L"all");result.more=value.GetNamedString(L"more");
        auto items=value.GetNamedArray(L"items");if(items.Size()>8)winrt::throw_hresult(E_INVALIDARG);
        std::set<uint32_t> ids,actions{255};result.actions.push_back(255);
        for(auto v:items){auto entry=v.GetObject();Item item;
            item.id=Id(entry,L"id");item.main=Id(entry,L"main",true);item.primary=Id(entry,L"primary",true);
            item.name=entry.GetNamedString(L"name");item.status=entry.GetNamedString(L"status");
            item.progress=entry.GetNamedNumber(L"progress");
            if(!ids.insert(item.id).second || !std::isfinite(item.progress) || item.progress < -1 || item.progress > 100)winrt::throw_hresult(E_INVALIDARG);
            item.progressVisible=entry.GetNamedBoolean(L"progressVisible");item.paused=entry.GetNamedBoolean(L"paused");item.warning=entry.GetNamedBoolean(L"warning");
            auto commands=entry.GetNamedArray(L"commands");if(commands.Size()>8)winrt::throw_hresult(E_INVALIDARG);
            std::set<uint32_t> local;
            for(auto c:commands){auto command=c.GetObject();Command next{Id(command,L"id"),command.GetNamedString(L"label"),command.GetNamedString(L"icon"),command.GetNamedBoolean(L"close"),command.GetNamedBoolean(L"pending")};
                if(!Glyph(next.icon) || !actions.insert(next.id).second || next.label.empty())winrt::throw_hresult(E_INVALIDARG);
                NativeJson::RequireLength(next.label,1024);local.insert(next.id);if(!next.pending)result.actions.push_back(next.id);item.commands.push_back(next);
            }
            if((item.main && !local.contains(item.main)) || (item.primary && !local.contains(item.primary)))winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(item.name,1024);NativeJson::RequireLength(item.status,1024);result.items.push_back(std::move(item));
        }
        for(auto text:{result.title,result.empty,result.all,result.more})NativeJson::RequireLength(text,1024);
        result.fingerprint=value.Stringify();if(result.fingerprint.size()>65536)winrt::throw_hresult(E_INVALIDARG);
        return result;
    }
    bool Resolve(Model const& next) {
        if(!next.session)return false;
        bool changed{};
        winrt::check_hresult(shell_core_downloads_panel_resolve(core,next.session,next.revision,next.actions.data(),next.actions.size(),
            reinterpret_cast<uint16_t const*>(next.fingerprint.c_str()),next.fingerprint.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);
        return changed;
    }
    void Close() {
        opening=0;afterClose={};openingLifetime.reset();if(loading)loading.IsActive(false);if(core)shell_core_downloads_panel_dismiss(core);
        if(flyout)flyout.Hide();ReleaseViews();
    }
    void Invoke(uint32_t id,bool close) {
        if(!opening || !dispatch)return;
        bool accepted{};
        const auto status=shell_core_downloads_panel_activate(core,opening,model.revision,id,&accepted);
        if(GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS",nullptr,0))fwprintf(stderr,L"Native downloads action session=%u revision=%u action=%u accepted=%d status=%u\n",opening,model.revision,id,accepted,unsigned(status));
        if(status!=SHELL_STATUS_OK || !accepted)return;
        auto command=L"downloads-panel:invoke:"+std::to_wstring(opening)+L":"+std::to_wstring(model.actionRevision)+L":"+std::to_wstring(id);
        for(auto const& item:model.items){auto it=rows.find(item.id);if(it!=rows.end()){if(item.main==id)it->second.main.IsEnabled(false);if(item.primary==id)it->second.primary.IsEnabled(false);}}
        if(id==255)all.IsEnabled(false);
        auto send=[this,command,button=anchor,alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime)] {if(!alive.expired() && !current.expired() && enabled && dispatch)dispatch(command.c_str(),0,button);};
        if(close && flyout){afterClose=send;flyout.Hide();}else send();
    }
    void Open(Button const& source) {
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        if(opening){if(flyout && flyout.IsOpen())flyout.Hide();else{dispatch((L"downloads-panel:dismiss:"+std::to_wstring(opening)).c_str(),0,anchor);Close();}return;}
        if(!enabled || !core || !dispatch)return;
        uint32_t session{};if(shell_core_downloads_panel_open(core,&session)!=SHELL_STATUS_OK || !session)return;
        openingLifetime=std::make_shared<int>(0);opening=session;anchor=source;rows.clear();model={};
        flyout=Flyout();flyout.ShouldConstrainToRootBounds(false);
        flyout.Placement(source.FlowDirection()==FlowDirection::RightToLeft?Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft:Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
        content=StackPanel();content.Spacing(8);content.Width(std::max(200.0,std::min(420.0,double(source.XamlRoot().Size().Width)-48.0)));
        Automation::AutomationProperties::SetAutomationId(content,L"FennDownloadsPanel");
        title=Markup::XamlReader::Load(LR"(<TextBlock xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtitleTextBlockStyle}" TextWrapping="Wrap"/>)").as<TextBlock>();content.Children().Append(title);
        loading=ProgressRing();loading.Width(24);loading.Height(24);loading.IsActive(true);content.Children().Append(loading);
        empty=TextBlock();empty.TextWrapping(TextWrapping::Wrap);empty.Visibility(Visibility::Collapsed);content.Children().Append(empty);
        list=ListView();list.SelectionMode(ListViewSelectionMode::None);list.MaxHeight(400);list.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        Automation::AutomationProperties::SetAutomationId(list,L"FennDownloadsList");content.Children().Append(list);
        all=Button();all.HorizontalAlignment(HorizontalAlignment::Stretch);all.IsEnabled(false);
        Automation::AutomationProperties::SetAutomationId(all,L"FennDownloadsAll");
        all.Click([this,alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime)](auto const&,auto const&){if(!alive.expired() && !current.expired())Invoke(255,true);});content.Children().Append(all);
        flyout.Content(content);
        flyout.Closed([this,session,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const&){
            if(alive.expired() || opening!=session)return;
            auto send=std::move(afterClose);afterClose={};opening=0;shell_core_downloads_panel_dismiss(core);
            loading.IsActive(false);
            if(send)anchor.DispatcherQueue().TryEnqueue(std::move(send));
            else if(dispatch)dispatch((L"downloads-panel:dismiss:"+std::to_wstring(session)).c_str(),0,anchor);
            ReleaseViews();
        });
        dispatch((L"downloads-panel:open:"+std::to_wstring(session)).c_str(),0,source);
    }
    void Apply(Model const& next,bool visible,bool publish) {
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        enabled=visible && next.enabled;
        if(!enabled || (opening && next.closed==opening)){Close();return;}
        if(!publish || !opening || next.session!=opening || !flyout)return;
        bool changed{};
        winrt::check_hresult(shell_core_downloads_panel_accept(core,next.session,next.revision,next.actions.data(),next.actions.size(),
            reinterpret_cast<uint16_t const*>(next.fingerprint.c_str()),next.fingerprint.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);
        if(!changed)return;model=next;
        if(GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS",nullptr,0))fwprintf(stderr,L"Native downloads publication session=%u revision=%u actions=%zu\n",next.session,next.revision,next.actions.size());
        if(title.Text()!=next.title){title.Text(next.title);Automation::AutomationProperties::SetName(content,next.title);}
        if(empty.Text()!=next.empty)empty.Text(next.empty);empty.Visibility(next.items.empty()?Visibility::Visible:Visibility::Collapsed);
        loading.IsActive(false);loading.Visibility(Visibility::Collapsed);
        if(Automation::AutomationProperties::GetName(all)!=next.all){all.Content(winrt::box_value(next.all));Automation::AutomationProperties::SetName(all,next.all);}if(!all.IsEnabled())all.IsEnabled(true);
        std::set<uint32_t> keep;uint32_t index=0;
        for(auto const& item:next.items){keep.insert(item.id);auto [it,added]=rows.try_emplace(item.id);auto& row=it->second;
            if(added){
                row.host=ListViewItem();row.host.IsTabStop(false);row.host.HorizontalContentAlignment(HorizontalAlignment::Stretch);row.host.Padding({0,0,0,0});
                auto grid=Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Padding="4,8"><Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="32"/><ColumnDefinition Width="32"/></Grid.ColumnDefinitions><Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions><Button Style="{StaticResource SubtleButtonStyle}" MinWidth="0" Padding="4" HorizontalAlignment="Stretch" HorizontalContentAlignment="Stretch"><Grid><Grid.ColumnDefinitions><ColumnDefinition Width="24"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions><FontIcon FontFamily="{ThemeResource SymbolThemeFontFamily}" FontSize="16"/><TextBlock Grid.Column="1" TextTrimming="CharacterEllipsis" VerticalAlignment="Center" Foreground="{ThemeResource TextFillColorPrimaryBrush}"/></Grid></Button><Button Grid.Column="1" Style="{StaticResource SubtleButtonStyle}" Width="32" Height="32" MinWidth="0" MinHeight="0" Padding="0"><FontIcon FontFamily="{ThemeResource SymbolThemeFontFamily}" FontSize="14"/></Button><Button Grid.Column="2" Style="{StaticResource SubtleButtonStyle}" Width="32" Height="32" MinWidth="0" MinHeight="0" Padding="0"><FontIcon FontFamily="{ThemeResource SymbolThemeFontFamily}" FontSize="14" Glyph="&#xE712;"/></Button><TextBlock Grid.Row="1" Grid.ColumnSpan="3" Margin="4,2,4,6" TextWrapping="Wrap" Style="{StaticResource CaptionTextBlockStyle}" Foreground="{ThemeResource TextFillColorSecondaryBrush}"/><ProgressBar Grid.Row="2" Grid.ColumnSpan="3" Margin="4,0" MinWidth="0"/></Grid>)").as<Grid>();
                row.main=grid.Children().GetAt(0).as<Button>();auto main=row.main.Content().as<Grid>();row.state=main.Children().GetAt(0).as<FontIcon>();row.name=main.Children().GetAt(1).as<TextBlock>();
                row.primary=grid.Children().GetAt(1).as<Button>();row.primaryIcon=row.primary.Content().as<FontIcon>();row.more=grid.Children().GetAt(2).as<Button>();row.status=grid.Children().GetAt(3).as<TextBlock>();row.progress=grid.Children().GetAt(4).as<ProgressBar>();row.host.Content(grid);
                Automation::AutomationProperties::SetAccessibilityView(row.state,Automation::Peers::AccessibilityView::Raw);
                Automation::AutomationProperties::SetAccessibilityView(row.primaryIcon,Automation::Peers::AccessibilityView::Raw);
                Automation::AutomationProperties::SetAccessibilityView(row.more.Content().as<FontIcon>(),Automation::Peers::AccessibilityView::Raw);
                auto invoke=[this,id=item.id,alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime)](bool primary){if(alive.expired() || current.expired())return;for(auto const& item:model.items)if(item.id==id){auto target=primary?item.primary:item.main;for(auto const& c:item.commands)if(c.id==target){Invoke(c.id,c.close);return;}}};
                row.main.Click([invoke](auto const&,auto const&){invoke(false);});row.primary.Click([invoke](auto const&,auto const&){invoke(true);});
                Automation::AutomationProperties::SetAutomationId(row.host,L"FennDownloadRow-"+winrt::to_hstring(item.id));
                Automation::AutomationProperties::SetAutomationId(row.main,L"FennDownloadOpen-"+winrt::to_hstring(item.id));
                Automation::AutomationProperties::SetAutomationId(row.primary,L"FennDownloadAction-"+winrt::to_hstring(item.id));
                Automation::AutomationProperties::SetAutomationId(row.more,L"FennDownloadMore-"+winrt::to_hstring(item.id));
            }
            uint32_t old;if(list.Items().IndexOf(row.host,old)){if(old!=index){list.Items().RemoveAt(old);list.Items().InsertAt(index,row.host);}}else list.Items().InsertAt(index,row.host);++index;
            if(row.name.Text()!=item.name){row.name.Text(item.name);Automation::AutomationProperties::SetName(row.main,item.name);Automation::AutomationProperties::SetName(row.host,item.name);ToolTipService::SetToolTip(row.main,winrt::box_value(item.name));}
            if(row.status.Text()!=item.status){row.status.Text(item.status);Automation::AutomationProperties::SetItemStatus(row.host,item.status);Automation::AutomationProperties::SetHelpText(row.main,item.status);}
            auto available=[&](uint32_t id){for(auto const& c:item.commands)if(c.id==id)return !c.pending;return false;};
            if(row.main.IsEnabled()!=available(item.main))row.main.IsEnabled(available(item.main));
            if(row.primary.IsEnabled()!=available(item.primary))row.primary.IsEnabled(available(item.primary));
            auto primaryVisibility=item.primary?Visibility::Visible:Visibility::Collapsed;if(row.primary.Visibility()!=primaryVisibility)row.primary.Visibility(primaryVisibility);
            winrt::hstring stateGlyph=item.warning?L"\xE7BA":L"\xE8A5";if(row.state.Glyph()!=stateGlyph)row.state.Glyph(stateGlyph);
            auto progressVisibility=item.progressVisible?Visibility::Visible:Visibility::Collapsed;if(row.progress.Visibility()!=progressVisibility)row.progress.Visibility(progressVisibility);
            const bool indeterminate=item.progressVisible && !item.paused && item.progress<0;
            if(row.progress.IsIndeterminate()!=indeterminate)row.progress.IsIndeterminate(indeterminate);
            if(row.progress.Value()!=std::max(0.0,item.progress))row.progress.Value(std::max(0.0,item.progress));
            if(row.progress.ShowPaused()!=item.paused)row.progress.ShowPaused(item.paused);
            winrt::hstring menuKey;for(auto const& c:item.commands)menuKey=menuKey+winrt::to_hstring(c.id)+c.label+c.icon+(c.pending?L"1":L"0");
            if(menuKey!=row.menuKey){row.menuKey=menuKey;MenuFlyout menu;menu.ShouldConstrainToRootBounds(false);
                menu.Placement(anchor.FlowDirection()==FlowDirection::RightToLeft?Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft:Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
                for(auto const& c:item.commands){if(c.id==item.main || c.id==item.primary)continue;MenuFlyoutItem entry;entry.Text(c.label);entry.IsEnabled(!c.pending);
                    auto icon=Markup::XamlReader::Load(LR"(<FontIcon xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" FontFamily="{ThemeResource SymbolThemeFontFamily}" FontSize="14"/>)").as<FontIcon>();icon.Glyph(Glyph(c.icon));entry.Icon(icon);
                    Automation::AutomationProperties::SetAutomationId(entry,L"FennDownloadCommand-"+winrt::to_hstring(c.id));
                    entry.Click([this,c,alive=std::weak_ptr<int>(lifetime),openingToken=std::weak_ptr<int>(openingLifetime)](auto const&,auto const&){if(alive.expired() || openingToken.expired())return;for(auto const& item:model.items)for(auto const& current:item.commands)if(current.id==c.id && !current.pending && current.label==c.label && current.icon==c.icon){Invoke(c.id,c.close);return;}});menu.Items().Append(entry);
                }
                row.more.Flyout(menu);row.more.IsEnabled(menu.Items().Size()!=0);
            }
            Automation::AutomationProperties::SetName(row.more,next.title+L" â€” "+item.name);
            for(auto const& c:item.commands)if(c.id==item.primary){if(row.primaryIcon.Glyph()!=Glyph(c.icon))row.primaryIcon.Glyph(Glyph(c.icon));if(Automation::AutomationProperties::GetName(row.primary)!=c.label){Automation::AutomationProperties::SetName(row.primary,c.label);ToolTipService::SetToolTip(row.primary,winrt::box_value(c.label));}}
        }
        if(!flyout.IsOpen())flyout.ShowAt(anchor);
        for(auto it=rows.begin();it!=rows.end();)if(!keep.contains(it->first)){uint32_t old;if(list.Items().IndexOf(it->second.host,old))list.Items().RemoveAt(old);it=rows.erase(it);}else ++it;
    }
};
