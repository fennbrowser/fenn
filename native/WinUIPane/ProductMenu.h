#pragma once
#include <functional>
#include <memory>
#include <set>
#include <string>
#include "JsonValidation.h"
#include "ToolbarMetrics.h"
#include "PackageUpdates.h"

// Firefox owns menu contents, state and browser commands. The native editor
// and Windows servicing host handle their own UI; other items report an id.
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
    Microsoft::UI::Xaml::Controls::MenuFlyoutItem updateItem{nullptr};
    std::shared_ptr<NativePackageUpdates> updates = std::make_shared<NativePackageUpdates>();
    bool enabled{};
    bool keepOpenForCheck{};
    std::function<void(wchar_t const*, uint32_t, Button const&)> dispatch;
    // Items the native toolbar handles itself instead of reporting to Firefox.
    std::function<void()> customize;
    std::function<bool()> closeEditor;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    struct Approval { winrt::hstring token; winrt::hstring status; };
    std::shared_ptr<Approval> approval = std::make_shared<Approval>();

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
        if (menu.HasKey(L"updateInstall")) {
            auto grant = menu.GetNamedObject(L"updateInstall");
            auto token = grant.GetNamedString(L"token");
            if (!ValidId(token)) winrt::throw_hresult(E_INVALIDARG);
            auto status = grant.GetNamedString(L"status");
            if (status != L"pending" && status != L"ready" && status != L"failed") winrt::throw_hresult(E_INVALIDARG);
        }
        NativeJson::RequireLength(menu.GetNamedString(L"label", L""), kMaxLabel);
        NativeJson::RequireLength(menu.GetNamedString(L"tooltip", L""), kMaxLabel);
        uint32_t total = 0;
        std::set<winrt::hstring> ids;
        ValidateItems(menu.GetNamedArray(L"items"), 1, total, ids);
        if (ids.count(L"check-updates") && !menu.HasKey(L"updateLabels")) winrt::throw_hresult(E_INVALIDARG);
        if (menu.HasKey(L"updateLabels")) {
            auto labels = menu.GetNamedObject(L"updateLabels");
            for (auto key : {L"title", L"checking", L"current", L"available-store", L"available-feed",
                             L"unavailable", L"failed", L"open-store", L"open-feed", L"close",
                             L"downloading", L"ready", L"install", L"installing"}) {
                const auto label = labels.GetNamedString(key);
                if (label.empty()) winrt::throw_hresult(E_INVALIDARG);
                NativeJson::RequireLength(label, kMaxLabel);
            }
        }
        return menu;
    }

    NativePackageUpdates::Labels UpdateLabels() const {
        NativePackageUpdates::Labels labels;
        if (model && model.HasKey(L"updateLabels"))
            for (auto const& pair : model.GetNamedObject(L"updateLabels"))
                if (pair.Value().ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                    labels.emplace(std::wstring(pair.Key()), pair.Value().GetString());
        return labels;
    }
    void RefreshUpdateItem() {
        if (!updateItem || !flyout || !model) return;
        auto labels = UpdateLabels();
        const auto state = updates->current;
        if (updates->installing) updateItem.Text(NativePackageUpdates::Text(labels, L"installing"));
        else if (state && state->ready) updateItem.Text(NativePackageUpdates::Text(labels,
            state->status == L"failed" ? L"failed" : L"install"));
        else if (updates->checking && state && state->status == L"downloading")
            updateItem.Text(NativePackageUpdates::Text(labels, L"downloading"));
        else if (updates->checking && updates->manualCheck) updateItem.Text(NativePackageUpdates::Text(labels, L"checking"));
        else if (updates->showingResult && state) updateItem.Text(NativePackageUpdates::Text(labels, state->status.c_str()));
        else {
            for (auto entry : model.GetNamedArray(L"items")) {
                auto item = entry.GetObject();
                if (item.GetNamedString(L"id", L"") == L"check-updates") updateItem.Text(item.GetNamedString(L"label"));
            }
        }
        bool offered = false;
        for (auto entry : model.GetNamedArray(L"items")) {
            auto item = entry.GetObject();
            if (item.GetNamedString(L"id", L"") == L"check-updates") offered = item.GetNamedBoolean(L"enabled", true);
        }
        const bool busy = updates->checking && (updates->manualCheck || (state && state->status == L"downloading"));
        updateItem.IsEnabled(enabled && offered && !updates->installing && !busy && !updates->showingResult);
    }

    NativeProductMenu() {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        updates->changed = [this, alive = std::weak_ptr<int>(lifetime)] {
            if (!alive.expired()) RefreshUpdateItem();
        };
        updates->prepare = [this, alive = std::weak_ptr<int>(lifetime), grant = approval]() -> NativePackageUpdates::Action {
            if (alive.expired() || !enabled || !dispatch) winrt::throw_hresult(E_ABORT);
            static uint64_t serial = 0; // UI thread only; unique across browser windows.
            grant->token = winrt::to_hstring(GetTickCount64()) + L"-" + winrt::to_hstring(++serial);
            grant->status = L"pending";
            dispatch((std::wstring(L"product:prepare-update-") + grant->token.c_str()).c_str(), 0, button);
            winrt::apartment_context ui;
            const auto deadline = GetTickCount64() + 60000;
            while (!alive.expired() && grant->status == L"pending" && GetTickCount64() < deadline) {
                co_await winrt::resume_after(std::chrono::milliseconds(25));
                co_await ui;
            }
            if (alive.expired() || grant->status != L"ready") winrt::throw_hresult(E_ABORT);
            grant->status = L"consumed";
        };
        updates->installFailed = [this, alive = std::weak_ptr<int>(lifetime), grant = approval] {
            if (!alive.expired() && dispatch && !grant->token.empty())
                dispatch((std::wstring(L"product:update-failed-") + grant->token.c_str()).c_str(), 0, button);
            grant->status = L"failed";
        };
        button = Markup::XamlReader::Load(
            LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" MinWidth="0" MinHeight="0" Padding="0"/>)").as<Button>();
        button.Width(ToolbarMetrics::kButton);
        button.Height(ToolbarMetrics::kButton);
        Viewbox host;
        host.Width(ToolbarMetrics::kIcon);
        host.Height(ToolbarMetrics::kIcon);
        host.Child(SymbolIcon(Symbol::GlobalNavigationButton));
        button.Content(host);
        button.VerticalAlignment(VerticalAlignment::Center);
        button.Visibility(Visibility::Collapsed);
        Automation::AutomationProperties::SetAutomationId(button, L"FennProductMenu");
        Automation::AutomationProperties::SetName(button, L"Menu");
        button.Click([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired() || !enabled) return;
            if (closeEditor && closeEditor()) return;
            Open();
        });
    }
    ~NativeProductMenu() { lifetime.reset(); updates->Cancel(); Close(); }

    void Close() {
        keepOpenForCheck = false;
        if (flyout) flyout.Hide();
        flyout = nullptr;
        updateItem = nullptr;
    }

    void Apply(Object const& next, bool visible) {
        using namespace Microsoft::UI::Xaml;
        enabled = visible && next.Size() != 0 && next.GetNamedBoolean(L"enabled", false);
        if (!enabled) {
            updates->Cancel();
            Close();
            model = Object();
            button.Visibility(Visibility::Collapsed);
            return;
        }
        model = next;
        if (next.HasKey(L"updateInstall") && approval->status == L"pending") {
            auto grant = next.GetNamedObject(L"updateInstall");
            if (grant.GetNamedString(L"token") == approval->token) approval->status = grant.GetNamedString(L"status");
        }
        button.Visibility(Visibility::Visible);
        const auto label = next.GetNamedString(L"label", L"");
        const auto tooltip = next.GetNamedString(L"tooltip", L"");
        if (!label.empty()) Automation::AutomationProperties::SetName(button, label);
        Controls::ToolTipService::SetToolTip(button, winrt::box_value(tooltip.empty() ? label : tooltip));
        RefreshUpdateItem();
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
                if (alive.expired() || !enabled) return;
                if (id == L"customize-toolbar") {
                    if (customize) customize();
                    return;
                }
                if (id == L"check-updates") {
                    if (!model.HasKey(L"updateLabels")) return;
                    if (updates->showingResult || updates->installing ||
                        (updates->checking && (updates->manualCheck || (updates->current && updates->current->status == L"downloading")))) return;
                    bool offered = false;
                    for (auto item : model.GetNamedArray(L"items")) {
                        auto current = item.GetObject();
                        if (current.GetNamedString(L"id", L"") == id && current.GetNamedBoolean(L"enabled", true)) offered = true;
                    }
                    if (!offered) return;
                    if (!updates->current || !updates->current->ready) {
                        keepOpenForCheck = true;
                        NativePackageUpdates::Probe(updates, true);
                        return;
                    }
                    auto labels = UpdateLabels();
                    const auto epoch = updates->generation;
                    button.DispatcherQueue().TryEnqueue([state = updates, alive, epoch, labels = std::move(labels)] {
                        if (alive.expired() || state->generation != epoch) return;
                        if (state->current && state->current->ready) NativePackageUpdates::InstallReady(state);
                    });
                    return;
                }
                if (!dispatch) return;
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
                if (id == L"check-updates") updateItem = item;
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
        keepOpenForCheck = false;
        flyout.Closing([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& args) {
            if (!alive.expired() && keepOpenForCheck) {
                keepOpenForCheck = false;
                args.Cancel(true);
            }
        });
        flyout.Placement(button.FlowDirection() == FlowDirection::RightToLeft
            ? Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight
            : Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
        Fill(flyout.Items(), model.GetNamedArray(L"items"));
        RefreshUpdateItem();
        flyout.ShowAt(button);
        if (model.HasKey(L"updateLabels")) NativePackageUpdates::Probe(updates);
    }
};
