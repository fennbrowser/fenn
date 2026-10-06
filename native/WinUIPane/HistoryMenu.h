#pragma once
#include "JsonValidation.h"
#include "shell_core.h"

// WinUI object lifetime only; Rust validates openings/actions, Firefox owns history.
struct NativeHistoryMenu {
    using Object=winrt::Windows::Data::Json::JsonObject;
    using Array=winrt::Windows::Data::Json::JsonArray;
    using Button=Microsoft::UI::Xaml::Controls::Button;
    ShellCore* core{};
    bool enabled{};
    uint32_t opening{};
    Microsoft::UI::Xaml::Controls::MenuFlyout flyout{nullptr};
    std::function<void(wchar_t const*,uint32_t,Button const&)> dispatch;
    std::shared_ptr<int> lifetime=std::make_shared<int>(0);
    ~NativeHistoryMenu(){lifetime.reset();if(flyout)flyout.Hide();}
    static uint32_t Id(Object const& value,wchar_t const* key){auto n=NativeJson::RequireRevision(value,key);if(n<1 || n>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);return uint32_t(n);}
    static Object Read(Object const& snapshot) {
        auto nav=snapshot.GetNamedObject(L"navigation",Object());
        auto model=nav.GetNamedObject(L"historyMenu",Object());
        model.GetNamedBoolean(L"enabled",false);
        if(model.HasKey(L"closed") && NativeJson::RequireRevision(model,L"closed")>UINT32_MAX)winrt::throw_hresult(E_INVALIDARG);
        if(!model.HasKey(L"session"))return model;
        Id(model,L"session");Id(model,L"revision");
        auto items=model.GetNamedArray(L"items");if(items.Size()>64)winrt::throw_hresult(E_INVALIDARG);
        std::set<uint32_t> ids;
        for(auto value:items){auto item=value.GetObject();if(item.GetNamedBoolean(L"separator",false))continue;
            auto id=Id(item,L"id");if(!ids.insert(id).second)winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(item.GetNamedString(L"label"),4096);item.GetNamedBoolean(L"enabled");item.GetNamedBoolean(L"navigable",false);}
        if(model.Stringify().size()>65536)winrt::throw_hresult(E_INVALIDARG);return model;
    }
    void Close(){opening=0;if(core)shell_core_surface_menu_dismiss(core);if(flyout)flyout.Hide();flyout=nullptr;}
    void Open(Button const& button) {
        using namespace Microsoft::UI::Xaml::Controls;
        if(flyout && flyout.IsOpen()){flyout.Hide();return;}
        uint32_t session{};winrt::check_hresult(shell_core_surface_menu_open(core,&session)==SHELL_STATUS_OK?S_OK:E_FAIL);
        if(!session)return;opening=session;flyout=MenuFlyout();
        flyout.Placement(button.FlowDirection()==Microsoft::UI::Xaml::FlowDirection::RightToLeft?Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft:Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
        MenuFlyoutItem pending;pending.Text(L"…");pending.IsEnabled(false);flyout.Items().Append(pending);
        flyout.Closed([this,button,session,alive=std::weak_ptr<int>(lifetime)](auto const&,auto const&){
            if(alive.expired() || opening!=session)return;
            opening=0;shell_core_surface_menu_dismiss(core);
            if(dispatch)dispatch((L"history-menu:dismiss:"+std::to_wstring(session)).c_str(),0,button);
        });
        flyout.ShowAt(button);
        if(dispatch)dispatch((L"history-menu:open:"+std::to_wstring(session)).c_str(),0,button);
    }
    void Apply(Object const& model,bool visible,Button const& button) {
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        enabled=visible && model.GetNamedBoolean(L"enabled",false);
        if(!enabled){if(opening || flyout)Close();return;}
        if(opening && model.GetNamedNumber(L"closed",0)==opening){Close();return;}
        if(!model.HasKey(L"session")){if(opening && flyout && flyout.Items().Size()>1)Close();return;}
        const auto session=uint32_t(model.GetNamedNumber(L"session")),revision=uint32_t(model.GetNamedNumber(L"revision"));
        if(!opening || opening!=session || !flyout || !flyout.IsOpen())return;
        std::vector<uint32_t> ids;auto entries=model.GetNamedArray(L"items");
        for(auto v:entries){auto i=v.GetObject();if(!i.GetNamedBoolean(L"separator",false) && i.GetNamedBoolean(L"enabled"))ids.push_back(uint32_t(i.GetNamedNumber(L"id")));}
        const auto fingerprint=model.Stringify();bool changed{};
        if(shell_core_surface_menu_accept(core,session,revision,ids.data(),ids.size(),reinterpret_cast<uint16_t const*>(fingerprint.c_str()),fingerprint.size(),&changed)!=SHELL_STATUS_OK || !changed)return;
        flyout.Items().Clear();
        for(auto v:entries){auto entry=v.GetObject();if(entry.GetNamedBoolean(L"separator",false)){flyout.Items().Append(MenuFlyoutSeparator());continue;}
            MenuFlyoutItem item;item.Text(entry.GetNamedString(L"label"));item.IsEnabled(entry.GetNamedBoolean(L"enabled"));
            auto id=uint32_t(entry.GetNamedNumber(L"id"));
            Automation::AutomationProperties::SetAutomationId(item,L"FirefoxWinUIHistoryMenu-"+winrt::to_hstring(id));
            auto invoke=[this,button,session,revision,id,alive=std::weak_ptr<int>(lifetime)](bool middle){
                if(alive.expired())return;bool accepted{};
                if(shell_core_surface_menu_activate(core,session,revision,id,&accepted)!=SHELL_STATUS_OK || !accepted)return;
                auto modifiers=((GetKeyState(VK_CONTROL)&0x8000)?1u:0u)|((GetKeyState(VK_MENU)&0x8000)?2u:0u)|((GetKeyState(VK_SHIFT)&0x8000)?4u:0u)|((GetKeyState(VK_LWIN)&0x8000)?8u:0u);
                if(dispatch)dispatch((std::wstring(middle?L"history-menu:middle:":L"history-menu:invoke:")+std::to_wstring(session)+L":"+std::to_wstring(revision)+L":"+std::to_wstring(id)).c_str(),modifiers,button);
                if(middle && flyout)flyout.Hide();
            };
            item.Click([invoke](auto const&,auto const&){invoke(false);});
            if(entry.GetNamedBoolean(L"navigable",false))item.AddHandler(UIElement::PointerReleasedEvent(),winrt::box_value(winrt::Microsoft::UI::Xaml::Input::PointerEventHandler([invoke](auto const&,auto const& args){
                if(args.GetCurrentPoint(nullptr).Properties().PointerUpdateKind()==winrt::Microsoft::UI::Input::PointerUpdateKind::MiddleButtonReleased){args.Handled(true);invoke(true);}
            })),true);
            flyout.Items().Append(item);
        }
    }
};
