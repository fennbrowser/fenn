#pragma once
#include "JsonValidation.h"
#include "shell_core.h"

// Stock WinUI presentation only; Firefox owns session history and navigation.
// ShellCore validates independent opening/revision/action identity.
struct NativeBackForwardMenu {
    using Object=winrt::Windows::Data::Json::JsonObject;
    using Button=Microsoft::UI::Xaml::Controls::Primitives::ButtonBase;
    ShellCore* core{};
    bool enabled{},keyboardActivation{};
    winrt::hstring direction;
    std::map<uint32_t,Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase> controls;
    struct PageIcon {
        Microsoft::UI::Xaml::Controls::MenuFlyoutItem item{nullptr};
        Microsoft::UI::Xaml::Controls::ImageIcon image;
        Microsoft::UI::Xaml::Controls::FontIcon fallback;
        bool showingImage{};
    };
    std::map<uint32_t,PageIcon> icons;
    uint32_t opening{},currentRevision{},hoveredId{};
    Button anchor{nullptr};
    Microsoft::UI::Xaml::Controls::MenuFlyout flyout{nullptr};
    std::function<void(wchar_t const*,uint32_t,Button const&)> dispatch;
    std::function<void()> afterClose;
    std::function<void(bool)> tooltipState;
    std::shared_ptr<int> lifetime=std::make_shared<int>(0),openingLifetime;
    ~NativeBackForwardMenu(){lifetime.reset();Close();}
    static uint32_t Id(Object const& value,wchar_t const* key){auto n=NativeJson::RequireRevision(value,key);if(!n || n>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);return uint32_t(n);}
    static Object Read(Object const& snapshot) {
        auto model=snapshot.GetNamedObject(L"navigation",Object()).GetNamedObject(L"backForwardMenu",Object());
        model.GetNamedBoolean(L"enabled",false);
        if(model.HasKey(L"closed") && NativeJson::RequireRevision(model,L"closed")>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);
        if(!model.HasKey(L"session"))return model;
        Id(model,L"session");Id(model,L"revision");NativeJson::RequireLength(model.GetNamedString(L"label"),512);
        auto entries=model.GetNamedArray(L"items");if(entries.Size()>15)winrt::throw_hresult(E_INVALIDARG);
        std::set<uint32_t> ids;std::set<winrt::hstring> keys;
        size_t pixels=0;
        for(auto value:entries){auto item=value.GetObject();if(item.GetNamedBoolean(L"separator",false))continue;
            if(!ids.insert(Id(item,L"id")).second)winrt::throw_hresult(E_INVALIDARG);
            const auto key=item.GetNamedString(L"key");if(key.empty() || key.size()>64 || !keys.insert(key).second)winrt::throw_hresult(E_INVALIDARG);
            for(wchar_t c:key)if(!((c>=L'a' && c<=L'z') || (c>=L'0' && c<=L'9') || c==L'-'))winrt::throw_hresult(E_INVALIDARG);
            const auto label=item.GetNamedString(L"label");if(label.empty())winrt::throw_hresult(E_INVALIDARG);NativeJson::RequireLength(label,2048);
            NativeJson::RequireLength(item.GetNamedString(L"tooltip"),512);
            NativeJson::ValidateImage(item,pixels,NativeJson::kIconImageLimits);
            item.GetNamedBoolean(L"enabled");item.GetNamedBoolean(L"checked");item.GetNamedBoolean(L"checkable");
        }
        if(model.Stringify().size()>65536)winrt::throw_hresult(E_INVALIDARG);return model;
    }
    static void SetImage(PageIcon& icon,Object const& entry){
        if(entry.HasKey(L"image") && entry.GetNamedValue(L"image").ValueType()==winrt::Windows::Data::Json::JsonValueType::Object){
            icon.image.Source(NativeJson::DecodeImage(entry.GetNamedObject(L"image")));
            if(!icon.showingImage){icon.item.Icon(icon.image);icon.showingImage=true;}
        }else if(icon.showingImage){icon.item.Icon(icon.fallback);icon.image.Source(nullptr);icon.showingImage=false;}
    }
    static std::vector<uint32_t> Actions(Object const& model) {
        std::vector<uint32_t> ids;
        for(auto value:model.GetNamedArray(L"items")){auto item=value.GetObject();if(!item.GetNamedBoolean(L"separator",false) && item.GetNamedBoolean(L"enabled"))ids.push_back(Id(item,L"id"));}
        return ids;
    }
    bool Resolve(Object const& model) {
        if(!model.HasKey(L"session"))return false;
        auto ids=Actions(model);auto fingerprint=model.Stringify();bool changed{};
        winrt::check_hresult(shell_core_back_forward_menu_resolve(core,Id(model,L"session"),Id(model,L"revision"),ids.data(),ids.size(),
            reinterpret_cast<uint16_t const*>(fingerprint.c_str()),fingerprint.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);return changed;
    }
    void LightDismiss(){if(flyout)flyout.Hide();else if(opening){if(dispatch)dispatch((L"back-forward-menu:dismiss:"+std::to_wstring(opening)).c_str(),0,anchor);Close();}}
    void Close(){icons.clear();controls.clear();hoveredId=0;opening=0;afterClose={};openingLifetime.reset();if(core)shell_core_back_forward_menu_dismiss(core);if(flyout)flyout.Hide();if(tooltipState)tooltipState(false);flyout=nullptr;anchor=nullptr;}
    bool Owns(Microsoft::UI::Xaml::DependencyObject const& target) const {
        if(!opening || !flyout)return false;
        for(auto const& item:flyout.Items())if(item==target)return true;
        return false;
    }
    void Open(Button const& button,wchar_t const* source) {
        if(!enabled || !core || !dispatch)return;
        if(opening){auto command=L"back-forward-menu:dismiss:"+std::to_wstring(opening);dispatch(command.c_str(),0,anchor);Close();return;}
        uint32_t session{};if(shell_core_back_forward_menu_open(core,&session)!=SHELL_STATUS_OK || !session)return;
        opening=session;hoveredId=0;anchor=button;direction=source;openingLifetime=std::make_shared<int>(0);
        if(tooltipState)tooltipState(true);
        dispatch((L"back-forward-menu:open:"+std::to_wstring(session)+L":"+direction).c_str(),0,button);
    }
    void Apply(Object const& model,bool visible,bool publish) {
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        enabled=visible && model.GetNamedBoolean(L"enabled",false);
        if(!enabled || (opening && model.GetNamedNumber(L"closed",0)==opening)){Close();return;}
        if(!publish || !opening || !model.HasKey(L"session") || Id(model,L"session")!=opening)return;
        const auto session=opening,revision=Id(model,L"revision");auto ids=Actions(model);auto fingerprint=model.Stringify();bool changed{};
        winrt::check_hresult(shell_core_back_forward_menu_accept(core,session,revision,ids.data(),ids.size(),reinterpret_cast<uint16_t const*>(fingerprint.c_str()),fingerprint.size(),&changed)==SHELL_STATUS_OK?S_OK:E_INVALIDARG);
        if(!changed)return;
        currentRevision=revision;
        // A new description updates metadata in place. Structural history changes
        // revoke the opening in Firefox; never reuse controls across openings.
        if(flyout){for(auto value:model.GetNamedArray(L"items")){auto entry=value.GetObject();auto found=icons.find(Id(entry,L"id"));if(found!=icons.end())SetImage(found->second,entry);}return;}
        flyout=MenuFlyout();flyout.XamlRoot(anchor.XamlRoot());flyout.ShouldConstrainToRootBounds(false);
        flyout.Placement(anchor.FlowDirection()==FlowDirection::RightToLeft?Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight:Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
        Automation::AutomationProperties::SetName(flyout,model.GetNamedString(L"label"));
        flyout.Closed([this,session,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const&){
            if(alive.expired() || opening!=session)return;
            auto send=std::move(afterClose);afterClose={};hoveredId=0;opening=0;shell_core_back_forward_menu_dismiss(core);
            if(send)anchor.DispatcherQueue().TryEnqueue(std::move(send));
            else if(dispatch)dispatch((L"back-forward-menu:dismiss:"+std::to_wstring(session)).c_str(),0,anchor);
            if(tooltipState)tooltipState(false);
            icons.clear();controls.clear();flyout=nullptr;anchor=nullptr;
        });
        for(auto value:model.GetNamedArray(L"items")){auto entry=value.GetObject();
            if(entry.GetNamedBoolean(L"separator",false)){flyout.Items().Append(MenuFlyoutSeparator());continue;}
            auto id=Id(entry,L"id");auto key=entry.GetNamedString(L"key");
            auto modifiers=std::make_shared<uint32_t>(0);
            auto mouseArmed=std::make_shared<bool>(false);
            auto activate=[this,session,id,modifiers,alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime)](bool middle){
                if(alive.expired() || current.expired() || opening!=session)return;
                const auto revision=currentRevision;
                bool accepted{};if(shell_core_back_forward_menu_activate(core,session,revision,id,&accepted)!=SHELL_STATUS_OK || !accepted)return;
                auto command=std::wstring(middle?L"back-forward-menu:middle:":L"back-forward-menu:invoke:")+std::to_wstring(session)+L":"+std::to_wstring(revision)+L":"+std::to_wstring(id);
                afterClose=[this,command,source=anchor,alive,current,value=*modifiers]{if(!alive.expired() && !current.expired() && enabled && dispatch)dispatch(command.c_str(),value,source);};
                flyout.Hide();
            };
            MenuFlyoutItemBase base{nullptr};
            if(entry.GetNamedBoolean(L"checkable")){ToggleMenuFlyoutItem item;item.Text(entry.GetNamedString(L"label"));item.IsChecked(entry.GetNamedBoolean(L"checked"));item.IsEnabled(entry.GetNamedBoolean(L"enabled"));item.Click([activate](auto const&,auto const&){activate(false);});base=item;}
            else {MenuFlyoutItem item;item.Text(entry.GetNamedString(L"label"));item.IsEnabled(entry.GetNamedBoolean(L"enabled"));item.Click([activate](auto const&,auto const&){activate(false);});base=item;}
            auto hover=[this,session,id,alive=std::weak_ptr<int>(lifetime),current=std::weak_ptr<int>(openingLifetime)]{
                if(alive.expired() || current.expired() || opening!=session || !dispatch || hoveredId==id)return;
                hoveredId=id;
                dispatch((L"back-forward-menu:hover:"+std::to_wstring(session)+L":"+std::to_wstring(currentRevision)+L":"+std::to_wstring(id)).c_str(),0,anchor);
            };
            // Keep the active row's destination until another row becomes active
            // or the menu closes. Routed exits during popup transitions must not
            // alternate browser status and disrupt the stock item's capture.
            base.PointerEntered([hover](auto const&,auto const&){hover();});
            base.GotFocus([this,hover,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const&){if(!alive.expired() && keyboardActivation)hover();});
            controls.insert_or_assign(id,base);
            // Reserve the stock icon column with a theme-aware default. Once
            // decoded, retain the ImageIcon and update its source in place.
            if(!entry.GetNamedBoolean(L"checked")){
                PageIcon icon;icon.item=base.as<MenuFlyoutItem>();icon.image.Width(16);icon.image.Height(16);icon.fallback.FontSize(16);icon.fallback.Glyph(L"\xE774");
                Automation::AutomationProperties::SetAccessibilityView(icon.image,Automation::Peers::AccessibilityView::Raw);
                Automation::AutomationProperties::SetAccessibilityView(icon.fallback,Automation::Peers::AccessibilityView::Raw);
                icon.item.Icon(icon.fallback);SetImage(icon,entry);icons.insert_or_assign(id,icon);
            }
            Automation::AutomationProperties::SetHelpText(base,entry.GetNamedString(L"tooltip"));
            // Stock Click runs first. Delayed cross-popup exits can clear its
            // pressed state even with capture; recover only a captured left
            // press released inside this same item. ShellCore consumes once.
            base.AddHandler(UIElement::PointerReleasedEvent(),winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler([activate,mouseArmed](auto const& sender,auto const& args){
                auto item=sender.template as<MenuFlyoutItemBase>();auto point=args.GetCurrentPoint(item);auto kind=point.Properties().PointerUpdateKind();
                if(kind==Microsoft::UI::Input::PointerUpdateKind::MiddleButtonReleased){args.Handled(true);activate(true);}
                if(args.Pointer().PointerDeviceType()==Microsoft::UI::Input::PointerDeviceType::Mouse){
                    const bool armed=std::exchange(*mouseArmed,false);auto p=point.Position();
                    if(armed && kind==Microsoft::UI::Input::PointerUpdateKind::LeftButtonReleased && p.X>=0 && p.Y>=0 && p.X<item.ActualWidth() && p.Y<item.ActualHeight())activate(false);
                    item.ReleasePointerCapture(args.Pointer());
                }
            })),true);
            base.PointerCanceled([mouseArmed](auto const&,auto const&){*mouseArmed=false;});
            base.AddHandler(UIElement::PointerPressedEvent(),winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler([this,modifiers,mouseArmed,alive=std::weak_ptr<int>(lifetime)](auto const& sender,auto const& args){if(!alive.expired()){
                keyboardActivation=false;*modifiers=uint32_t(args.KeyModifiers())&15u;
                if(args.Pointer().PointerDeviceType()==Microsoft::UI::Input::PointerDeviceType::Mouse && args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed())*mouseArmed=sender.template as<MenuFlyoutItemBase>().CapturePointer(args.Pointer());
            }})),true);
            base.PreviewKeyDown([this,modifiers,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const&){if(!alive.expired()){keyboardActivation=true;*modifiers=((GetKeyState(VK_CONTROL)&0x8000)?1u:0u)|((GetKeyState(VK_MENU)&0x8000)?2u:0u)|((GetKeyState(VK_SHIFT)&0x8000)?4u:0u)|((GetKeyState(VK_LWIN)&0x8000)?8u:0u);}});
            Automation::AutomationProperties::SetAutomationId(base,L"FennBackForwardMenu-"+key);flyout.Items().Append(base);
        }
        Primitives::FlyoutShowOptions options;options.Placement(flyout.Placement());options.ShowMode(Primitives::FlyoutShowMode::Standard);
        flyout.ShowAt(anchor,options);
    }
};
