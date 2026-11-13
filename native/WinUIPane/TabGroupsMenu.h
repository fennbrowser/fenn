#pragma once
#include "JsonValidation.h"
#include "shell_core.h"

// WinUI controls only. Firefox owns group inventory, eligibility and operations;
// ShellCore owns opening/revision identity and single-use activation.
struct NativeTabGroupsMenu {
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Button = Microsoft::UI::Xaml::Controls::Primitives::ButtonBase;
    ShellCore* core{};
    bool enabled{}, keyboardActivation{};
    uint32_t opening{};
    Button anchor{nullptr};
    Microsoft::UI::Xaml::Controls::Flyout flyout{nullptr};
    std::vector<Microsoft::UI::Xaml::Controls::MenuFlyout> contexts;
    std::function<void(wchar_t const*, uint32_t, Button const&)> dispatch;
    std::function<void()> afterClose;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0), openingLifetime;
    ~NativeTabGroupsMenu() {
        lifetime.reset();
        Close();
    }
    static uint32_t Id(Object const& value, wchar_t const* key) {
        auto n = NativeJson::RequireRevision(value, key);
        if (!n || n > UINT32_MAX) winrt::throw_hresult(E_INVALIDARG);
        return uint32_t(n);
    }
    static Object Read(Object const& snapshot) {
        auto model = snapshot.GetNamedObject(L"navigation", Object()).GetNamedObject(L"tabGroupsMenu", Object());
        model.GetNamedBoolean(L"enabled", false);
        if (model.HasKey(L"closed") && NativeJson::RequireRevision(model, L"closed") > UINT32_MAX)
            winrt::throw_hresult(E_INVALIDARG);
        if (!model.HasKey(L"session")) return model;
        Id(model, L"session");
        Id(model, L"revision");
        for (auto key : {L"label", L"emptyTitle", L"emptyDescription"})
            NativeJson::RequireLength(model.GetNamedString(key), 512);
        std::set<uint32_t> ids;
        std::set<winrt::hstring> keys;
        auto command = [&](Object const& entry) {
            if (!ids.insert(Id(entry, L"id")).second) winrt::throw_hresult(E_INVALIDARG);
            auto key = entry.GetNamedString(L"key"), label = entry.GetNamedString(L"label");
            if (key.empty() || key.size() > 64 || !keys.insert(key).second || label.empty())
                winrt::throw_hresult(E_INVALIDARG);
            for (wchar_t c : key)
                if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-'))
                    winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(label, 512);
            entry.GetNamedBoolean(L"enabled");
        };
        command(model.GetNamedObject(L"create"));
        if (model.HasKey(L"more")) command(model.GetNamedObject(L"more"));
        auto items = model.GetNamedArray(L"items");
        if (items.Size() > 40) winrt::throw_hresult(E_INVALIDARG);
        for (auto value : items) {
            auto item = value.GetObject();
            command(item);
            auto context = item.GetNamedArray(L"context");
            if (context.Size() > 4) winrt::throw_hresult(E_INVALIDARG);
            for (auto child : context) command(child.GetObject());
        }
        if (ids.size() > 256 || model.Stringify().size() > 65536) winrt::throw_hresult(E_INVALIDARG);
        return model;
    }
    static std::vector<uint32_t> Actions(Object const& model) {
        std::vector<uint32_t> ids;
        auto add = [&](Object const& item) {
            if (item.GetNamedBoolean(L"enabled")) ids.push_back(Id(item, L"id"));
        };
        add(model.GetNamedObject(L"create"));
        if (model.HasKey(L"more")) add(model.GetNamedObject(L"more"));
        for (auto value : model.GetNamedArray(L"items")) {
            auto item = value.GetObject();
            add(item);
            for (auto child : item.GetNamedArray(L"context")) add(child.GetObject());
        }
        return ids;
    }
    bool Resolve(Object const& model) {
        if (!model.HasKey(L"session")) return false;
        auto ids = Actions(model);
        auto fingerprint = model.Stringify();
        bool changed{};
        winrt::check_hresult(shell_core_tab_groups_menu_resolve(core, Id(model, L"session"), Id(model, L"revision"),
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
        if (core) shell_core_tab_groups_menu_dismiss(core);
        for (auto context : contexts) context.Hide();
        contexts.clear();
        if (flyout) flyout.Hide();
        flyout = nullptr;
        anchor = nullptr;
    }
    void Open(Button const& button) {
        if (!enabled || !core || !dispatch) return;
        if (opening) {
            dispatch((L"tab-groups-menu:dismiss:" + std::to_wstring(opening)).c_str(), 0, anchor);
            Close();
            return;
        }
        uint32_t session{};
        if (shell_core_tab_groups_menu_open(core, &session) != SHELL_STATUS_OK || !session) return;
        opening = session;
        anchor = button;
        openingLifetime = std::make_shared<int>(0);
        dispatch((L"tab-groups-menu:open:" + std::to_wstring(session)).c_str(), 0, button);
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
        winrt::check_hresult(shell_core_tab_groups_menu_accept(core, session, revision, ids.data(), ids.size(),
                                                               reinterpret_cast<uint16_t const*>(fingerprint.c_str()),
                                                               fingerprint.size(), &changed) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        if (!changed || flyout) return;
        const auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
        auto invoke = [this, session, revision, alive, current](uint32_t id) {
            if (alive.expired() || current.expired() || opening != session) return;
            bool accepted{};
            if (shell_core_tab_groups_menu_activate(core, session, revision, id, &accepted) != SHELL_STATUS_OK ||
                !accepted)
                return;
            auto command = L"tab-groups-menu:invoke:" + std::to_wstring(session) + L":" + std::to_wstring(revision) +
                           L":" + std::to_wstring(id);
            afterClose = [this, command, source = anchor, alive, current] {
                if (!alive.expired() && !current.expired() && enabled && dispatch) dispatch(command.c_str(), 0, source);
            };
            for (auto context : contexts) context.Hide();
            flyout.Hide();
        };
        flyout = Flyout();
        flyout.ShouldConstrainToRootBounds(false);
        flyout.Placement(anchor.FlowDirection() == FlowDirection::RightToLeft
                             ? Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft
                             : Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
        StackPanel content;
        content.Width(320);
        content.Spacing(4);
        Automation::AutomationProperties::SetAutomationId(content, L"FennTabGroupsMenu");
        auto text = [&](winrt::hstring const& value, bool heading) {
            TextBlock block;
            block.Text(value);
            block.TextWrapping(TextWrapping::Wrap);
            if (heading) {
                block.FontSize(18);
                block.FontWeight({600});
            }
            content.Children().Append(block);
        };
        text(model.GetNamedString(L"label"), true);
        if (!model.GetNamedString(L"emptyTitle").empty()) {
            text(model.GetNamedString(L"emptyTitle"), false);
            text(model.GetNamedString(L"emptyDescription"), false);
        }
        auto input = [this, alive](UIElement const& element) {
            element.AddHandler(UIElement::PointerPressedEvent(),
                               winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                                   [this, alive](auto const&, auto const&) {
                                       if (!alive.expired()) keyboardActivation = false;
                                   })),
                               true);
            element.PreviewKeyDown([this, alive](auto const&, auto const&) {
                if (!alive.expired()) keyboardActivation = true;
            });
        };
        auto makeButton = [&](Object const& entry) {
            auto button =
                Markup::XamlReader::Load(
                    LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" HorizontalAlignment="Stretch" HorizontalContentAlignment="Left"/>)")
                    .as<Controls::Button>();
            TextBlock label;
            label.Text(entry.GetNamedString(L"label"));
            label.TextWrapping(TextWrapping::Wrap);
            button.Content(label);
            button.IsEnabled(entry.GetNamedBoolean(L"enabled"));
            Automation::AutomationProperties::SetName(button, entry.GetNamedString(L"label"));
            Automation::AutomationProperties::SetAutomationId(button,
                                                              L"FennTabGroupsMenu-" + entry.GetNamedString(L"key"));
            input(button);
            button.Click([invoke, id = Id(entry, L"id")](auto const&, auto const&) { invoke(id); });
            content.Children().Append(button);
            return button;
        };
        auto create = makeButton(model.GetNamedObject(L"create"));
        for (auto value : model.GetNamedArray(L"items")) {
            auto entry = value.GetObject();
            auto button = makeButton(entry);
            MenuFlyout menu;
            for (auto child : entry.GetNamedArray(L"context")) {
                auto command = child.GetObject();
                MenuFlyoutItem item;
                item.Text(command.GetNamedString(L"label"));
                item.IsEnabled(command.GetNamedBoolean(L"enabled"));
                Automation::AutomationProperties::SetAutomationId(
                    item, L"FennTabGroupsMenu-" + command.GetNamedString(L"key"));
                input(item);
                item.Click([invoke, id = Id(command, L"id")](auto const&, auto const&) { invoke(id); });
                menu.Items().Append(item);
            }
            button.ContextFlyout(menu);
            contexts.push_back(menu);
        }
        if (model.HasKey(L"more")) makeButton(model.GetNamedObject(L"more"));
        ScrollViewer scroll;
        scroll.MaxHeight(520);
        scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.Content(content);
        flyout.Content(scroll);
        flyout.Opened([create, alive, current](auto const&, auto const&) {
            if (!alive.expired() && !current.expired()) create.Focus(FocusState::Programmatic);
        });
        flyout.Closed([this, session, alive](auto const&, auto const&) {
            if (alive.expired() || opening != session) return;
            auto send = std::move(afterClose);
            afterClose = {};
            opening = 0;
            shell_core_tab_groups_menu_dismiss(core);
            if (send)
                anchor.DispatcherQueue().TryEnqueue(std::move(send));
            else if (dispatch)
                dispatch((L"tab-groups-menu:dismiss:" + std::to_wstring(session)).c_str(), 0, anchor);
            contexts.clear();
            flyout = nullptr;
            anchor = nullptr;
        });
        flyout.ShowAt(anchor);
    }
};
