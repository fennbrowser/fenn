#pragma once
#include "JsonValidation.h"
#include "OmnibarIcons.h"
#include "shell_core.h"

// WinUI objects only: security classification and commands stay in Firefox.
struct NativeConnectionPanel {
    using Object=winrt::Windows::Data::Json::JsonObject;
    using Value=winrt::Windows::Data::Json::JsonValue;
    using Button=Microsoft::UI::Xaml::Controls::Button;
    ShellCore* core{};uint32_t opening{},revision{};bool applying{},pending{};
    Button anchor{nullptr},close{nullptr};
    Microsoft::UI::Xaml::Controls::Flyout flyout{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock heading{nullptr},summary{nullptr},choiceLabel{nullptr},error{nullptr};
    Microsoft::UI::Xaml::Controls::FontIcon icon{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel details{nullptr},extras{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox choice{nullptr};
    Microsoft::UI::Xaml::Controls::Image trustMark{nullptr};winrt::hstring markPixels;
    std::vector<uint32_t> options;std::vector<winrt::hstring> optionLabels,paragraphs;
    std::map<uint32_t,bool> retained;std::map<uint32_t,Button> actionButtons;
    std::shared_ptr<int> lifetime=std::make_shared<int>(0),openingLifetime;
    std::function<void(Object const&,Button const&)> dispatch;
    std::function<void()> afterClose;
    ~NativeConnectionPanel(){lifetime.reset();Close();}
    static uint32_t Id(Object const& m,wchar_t const* key){auto value=NativeJson::RequireRevision(m,key);if(!value || value>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);return uint32_t(value);}
    static Object Read(Object const& snapshot){
        auto m=snapshot.GetNamedObject(L"navigation",Object()).GetNamedObject(L"connectionPanel",Object());m.GetNamedBoolean(L"enabled",false);
        if(m.HasKey(L"closed") && NativeJson::RequireRevision(m,L"closed")>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);
        if(!m.HasKey(L"request"))return m;Id(m,L"request");auto source=m.GetNamedString(L"source");if(source.empty() || source.size()>64)winrt::throw_hresult(E_INVALIDARG);
        if(!m.HasKey(L"session"))return m;Id(m,L"session");Id(m,L"revision");
        for(auto key:{L"title",L"summary",L"choiceLabel",L"error",L"close"})NativeJson::RequireLength(m.GetNamedString(key),2048);
        if(m.GetNamedString(L"title").empty() || m.GetNamedString(L"summary").empty() || m.GetNamedString(L"close").empty())winrt::throw_hresult(E_INVALIDARG);
        auto kind=m.GetNamedString(L"icon");if(kind!=L"connection" && kind!=L"warning")winrt::throw_hresult(E_INVALIDARG);
        auto blocks=m.GetNamedArray(L"blocks");if(blocks.Size()>20)winrt::throw_hresult(E_INVALIDARG);for(auto v:blocks)NativeJson::RequireLength(v.GetString(),2048);
        std::set<uint32_t> ids;auto choices=m.GetNamedArray(L"options");if(choices.Size()>3)winrt::throw_hresult(E_INVALIDARG);
        const auto selected=NativeJson::RequireRevision(m,L"selected");bool found=choices.Size()==0 && selected==0;
        for(auto v:choices){auto option=v.GetObject();auto id=Id(option,L"id");if(id<100 || id>102 || !ids.insert(id).second)winrt::throw_hresult(E_INVALIDARG);option.GetNamedBoolean(L"retained");NativeJson::RequireLength(option.GetNamedString(L"label"),512);if(id==selected)found=true;}if(!found)winrt::throw_hresult(E_INVALIDARG);
        auto actions=m.GetNamedArray(L"actions");if(actions.Size()>6)winrt::throw_hresult(E_INVALIDARG);for(auto v:actions){auto a=v.GetObject();auto id=Id(a,L"id");if(id<3 || id>8 || !ids.insert(id).second || a.GetNamedBoolean(L"retained"))winrt::throw_hresult(E_INVALIDARG);NativeJson::RequireLength(a.GetNamedString(L"label"),2048);}
        size_t pixels{};NativeJson::ValidateImage(m,pixels,{95,6080});if(pixels && (m.GetNamedObject(L"image").GetNamedNumber(L"width")!=64 || m.GetNamedObject(L"image").GetNamedNumber(L"height")!=95))winrt::throw_hresult(E_INVALIDARG);
        if(m.Stringify().size()>65536)winrt::throw_hresult(E_INVALIDARG);return m;
    }
    static std::vector<uint32_t> Actions(Object const& m){std::vector<uint32_t> ids;for(auto v:m.GetNamedArray(L"options"))ids.push_back(Id(v.GetObject(),L"id"));for(auto v:m.GetNamedArray(L"actions"))ids.push_back(Id(v.GetObject(),L"id"));return ids;}
    bool Resolve(Object const& m){if(!m.HasKey(L"session"))return false;auto ids=Actions(m);auto fp=m.Stringify();bool changed{};winrt::check_hresult(shell_core_connection_panel_resolve(core,Id(m,L"session"),Id(m,L"revision"),ids.data(),ids.size(),reinterpret_cast<uint16_t const*>(fp.c_str()),fp.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);return changed;}
    void LightDismiss(){if(flyout)flyout.Hide();}
    void Close(bool cancel=true){opening=revision=0;openingLifetime.reset();afterClose={};pending=false;options.clear();optionLabels.clear();paragraphs.clear();retained.clear();actionButtons.clear();if(cancel && core)shell_core_connection_panel_dismiss(core);if(flyout)flyout.Hide();flyout=nullptr;anchor=close=nullptr;heading=summary=choiceLabel=error=nullptr;icon=nullptr;details=extras=nullptr;choice=nullptr;trustMark=nullptr;markPixels=L"";}
    void Enable(bool value){choice.IsEnabled(value);for(auto child:extras.Children())if(auto b=child.try_as<Button>())b.IsEnabled(value);}
    void Send(uint32_t id){
        if(!opening || applying || pending || !dispatch || !retained.count(id))return;bool accepted{};bool keep=retained.at(id);
        if(shell_core_connection_panel_activate(core,opening,revision,id,keep,&accepted)!=SHELL_STATUS_OK || !accepted)return;
        Object action;action.SetNamedValue(L"command",Value::CreateStringValue(L"connection-panel:invoke:"+std::to_wstring(opening)+L":"+std::to_wstring(revision)+L":"+std::to_wstring(id)));
        if(keep){pending=true;Enable(false);dispatch(action,anchor);}
        else {auto source=anchor;auto send=dispatch;auto alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime);afterClose=[action,source,send,alive,current]{if(!alive.expired() && !current.expired())send(action,source);};flyout.Hide();}
    }
    void Apply(Object const& m,bool visible,Button const& source,bool publish){
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        if(!visible || !m.GetNamedBoolean(L"enabled",false) || !m.HasKey(L"request")){Close();return;}
        if(opening && m.GetNamedNumber(L"closed",0)==opening){Close();if(m.HasKey(L"session"))return;}
        if(!m.HasKey(L"session")){auto fp=m.GetNamedString(L"source");uint32_t session{};winrt::check_hresult(shell_core_connection_panel_request(core,Id(m,L"request"),reinterpret_cast<uint16_t const*>(fp.c_str()),fp.size(),&session)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);if(!session || session==opening)return;Close(false);opening=session;anchor=source;openingLifetime=std::make_shared<int>(0);Object action;action.SetNamedValue(L"command",Value::CreateStringValue(L"connection-panel:open:"+std::to_wstring(Id(m,L"request"))+L":"+std::to_wstring(session)));dispatch(action,anchor);return;}
        if(!publish || !opening || Id(m,L"session")!=opening)return;auto ids=Actions(m);auto fp=m.Stringify();bool changed{};
        winrt::check_hresult(shell_core_connection_panel_accept(core,opening,Id(m,L"revision"),ids.data(),ids.size(),reinterpret_cast<uint16_t const*>(fp.c_str()),fp.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);if(!changed)return;
        const bool fresh=!flyout,ack=pending;revision=Id(m,L"revision");pending=false;applying=true;auto alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime);
        if(fresh){
            flyout=Flyout();flyout.ShouldConstrainToRootBounds(false);auto point=source.TransformToVisual(nullptr).TransformPoint({0,0});flyout.Placement(point.X<source.XamlRoot().Size().Width/2?Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft:Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
            StackPanel panel;panel.Width(360);panel.Spacing(12);heading=TextBlock();heading.FontSize(18);heading.FontWeight({600});heading.TextWrapping(TextWrapping::Wrap);Automation::AutomationProperties::SetAutomationId(heading,L"FennConnection-title");panel.Children().Append(heading);
            Grid status;ColumnDefinition glyph;glyph.Width({28,GridUnitType::Pixel});status.ColumnDefinitions().Append(glyph);ColumnDefinition text;text.Width({1,GridUnitType::Star});status.ColumnDefinitions().Append(text);icon=FontIcon();icon.FontSize(18);Automation::AutomationProperties::SetAccessibilityView(icon,Automation::Peers::AccessibilityView::Raw);status.Children().Append(icon);summary=TextBlock();summary.TextWrapping(TextWrapping::Wrap);summary.FontWeight({600});Grid::SetColumn(summary,1);status.Children().Append(summary);Automation::AutomationProperties::SetAutomationId(summary,L"FennConnection-summary");Automation::AutomationProperties::SetLiveSetting(summary,Automation::Peers::AutomationLiveSetting::Polite);panel.Children().Append(status);
            StackPanel body;body.Spacing(12);trustMark=Image();trustMark.Width(64);trustMark.Height(95);trustMark.HorizontalAlignment(HorizontalAlignment::Left);Automation::AutomationProperties::SetAccessibilityView(trustMark,Automation::Peers::AccessibilityView::Raw);body.Children().Append(trustMark);
            details=StackPanel();details.Spacing(10);body.Children().Append(details);ScrollViewer scroll;scroll.MaxHeight(380);scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);scroll.Content(body);panel.Children().Append(scroll);
            choiceLabel=TextBlock();choiceLabel.TextWrapping(TextWrapping::Wrap);panel.Children().Append(choiceLabel);choice=ComboBox();choice.HorizontalAlignment(HorizontalAlignment::Stretch);Automation::AutomationProperties::SetAutomationId(choice,L"FennConnection-choice");choice.SelectionChanged([this,alive,current](auto const&,auto const&){if(alive.expired() || current.expired() || applying)return;const auto i=choice.SelectedIndex();if(i>=0 && size_t(i)<options.size())Send(options[i]);});panel.Children().Append(choice);
            extras=StackPanel();extras.Spacing(8);panel.Children().Append(extras);error=TextBlock();error.TextWrapping(TextWrapping::Wrap);Automation::AutomationProperties::SetAutomationId(error,L"FennConnection-error");Automation::AutomationProperties::SetLiveSetting(error,Automation::Peers::AutomationLiveSetting::Assertive);panel.Children().Append(error);
            close=Button();Automation::AutomationProperties::SetAutomationId(close,L"FennConnection-close");close.Click([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired())LightDismiss();});panel.Children().Append(close);
            panel.KeyDown([this,alive,current](auto const&,auto const& e){if(!alive.expired() && !current.expired() && e.Key()==Windows::System::VirtualKey::Escape && !choice.IsDropDownOpen()){e.Handled(true);LightDismiss();}});
            flyout.Content(panel);flyout.Opened([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired()){if(options.empty())close.Focus(FocusState::Programmatic);else choice.Focus(FocusState::Programmatic);}});
            const auto session=opening;flyout.Closed([this,alive,current,session](auto const&,auto const&){if(alive.expired() || current.expired() || opening!=session)return;auto callback=std::move(afterClose);opening=revision=0;shell_core_connection_panel_dismiss(core);if(callback)anchor.DispatcherQueue().TryEnqueue(std::move(callback));else if(dispatch){Object action;action.SetNamedValue(L"command",Value::CreateStringValue(L"connection-panel:dismiss:"+std::to_wstring(session)));dispatch(action,anchor);}});
        }
        heading.Text(m.GetNamedString(L"title"));summary.Text(m.GetNamedString(L"summary"));icon.Glyph(OmnibarIcons::Glyph(m.GetNamedString(L"icon")));close.Content(winrt::box_value(m.GetNamedString(L"close")));error.Text(m.GetNamedString(L"error"));error.Visibility(error.Text().empty()?Visibility::Collapsed:Visibility::Visible);
        auto image=m.GetNamedValue(L"image",Value::CreateNullValue());if(image.ValueType()==winrt::Windows::Data::Json::JsonValueType::Object){auto data=image.GetObject();auto pixels=data.GetNamedString(L"pixels");if(markPixels!=pixels){markPixels=pixels;trustMark.Source(NativeJson::DecodeImage(data));}trustMark.Visibility(Visibility::Visible);}else{trustMark.Source(nullptr);markPixels=L"";trustMark.Visibility(Visibility::Collapsed);}
        std::vector<winrt::hstring> next;for(auto v:m.GetNamedArray(L"blocks"))next.push_back(v.GetString());if(fresh || next!=paragraphs){paragraphs=next;details.Children().Clear();for(size_t i=0;i<paragraphs.size();i++){TextBlock block;block.Text(paragraphs[i]);block.TextWrapping(TextWrapping::Wrap);Automation::AutomationProperties::SetAutomationId(block,L"FennConnection-detail-"+std::to_wstring(i));details.Children().Append(block);}}
        std::vector<uint32_t> nextOptions;std::vector<winrt::hstring> labels;retained.clear();for(auto v:m.GetNamedArray(L"options")){auto option=v.GetObject();auto id=Id(option,L"id");nextOptions.push_back(id);labels.push_back(option.GetNamedString(L"label"));retained[id]=option.GetNamedBoolean(L"retained");}
        if(nextOptions!=options || labels!=optionLabels){options=nextOptions;optionLabels=labels;choice.Items().Clear();for(auto const& label:labels)choice.Items().Append(winrt::box_value(label));}
        auto selected=uint32_t(m.GetNamedNumber(L"selected"));auto index=std::find(options.begin(),options.end(),selected);choice.SelectedIndex(index==options.end()?-1:int32_t(index-options.begin()));choice.Visibility(options.empty()?Visibility::Collapsed:Visibility::Visible);choiceLabel.Text(m.GetNamedString(L"choiceLabel"));choiceLabel.Visibility(choice.Visibility());Automation::AutomationProperties::SetName(choice,choiceLabel.Text());
        std::set<uint32_t> nextActions;for(auto v:m.GetNamedArray(L"actions"))nextActions.insert(Id(v.GetObject(),L"id"));
        for(auto it=actionButtons.begin();it!=actionButtons.end();)if(!nextActions.count(it->first)){uint32_t index{};if(extras.Children().IndexOf(it->second,index))extras.Children().RemoveAt(index);it=actionButtons.erase(it);}else ++it;
        uint32_t position{};for(auto v:m.GetNamedArray(L"actions")){auto a=v.GetObject();auto id=Id(a,L"id");retained[id]=false;auto found=actionButtons.find(id);if(found==actionButtons.end()){Button button;Automation::AutomationProperties::SetAutomationId(button,L"FennConnection-action-"+std::to_wstring(id));button.Click([this,alive,current,id](auto const&,auto const&){if(!alive.expired() && !current.expired())Send(id);});found=actionButtons.emplace(id,button).first;}auto button=found->second;button.Content(winrt::box_value(a.GetNamedString(L"label")));uint32_t currentIndex{};if(!extras.Children().IndexOf(button,currentIndex))extras.Children().InsertAt(position,button);else if(currentIndex!=position){extras.Children().RemoveAt(currentIndex);extras.Children().InsertAt(position,button);}position++;}
        Enable(true);applying=false;
        if(fresh){auto target=source.XamlRoot().Content().as<FrameworkElement>();auto bounds=source.TransformToVisual(target).TransformBounds({0,0,float(source.ActualWidth()),float(source.ActualHeight())});Primitives::FlyoutShowOptions placement;placement.Placement(flyout.Placement());placement.Position(winrt::Windows::Foundation::Point{bounds.X+(flyout.Placement()==Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight?bounds.Width:0),bounds.Y+bounds.Height});flyout.ShowAt(target,placement);}else if(ack)choice.Focus(FocusState::Programmatic);
    }
};
