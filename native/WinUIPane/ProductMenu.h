#pragma once
#include <functional>
#include <memory>
#include <set>
#include <string>
#include "JsonValidation.h"
#include "ToolbarMetrics.h"

// WinUI presentation only. Firefox owns the menu's contents, state and every
// command; this validates the bounded model and reports an opaque item id.
struct NativeProductMenu {
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Array = winrt::Windows::Data::Json::JsonArray;
    using Button = Microsoft::UI::Xaml::Controls::Button;

    static constexpr uint32_t kMaxItemsPerLevel = 32;
    static constexpr uint32_t kMaxDepth = 3;
    static constexpr uint32_t kMaxItems = 128;
    static constexpr size_t kMaxLabel = 256;
    static constexpr size_t kMaxShortcut = 64;
    static constexpr size_t kMaxModelBytes = 65536;

    Button button{nullptr};
    Microsoft::UI::Xaml::Controls::MenuFlyout flyout{nullptr};
    Object model;
    bool enabled{};
    std::function<void(wchar_t const*, uint32_t, Button const&)> dispatch;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    static bool ValidId(winrt::hstring const& id) {
        if (id.empty() || id.size() > NativeJson::kMaxOpaqueId) return false;
        bool boundary = true;
        for (wchar_t c : id) {
            const bool alnum = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
            if (c == L'-') { if (boundary) return false; boundary = true; continue; }
            if (!alnum) return false;
            boundary = false;
        }
        return !boundary;
    }

    static void ValidateItems(Array const& items, uint32_t depth, uint32_t& total, std::set<winrt::hstring>& ids) {
        using winrt::Windows::Data::Json::JsonValueType;
        if (depth > kMaxDepth || items.Size() > kMaxItemsPerLevel) winrt::throw_hresult(E_INVALIDARG);
        for (auto value : items) {
            if (value.ValueType() != JsonValueType::Object) winrt::throw_hresult(E_INVALIDARG);
            auto item = value.GetObject();
            if (++total > kMaxItems) winrt::throw_hresult(E_INVALIDARG);
            const auto kind = item.GetNamedString(L"kind");
            if (kind == L"separator") continue;
            if (kind != L"item" && kind != L"submenu") winrt::throw_hresult(E_INVALIDARG);
            const auto id = item.GetNamedString(L"id");
            if (!ValidId(id) || !ids.insert(id).second) winrt::throw_hresult(E_INVALIDARG);
            const auto label = item.GetNamedString(L"label");
            if (label.empty()) winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(label, kMaxLabel);
            NativeJson::RequireLength(item.GetNamedString(L"shortcut", L""), kMaxShortcut);
            item.GetNamedBoolean(L"enabled", true);
            item.GetNamedBoolean(L"checked", false);
            if (kind == L"submenu") {
                auto children = item.GetNamedArray(L"items");
                if (children.Size() == 0) winrt::throw_hresult(E_INVALIDARG);
                ValidateItems(children, depth + 1, total, ids);
            } else if (item.HasKey(L"items")) {
                winrt::throw_hresult(E_INVALIDARG);
            }
        }
    }

    // The validated productMenu object, or an object with enabled == false.
    static Object Read(Object const& snapshot) {
        if (!snapshot.HasKey(L"navigation")) return Object();
        auto navigation = snapshot.GetNamedObject(L"navigation");
        if (!navigation.HasKey(L"productMenu")) return Object();
        auto menu = navigation.GetNamedObject(L"productMenu");
        if (!menu.GetNamedBoolean(L"enabled", false)) return Object();
        if (menu.Stringify().size() > kMaxModelBytes) winrt::throw_hresult(E_INVALIDARG);
        NativeJson::RequireLength(menu.GetNamedString(L"label", L""), kMaxLabel);
        NativeJson::RequireLength(menu.GetNamedString(L"tooltip", L""), kMaxLabel);
        uint32_t total = 0;
        std::set<winrt::hstring> ids;
        ValidateItems(menu.GetNamedArray(L"items"), 1, total, ids);
        return menu;
    }

    NativeProductMenu() {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        button = Markup::XamlReader::Load(
            LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" MinWidth="0" MinHeight="0" Padding="0"/>)").as<Button>();
        button.Width(ToolbarMetrics::kButton);
        button.Height(ToolbarMetrics::kButton);
        Viewbox host;
        host.Width(ToolbarMetrics::kIcon);
        host.Height(ToolbarMetrics::kIcon);
        // The stock glyph draws about 1.5 DIP above the button's centre; a 3-DIP top
        // margin moves the centred glyph down by that much, as in Dia.
        host.Margin({0, 3, 0, 0});
        host.Child(SymbolIcon(Symbol::GlobalNavigationButton));
        button.Content(host);
        button.VerticalAlignment(VerticalAlignment::Center);
        button.Visibility(Visibility::Collapsed);
        Automation::AutomationProperties::SetAutomationId(button, L"FennProductMenu");
        Automation::AutomationProperties::SetName(button, L"Menu");
        button.Click([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired() || !enabled) return;
            Open();
        });
    }
    ~NativeProductMenu() { lifetime.reset(); Close(); }

    void Close() {
        if (flyout) flyout.Hide();
        flyout = nullptr;
    }

    void Apply(Object const& next, bool visible) {
        using namespace Microsoft::UI::Xaml;
        enabled = visible && next.Size() != 0 && next.GetNamedBoolean(L"enabled", false);
        if (!enabled) {
            Close();
            model = Object();
            button.Visibility(Visibility::Collapsed);
            return;
        }
        model = next;
        button.Visibility(Visibility::Visible);
        const auto label = next.GetNamedString(L"label", L"");
        const auto tooltip = next.GetNamedString(L"tooltip", L"");
        if (!label.empty()) Automation::AutomationProperties::SetName(button, label);
        Controls::ToolTipService::SetToolTip(button, winrt::box_value(tooltip.empty() ? label : tooltip));
    }

    static uint32_t Modifiers() {
        return ((GetKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) | ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) |
               ((GetKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u) | ((GetKeyState(VK_LWIN) & 0x8000) ? 8u : 0u);
    }

    void Fill(winrt::Windows::Foundation::Collections::IVector<Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase> const& target,
              Array const& items) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        for (auto value : items) {
            auto entry = value.GetObject();
            const auto kind = entry.GetNamedString(L"kind");
            if (kind == L"separator") {
                target.Append(MenuFlyoutSeparator());
                continue;
            }
            const auto id = entry.GetNamedString(L"id");
            const bool itemEnabled = entry.GetNamedBoolean(L"enabled", true);
            const auto automationId = winrt::hstring(L"FennProductMenu-") + id;
            if (kind == L"submenu") {
                MenuFlyoutSubItem sub;
                sub.Text(entry.GetNamedString(L"label"));
                sub.IsEnabled(itemEnabled);
                Automation::AutomationProperties::SetAutomationId(sub, automationId);
                Fill(sub.Items(), entry.GetNamedArray(L"items"));
                target.Append(sub);
                continue;
            }
            const bool checkable = entry.HasKey(L"checked");
            const bool checked = entry.GetNamedBoolean(L"checked", false);
            auto invoke = [this, id, alive = std::weak_ptr<int>(lifetime)] {
                if (alive.expired() || !dispatch || !enabled) return;
                dispatch((std::wstring(L"product:") + id.c_str()).c_str(), Modifiers(), button);
            };
            MenuFlyoutItemBase base{nullptr};
            if (checkable) {
                ToggleMenuFlyoutItem toggle;
                toggle.Text(entry.GetNamedString(L"label"));
                toggle.IsChecked(checked);
                toggle.IsEnabled(itemEnabled);
                // Firefox owns the state: undo the local toggle, then wait for its republication.
                toggle.Click([checked, invoke](auto const& sender, auto const&) {
                    sender.template as<ToggleMenuFlyoutItem>().IsChecked(checked);
                    invoke();
                });
                base = toggle;
                const auto shortcut = entry.GetNamedString(L"shortcut", L"");
                if (!shortcut.empty()) toggle.KeyboardAcceleratorTextOverride(shortcut);
                Automation::AutomationProperties::SetAutomationId(toggle, automationId);
            } else {
                MenuFlyoutItem item;
                item.Text(entry.GetNamedString(L"label"));
                item.IsEnabled(itemEnabled);
                item.Click([invoke](auto const&, auto const&) { invoke(); });
                const auto shortcut = entry.GetNamedString(L"shortcut", L"");
                if (!shortcut.empty()) item.KeyboardAcceleratorTextOverride(shortcut);
                Automation::AutomationProperties::SetAutomationId(item, automationId);
                base = item;
            }
            target.Append(base);
        }
    }

    void Open() {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        if (flyout && flyout.IsOpen()) {
            flyout.Hide();
            return;
        }
        if (!enabled || !model || model.Size() == 0) return;
        flyout = MenuFlyout();
        flyout.Placement(button.FlowDirection() == FlowDirection::RightToLeft
            ? Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight
            : Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
        Fill(flyout.Items(), model.GetNamedArray(L"items"));
        flyout.ShowAt(button);
    }
};
