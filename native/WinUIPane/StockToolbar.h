#pragma once
#include "ToolbarCustomization.h"
#include "ToolbarMetrics.h"

// Only WinUI objects live here; Firefox supplies all state and dispatches commands.
struct NativeStockToolbar {
    using Button=Microsoft::UI::Xaml::Controls::Primitives::ButtonBase;
    using Grid=Microsoft::UI::Xaml::Controls::Grid;
    struct Row { Grid host{nullptr}; std::vector<Button> buttons; std::vector<bool> checked; };
    std::map<winrt::hstring,Row> rows;
    std::function<void(wchar_t const*,uint32_t,Button const&,bool)> activate;
    std::shared_ptr<int> lifetime=std::make_shared<int>(0);
    ~NativeStockToolbar(){lifetime.reset();}
    static wchar_t const* Glyph(winrt::hstring const& id) {
        static const std::map<winrt::hstring,wchar_t const*> glyphs={
            {L"find-button",L"\xE721"},{L"print-button",L"\xE749"},
            {L"new-tab-button",L"\xE710"},{L"new-window-button",L"\xE78B"},
            {L"save-page-button",L"\xE74E"},{L"open-file-button",L"\xE8E5"},
            {L"fullscreen-button",L"\xE740"},{L"screenshot-button",L"\xE722"},
            {L"developer-button",L"\xE943"},{L"zoom-out-button",L"\xE71F"},
            {L"zoom-reset-button",L"\xE71E"},{L"zoom-in-button",L"\xE8A3"},
            {L"cut-button",L"\xE8C6"},{L"copy-button",L"\xE8C8"},{L"paste-button",L"\xE77F"},
            {L"email-link-button",L"\xE715"},{L"characterencoding-button",L"\xE8C1"},
            {L"sync-button",L"\xE895"},{L"library-button",L"\xE8F1"},
            {L"logins-button",L"\xE72E"},{L"preferences-button",L"\xE713"},
            {L"panic-button",L"\xE74D"},{L"privatebrowsing-button",L"\xE727"},
            {L"tab-groups-button",L"\xE902"},{L"import-button",L"\xE8B5"},
            {L"send-tab-button",L"\xE8A1"},{L"share-tab-button",L"\xE72D"},
            {L"mini-window-button",L"\xE8A7"},{L"firefox-view-button",L"\xE8A5"},
            {L"profiler-button",L"\xE9D9"}};
        auto it=glyphs.find(id);if(it==glyphs.end())throw winrt::hresult_invalid_argument();return it->second;
    }
    static void Validate(NativeToolbarCustomization::State const& state) {
        for(auto const& item:state.items)if(!item.stock.empty()) {
            if(item.spring || item.extension || !std::wstring_view(item.control).starts_with(L"stock:"))throw winrt::hresult_invalid_argument();
            for(auto const& part:item.stock)Glyph(part.id);
            if(item.stock.size()==1) {
                if(item.control!=L"stock:"+item.stock[0].id)throw winrt::hresult_invalid_argument();
            } else {
                const std::vector<winrt::hstring> ids=item.control==L"stock:zoom-controls"
                    ? std::vector<winrt::hstring>{L"zoom-out-button",L"zoom-reset-button",L"zoom-in-button"}
                    : item.control==L"stock:edit-controls"
                    ? std::vector<winrt::hstring>{L"cut-button",L"copy-button",L"paste-button"}:std::vector<winrt::hstring>{};
                if(ids.size()!=item.stock.size())throw winrt::hresult_invalid_argument();
                for(size_t i=0;i<ids.size();i++)if(ids[i]!=item.stock[i].id)throw winrt::hresult_invalid_argument();
            }
        }
    }
    Row Create(NativeToolbarCustomization::Item const& item,bool preview=false) {
        using namespace Microsoft::UI::Xaml;using namespace Microsoft::UI::Xaml::Controls;
        Row row;row.host=Grid();
        for(size_t i=0;i<item.stock.size();i++) {
            auto const& part=item.stock[i];
            ColumnDefinition col;col.Width({ToolbarMetrics::kButton+(i?ToolbarMetrics::kGap:0),GridUnitType::Pixel});row.host.ColumnDefinitions().Append(col);
            auto button=Markup::XamlReader::Load(part.checkable
                ? LR"(<ToggleButton xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" MinWidth="0" MinHeight="0" Padding="0"/>)"
                : LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" BorderBrush="{ThemeResource ControlStrongStrokeColorDefaultBrush}" BorderThickness="0" MinWidth="0" MinHeight="0" Padding="0"/>)").as<Button>();
            button.Width(ToolbarMetrics::kButton);button.Height(ToolbarMetrics::kButton);
            button.IsTabStop(!preview);
            if(preview)Automation::AutomationProperties::SetAccessibilityView(button,Automation::Peers::AccessibilityView::Raw);
            if(i)button.Margin({ToolbarMetrics::kGap,0,0,0});
            Grid::SetColumn(button,int(i));
            if(part.id==L"zoom-reset-button" && (!preview || !part.text.empty())) {
                TextBlock text;text.FontSize(10);text.Text(part.text);button.Content(text);
            } else {
                auto icon=Markup::XamlReader::Load(LR"(<FontIcon xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" FontFamily="{ThemeResource SymbolThemeFontFamily}"/>)").as<FontIcon>();
                icon.Glyph(Glyph(part.id));icon.FontSize(ToolbarMetrics::kIcon);button.Content(icon);
            }
            Automation::AutomationProperties::SetAutomationId(button,L"FennStock-"+part.id);
            Automation::AutomationProperties::SetName(button,part.label);
            ToolTipService::SetToolTip(button,winrt::box_value(part.tooltip.empty()?part.label:part.tooltip));
            if(!preview) {
                auto command=L"stock:"+item.id+L":"+part.id;
                struct ActivationInput { uint32_t modifiers{};bool keyboard{}; };
                auto input=std::make_shared<ActivationInput>();
                button.AddHandler(UIElement::PointerPressedEvent(),winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                    [input](auto const&,auto const& args){input->modifiers=uint32_t(args.KeyModifiers())&15u;input->keyboard=false;})),true);
                button.PreviewKeyDown([input](auto const&,auto const&) {
                    input->keyboard=true;
                    input->modifiers=((GetKeyState(VK_CONTROL)&0x8000)?1u:0u)|((GetKeyState(VK_MENU)&0x8000)?2u:0u)|
                        ((GetKeyState(VK_SHIFT)&0x8000)?4u:0u)|((GetKeyState(VK_LWIN)&0x8000)?8u:0u);
                });
                // Do not steal content selection merely by clicking an edit command.
                button.AllowFocusOnInteraction(false);
                button.Click([this,alive=std::weak_ptr<int>(lifetime),command,input,id=item.id,i](auto const& sender,auto const&) {
                    if(alive.expired() || !activate)return;
                    Button b=sender.template as<Button>();
                    // ToggleButton supplies its stock UIA pattern and visuals,
                    // but Firefox's publication is the authority for checked.
                    if(auto toggle=b.try_as<Primitives::ToggleButton>()) {
                        auto row=rows.find(id);
                        if(row!=rows.end() && i<row->second.checked.size())
                            toggle.IsChecked(winrt::box_value(bool(row->second.checked[i])).as<winrt::Windows::Foundation::IReference<bool>>());
                    }
                    activate(command.c_str(),input->modifiers,b,input->keyboard);
                    input->modifiers=0;input->keyboard=false;
                });
            }
            row.host.Children().Append(button);row.buttons.push_back(button);row.checked.push_back(part.checked);
        }
        return row;
    }
    Grid Control(NativeToolbarCustomization::Item const& item,bool visible,bool editing) {
        using namespace Microsoft::UI::Xaml;
        auto [it,inserted]=rows.try_emplace(item.id);if(inserted)it->second=Create(item);
        for(size_t i=0;i<item.stock.size();i++) {
            auto const& part=item.stock[i];auto button=it->second.buttons.at(i);
            it->second.checked[i]=part.checked;
            button.IsEnabled(visible && (editing || part.enabled));
            button.Visibility(editing || part.shown?Visibility::Visible:Visibility::Collapsed);
            if(!part.checkable)button.BorderThickness(part.checked?Thickness{2,2,2,2}:Thickness{0,0,0,0});
            if(auto toggle=button.try_as<Microsoft::UI::Xaml::Controls::Primitives::ToggleButton>())toggle.IsChecked(winrt::box_value(part.checked).as<winrt::Windows::Foundation::IReference<bool>>());
            Automation::AutomationProperties::SetName(button,part.label);
            Automation::AutomationProperties::SetItemStatus(button,part.checked?part.label:L"");
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(button,winrt::box_value(part.tooltip.empty()?part.label:part.tooltip));
            if(part.id==L"zoom-reset-button")button.Content().as<Microsoft::UI::Xaml::Controls::TextBlock>().Text(part.text);
        }
        return it->second.host;
    }
};
