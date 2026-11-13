#pragma once
#include "JsonValidation.h"
#include "shell_core.h"

// WinUI objects only. ShellCore validates independent opening/revision/action
// identity; Firefox owns developer membership, tool commands and browser policy.
struct NativeDeveloperMenu {
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Button = Microsoft::UI::Xaml::Controls::Primitives::ButtonBase;
    ShellCore* core{};
    bool enabled{}, keyboardActivation{};
    uint32_t opening{};
    Button anchor{nullptr};
    Microsoft::UI::Xaml::Controls::MenuFlyout flyout{nullptr};
    std::function<void(wchar_t const*, uint32_t, Button const&)> dispatch;
    std::function<void()> afterClose;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0), openingLifetime;
    ~NativeDeveloperMenu() {
        lifetime.reset();
        Close();
    }
    static uint32_t Id(Object const& value, wchar_t const* key) {
        auto n = NativeJson::RequireRevision(value, key);
        if (!n || n > UINT32_MAX) winrt::throw_hresult(E_INVALIDARG);
        return uint32_t(n);
    }
    static Object Read(Object const& snapshot) {
        auto model = snapshot.GetNamedObject(L"navigation", Object()).GetNamedObject(L"developerMenu", Object());
        model.GetNamedBoolean(L"enabled", false);
        if (model.HasKey(L"closed") && NativeJson::RequireRevision(model, L"closed") > UINT32_MAX)
            winrt::throw_hresult(E_INVALIDARG);
        if (!model.HasKey(L"session")) return model;
        Id(model, L"session");
        Id(model, L"revision");
        NativeJson::RequireLength(model.GetNamedString(L"label"), 512);
        auto entries = model.GetNamedArray(L"items");
        if (entries.Size() > 64) winrt::throw_hresult(E_INVALIDARG);
        std::set<uint32_t> ids;
        std::set<winrt::hstring> keys;
        for (auto value : entries) {
            auto item = value.GetObject();
            if (item.GetNamedBoolean(L"separator", false)) continue;
            if (!ids.insert(Id(item, L"id")).second) winrt::throw_hresult(E_INVALIDARG);
            const auto key = item.GetNamedString(L"key");
            if (key.empty() || key.size() > 64 || !keys.insert(key).second) winrt::throw_hresult(E_INVALIDARG);
            for (wchar_t c : key)
                if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-'))
                    winrt::throw_hresult(E_INVALIDARG);
            const auto label = item.GetNamedString(L"label");
            if (label.empty()) winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(label, 512);
            item.GetNamedBoolean(L"enabled");
            item.GetNamedBoolean(L"checked");
            item.GetNamedBoolean(L"checkable");
        }
        if (model.Stringify().size() > 65536) winrt::throw_hresult(E_INVALIDARG);
        return model;
    }
    static std::vector<uint32_t> Actions(Object const& model) {
        std::vector<uint32_t> ids;
        for (auto value : model.GetNamedArray(L"items")) {
            auto item = value.GetObject();
            if (!item.GetNamedBoolean(L"separator", false) && item.GetNamedBoolean(L"enabled"))
                ids.push_back(Id(item, L"id"));
        }
        return ids;
    }
    bool Resolve(Object const& model) {
        if (!model.HasKey(L"session")) return false;
        auto ids = Actions(model);
        auto fingerprint = model.Stringify();
        bool changed{};
        winrt::check_hresult(shell_core_developer_menu_resolve(core, Id(model, L"session"), Id(model, L"revision"),
                                                               ids.data(), ids.size(),
                                                               reinterpret_cast<uint16_t const*>(fingerprint.c_str()),
                                                               fingerprint.size(), &changed) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        return changed;
    }
    void Close() {
        opening = 0;
        afterClose = {};
        openingLifetime.reset();
        if (core) shell_core_developer_menu_dismiss(core);
        if (flyout) flyout.Hide();
        flyout = nullptr;
        anchor = nullptr;
    }
    bool Owns(Microsoft::UI::Xaml::DependencyObject const& target) const {
        if (!opening || !flyout) return false;
        for (auto const& item : flyout.Items())
            if (item == target) return true;
        return false;
    }
    void Open(Button const& button) {
        if (!enabled || !core || !dispatch) return;
        if (opening) {
            auto command = L"developer-menu:dismiss:" + std::to_wstring(opening);
            dispatch(command.c_str(), 0, anchor);
            Close();
            return;
        }
        uint32_t session{};
        if (shell_core_developer_menu_open(core, &session) != SHELL_STATUS_OK || !session) return;
        opening = session;
        anchor = button;
        openingLifetime = std::make_shared<int>(0);
        dispatch((L"developer-menu:open:" + std::to_wstring(session)).c_str(), 0, button);
    }
    void Apply(Object const& model, bool visible, bool publish) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        enabled = visible && model.GetNamedBoolean(L"enabled", false);
        if (!enabled || (opening && model.GetNamedNumber(L"closed", 0) == opening)) {
            Close();
            return;
        }
        if (!publish || !opening || !model.HasKey(L"session") || Id(model, L"session") != opening) return;
        const auto session = opening, revision = Id(model, L"revision");
        auto ids = Actions(model);
        auto fingerprint = model.Stringify();
        bool changed{};
        winrt::check_hresult(shell_core_developer_menu_accept(core, session, revision, ids.data(), ids.size(),
                                                              reinterpret_cast<uint16_t const*>(fingerprint.c_str()),
                                                              fingerprint.size(), &changed) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        if (!changed) return;
        // An developer/command mutation dismisses the opening in Firefox. Never
        // clear/repopulate a MenuFlyout while its previous instance is closing.
        if (flyout) return;
        flyout = MenuFlyout();
        flyout.ShouldConstrainToRootBounds(false);
        flyout.Placement(anchor.FlowDirection() == FlowDirection::RightToLeft
                             ? Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft
                             : Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
        Automation::AutomationProperties::SetName(flyout, model.GetNamedString(L"label"));
        flyout.Closed([this, session, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired() || opening != session) return;
            auto send = std::move(afterClose);
            afterClose = {};
            opening = 0;
            shell_core_developer_menu_dismiss(core);
            if (send)
                anchor.DispatcherQueue().TryEnqueue(std::move(send));
            else if (dispatch)
                dispatch((L"developer-menu:dismiss:" + std::to_wstring(session)).c_str(), 0, anchor);
            flyout = nullptr;
            anchor = nullptr;
        });
        for (auto value : model.GetNamedArray(L"items")) {
            auto entry = value.GetObject();
            if (entry.GetNamedBoolean(L"separator", false)) {
                flyout.Items().Append(MenuFlyoutSeparator());
                continue;
            }
            auto id = Id(entry, L"id");
            auto key = entry.GetNamedString(L"key");
            auto modifiers = std::make_shared<uint32_t>(0);
            auto activate = [this, session, revision, id, modifiers, alive = std::weak_ptr<int>(lifetime),
                             current = std::weak_ptr<int>(openingLifetime)](auto const&, auto const&) {
                if (alive.expired() || current.expired() || opening != session) return;
                bool accepted{};
                if (shell_core_developer_menu_activate(core, session, revision, id, &accepted) != SHELL_STATUS_OK ||
                    !accepted)
                    return;
                auto command = L"developer-menu:invoke:" + std::to_wstring(session) + L":" + std::to_wstring(revision) +
                               L":" + std::to_wstring(id);
                afterClose = [this, command, source = anchor, alive, current, value = *modifiers] {
                    if (!alive.expired() && !current.expired() && enabled && dispatch)
                        dispatch(command.c_str(), value, source);
                };
                flyout.Hide();
            };
            MenuFlyoutItemBase base{nullptr};
            if (entry.GetNamedBoolean(L"checkable")) {
                ToggleMenuFlyoutItem item;
                item.Text(entry.GetNamedString(L"label"));
                item.IsChecked(entry.GetNamedBoolean(L"checked"));
                item.IsEnabled(entry.GetNamedBoolean(L"enabled"));
                item.Click(activate);
                base = item;
            } else {
                MenuFlyoutItem item;
                item.Text(entry.GetNamedString(L"label"));
                item.IsEnabled(entry.GetNamedBoolean(L"enabled"));
                item.Click(activate);
                base = item;
            }
            base.AddHandler(UIElement::PointerPressedEvent(),
                            winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                                [this, modifiers, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& args) {
                                    if (!alive.expired()) {
                                        keyboardActivation = false;
                                        *modifiers = uint32_t(args.KeyModifiers()) & 15u;
                                    }
                                })),
                            true);
            base.PreviewKeyDown([this, modifiers, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (!alive.expired()) {
                    keyboardActivation = true;
                    *modifiers =
                        ((GetKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) | ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) |
                        ((GetKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u) | ((GetKeyState(VK_LWIN) & 0x8000) ? 8u : 0u);
                }
            });
            Automation::AutomationProperties::SetAutomationId(base, L"FennDeveloperMenu-" + key);
            flyout.Items().Append(base);
        }
        flyout.ShowAt(anchor);
    }
};
