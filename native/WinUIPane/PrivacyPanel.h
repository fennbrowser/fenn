#pragma once
#include "JsonValidation.h"
#include "shell_core.h"

// WinUI presentation only. ShellCore owns opening/action identity; Firefox
// supplies every label/choice and owns sanitization and private-session reset.
struct NativePrivacyPanel {
    using Object=winrt::Windows::Data::Json::JsonObject;
    using Button=Microsoft::UI::Xaml::Controls::Primitives::ButtonBase;
    uint32_t kind{},opening{},choice{};
    wchar_t const* modelKey;
    winrt::hstring automationId;
    ShellCore* core{};
    bool enabled{};
    Button anchor{nullptr};
    Microsoft::UI::Xaml::Controls::Flyout flyout{nullptr};
    Microsoft::UI::Xaml::Controls::CheckBox remember{nullptr};
    std::function<void(wchar_t const*,uint32_t,Button const&)> dispatch;
    std::function<void()> afterClose;
    std::shared_ptr<int> lifetime=std::make_shared<int>(0),openingLifetime;
    NativePrivacyPanel(uint32_t value,wchar_t const* key,wchar_t const* id):kind(value),modelKey(key),automationId(id){}
    ~NativePrivacyPanel(){lifetime.reset();Close();}
    static uint32_t Id(Object const& model,wchar_t const* key,bool zero=false){auto n=NativeJson::RequireRevision(model,key);if(n>UINT32_MAX || (!zero && !n))winrt::throw_hresult(E_INVALIDARG);return uint32_t(n);}
    Object Read(Object const& snapshot) const {
        auto model=snapshot.GetNamedObject(L"navigation",Object()).GetNamedObject(modelKey,Object());
        model.GetNamedBoolean(L"enabled",false);
        if(model.HasKey(L"closed"))Id(model,L"closed",true);
        if(!model.HasKey(L"session"))return model;
        Id(model,L"session");Id(model,L"revision");model.GetNamedBoolean(L"remember");
        for(auto key:{L"title",L"group",L"warning",L"checkbox"})NativeJson::RequireLength(model.GetNamedString(key),2048);
        for(auto key:{L"sourceLabel",L"confirm",L"cancel"})NativeJson::RequireLength(model.GetNamedString(key),512);
        NativeJson::RequireLength(model.GetNamedString(L"body"),10240);
        if(model.GetNamedString(L"title").empty() || model.GetNamedString(L"body").empty() || model.GetNamedString(L"confirm").empty() || model.GetNamedString(L"cancel").empty())winrt::throw_hresult(E_INVALIDARG);
        if(kind==SHELL_PRIVACY_PANEL_KIND_FORGET ? (model.GetNamedString(L"group").empty() || model.GetNamedString(L"warning").empty()) : model.GetNamedString(L"checkbox").empty())winrt::throw_hresult(E_INVALIDARG);
        auto options=model.GetNamedArray(L"options");
        if(options.Size()!=(kind==SHELL_PRIVACY_PANEL_KIND_FORGET?3u:0u))winrt::throw_hresult(E_INVALIDARG);
        std::set<uint32_t> ids;
        for(auto value:options){auto item=value.GetObject();auto id=Id(item,L"id");
            if(id>3 || !ids.insert(id).second || item.GetNamedString(L"label").empty())winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(item.GetNamedString(L"label"),512);
        }
        if(model.Stringify().size()>65536)winrt::throw_hresult(E_INVALIDARG);
        return model;
    }
    bool Resolve(Object const& model){
        if(!model.HasKey(L"session"))return false;
        uint32_t action=1;auto fingerprint=model.Stringify();bool changed{};
        winrt::check_hresult(shell_core_privacy_panel_resolve(core,kind,Id(model,L"session"),Id(model,L"revision"),&action,1,
            reinterpret_cast<uint16_t const*>(fingerprint.c_str()),fingerprint.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);return changed;
    }
    std::wstring Command(wchar_t const* operation) const {return L"privacy-panel:"+std::wstring(operation)+L":"+std::to_wstring(kind)+L":"+std::to_wstring(opening);}
    void Close(){opening=0;afterClose={};openingLifetime.reset();if(core)shell_core_privacy_panel_dismiss(core,kind);if(flyout)flyout.Hide();flyout=nullptr;anchor=nullptr;remember=nullptr;}
    void Open(Button const& button){
        if(!enabled || !core || !dispatch)return;
        if(opening){auto command=Command(L"dismiss");dispatch(command.c_str(),0,anchor);Close();return;}
        uint32_t session{};if(shell_core_privacy_panel_open(core,kind,&session)!=SHELL_STATUS_OK || !session)return;
        opening=session;anchor=button;openingLifetime=std::make_shared<int>(0);
        dispatch(Command(L"open").c_str(),0,button);
    }
    void Apply(Object const& model,bool visible,bool publish){
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        enabled=visible && model.GetNamedBoolean(L"enabled",false);
        if(!enabled || (opening && model.GetNamedNumber(L"closed",0)==opening)){Close();return;}
        if(!publish || !opening || !model.HasKey(L"session") || Id(model,L"session")!=opening)return;
        const auto session=opening,revision=Id(model,L"revision");uint32_t action=1;auto fingerprint=model.Stringify();bool changed{};
        winrt::check_hresult(shell_core_privacy_panel_accept(core,kind,session,revision,&action,1,reinterpret_cast<uint16_t const*>(fingerprint.c_str()),fingerprint.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);
        if(!changed || flyout)return;
        const auto alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime);
        flyout=Flyout();flyout.ShouldConstrainToRootBounds(false);
        flyout.Placement(anchor.FlowDirection()==FlowDirection::RightToLeft?Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft:Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
        StackPanel content;content.Width(320);content.Spacing(12);
        Automation::AutomationProperties::SetAutomationId(content,automationId);
        Automation::AutomationProperties::SetName(content,model.GetNamedString(L"title"));
        auto text=[&](wchar_t const* key,bool heading=false){TextBlock block;block.Text(model.GetNamedString(key));block.TextWrapping(TextWrapping::Wrap);if(heading){block.FontSize(18);block.FontWeight({600});}Automation::AutomationProperties::SetAutomationId(block,automationId+L"-"+key);content.Children().Append(block);return block;};
        text(L"title",true);
        if(!model.GetNamedString(L"group").empty())text(L"group");
        auto options=model.GetNamedArray(L"options");choice=options.Size()?Id(options.GetObjectAt(0),L"id"):0;
        for(auto value:options){auto option=value.GetObject();auto id=Id(option,L"id");RadioButton button;
            button.Content(winrt::box_value(option.GetNamedString(L"label")));button.GroupName(automationId+L"-"+winrt::to_hstring(session));button.IsChecked(winrt::box_value(id==choice).as<winrt::Windows::Foundation::IReference<bool>>());
            Automation::AutomationProperties::SetAutomationId(button,automationId+L"-range-"+winrt::to_hstring(id));
            button.Checked([this,id,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired())choice=id;});content.Children().Append(button);
        }
        text(L"body");
        if(!model.GetNamedString(L"warning").empty())text(L"warning").FontWeight({600});
        remember=CheckBox();remember.Content(winrt::box_value(model.GetNamedString(L"checkbox")));remember.IsChecked(winrt::box_value(model.GetNamedBoolean(L"remember")).as<winrt::Windows::Foundation::IReference<bool>>());
        remember.Visibility(model.GetNamedString(L"checkbox").empty()?Visibility::Collapsed:Visibility::Visible);
        Automation::AutomationProperties::SetAutomationId(remember,automationId+L"-remember");content.Children().Append(remember);
        StackPanel buttons;buttons.Orientation(Orientation::Horizontal);buttons.Spacing(8);buttons.HorizontalAlignment(HorizontalAlignment::Right);
        Controls::Button cancel;cancel.Content(winrt::box_value(model.GetNamedString(L"cancel")));Automation::AutomationProperties::SetAutomationId(cancel,automationId+L"-cancel");
        auto confirm=Markup::XamlReader::Load(LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource AccentButtonStyle}"/>)").as<Controls::Button>();
        confirm.Content(winrt::box_value(model.GetNamedString(L"confirm")));Automation::AutomationProperties::SetAutomationId(confirm,automationId+L"-confirm");
        auto queue=[this,alive,current](std::wstring command){afterClose=[this,command,source=anchor,alive,current]{if(!alive.expired() && !current.expired() && enabled && dispatch)dispatch(command.c_str(),0,source);};flyout.Hide();};
        cancel.Click([this,queue,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired())queue(Command(L"cancel"));});
        confirm.Click([this,session,revision,queue,alive,current](auto const&,auto const&){
            if(alive.expired() || current.expired() || opening!=session)return;
            bool accepted{};if(shell_core_privacy_panel_activate(core,kind,session,revision,1,&accepted)!=SHELL_STATUS_OK || !accepted)return;
            auto checked=remember.IsChecked();queue(Command(L"confirm")+L":"+std::to_wstring(revision)+L":"+std::to_wstring(choice)+L":"+(checked && checked.Value()?L"1":L"0"));
        });
        buttons.Children().Append(cancel);buttons.Children().Append(confirm);content.Children().Append(buttons);
        ScrollViewer scroll;scroll.MaxHeight(620);scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);scroll.Content(content);flyout.Content(scroll);
        flyout.Opened([cancel,alive,current](auto const&,auto const&){if(!alive.expired() && !current.expired())cancel.Focus(FocusState::Programmatic);});
        flyout.Closed([this,session,alive](auto const&,auto const&){
            if(alive.expired() || opening!=session)return;
            auto command=Command(L"dismiss");auto send=std::move(afterClose);afterClose={};opening=0;shell_core_privacy_panel_dismiss(core,kind);
            if(send)anchor.DispatcherQueue().TryEnqueue(std::move(send));else if(dispatch)dispatch(command.c_str(),0,anchor);
            flyout=nullptr;anchor=nullptr;remember=nullptr;
        });
        flyout.ShowAt(anchor);
    }
};
