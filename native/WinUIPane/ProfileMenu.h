#pragma once
#include "JsonValidation.h"
#include "shell_core.h"

// WinUI objects only. ShellCore validates independent opening/revision/action
// identity; Firefox owns profiles, policy, accounts, launches and confirmations.
struct NativeProfileMenu {
    using Object=winrt::Windows::Data::Json::JsonObject;
    using Button=Microsoft::UI::Xaml::Controls::Button;
    ShellCore* core{};
    bool enabled{};
    uint32_t opening{};
    Button anchor{nullptr};
    Microsoft::UI::Xaml::Controls::MenuFlyout flyout{nullptr};
    std::function<void(wchar_t const*,uint32_t,Button const&)> dispatch;
    std::function<void()> afterClose;
    std::shared_ptr<int> lifetime=std::make_shared<int>(0),openingLifetime;
    ~NativeProfileMenu(){lifetime.reset();Close();}
    static uint32_t Id(Object const& value,wchar_t const* key){auto n=NativeJson::RequireRevision(value,key);if(!n || n>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);return uint32_t(n);}
    static Object Read(Object const& snapshot) {
        auto model=snapshot.GetNamedObject(L"navigation",Object()).GetNamedObject(L"profileMenu",Object());
        model.GetNamedBoolean(L"enabled",false);
        if(model.HasKey(L"closed") && NativeJson::RequireRevision(model,L"closed")>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);
        if(!model.HasKey(L"session"))return model;
        Id(model,L"session");Id(model,L"revision");NativeJson::RequireLength(model.GetNamedString(L"label"),512);
        auto entries=model.GetNamedArray(L"items");if(entries.Size()>32)winrt::throw_hresult(E_INVALIDARG);
        std::set<uint32_t> ids;std::set<winrt::hstring> keys;
        for(auto value:entries){auto item=value.GetObject();if(item.GetNamedBoolean(L"separator",false))continue;
            if(!ids.insert(Id(item,L"id")).second)winrt::throw_hresult(E_INVALIDARG);
            const auto key=item.GetNamedString(L"key");if(key.empty() || key.size()>64 || !keys.insert(key).second)winrt::throw_hresult(E_INVALIDARG);
            for(wchar_t c:key)if(!((c>=L'a' && c<=L'z') || (c>=L'0' && c<=L'9') || c==L'-'))winrt::throw_hresult(E_INVALIDARG);
            const auto label=item.GetNamedString(L"label");if(label.empty())winrt::throw_hresult(E_INVALIDARG);NativeJson::RequireLength(label,512);
            item.GetNamedBoolean(L"enabled");item.GetNamedBoolean(L"checked");
        }
        if(model.Stringify().size()>65536)winrt::throw_hresult(E_INVALIDARG);return model;
    }
    static std::vector<uint32_t> Actions(Object const& model) {
        std::vector<uint32_t> ids;
        for(auto value:model.GetNamedArray(L"items")){auto item=value.GetObject();if(!item.GetNamedBoolean(L"separator",false) && item.GetNamedBoolean(L"enabled"))ids.push_back(Id(item,L"id"));}
        return ids;
    }
    bool Resolve(Object const& model) {
        if(!model.HasKey(L"session"))return false;
        auto ids=Actions(model);auto fingerprint=model.Stringify();bool changed{};
        winrt::check_hresult(shell_core_profile_menu_resolve(core,Id(model,L"session"),Id(model,L"revision"),ids.data(),ids.size(),
            reinterpret_cast<uint16_t const*>(fingerprint.c_str()),fingerprint.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);return changed;
    }
    void Close(){opening=0;afterClose={};openingLifetime.reset();if(core)shell_core_profile_menu_dismiss(core);if(flyout)flyout.Hide();flyout=nullptr;anchor=nullptr;}
    void Open(Button const& button) {
        if(!enabled || !core || !dispatch)return;
        if(opening){auto command=L"profile-menu:dismiss:"+std::to_wstring(opening);dispatch(command.c_str(),0,anchor);Close();return;}
        uint32_t session{};if(shell_core_profile_menu_open(core,&session)!=SHELL_STATUS_OK || !session)return;
        opening=session;anchor=button;openingLifetime=std::make_shared<int>(0);
        dispatch((L"profile-menu:open:"+std::to_wstring(session)).c_str(),0,button);
    }
    void Apply(Object const& model,bool visible,bool publish) {
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        enabled=visible && model.GetNamedBoolean(L"enabled",false);
        if(!enabled || (opening && model.GetNamedNumber(L"closed",0)==opening)){Close();return;}
        if(!publish || !opening || !model.HasKey(L"session") || Id(model,L"session")!=opening)return;
        const auto session=opening,revision=Id(model,L"revision");auto ids=Actions(model);auto fingerprint=model.Stringify();bool changed{};
        winrt::check_hresult(shell_core_profile_menu_accept(core,session,revision,ids.data(),ids.size(),reinterpret_cast<uint16_t const*>(fingerprint.c_str()),fingerprint.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);
        if(!changed)return;
        // A profile/account mutation dismisses the opening in Firefox. Never
        // clear/repopulate a MenuFlyout while its previous instance is closing.
        if(flyout)return;
        flyout=MenuFlyout();flyout.ShouldConstrainToRootBounds(false);
        flyout.Placement(anchor.FlowDirection()==FlowDirection::RightToLeft?Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft:Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
        Automation::AutomationProperties::SetName(flyout,model.GetNamedString(L"label"));
        flyout.Closed([this,session,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const&){
            if(alive.expired() || opening!=session)return;
            auto send=std::move(afterClose);afterClose={};opening=0;shell_core_profile_menu_dismiss(core);
            if(send)anchor.DispatcherQueue().TryEnqueue(std::move(send));
            else if(dispatch)dispatch((L"profile-menu:dismiss:"+std::to_wstring(session)).c_str(),0,anchor);
            flyout=nullptr;anchor=nullptr;
        });
        for(auto value:model.GetNamedArray(L"items")){auto entry=value.GetObject();
            if(entry.GetNamedBoolean(L"separator",false)){flyout.Items().Append(MenuFlyoutSeparator());continue;}
            auto id=Id(entry,L"id");auto key=entry.GetNamedString(L"key");
            MenuFlyoutItemBase base{nullptr};
            if(entry.GetNamedBoolean(L"checked")){ToggleMenuFlyoutItem item;item.Text(entry.GetNamedString(L"label"));item.IsChecked(true);item.IsEnabled(false);base=item;}
            else {MenuFlyoutItem item;item.Text(entry.GetNamedString(L"label"));item.IsEnabled(entry.GetNamedBoolean(L"enabled"));
                auto activate=[this,session,revision,id,alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime)]{
                    if(alive.expired() || current.expired() || opening!=session)return;
                    bool accepted{};if(shell_core_profile_menu_activate(core,session,revision,id,&accepted)!=SHELL_STATUS_OK || !accepted)return;
                    auto command=L"profile-menu:invoke:"+std::to_wstring(session)+L":"+std::to_wstring(revision)+L":"+std::to_wstring(id);
                    afterClose=[this,command,source=anchor,alive,current]{if(!alive.expired() && !current.expired() && enabled && dispatch)dispatch(command.c_str(),0,source);};
                    if(flyout)flyout.Hide();
                };
                item.Click([activate](auto const&,auto const&){activate();});
                // Stock Click runs first. In a later window, WinUI's delayed exits from a stale pointer
                // record can clear the item's pressed state even with capture (as in the back/forward
                // menu): recover only a captured left press released inside this same item. ShellCore
                // consumes the activation once.
                auto mouseArmed=std::make_shared<bool>(false);
                item.AddHandler(UIElement::PointerPressedEvent(),winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler([mouseArmed](auto const& sender,auto const& args){
                    if(args.Pointer().PointerDeviceType()==Microsoft::UI::Input::PointerDeviceType::Mouse && args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed())
                        *mouseArmed=sender.template as<MenuFlyoutItemBase>().CapturePointer(args.Pointer());
                })),true);
                item.AddHandler(UIElement::PointerReleasedEvent(),winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler([activate,mouseArmed](auto const& sender,auto const& args){
                    if(args.Pointer().PointerDeviceType()!=Microsoft::UI::Input::PointerDeviceType::Mouse)return;
                    auto row=sender.template as<MenuFlyoutItemBase>();auto point=args.GetCurrentPoint(row);auto p=point.Position();
                    const bool armed=std::exchange(*mouseArmed,false);
                    if(armed && point.Properties().PointerUpdateKind()==Microsoft::UI::Input::PointerUpdateKind::LeftButtonReleased &&
                       p.X>=0 && p.Y>=0 && p.X<row.ActualWidth() && p.Y<row.ActualHeight())activate();
                    row.ReleasePointerCapture(args.Pointer());
                })),true);
                item.PointerCanceled([mouseArmed](auto const&,auto const&){*mouseArmed=false;});
                base=item;
            }
            Automation::AutomationProperties::SetAutomationId(base,L"FennProfileMenu-"+key);flyout.Items().Append(base);
        }
        flyout.ShowAt(anchor);
    }
};
