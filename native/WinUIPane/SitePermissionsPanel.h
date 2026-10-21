#pragma once
#include "JsonValidation.h"
#include "OmnibarIcons.h"
#include "shell_core.h"

// WinUI presentation only. Browser principals, permission changes and sharing
// remain Firefox-owned; SurfaceMenu owns opening/revision/action freshness.
struct NativeSitePermissionsPanel {
    using Object=winrt::Windows::Data::Json::JsonObject;
    using Value=winrt::Windows::Data::Json::JsonValue;
    using Button=Microsoft::UI::Xaml::Controls::Button;
    using Combo=Microsoft::UI::Xaml::Controls::ComboBox;
    struct Row {
        Microsoft::UI::Xaml::Controls::Grid host{nullptr};
        Microsoft::UI::Xaml::Controls::StackPanel text{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock label{nullptr},state{nullptr},detail{nullptr};
        Microsoft::UI::Xaml::Controls::FontIcon icon{nullptr},badge{nullptr};
        Button clear{nullptr};Combo combo{nullptr};
        uint32_t clearId{};bool locked{};std::vector<uint32_t> options;std::vector<winrt::hstring> optionLabels;
    };
    ShellCore* core{};uint32_t opening{},revision{};bool applying{},pending{},focusAfterAck{};
    Button anchor{nullptr},close{nullptr};winrt::hstring focusKey;
    Microsoft::UI::Xaml::Controls::Flyout flyout{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock heading{nullptr},reload{nullptr},error{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel content{nullptr},extras{nullptr};
    std::map<winrt::hstring,Row> rows;
    std::vector<winrt::hstring> order;
    std::function<void(Object const&,Button const&)> dispatch;
    std::shared_ptr<int> lifetime=std::make_shared<int>(0),openingLifetime;
    ~NativeSitePermissionsPanel(){lifetime.reset();Close();}
    static uint32_t Id(Object const& m,wchar_t const* key){auto value=NativeJson::RequireRevision(m,key);if(!value || value>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);return uint32_t(value);}
    static Object Read(Object const& snapshot){
        auto m=snapshot.GetNamedObject(L"navigation",Object()).GetNamedObject(L"permissionsPanel",Object());m.GetNamedBoolean(L"enabled",false);
        if(m.HasKey(L"closed") && NativeJson::RequireRevision(m,L"closed")>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);
        if(!m.HasKey(L"request"))return m;Id(m,L"request");auto source=m.GetNamedString(L"source");if(source.empty() || source.size()>64)winrt::throw_hresult(E_INVALIDARG);
        if(!m.HasKey(L"session"))return m;Id(m,L"session");Id(m,L"revision");
        for(auto key:{L"title",L"reload",L"error",L"empty",L"remove",L"managed",L"storageTitle",L"storageHint",L"protocolTitle",L"close"})NativeJson::RequireLength(m.GetNamedString(key),2048);
        if(m.GetNamedString(L"title").empty() || m.GetNamedString(L"remove").empty() || m.GetNamedString(L"close").empty())winrt::throw_hresult(E_INVALIDARG);
        auto entries=m.GetNamedArray(L"rows");if(entries.Size()>16)winrt::throw_hresult(E_INVALIDARG);std::set<uint32_t> ids;std::set<winrt::hstring> keys;
        for(auto v:entries){auto row=v.GetObject();auto key=row.GetNamedString(L"key");if(key.empty() || key.size()>64 || !keys.insert(key).second)winrt::throw_hresult(E_INVALIDARG);
            for(auto field:{L"label",L"state",L"detail",L"usage"})NativeJson::RequireLength(row.GetNamedString(field),1024);
            auto group=row.GetNamedString(L"group");if(group!=L"" && group!=L"storage" && group!=L"protocol")winrt::throw_hresult(E_INVALIDARG);
            if(!OmnibarIcons::Glyph(row.GetNamedString(L"icon")) || !OmnibarIcons::ValidState(row.GetNamedString(L"iconState")))winrt::throw_hresult(E_INVALIDARG);
            auto clear=NativeJson::RequireRevision(row,L"clear");if(clear>UINT32_MAX || (clear && (clear<100 || !ids.insert(uint32_t(clear)).second)))winrt::throw_hresult(E_INVALIDARG);
            bool locked=row.GetNamedBoolean(L"locked");if(locked && clear)winrt::throw_hresult(E_INVALIDARG);
            auto options=row.GetNamedArray(L"options");if(options.Size()>8 || (options.Size() && clear))winrt::throw_hresult(E_INVALIDARG);
            auto selected=NativeJson::RequireRevision(row,L"selected");bool found=options.Size()==0 && selected==0;
            for(auto value:options){auto option=value.GetObject();auto id=Id(option,L"id");if(id<100 || !ids.insert(id).second)winrt::throw_hresult(E_INVALIDARG);NativeJson::RequireLength(option.GetNamedString(L"label"),512);if(id==selected)found=true;}
            if(!found)winrt::throw_hresult(E_INVALIDARG);
        }
        auto actions=m.GetNamedArray(L"actions");if(actions.Size()>5)winrt::throw_hresult(E_INVALIDARG);for(auto v:actions){auto action=v.GetObject();auto id=Id(action,L"id");if(id<3 || id>7 || !ids.insert(id).second)winrt::throw_hresult(E_INVALIDARG);NativeJson::RequireLength(action.GetNamedString(L"label"),1024);}
        if(m.Stringify().size()>65536)winrt::throw_hresult(E_INVALIDARG);return m;
    }
    static std::vector<uint32_t> Actions(Object const& m){std::vector<uint32_t> result;for(auto v:m.GetNamedArray(L"rows")){auto row=v.GetObject();if(row.GetNamedBoolean(L"locked"))continue;auto clear=uint32_t(row.GetNamedNumber(L"clear"));if(clear)result.push_back(clear);for(auto o:row.GetNamedArray(L"options"))result.push_back(Id(o.GetObject(),L"id"));}for(auto v:m.GetNamedArray(L"actions"))result.push_back(Id(v.GetObject(),L"id"));return result;}
    bool Resolve(Object const& m){if(!m.HasKey(L"session"))return false;auto ids=Actions(m);auto fp=m.Stringify();bool changed{};winrt::check_hresult(shell_core_permissions_panel_resolve(core,Id(m,L"session"),Id(m,L"revision"),ids.data(),ids.size(),reinterpret_cast<uint16_t const*>(fp.c_str()),fp.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);return changed;}
    void LightDismiss(){if(flyout)flyout.Hide();}
    void Close(bool cancel=true){opening=revision=0;openingLifetime.reset();afterClose={};focusKey=L"";pending=focusAfterAck=false;if(cancel && core)shell_core_permissions_panel_dismiss(core);if(flyout)flyout.Hide();flyout=nullptr;anchor=close=nullptr;heading=reload=error=nullptr;content=extras=nullptr;rows.clear();order.clear();}
    void Enable(bool value){for(auto& [key,row]:rows){row.clear.IsEnabled(value && !row.locked);row.combo.IsEnabled(value && !row.locked);}if(extras)for(auto child:extras.Children())if(auto b=child.try_as<Button>())b.IsEnabled(value);}
    void Send(uint32_t id){
        if(!opening || applying || pending || !dispatch)return;bool accepted{};const bool retained=id>=6 || id>=100;
        if(shell_core_permissions_panel_activate(core,opening,revision,id,retained,&accepted)!=SHELL_STATUS_OK || !accepted)return;
        Object action;action.SetNamedValue(L"command",Value::CreateStringValue(L"permissions-panel:invoke:"+std::to_wstring(opening)+L":"+std::to_wstring(revision)+L":"+std::to_wstring(id)));
        if(retained){focusKey=L"";for(auto const& [key,row]:rows)if(row.clearId==id || std::find(row.options.begin(),row.options.end(),id)!=row.options.end())focusKey=key;pending=true;focusAfterAck=true;Enable(false);dispatch(action,anchor);}
        else {auto source=anchor;auto send=dispatch;auto alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime);auto callback=[action,source,send,alive,current]{if(!alive.expired() && !current.expired())send(action,source);};afterClose=callback;flyout.Hide();}
    }
    std::function<void()> afterClose;
    void Apply(Object const& m,bool visible,Button const& source,bool publish){
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        if(!visible || !m.GetNamedBoolean(L"enabled",false) || !m.HasKey(L"request")){Close();return;}
        if(opening && m.GetNamedNumber(L"closed",0)==opening){Close();if(m.HasKey(L"session"))return;}
        if(!m.HasKey(L"session")){
            auto fp=m.GetNamedString(L"source");uint32_t session{};winrt::check_hresult(shell_core_permissions_panel_request(core,Id(m,L"request"),reinterpret_cast<uint16_t const*>(fp.c_str()),fp.size(),&session)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);
            if(!session || session==opening)return;Close(false);opening=session;anchor=source;openingLifetime=std::make_shared<int>(0);Object action;action.SetNamedValue(L"command",Value::CreateStringValue(L"permissions-panel:open:"+std::to_wstring(Id(m,L"request"))+L":"+std::to_wstring(session)));dispatch(action,anchor);return;
        }
        if(!publish || !opening || Id(m,L"session")!=opening)return;auto ids=Actions(m);auto fp=m.Stringify();bool changed{};
        winrt::check_hresult(shell_core_permissions_panel_accept(core,opening,Id(m,L"revision"),ids.data(),ids.size(),reinterpret_cast<uint16_t const*>(fp.c_str()),fp.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);if(!changed)return;
        revision=Id(m,L"revision");const bool fresh=!flyout;pending=false;applying=true;const auto alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime);
        if(fresh){
            flyout=Flyout();flyout.ShouldConstrainToRootBounds(false);auto point=source.TransformToVisual(nullptr).TransformPoint({0,0});flyout.Placement(point.X<source.XamlRoot().Size().Width/2?Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft:Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
            StackPanel panel;panel.Width(360);panel.Spacing(12);heading=TextBlock();heading.FontSize(18);heading.FontWeight({600});heading.TextWrapping(TextWrapping::Wrap);Automation::AutomationProperties::SetAutomationId(heading,L"FennPermissions-title");panel.Children().Append(heading);
            content=StackPanel();content.Spacing(12);ScrollViewer scroll;scroll.MaxHeight(480);scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);scroll.Content(content);panel.Children().Append(scroll);
            extras=StackPanel();extras.Spacing(8);panel.Children().Append(extras);reload=TextBlock();reload.TextWrapping(TextWrapping::Wrap);Automation::AutomationProperties::SetAutomationId(reload,L"FennPermissions-reload");Automation::AutomationProperties::SetLiveSetting(reload,Automation::Peers::AutomationLiveSetting::Polite);panel.Children().Append(reload);
            error=TextBlock();error.TextWrapping(TextWrapping::Wrap);Automation::AutomationProperties::SetAutomationId(error,L"FennPermissions-error");Automation::AutomationProperties::SetLiveSetting(error,Automation::Peers::AutomationLiveSetting::Assertive);panel.Children().Append(error);
            close=Button();close.Content(winrt::box_value(m.GetNamedString(L"close")));Automation::AutomationProperties::SetAutomationId(close,L"FennPermissions-close");close.Click([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired())LightDismiss();});panel.Children().Append(close);
            panel.KeyDown([this,alive,current](auto const&,auto const& e){if(alive.expired() || current.expired())return;if(e.Key()==Windows::System::VirtualKey::Escape){for(auto const& [key,row]:rows)if(row.combo.IsDropDownOpen())return;e.Handled(true);LightDismiss();}});
            flyout.Content(panel);flyout.Opened([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired()){if(!rows.empty()){auto& row=rows.begin()->second;if(!row.locked && !row.options.empty())row.combo.Focus(FocusState::Programmatic);else if(row.clearId)row.clear.Focus(FocusState::Programmatic);else close.Focus(FocusState::Programmatic);}else close.Focus(FocusState::Programmatic);}});
            const auto session=opening;flyout.Closed([this,alive,current,session](auto const&,auto const&){if(alive.expired() || current.expired() || opening!=session)return;auto callback=std::move(afterClose);opening=revision=0;shell_core_permissions_panel_dismiss(core);if(callback)anchor.DispatcherQueue().TryEnqueue(std::move(callback));else if(dispatch){Object action;action.SetNamedValue(L"command",Value::CreateStringValue(L"permissions-panel:dismiss:"+std::to_wstring(session)));dispatch(action,anchor);}});
        }
        close.Content(winrt::box_value(m.GetNamedString(L"close")));heading.Text(m.GetNamedString(L"title"));reload.Text(m.GetNamedString(L"reload"));reload.Visibility(reload.Text().empty()?Visibility::Collapsed:Visibility::Visible);error.Text(m.GetNamedString(L"error"));error.Visibility(error.Text().empty()?Visibility::Collapsed:Visibility::Visible);
        std::vector<winrt::hstring> next;for(auto v:m.GetNamedArray(L"rows"))next.push_back(v.GetObject().GetNamedString(L"key"));const bool structure=fresh || order!=next;
        for(auto it=rows.begin();it!=rows.end();)if(std::find(next.begin(),next.end(),it->first)==next.end())it=rows.erase(it);else ++it;
        if(structure)content.Children().Clear();order=next;std::set<winrt::hstring> groups;
        for(auto v:m.GetNamedArray(L"rows")){auto model=v.GetObject();auto key=model.GetNamedString(L"key");auto found=rows.find(key);if(found==rows.end()){
                Row row;row.host=Grid();ColumnDefinition iconColumn;iconColumn.Width({24,GridUnitType::Pixel});row.host.ColumnDefinitions().Append(iconColumn);ColumnDefinition textColumn;textColumn.Width({1,GridUnitType::Star});row.host.ColumnDefinitions().Append(textColumn);ColumnDefinition controlColumn;controlColumn.Width({1,GridUnitType::Auto});row.host.ColumnDefinitions().Append(controlColumn);row.host.ColumnSpacing(8);Automation::AutomationProperties::SetAutomationId(row.host,L"FennPermissions-"+key);
                StackPanel glyphs;glyphs.Spacing(4);row.icon=FontIcon();row.icon.FontSize(16);row.badge=FontIcon();row.badge.FontSize(10);glyphs.Children().Append(row.icon);glyphs.Children().Append(row.badge);Automation::AutomationProperties::SetAccessibilityView(glyphs,Automation::Peers::AccessibilityView::Raw);row.host.Children().Append(glyphs);
                row.text=StackPanel();row.text.Spacing(4);row.label=TextBlock();row.label.TextWrapping(TextWrapping::Wrap);row.state=TextBlock();row.state.TextWrapping(TextWrapping::Wrap);row.detail=TextBlock();row.detail.TextWrapping(TextWrapping::Wrap);row.text.Children().Append(row.label);row.text.Children().Append(row.state);row.text.Children().Append(row.detail);Grid::SetColumn(row.text,1);row.host.Children().Append(row.text);
                row.clear=Button();FontIcon x;x.Glyph(L"\xE711");x.FontSize(12);row.clear.Content(x);row.clear.MinWidth(32);Grid::SetColumn(row.clear,2);row.host.Children().Append(row.clear);
                row.clear.Click([this,alive,current,key](auto const&,auto const&){if(!alive.expired() && !current.expired()){auto row=rows.find(key);if(row!=rows.end())Send(row->second.clearId);}});
                row.combo=Combo();row.combo.MaxWidth(190);row.combo.MinWidth(120);Grid::SetColumn(row.combo,2);row.host.Children().Append(row.combo);
                row.combo.SelectionChanged([this,alive,current,key](auto const&,auto const&){if(alive.expired() || current.expired() || applying)return;auto row=rows.find(key);if(row==rows.end())return;auto index=row->second.combo.SelectedIndex();if(index>=0 && size_t(index)<row->second.options.size())Send(row->second.options[index]);});
                found=rows.emplace(key,std::move(row)).first;
            }
            auto& row=found->second;row.label.Text(model.GetNamedString(L"label"));row.state.Text(model.GetNamedString(L"state"));auto detail=model.GetNamedString(L"detail"),usage=model.GetNamedString(L"usage");if(model.GetNamedBoolean(L"locked"))detail=detail.empty()?m.GetNamedString(L"managed"):detail+L"\n"+m.GetNamedString(L"managed");row.detail.Text(usage.empty()?detail:detail.empty()?usage:usage+L"\n"+detail);row.detail.Visibility(row.detail.Text().empty()?Visibility::Collapsed:Visibility::Visible);row.icon.Glyph(OmnibarIcons::Glyph(model.GetNamedString(L"icon")));auto state=model.GetNamedString(L"iconState");row.badge.Glyph(state==L"blocked"?L"\xE733":state==L"paused"?L"\xE769":L"\xE73E");row.badge.Visibility(state==L"normal"?Visibility::Collapsed:Visibility::Visible);
            row.clearId=uint32_t(model.GetNamedNumber(L"clear"));row.locked=model.GetNamedBoolean(L"locked");row.clear.Visibility(row.clearId?Visibility::Visible:Visibility::Collapsed);Automation::AutomationProperties::SetAutomationId(row.clear,L"FennPermissions-clear-"+std::to_wstring(row.clearId));Automation::AutomationProperties::SetName(row.clear,m.GetNamedString(L"remove")+L": "+model.GetNamedString(L"label"));ToolTipService::SetToolTip(row.clear,winrt::box_value(m.GetNamedString(L"remove")));Automation::AutomationProperties::SetHelpText(row.host,row.locked?m.GetNamedString(L"managed"):L"");
            std::vector<uint32_t> options;std::vector<winrt::hstring> labels;for(auto option:model.GetNamedArray(L"options")){auto o=option.GetObject();options.push_back(Id(o,L"id"));labels.push_back(o.GetNamedString(L"label"));}
            if(options!=row.options || labels!=row.optionLabels){row.options=options;row.optionLabels=labels;row.combo.Items().Clear();for(auto const& label:labels)row.combo.Items().Append(winrt::box_value(label));}
            auto selected=uint32_t(model.GetNamedNumber(L"selected"));auto index=std::find(options.begin(),options.end(),selected);row.combo.SelectedIndex(index==options.end()?-1:int32_t(index-options.begin()));row.combo.Visibility(options.empty()?Visibility::Collapsed:Visibility::Visible);row.state.Visibility(options.empty()?Visibility::Visible:Visibility::Collapsed);Automation::AutomationProperties::SetAutomationId(row.combo,L"FennPermissions-choice-"+key);Automation::AutomationProperties::SetName(row.combo,model.GetNamedString(L"label"));Automation::AutomationProperties::SetHelpText(row.combo,row.locked?m.GetNamedString(L"managed"):L"");
            if(structure){auto group=model.GetNamedString(L"group");if(!group.empty() && groups.insert(group).second){StackPanel section;TextBlock title;title.FontWeight({600});title.Text(m.GetNamedString(group==L"storage"?L"storageTitle":L"protocolTitle"));section.Children().Append(title);if(group==L"storage"){TextBlock hint;hint.Text(m.GetNamedString(L"storageHint"));hint.TextWrapping(TextWrapping::Wrap);section.Children().Append(hint);}content.Children().Append(section);}content.Children().Append(row.host);}
        }
        if(structure && order.empty()){TextBlock empty;empty.Text(m.GetNamedString(L"empty"));empty.TextWrapping(TextWrapping::Wrap);content.Children().Append(empty);}
        extras.Children().Clear();for(auto v:m.GetNamedArray(L"actions")){auto a=v.GetObject();auto id=Id(a,L"id");Button action;action.Content(winrt::box_value(a.GetNamedString(L"label")));Automation::AutomationProperties::SetAutomationId(action,L"FennPermissions-action-"+std::to_wstring(id));action.Click([this,alive,current,id](auto const&,auto const&){if(!alive.expired() && !current.expired())Send(id);});extras.Children().Append(action);}
        Enable(true);applying=false;if(fresh){
            // The indicator may disappear when its permission is cleared. Keep
            // the supported placement target rooted for the whole Flyout lifetime.
            auto target=source.XamlRoot().Content().as<FrameworkElement>();
            auto bounds=source.TransformToVisual(target).TransformBounds({0,0,float(source.ActualWidth()),float(source.ActualHeight())});
            Primitives::FlyoutShowOptions options;options.Placement(flyout.Placement());
            options.Position(winrt::Windows::Foundation::Point{bounds.X+(flyout.Placement()==Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight?bounds.Width:0),bounds.Y+bounds.Height});
            flyout.ShowAt(target,options);
        }else if(focusAfterAck){focusAfterAck=false;auto focused=rows.find(focusKey);if(focused!=rows.end() && !focused->second.locked){if(!focused->second.options.empty())focused->second.combo.Focus(FocusState::Programmatic);else if(focused->second.clearId)focused->second.clear.Focus(FocusState::Programmatic);else close.Focus(FocusState::Programmatic);}else close.Focus(FocusState::Programmatic);}
    }
};
