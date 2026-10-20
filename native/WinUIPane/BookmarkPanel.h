#pragma once
#include "JsonValidation.h"
#include "shell_core.h"

// Actual WinUI editing controls. Firefox owns bookmark drafts and transactions;
// Rust owns request replay, opening identity and action freshness.
struct NativeBookmarkPanel {
    using Object=winrt::Windows::Data::Json::JsonObject;
    using Value=winrt::Windows::Data::Json::JsonValue;
    using Button=Microsoft::UI::Xaml::Controls::Button;
    ShellCore* core{};uint32_t opening{},revision{};bool popupOpen{},applying{},pendingEdit{},pendingChoice{},interacted{},composing{};
    winrt::hstring observedName,observedTags;bool observedShowEditor{};
    uint64_t editEpoch{},sentEpoch{};uint32_t pendingTerminal{},tagSelection{};
    Button anchor{nullptr},save{nullptr},remove{nullptr},createFolder{nullptr};
    Microsoft::UI::Xaml::Controls::Flyout flyout{nullptr};
    Microsoft::UI::Xaml::Controls::TextBox name{nullptr},folderName{nullptr};
    Microsoft::UI::Xaml::Controls::AutoSuggestBox tags{nullptr};
    Microsoft::UI::Xaml::Controls::CheckBox showEditor{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock location{nullptr},error{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel choices{nullptr},tagChoices{nullptr};
    std::map<uint32_t,Microsoft::UI::Xaml::Controls::CheckBox> tagChecks;
    std::vector<uint32_t> tagOrder;
    std::vector<winrt::hstring> lastSuggestions;
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    std::function<void(Object const&,Button const&)> dispatch;
    std::function<void()> afterClose;
    std::shared_ptr<int> lifetime=std::make_shared<int>(0),openingLifetime;
    ~NativeBookmarkPanel(){lifetime.reset();Close();}
    static uint32_t Id(Object const& m,wchar_t const* key){auto n=NativeJson::RequireRevision(m,key);if(!n || n>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);return uint32_t(n);}
    static Object Read(Object const& snapshot){
        auto m=snapshot.GetNamedObject(L"navigation",Object()).GetNamedObject(L"bookmarkPanel",Object());m.GetNamedBoolean(L"enabled",false);
        if(m.HasKey(L"closed") && NativeJson::RequireRevision(m,L"closed")>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);
        if(!m.HasKey(L"request"))return m;Id(m,L"request");auto source=m.GetNamedString(L"source");if(source.empty() || source.size()>64)winrt::throw_hresult(E_INVALIDARG);
        if(!m.HasKey(L"session"))return m;Id(m,L"session");Id(m,L"revision");
        for(auto key:{L"name",L"tags"})NativeJson::RequireLength(m.GetNamedString(key),4096);
        NativeJson::RequireLength(m.GetNamedString(L"folder"),512);NativeJson::RequireLength(m.GetNamedString(L"error"),2048);
        for(auto key:{L"isNew",L"tagsVisible",L"showEditor",L"previous",L"next",L"parent"})m.GetNamedBoolean(key);m.GetNamedBoolean(L"focusTags");auto timeout=m.GetNamedNumber(L"autoClose");if(!std::isfinite(timeout) || timeout<1 || timeout>60000)winrt::throw_hresult(E_INVALIDARG);
        auto labels=m.GetNamedObject(L"labels");for(auto key:{L"title",L"name",L"folder",L"tags",L"tagsHint",L"tagsHelp",L"tagsAll",L"showEditor",L"save",L"remove",L"newFolder",L"parent",L"previous",L"next",L"error"}){auto text=labels.GetNamedString(key);if(text.empty())winrt::throw_hresult(E_INVALIDARG);NativeJson::RequireLength(text,2048);}
        std::set<uint32_t> ids;for(auto key:{L"roots",L"folders"}){auto entries=m.GetNamedArray(key);if(entries.Size()>(wcscmp(key,L"roots")==0?32u:64u))winrt::throw_hresult(E_INVALIDARG);for(auto v:entries){auto item=v.GetObject();auto id=Id(item,L"id");if(id<100 || !ids.insert(id).second)winrt::throw_hresult(E_INVALIDARG);NativeJson::RequireLength(item.GetNamedString(L"label"),512);}}
        m.GetNamedBoolean(L"tagPrevious");m.GetNamedBoolean(L"tagNext");if(NativeJson::RequireRevision(m,L"tagSelection")>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);
        auto tagOptions=m.GetNamedArray(L"tagOptions");if(tagOptions.Size()>32)winrt::throw_hresult(E_INVALIDARG);for(auto v:tagOptions){auto item=v.GetObject();auto id=Id(item,L"id");if(id<100 || !ids.insert(id).second)winrt::throw_hresult(E_INVALIDARG);NativeJson::RequireLength(item.GetNamedString(L"label"),512);item.GetNamedBoolean(L"checked");}
        auto suggestions=m.GetNamedArray(L"suggestions");if(suggestions.Size()>64)winrt::throw_hresult(E_INVALIDARG);for(auto v:suggestions)NativeJson::RequireLength(v.GetString(),4096);
        if(m.Stringify().size()>65536)winrt::throw_hresult(E_INVALIDARG);return m;
    }
    static std::vector<uint32_t> Actions(Object const& m){std::vector<uint32_t> ids{1,2,6,7};for(auto pair:{std::pair{L"parent",3u},std::pair{L"previous",4u},std::pair{L"next",5u}})if(m.GetNamedBoolean(pair.first))ids.push_back(pair.second);for(auto key:{L"roots",L"folders"})for(auto v:m.GetNamedArray(key))ids.push_back(Id(v.GetObject(),L"id"));if(m.GetNamedBoolean(L"tagsVisible")){ids.push_back(8);if(m.GetNamedBoolean(L"tagPrevious"))ids.push_back(9);if(m.GetNamedBoolean(L"tagNext"))ids.push_back(10);for(auto v:m.GetNamedArray(L"tagOptions"))ids.push_back(Id(v.GetObject(),L"id"));}return ids;}
    bool Resolve(Object const& m){if(!m.HasKey(L"session"))return false;auto ids=Actions(m);auto fp=m.Stringify();bool changed{};winrt::check_hresult(shell_core_bookmark_panel_resolve(core,Id(m,L"session"),Id(m,L"revision"),ids.data(),ids.size(),reinterpret_cast<uint16_t const*>(fp.c_str()),fp.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);return changed;}
    Object Draft(){Object value;value.SetNamedValue(L"name",Value::CreateStringValue(name.Text()));value.SetNamedValue(L"tags",Value::CreateStringValue(tags.Text()));value.SetNamedValue(L"showEditor",Value::CreateBooleanValue(showEditor.IsChecked().GetBoolean()));value.SetNamedValue(L"folderName",Value::CreateStringValue(folderName.Text()));return value;}
    void EnableChoices(bool value){if(choices)for(auto child:choices.Children())if(auto c=child.try_as<Microsoft::UI::Xaml::Controls::Control>())c.IsEnabled(value);if(createFolder)createFolder.IsEnabled(value);if(tagChoices)for(auto child:tagChoices.Children())if(auto c=child.try_as<Microsoft::UI::Xaml::Controls::Control>())c.IsEnabled(value);}
    void LightDismiss(){if(flyout && popupOpen)flyout.Hide();}
    void StopTimer(){if(timer)timer.Stop();}
    void Interact(){interacted=true;StopTimer();}
    void Close(bool cancel=true){opening=revision=0;openingLifetime.reset();afterClose={};popupOpen=false;StopTimer();timer=nullptr;if(cancel && core)shell_core_bookmark_panel_dismiss(core);if(flyout)flyout.Hide();flyout=nullptr;anchor=save=remove=createFolder=nullptr;name=folderName=nullptr;tags=nullptr;showEditor=nullptr;location=error=nullptr;choices=tagChoices=nullptr;tagChecks.clear();tagOrder.clear();lastSuggestions.clear();tagSelection=0;pendingEdit=pendingChoice=false;pendingTerminal=0;editEpoch=sentEpoch=0;interacted=composing=false;}
    void Send(uint32_t id,bool escape=false){
        if(!opening || applying || !dispatch || composing)return;
        if(pendingChoice || (pendingEdit && id!=7)){if(id==1 || id==2)pendingTerminal=escape?3:id;return;}
        const bool retained=id>=3;bool accepted{};
        if(shell_core_bookmark_panel_activate(core,opening,revision,escape?2:id,retained,&accepted)!=SHELL_STATUS_OK || !accepted)return;
        Object action;action.SetNamedValue(L"command",Value::CreateStringValue(winrt::hstring(escape?L"bookmark-panel:escape:":L"bookmark-panel:invoke:")+std::to_wstring(opening)+L":"+std::to_wstring(revision)+L":"+std::to_wstring(escape?2:id)));
        action.SetNamedValue(L"bookmark",Draft());action.SetNamedValue(L"modifiers",Value::CreateNumberValue(0));
        if(retained){pendingEdit=id==7;pendingChoice=id!=7;sentEpoch=editEpoch;save.IsEnabled(false);remove.IsEnabled(false);EnableChoices(false);dispatch(action,anchor);}
        else {auto source=anchor;auto send=dispatch;opening=revision=0;StopTimer();auto alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime);auto task=[send,action,source,alive,current]{if(!alive.expired() && !current.expired())send(action,source);};if(popupOpen){afterClose=task;flyout.Hide();}else source.DispatcherQueue().TryEnqueue(task);}
    }
    void Changed(){if(applying || !opening)return;const auto n=name.Text(),t=tags.Text();const bool show=showEditor.IsChecked().GetBoolean();if(n==observedName && t==observedTags && show==observedShowEditor)return;observedName=n;observedTags=t;observedShowEditor=show;Interact();++editEpoch;Send(7);}
    void Apply(Object const& m,bool visible,Button const& source,bool publish){
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        if(!visible || !m.GetNamedBoolean(L"enabled",false) || !m.HasKey(L"request")){Close();return;}
        if(opening && m.GetNamedNumber(L"closed",0)==opening){Close();if(m.HasKey(L"session"))return;}
        if(!m.HasKey(L"session")){
            auto fp=m.GetNamedString(L"source");uint32_t session{};
            winrt::check_hresult(shell_core_bookmark_panel_request(core,Id(m,L"request"),reinterpret_cast<uint16_t const*>(fp.c_str()),fp.size(),&session)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);
            if(!session || session==opening)return;Close(false);
            opening=session;anchor=source;openingLifetime=std::make_shared<int>(0);
            Object action;action.SetNamedValue(L"command",Value::CreateStringValue(L"bookmark-panel:open:"+std::to_wstring(Id(m,L"request"))+L":"+std::to_wstring(session)));action.SetNamedValue(L"modifiers",Value::CreateNumberValue(0));dispatch(action,anchor);return;
        }
        if(!publish || !opening || Id(m,L"session")!=opening)return;
        auto ids=Actions(m);auto fp=m.Stringify();bool changed{};
        winrt::check_hresult(shell_core_bookmark_panel_accept(core,opening,Id(m,L"revision"),ids.data(),ids.size(),reinterpret_cast<uint16_t const*>(fp.c_str()),fp.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);if(!changed)return;
        const bool fresh=!flyout;const bool folderAck=pendingChoice;pendingEdit=pendingChoice=false;revision=Id(m,L"revision");applying=true;
        const auto alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime);auto labels=m.GetNamedObject(L"labels");
        if(fresh){
            flyout=Flyout();flyout.ShouldConstrainToRootBounds(false);flyout.Placement(Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
            StackPanel frame;frame.Width(340);frame.Spacing(12);StackPanel panel;panel.Width(340);panel.Spacing(12);TextBlock heading;heading.Text(labels.GetNamedString(L"title"));heading.FontSize(18);heading.FontWeight({600});panel.Children().Append(heading);
            name=TextBox();name.Header(winrt::box_value(labels.GetNamedString(L"name")));name.MaxLength(4096);name.Text(m.GetNamedString(L"name"));Automation::AutomationProperties::SetAutomationId(name,L"FennBookmark-name");panel.Children().Append(name);
            location=TextBlock();location.TextWrapping(TextWrapping::Wrap);location.MaxLines(2);location.TextTrimming(TextTrimming::CharacterEllipsis);Automation::AutomationProperties::SetAutomationId(location,L"FennBookmark-location");
            Expander folders;folders.Header(location);folders.HorizontalAlignment(HorizontalAlignment::Stretch);folders.HorizontalContentAlignment(HorizontalAlignment::Stretch);Automation::AutomationProperties::SetAutomationId(folders,L"FennBookmark-folders");
            StackPanel folderContent;folderContent.Spacing(8);choices=StackPanel();choices.Spacing(4);folderContent.Children().Append(choices);
            folderName=TextBox();folderName.Header(winrt::box_value(labels.GetNamedString(L"newFolder")));folderName.MaxLength(512);folderName.Text(labels.GetNamedString(L"newFolder"));Automation::AutomationProperties::SetAutomationId(folderName,L"FennBookmark-folder-name");folderContent.Children().Append(folderName);
            Button create;create.Content(winrt::box_value(labels.GetNamedString(L"newFolder")));Automation::AutomationProperties::SetAutomationId(create,L"FennBookmark-new-folder");create.Click([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired()){Interact();Send(6);}});createFolder=create;folderContent.Children().Append(create);folders.Content(folderContent);panel.Children().Append(folders);
            tags=AutoSuggestBox();tags.TextBoxStyle(Markup::XamlReader::Load(LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="TextBox"><Setter Property="MaxLength" Value="4096"/></Style>)").as<Style>());tags.Header(winrt::box_value(labels.GetNamedString(L"tags")));tags.PlaceholderText(labels.GetNamedString(L"tagsHint"));tags.Text(m.GetNamedString(L"tags"));tags.MaxSuggestionListHeight(240);tags.Visibility(m.GetNamedBoolean(L"tagsVisible")?Visibility::Visible:Visibility::Collapsed);Automation::AutomationProperties::SetAutomationId(tags,L"FennBookmark-tags");Automation::AutomationProperties::SetHelpText(tags,labels.GetNamedString(L"tagsHelp"));panel.Children().Append(tags);
            Expander tagPicker;tagPicker.Header(winrt::box_value(labels.GetNamedString(L"tagsAll")));tagPicker.Visibility(m.GetNamedBoolean(L"tagsVisible")?Visibility::Visible:Visibility::Collapsed);tagPicker.HorizontalAlignment(HorizontalAlignment::Stretch);tagPicker.HorizontalContentAlignment(HorizontalAlignment::Stretch);Automation::AutomationProperties::SetAutomationId(tagPicker,L"FennBookmark-tags-list");tagChoices=StackPanel();tagChoices.Spacing(4);tagPicker.Content(tagChoices);tagPicker.Expanding([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired()){Interact();Send(8);}});panel.Children().Append(tagPicker);
            showEditor=CheckBox();showEditor.Content(winrt::box_value(labels.GetNamedString(L"showEditor")));showEditor.IsChecked(m.GetNamedBoolean(L"showEditor"));Automation::AutomationProperties::SetAutomationId(showEditor,L"FennBookmark-show-editor");panel.Children().Append(showEditor);
            error=TextBlock();error.TextWrapping(TextWrapping::Wrap);Automation::AutomationProperties::SetAutomationId(error,L"FennBookmark-error");Automation::AutomationProperties::SetLiveSetting(error,Automation::Peers::AutomationLiveSetting::Assertive);panel.Children().Append(error);
            StackPanel footer;footer.Orientation(Orientation::Horizontal);footer.Spacing(8);save=Markup::XamlReader::Load(LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource AccentButtonStyle}"/>)").as<Button>();save.Content(winrt::box_value(labels.GetNamedString(L"save")));Automation::AutomationProperties::SetAutomationId(save,L"FennBookmark-save");remove=Button();remove.Content(winrt::box_value(labels.GetNamedString(L"remove")));Automation::AutomationProperties::SetAutomationId(remove,L"FennBookmark-remove");footer.Children().Append(save);footer.Children().Append(remove);
            observedName=name.Text();observedTags=tags.Text();observedShowEditor=showEditor.IsChecked().GetBoolean();
            save.Click([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired())Send(1);});remove.Click([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired())Send(2);});
            name.TextChanged([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired())Changed();});
            name.TextCompositionStarted([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired()){composing=true;Interact();}});name.TextCompositionEnded([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired()){composing=false;Changed();if(editEpoch>sentEpoch)Send(7);}});
            tags.TextChanged([this,alive,current](auto const&,auto const& args){if(!alive.expired() && !current.expired() && args.Reason()==AutoSuggestionBoxTextChangeReason::UserInput){Changed();}});
            tags.SuggestionChosen([this,alive,current](auto const&,auto const& args){if(!alive.expired() && !current.expired()){tags.Text(winrt::unbox_value<winrt::hstring>(args.SelectedItem()));}});
            tags.QuerySubmitted([this,alive,current](auto const&,auto const& args){if(!alive.expired() && !current.expired()){if(args.ChosenSuggestion()){tags.Text(winrt::unbox_value<winrt::hstring>(args.ChosenSuggestion()));Changed();tags.IsSuggestionListOpen(false);}else Send(1);}});
            showEditor.Click([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired())Changed();});
            frame.AddHandler(UIElement::PointerPressedEvent(),winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired())Interact();})),true);
            frame.PreviewKeyDown([this,alive,current](auto const&,auto const& args){if(alive.expired() || current.expired() || composing)return;for(auto target=args.OriginalSource().try_as<DependencyObject>();target;target=Media::VisualTreeHelper::GetParent(target))if(target==tags)return;if(args.Key()==Windows::System::VirtualKey::Escape && !tags.IsSuggestionListOpen()){args.Handled(true);Send(2,true);}});
            frame.AddHandler(UIElement::KeyDownEvent(),winrt::box_value(Microsoft::UI::Xaml::Input::KeyEventHandler([this,alive,current](auto const&,auto const& args){if(alive.expired() || current.expired())return;Interact();if(args.Key()==Windows::System::VirtualKey::Escape && !composing){args.Handled(true);Send(2,true);}else if(args.Key()==Windows::System::VirtualKey::Enter && !composing && !tags.IsSuggestionListOpen() && args.OriginalSource()==name){args.Handled(true);Send(1);}})),false);
            ScrollViewer scroll;scroll.MaxHeight(540);scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);scroll.Content(panel);frame.Children().Append(scroll);frame.Children().Append(footer);flyout.Content(frame);
            flyout.Opened([this,alive,current,focusTags=m.GetNamedBoolean(L"focusTags")](auto const&,auto const&){if(!alive.expired() && !current.expired()){popupOpen=true;if(focusTags)tags.Focus(FocusState::Programmatic);else{name.Focus(FocusState::Programmatic);name.SelectAll();}}});
            flyout.Closed([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired()){popupOpen=false;if(afterClose){auto task=std::move(afterClose);anchor.DispatcherQueue().TryEnqueue(std::move(task));}else if(opening)Send(1);}});
            if(m.GetNamedBoolean(L"isNew") && m.GetNamedString(L"error").empty()){timer=source.DispatcherQueue().CreateTimer();timer.IsRepeating(false);timer.Interval(std::chrono::milliseconds(int64_t(m.GetNamedNumber(L"autoClose"))));timer.Tick([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired() && !interacted && !composing)Send(1);});timer.Start();frame.PointerMoved([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired())StopTimer();});frame.PointerEntered([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired())StopTimer();});frame.PointerExited([this,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired() && !interacted && timer)timer.Start();});}
        }
        location.Text(labels.GetNamedString(L"folder")+L": "+m.GetNamedString(L"folder"));error.Text(m.GetNamedString(L"error"));error.Visibility(error.Text().empty()?Visibility::Collapsed:Visibility::Visible);
        if(fresh || folderAck){choices.Children().Clear();auto add=[&](winrt::hstring const& label,uint32_t id,winrt::hstring const& automation){Button b;b.Content(winrt::box_value(label));b.HorizontalAlignment(HorizontalAlignment::Stretch);b.HorizontalContentAlignment(HorizontalAlignment::Left);Automation::AutomationProperties::SetAutomationId(b,automation);b.Click([this,alive,current,id](auto const&,auto const&){if(!alive.expired() && !current.expired()){Interact();Send(id);}});choices.Children().Append(b);};
            if(m.GetNamedBoolean(L"parent"))add(labels.GetNamedString(L"parent"),3,L"FennBookmark-parent");
            for(auto key:{L"roots",L"folders"})for(auto v:m.GetNamedArray(key)){auto item=v.GetObject();add(item.GetNamedString(L"label"),Id(item,L"id"),winrt::hstring(L"FennBookmark-folder-"+std::to_wstring(Id(item,L"id"))));}
            if(m.GetNamedBoolean(L"previous"))add(labels.GetNamedString(L"previous"),4,L"FennBookmark-previous");if(m.GetNamedBoolean(L"next"))add(labels.GetNamedString(L"next"),5,L"FennBookmark-next");}
        std::vector<uint32_t> nextTags;if(m.GetNamedBoolean(L"tagPrevious"))nextTags.push_back(9);for(auto v:m.GetNamedArray(L"tagOptions"))nextTags.push_back(Id(v.GetObject(),L"id"));if(m.GetNamedBoolean(L"tagNext"))nextTags.push_back(10);
        if(nextTags!=tagOrder){tagOrder=nextTags;tagChecks.clear();tagChoices.Children().Clear();
            for(auto id:nextTags){if(id==9 || id==10){Button pager;pager.Content(winrt::box_value(labels.GetNamedString(id==9?L"previous":L"next")));Automation::AutomationProperties::SetAutomationId(pager,id==9?L"FennBookmark-tags-previous":L"FennBookmark-tags-next");pager.Click([this,alive,current,id](auto const&,auto const&){if(!alive.expired() && !current.expired())Send(id);});tagChoices.Children().Append(pager);}
                else {CheckBox check;Automation::AutomationProperties::SetAutomationId(check,L"FennBookmark-tag-"+std::to_wstring(id));check.Click([this,alive,current,id](auto const&,auto const&){if(!alive.expired() && !current.expired())Send(id);});tagChecks.emplace(id,check);tagChoices.Children().Append(check);}}
        }
        for(auto v:m.GetNamedArray(L"tagOptions")){auto item=v.GetObject();auto check=tagChecks.at(Id(item,L"id"));check.Content(winrt::box_value(item.GetNamedString(L"label")));check.IsChecked(item.GetNamedBoolean(L"checked"));}
        auto nextSelection=uint32_t(m.GetNamedNumber(L"tagSelection"));if(nextSelection!=tagSelection){tagSelection=nextSelection;if(editEpoch==sentEpoch){tags.Text(m.GetNamedString(L"tags"));observedTags=tags.Text();}}
        std::vector<winrt::hstring> nextSuggestions;for(auto v:m.GetNamedArray(L"suggestions"))nextSuggestions.push_back(v.GetString());
        if(nextSuggestions!=lastSuggestions){lastSuggestions=nextSuggestions;auto suggestions=winrt::single_threaded_observable_vector<winrt::Windows::Foundation::IInspectable>();for(auto const& text:nextSuggestions)suggestions.Append(winrt::box_value(text));tags.ItemsSource(suggestions);if(nextSuggestions.empty())tags.IsSuggestionListOpen(false);}
        save.IsEnabled(true);remove.IsEnabled(true);EnableChoices(true);applying=false;
        if(fresh)flyout.ShowAt(anchor);
        if(pendingTerminal){auto id=pendingTerminal;pendingTerminal=0;Send(id==3?2:id,id==3);}else if(editEpoch>sentEpoch)Send(7);
    }
};
