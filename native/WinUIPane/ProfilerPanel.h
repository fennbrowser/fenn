#pragma once
#include "JsonValidation.h"
#include "shell_core.h"

// WinUI controls only. Firefox publishes recording state and owns every operation.
struct NativeProfilerPanel {
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Button = Microsoft::UI::Xaml::Controls::Primitives::ButtonBase;
    ShellCore* core{};
    bool enabled{}, applying{}, keyboardActivation{};
    uint32_t opening{}, revision{}, focusAfterAck{};
    Button anchor{nullptr};
    Microsoft::UI::Xaml::Controls::Flyout flyout{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock status{nullptr}, description{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel settings{nullptr};
    Microsoft::UI::Xaml::Controls::DropDownButton presets{nullptr};
    Microsoft::UI::Xaml::Controls::MenuFlyout presetMenu{nullptr};
    std::map<uint32_t, Microsoft::UI::Xaml::Controls::RadioMenuFlyoutItem> presetItems;
    std::array<Microsoft::UI::Xaml::Controls::Button, 5> buttons{nullptr, nullptr, nullptr, nullptr, nullptr};
    std::array<Microsoft::UI::Xaml::Input::KeyboardAccelerator, 3> accelerators{nullptr, nullptr, nullptr};
    std::function<void(wchar_t const*, uint32_t, Button const&)> dispatch;
    std::function<void()> afterClose, releaseFocus;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0), openingLifetime;
    ~NativeProfilerPanel() {
        lifetime.reset();
        Close();
    }
    static uint32_t Id(Object const& m, wchar_t const* key) {
        auto n = NativeJson::RequireRevision(m, key);
        if (!n || n > UINT32_MAX) winrt::throw_hresult(E_INVALIDARG);
        return uint32_t(n);
    }
    static Object Read(Object const& snapshot) {
        auto m = snapshot.GetNamedObject(L"navigation", Object()).GetNamedObject(L"profilerPanel", Object());
        m.GetNamedBoolean(L"enabled", false);
        if (m.HasKey(L"closed") && NativeJson::RequireRevision(m, L"closed") > UINT32_MAX)
            winrt::throw_hresult(E_INVALIDARG);
        if (!m.HasKey(L"session")) return m;
        Id(m, L"session");
        Id(m, L"revision");
        m.GetNamedBoolean(L"active");
        m.GetNamedBoolean(L"paused");
        for (auto key : {L"title", L"settings", L"description", L"status", L"infoLabel", L"infoTitle", L"info"}) {
            auto text = m.GetNamedString(key);
            NativeJson::RequireLength(text, 2048);
            if (text.empty() && wcscmp(key, L"description") && wcscmp(key, L"status"))
                winrt::throw_hresult(E_INVALIDARG);
        }
        std::set<winrt::hstring> keys;
        std::set<uint32_t> ids;
        auto options = m.GetNamedArray(L"options");
        if (!options.Size() || options.Size() > 32) winrt::throw_hresult(E_INVALIDARG);
        for (auto v : options) {
            auto o = v.GetObject();
            auto id = Id(o, L"id");
            auto key = o.GetNamedString(L"key"), label = o.GetNamedString(L"label");
            if (id < 100 || id > 131 || !ids.insert(id).second || key.empty() || key.size() > 64 ||
                !keys.insert(key).second || label.empty())
                winrt::throw_hresult(E_INVALIDARG);
            for (auto c : key)
                if (!((c >= L'a' && c <= L'z') || c == L'-')) winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(label, 512);
        }
        if (!keys.count(m.GetNamedString(L"selected"))) winrt::throw_hresult(E_INVALIDARG);
        auto actions = m.GetNamedArray(L"actions");
        if (actions.Size() != 5) winrt::throw_hresult(E_INVALIDARG);
        const wchar_t* names[] = {L"start", L"discard", L"capture", L"settings", L"help"};
        for (uint32_t i = 0; i < 5; i++) {
            auto a = actions.GetAt(i).GetObject();
            if (Id(a, L"id") != i + 1 || a.GetNamedString(L"key") != names[i] || a.GetNamedString(L"label").empty())
                winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(a.GetNamedString(L"label"), 512);
            NativeJson::RequireLength(a.GetNamedString(L"shortcut"), 128);
            a.GetNamedBoolean(L"shown");
            a.GetNamedBoolean(L"enabled");
            a.GetNamedBoolean(L"shortcutEnabled");
        }
        if (m.Stringify().size() > 32768) winrt::throw_hresult(E_INVALIDARG);
        return m;
    }
    static std::vector<uint32_t> Actions(Object const& m) {
        std::vector<uint32_t> ids;
        for (auto v : m.GetNamedArray(L"actions")) {
            auto a = v.GetObject();
            if (a.GetNamedBoolean(L"shown") && a.GetNamedBoolean(L"enabled")) ids.push_back(Id(a, L"id"));
        }
        if (!m.GetNamedBoolean(L"active"))
            for (auto v : m.GetNamedArray(L"options")) ids.push_back(Id(v.GetObject(), L"id"));
        return ids;
    }
    bool Resolve(Object const& m) {
        if (!m.HasKey(L"session")) return false;
        auto ids = Actions(m);
        auto fp = m.Stringify();
        bool changed{};
        winrt::check_hresult(shell_core_profiler_panel_resolve(
                                 core, Id(m, L"session"), Id(m, L"revision"), ids.data(), ids.size(),
                                 reinterpret_cast<uint16_t const*>(fp.c_str()), fp.size(), &changed) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        return changed;
    }
    void Close() {
        opening = revision = focusAfterAck = 0;
        afterClose = {};
        openingLifetime.reset();
        if (core) shell_core_profiler_panel_dismiss(core);
        if (presetMenu) presetMenu.Hide();
        presetMenu = nullptr;
        presetItems.clear();
        if (flyout) flyout.Hide();
        flyout = nullptr;
        anchor = nullptr;
        presets = nullptr;
        settings = nullptr;
        status = description = nullptr;
        buttons.fill(nullptr);
        accelerators.fill(nullptr);
    }
    void Open(Button const& source) {
        if (!enabled || !core || !dispatch) return;
        if (opening) {
            dispatch((L"profiler-panel:dismiss:" + std::to_wstring(opening)).c_str(), 0, anchor);
            Close();
            return;
        }
        if (shell_core_profiler_panel_open(core, &opening) != SHELL_STATUS_OK || !opening) return;
        anchor = source;
        openingLifetime = std::make_shared<int>(0);
        dispatch((L"profiler-panel:open:" + std::to_wstring(opening)).c_str(), 0, source);
    }
    void Send(uint32_t id) {
        const bool retained = id < 3 || id >= 100;
        bool accepted{};
        if (!opening || !dispatch || applying ||
            shell_core_profiler_panel_activate(core, opening, revision, id, retained, &accepted) != SHELL_STATUS_OK ||
            !accepted)
            return;
        auto command = L"profiler-panel:invoke:" + std::to_wstring(opening) + L":" + std::to_wstring(revision) + L":" +
                       std::to_wstring(id);
        if (retained) {
            if (id < 3) {
                focusAfterAck = id == 1 ? 2 : 1;
                presets.IsEnabled(false);
            }
            for (auto b : buttons) b.IsEnabled(false);
            dispatch(command.c_str(), 0, anchor);
            return;
        }
        auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
        afterClose = [this, command, source = anchor, alive, current] {
            if (!alive.expired() && !current.expired() && dispatch) {
                if (releaseFocus) releaseFocus();
                dispatch(command.c_str(), 0, source);
            }
        };
        flyout.Hide();
    }
    void Apply(Object const& m, bool visible, bool publish) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        enabled = visible && m.GetNamedBoolean(L"enabled", false);
        if (!enabled || (opening && m.GetNamedNumber(L"closed", 0) == opening)) {
            Close();
            return;
        }
        if (!publish || !opening || !m.HasKey(L"session") || Id(m, L"session") != opening) return;
        auto ids = Actions(m);
        auto fp = m.Stringify();
        bool changed{};
        winrt::check_hresult(shell_core_profiler_panel_accept(core, opening, Id(m, L"revision"), ids.data(), ids.size(),
                                                              reinterpret_cast<uint16_t const*>(fp.c_str()), fp.size(),
                                                              &changed) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        if (!changed) return;
        revision = Id(m, L"revision");
        applying = true;
        auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
        const bool fresh = !flyout;
        if (fresh) {
            flyout = Flyout();
            flyout.ShouldConstrainToRootBounds(false);
            flyout.Placement(anchor.FlowDirection() == FlowDirection::RightToLeft
                                 ? Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft
                                 : Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
            StackPanel panel;
            panel.Width(340);
            panel.Spacing(12);
            TextBlock title;
            title.Text(m.GetNamedString(L"title"));
            title.FontSize(18);
            title.FontWeight({600});
            panel.Children().Append(title);
            status = TextBlock();
            status.TextWrapping(TextWrapping::Wrap);
            Automation::AutomationProperties::SetAutomationId(status, L"FennProfiler-status");
            Automation::AutomationProperties::SetLiveSetting(status, Automation::Peers::AutomationLiveSetting::Polite);
            panel.Children().Append(status);
            settings = StackPanel();
            settings.Spacing(8);
            TextBlock settingsLabel;
            settingsLabel.Text(m.GetNamedString(L"settings"));
            settings.Children().Append(settingsLabel);
            presets = DropDownButton();
            presets.HorizontalAlignment(HorizontalAlignment::Stretch);
            Automation::AutomationProperties::SetName(presets, m.GetNamedString(L"settings"));
            Automation::AutomationProperties::SetAutomationId(presets, L"FennProfiler-presets");
            presetMenu = MenuFlyout();
            presetMenu.ShouldConstrainToRootBounds(false);
            for (auto v : m.GetNamedArray(L"options")) {
                auto o = v.GetObject();
                auto id = Id(o, L"id");
                RadioMenuFlyoutItem option;
                option.Text(o.GetNamedString(L"label"));
                option.GroupName(L"FennProfilerPreset-" + std::to_wstring(reinterpret_cast<uintptr_t>(this)) + L"-" +
                                 std::to_wstring(opening));
                Automation::AutomationProperties::SetAutomationId(option,
                                                                  L"FennProfiler-preset-" + o.GetNamedString(L"key"));
                option.Click([this, alive, current, id](auto const&, auto const&) {
                    if (!alive.expired() && !current.expired()) Send(id);
                });
                presetItems.emplace(id, option);
                presetMenu.Items().Append(option);
            }
            presets.Flyout(presetMenu);
            settings.Children().Append(presets);
            description = TextBlock();
            description.TextWrapping(TextWrapping::Wrap);
            Automation::AutomationProperties::SetAutomationId(description, L"FennProfiler-description");
            settings.Children().Append(description);
            panel.Children().Append(settings);
            auto makeButton = [&](Object const& a) {
                auto id = Id(a, L"id");
                Controls::Button b =
                    Markup::XamlReader::Load(
                        LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" HorizontalAlignment="Stretch" HorizontalContentAlignment="Left"/>)")
                        .as<Controls::Button>();
                b.Content(winrt::box_value(a.GetNamedString(L"label")));
                Automation::AutomationProperties::SetAutomationId(b, L"FennProfiler-" + a.GetNamedString(L"key"));
                Automation::AutomationProperties::SetHelpText(b, a.GetNamedString(L"shortcut"));
                ToolTipService::SetToolTip(b, winrt::box_value(a.GetNamedString(L"shortcut")));
                b.Click([this, alive, current, id](auto const&, auto const&) {
                    if (!alive.expired() && !current.expired()) Send(id);
                });
                if (id <= 3) {
                    Microsoft::UI::Xaml::Input::KeyboardAccelerator accelerator;
                    accelerator.Key(id == 3 ? Windows::System::VirtualKey::Number2
                                            : Windows::System::VirtualKey::Number1);
                    accelerator.Modifiers(
                        Windows::System::VirtualKeyModifiers(uint32_t(Windows::System::VirtualKeyModifiers::Control) |
                                                             uint32_t(Windows::System::VirtualKeyModifiers::Shift)));
                    accelerator.ScopeOwner(panel);
                    accelerator.IsEnabled(false);
                    accelerator.Invoked([this, alive, current, id](auto const&, auto const& args) {
                        if (!alive.expired() && !current.expired()) {
                            args.Handled(true);
                            keyboardActivation = true;
                            Send(id);
                        }
                    });
                    b.KeyboardAccelerators().Append(accelerator);
                    accelerators[id - 1] = accelerator;
                }
                buttons[id - 1] = b.as<Controls::Button>();
                return b;
            };
            for (auto v : m.GetNamedArray(L"actions")) {
                auto a = v.GetObject();
                if (Id(a, L"id") != 5) panel.Children().Append(makeButton(a));
            }
            Expander info;
            info.HorizontalAlignment(HorizontalAlignment::Stretch);
            info.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            info.Header(winrt::box_value(m.GetNamedString(L"infoLabel")));
            Automation::AutomationProperties::SetAutomationId(info, L"FennProfiler-info");
            StackPanel content;
            content.Spacing(8);
            TextBlock heading;
            heading.Text(m.GetNamedString(L"infoTitle"));
            heading.TextWrapping(TextWrapping::Wrap);
            content.Children().Append(heading);
            TextBlock text;
            text.Text(m.GetNamedString(L"info"));
            text.TextWrapping(TextWrapping::Wrap);
            content.Children().Append(text);
            content.Children().Append(makeButton(m.GetNamedArray(L"actions").GetAt(4).GetObject()));
            info.Content(content);
            panel.Children().Append(info);
            ScrollViewer scroll;
            scroll.MaxHeight(600);
            scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            scroll.Content(panel);
            flyout.Content(scroll);
            flyout.Opened([this, alive, current](auto const&, auto const&) {
                if (!alive.expired() && !current.expired()) {
                    if (settings.Visibility() == Visibility::Visible)
                        presets.Focus(FocusState::Programmatic);
                    else
                        buttons[1].Focus(FocusState::Programmatic);
                }
            });
            const auto session = opening;
            flyout.Closed([this, session, alive](auto const&, auto const&) {
                if (alive.expired() || session != opening) return;
                auto send = std::move(afterClose);
                opening = revision = 0;
                shell_core_profiler_panel_dismiss(core);
                if (send)
                    anchor.DispatcherQueue().TryEnqueue(std::move(send));
                else if (dispatch)
                    dispatch((L"profiler-panel:dismiss:" + std::to_wstring(session)).c_str(), 0, anchor);
                flyout = nullptr;
                presetMenu = nullptr;
                presetItems.clear();
                anchor = nullptr;
                presets = nullptr;
                settings = nullptr;
                status = description = nullptr;
                buttons.fill(nullptr);
                accelerators.fill(nullptr);
            });
        }
        status.Text(m.GetNamedString(L"status"));
        status.Visibility(status.Text().empty() ? Visibility::Collapsed : Visibility::Visible);
        settings.Visibility(m.GetNamedBoolean(L"active") ? Visibility::Collapsed : Visibility::Visible);
        presets.IsEnabled(!m.GetNamedBoolean(L"active"));
        for (auto v : m.GetNamedArray(L"options")) {
            auto o = v.GetObject();
            const bool selected = o.GetNamedString(L"key") == m.GetNamedString(L"selected");
            auto item = presetItems.at(Id(o, L"id"));
            item.IsChecked(selected);
            if (selected) presets.Content(winrt::box_value(o.GetNamedString(L"label")));
        }
        description.Text(m.GetNamedString(L"description"));
        description.Visibility(description.Text().empty() ? Visibility::Collapsed : Visibility::Visible);
        for (auto v : m.GetNamedArray(L"actions")) {
            auto a = v.GetObject();
            auto b = buttons[Id(a, L"id") - 1];
            b.Visibility(a.GetNamedBoolean(L"shown") ? Visibility::Visible : Visibility::Collapsed);
            b.IsEnabled(a.GetNamedBoolean(L"enabled"));
            if (Id(a, L"id") <= 3) accelerators[Id(a, L"id") - 1].IsEnabled(a.GetNamedBoolean(L"shortcutEnabled"));
        }
        applying = false;
        if (fresh)
            flyout.ShowAt(anchor);
        else if (focusAfterAck) {
            auto b = buttons[focusAfterAck - 1];
            focusAfterAck = 0;
            if (b.IsEnabled() && b.Visibility() == Visibility::Visible) b.Focus(FocusState::Programmatic);
        }
    }
};
