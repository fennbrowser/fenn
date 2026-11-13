#pragma once
#include "JsonValidation.h"
#include "shell_core.h"
#include <map>
#include <set>

struct NativeNotificationPanel {
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Value = winrt::Windows::Data::Json::JsonValue;
    using Button = winrt::Microsoft::UI::Xaml::Controls::Button;
    struct Row {
        winrt::Microsoft::UI::Xaml::Controls::StackPanel host{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::TextBlock origin{nullptr}, description{nullptr}, hint{nullptr},
            warning{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::CheckBox check{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::StackPanel actions{nullptr};
        winrt::hstring fingerprint;
    };
    ShellCore* core{};
    uint32_t opening{}, revision{}, focusRevision{};
    bool applying{}, pending{}, persistent{}, forceClose{};
    Button anchor{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Control focusAfterAck{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Flyout flyout{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ScrollViewer viewport{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel content{nullptr};
    std::map<uint32_t, Row> rows;
    std::vector<uint32_t> order;
    std::function<void(Object const&, Button const&)> dispatch;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0), openingLifetime;
    ~NativeNotificationPanel() {
        lifetime.reset();
        Close();
    }
    static uint32_t Id(Object const& m, wchar_t const* key) {
        auto n = NativeJson::RequireRevision(m, key);
        if (!n || n > UINT32_MAX) winrt::throw_hresult(E_INVALIDARG);
        return uint32_t(n);
    }
    static Object Read(Object const& snapshot) {
        auto m = snapshot.GetNamedObject(L"navigation", Object()).GetNamedObject(L"notificationPanel", Object());
        m.GetNamedBoolean(L"enabled", false);
        if (m.HasKey(L"closed") && NativeJson::RequireRevision(m, L"closed") > UINT32_MAX)
            winrt::throw_hresult(E_INVALIDARG);
        if (!m.HasKey(L"request")) return m;
        Id(m, L"request");
        auto source = m.GetNamedString(L"source");
        if (source.empty() || source.size() > 64) winrt::throw_hresult(E_INVALIDARG);
        if (!m.HasKey(L"session")) return m;
        Id(m, L"session");
        Id(m, L"revision");
        if (NativeJson::RequireRevision(m, L"focus") > UINT32_MAX) winrt::throw_hresult(E_INVALIDARG);
        m.GetNamedBoolean(L"persistent");
        NativeJson::RequireLength(m.GetNamedString(L"more"), 4096);
        auto entries = m.GetNamedArray(L"rows");
        if (!entries.Size() || entries.Size() > 4) winrt::throw_hresult(E_INVALIDARG);
        std::set<uint32_t> ids{1, 2, 3}, keys;
        for (auto v : entries) {
            auto r = v.GetObject();
            auto id = Id(r, L"id");
            if (id < 100 || id >= 164 || (id - 100) % 16 || !keys.insert(id).second) winrt::throw_hresult(E_INVALIDARG);
            for (auto key : {L"origin", L"hint", L"warning", L"checkbox"})
                NativeJson::RequireLength(r.GetNamedString(key), 4096);
            auto parts = r.GetNamedArray(L"parts");
            if (parts.Size() != 5) winrt::throw_hresult(E_INVALIDARG);
            for (auto part : parts) NativeJson::RequireLength(part.GetString(), 4096);
            r.GetNamedBoolean(L"checked");
            auto check = NativeJson::RequireRevision(r, L"check");
            if (check && (check != id || !ids.insert(uint32_t(check)).second)) winrt::throw_hresult(E_INVALIDARG);
            auto actions = r.GetNamedArray(L"actions");
            if (actions.Size() > 11) winrt::throw_hresult(E_INVALIDARG);
            for (auto value : actions) {
                auto a = value.GetObject();
                auto action = Id(a, L"id");
                if (action <= id || action > id + 12 || !ids.insert(action).second) winrt::throw_hresult(E_INVALIDARG);
                NativeJson::RequireLength(a.GetNamedString(L"label"), 4096);
                a.GetNamedBoolean(L"enabled");
            }
        }
        if (m.Stringify().size() > 65536) winrt::throw_hresult(E_INVALIDARG);
        return m;
    }
    static std::vector<uint32_t> Actions(Object const& m) {
        std::vector<uint32_t> ids{1, 2, 3};
        for (auto v : m.GetNamedArray(L"rows")) {
            auto r = v.GetObject();
            auto check = uint32_t(r.GetNamedNumber(L"check"));
            if (check) ids.push_back(check);
            for (auto a : r.GetNamedArray(L"actions"))
                if (a.GetObject().GetNamedBoolean(L"enabled")) ids.push_back(Id(a.GetObject(), L"id"));
        }
        return ids;
    }
    bool Resolve(Object const& m) {
        if (!m.HasKey(L"session")) return false;
        auto ids = Actions(m);
        auto fp = m.Stringify();
        bool changed{};
        winrt::check_hresult(shell_core_notification_panel_resolve(
                                 core, Id(m, L"session"), Id(m, L"revision"), ids.data(), ids.size(),
                                 reinterpret_cast<uint16_t const*>(fp.c_str()), fp.size(), &changed) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        return changed;
    }
    void Close(bool cancel = true) {
        opening = revision = focusRevision = 0;
        openingLifetime.reset();
        pending = false;
        forceClose = true;
        if (cancel && core) shell_core_notification_panel_dismiss(core);
        if (flyout) flyout.Hide();
        flyout = nullptr;
        anchor = nullptr;
        focusAfterAck = nullptr;
        content = nullptr;
        viewport = nullptr;
        rows.clear();
        order.clear();
    }
    void LightDismiss(bool force = false) {
        if (!opening || !flyout) return;
        if (force) {
            forceClose = true;
            flyout.Hide();
        } else if (!persistent)
            flyout.Hide();
    }
    void Send(uint32_t id) {
        if (!opening || applying || pending || !dispatch) return;
        bool accepted{};
        if (shell_core_notification_panel_activate(core, opening, revision, id, true, &accepted) != SHELL_STATUS_OK ||
            !accepted)
            return;
        if (content) {
            auto focused = winrt::Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(content.XamlRoot())
                               .try_as<winrt::Microsoft::UI::Xaml::Controls::Control>();
            for (auto node = focused.try_as<winrt::Microsoft::UI::Xaml::DependencyObject>(); node;
                 node = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(node))
                if (node == viewport) {
                    focusAfterAck = focused;
                    break;
                }
        }
        pending = true;
        if (viewport) viewport.IsEnabled(false);
        Object action;
        action.SetNamedValue(L"command",
                             Value::CreateStringValue(L"notification-panel:invoke:" + std::to_wstring(opening) + L":" +
                                                      std::to_wstring(revision) + L":" + std::to_wstring(id)));
        dispatch(action, anchor);
    }
    void Apply(Object const& m, bool visible, Button const& source, bool publish) {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        if (!visible || !m.GetNamedBoolean(L"enabled", false) || !m.HasKey(L"request")) {
            Close();
            return;
        }
        if (opening && m.GetNamedNumber(L"closed", 0) == opening) {
            Close();
            if (m.HasKey(L"session")) return;
        }
        if (!m.HasKey(L"session")) {
            auto fp = m.GetNamedString(L"source");
            uint32_t session{};
            winrt::check_hresult(shell_core_notification_panel_request(core, Id(m, L"request"),
                                                                       reinterpret_cast<uint16_t const*>(fp.c_str()),
                                                                       fp.size(), &session) == SHELL_STATUS_OK
                                     ? S_OK
                                     : E_INVALIDARG);
            if (!session || session == opening) return;
            Close(false);
            opening = session;
            anchor = source;
            openingLifetime = std::make_shared<int>(0);
            Object action;
            action.SetNamedValue(
                L"command", Value::CreateStringValue(L"notification-panel:open:" + std::to_wstring(Id(m, L"request")) +
                                                     L":" + std::to_wstring(session)));
            dispatch(action, anchor);
            return;
        }
        if (!publish || !opening || Id(m, L"session") != opening) return;
        auto ids = Actions(m);
        auto fp = m.Stringify();
        bool changed{};
        winrt::check_hresult(shell_core_notification_panel_accept(
                                 core, opening, Id(m, L"revision"), ids.data(), ids.size(),
                                 reinterpret_cast<uint16_t const*>(fp.c_str()), fp.size(), &changed) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        if (!changed) return;
        revision = Id(m, L"revision");
        persistent = m.GetNamedBoolean(L"persistent");
        pending = false;
        applying = true;
        const bool fresh = !flyout;
        auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
        if (fresh) {
            forceClose = false;
            flyout = Flyout();
            flyout.ShouldConstrainToRootBounds(false);
            auto point = source.TransformToVisual(nullptr).TransformPoint({0, 0});
            flyout.Placement(point.X < source.XamlRoot().Size().Width / 2
                                 ? Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft
                                 : Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight);
            content = StackPanel();
            content.Spacing(16);
            content.Width(360);
            Automation::AutomationProperties::SetAutomationId(content, L"FennNotification-panel");
            viewport = ScrollViewer();
            viewport.MaxHeight(520);
            viewport.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            viewport.Content(content);
            flyout.Content(viewport);
            content.KeyDown([this, alive, current](auto const&, auto const& e) {
                if (alive.expired() || current.expired()) return;
                if (e.Key() == winrt::Windows::System::VirtualKey::Escape) {
                    e.Handled(true);
                    Send(2);
                }
            });
            flyout.Opened([this, alive, current](auto const&, auto const&) {
                if (!alive.expired() && !current.expired()) {
                    for (auto const& [id, row] : rows)
                        if (auto peer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(row.description))
                            peer.RaiseAutomationEvent(Automation::Peers::AutomationEvents::LiveRegionChanged);
                    Send(1);
                }
            });
            flyout.Closing([this, alive, current](auto const&, auto const& e) {
                if (!alive.expired() && !current.expired() && persistent && !forceClose) e.Cancel(true);
            });
            const auto session = opening;
            flyout.Closed([this, alive, current, session](auto const&, auto const&) {
                if (alive.expired() || current.expired() || opening != session) return;
                opening = revision = 0;
                shell_core_notification_panel_dismiss(core);
                if (dispatch) {
                    Object action;
                    action.SetNamedValue(
                        L"command", Value::CreateStringValue(
                                        (forceClose ? L"notification-panel:suspend:" : L"notification-panel:dismiss:") +
                                        std::to_wstring(session)));
                    dispatch(action, anchor);
                }
            });
        }
        std::vector<uint32_t> next;
        for (auto v : m.GetNamedArray(L"rows")) next.push_back(Id(v.GetObject(), L"id"));
        const bool structure = next != order;
        order = next;
        for (auto it = rows.begin(); it != rows.end();)
            if (std::find(next.begin(), next.end(), it->first) == next.end())
                it = rows.erase(it);
            else
                ++it;
        if (structure) content.Children().Clear();
        for (auto v : m.GetNamedArray(L"rows")) {
            auto r = v.GetObject();
            auto id = Id(r, L"id");
            auto& row = rows[id];
            if (!row.host) {
                row.host = StackPanel();
                row.host.Spacing(10);
                row.origin = TextBlock();
                row.origin.FontWeight({600});
                row.origin.TextWrapping(TextWrapping::Wrap);
                row.host.Children().Append(row.origin);
                row.description = TextBlock();
                row.description.TextWrapping(TextWrapping::Wrap);
                Automation::AutomationProperties::SetLiveSetting(row.description,
                                                                 Automation::Peers::AutomationLiveSetting::Assertive);
                Automation::AutomationProperties::SetAutomationId(
                    row.description, L"FennNotification-description-" + std::to_wstring(id));
                row.host.Children().Append(row.description);
                row.hint = TextBlock();
                row.hint.TextWrapping(TextWrapping::Wrap);
                row.host.Children().Append(row.hint);
                row.check = CheckBox();
                Automation::AutomationProperties::SetAutomationId(row.check,
                                                                  L"FennNotification-check-" + std::to_wstring(id));
                row.host.Children().Append(row.check);
                row.check.Click([this, alive, current, id](auto const&, auto const&) {
                    if (!alive.expired() && !current.expired()) Send(id);
                });
                row.warning = TextBlock();
                row.warning.TextWrapping(TextWrapping::Wrap);
                Automation::AutomationProperties::SetLiveSetting(row.warning,
                                                                 Automation::Peers::AutomationLiveSetting::Assertive);
                row.host.Children().Append(row.warning);
                row.actions = StackPanel();
                row.actions.Spacing(8);
                row.host.Children().Append(row.actions);
            }
            row.origin.Text(r.GetNamedString(L"origin"));
            row.origin.Visibility(row.origin.Text().empty() ? Visibility::Collapsed : Visibility::Visible);
            winrt::hstring text;
            row.description.Inlines().Clear();
            auto parts = r.GetNamedArray(L"parts");
            for (uint32_t i = 0; i < parts.Size(); i++) {
                auto part = parts.GetStringAt(i);
                text = text + part;
                Documents::Run run;
                run.Text(part);
                if (i == 1 || i == 3) run.FontWeight({600});
                row.description.Inlines().Append(run);
            }
            Automation::AutomationProperties::SetName(row.description, text);
            row.hint.Text(r.GetNamedString(L"hint"));
            row.hint.Visibility(row.hint.Text().empty() ? Visibility::Collapsed : Visibility::Visible);
            row.warning.Text(r.GetNamedString(L"warning"));
            row.warning.Visibility(row.warning.Text().empty() ? Visibility::Collapsed : Visibility::Visible);
            TextBlock checkLabel;
            checkLabel.Text(r.GetNamedString(L"checkbox"));
            checkLabel.TextWrapping(TextWrapping::Wrap);
            checkLabel.MaxWidth(310);
            row.check.Content(checkLabel);
            Automation::AutomationProperties::SetName(row.check, checkLabel.Text());
            row.check.IsChecked(r.GetNamedBoolean(L"checked"));
            row.check.Visibility(r.GetNamedNumber(L"check") ? Visibility::Visible : Visibility::Collapsed);
            auto actions = r.GetNamedArray(L"actions");
            auto fingerprint = actions.Stringify();
            if (fingerprint != row.fingerprint) {
                row.fingerprint = fingerprint;
                row.actions.Children().Clear();
                for (auto value : actions) {
                    auto a = value.GetObject();
                    auto action = Id(a, L"id");
                    Button button;
                    TextBlock label;
                    label.Text(a.GetNamedString(L"label"));
                    label.TextWrapping(TextWrapping::Wrap);
                    label.MaxWidth(330);
                    button.Content(label);
                    button.IsEnabled(a.GetNamedBoolean(L"enabled"));
                    Automation::AutomationProperties::SetName(button, label.Text());
                    Automation::AutomationProperties::SetAutomationId(
                        button, L"FennNotification-action-" + std::to_wstring(action));
                    button.Click([this, alive, current, action](auto const&, auto const&) {
                        if (!alive.expired() && !current.expired()) Send(action);
                    });
                    row.actions.Children().Append(button);
                }
            }
            if (structure) content.Children().Append(row.host);
        }
        applying = false;
        viewport.IsEnabled(true);
        auto focus = uint32_t(m.GetNamedNumber(L"focus"));
        if (fresh) {
            auto target = source.XamlRoot().Content().as<FrameworkElement>();
            auto bounds = source.TransformToVisual(target).TransformBounds(
                {0, 0, float(source.ActualWidth()), float(source.ActualHeight())});
            Primitives::FlyoutShowOptions options;
            options.Placement(flyout.Placement());
            options.ShowMode(focus ? Primitives::FlyoutShowMode::Standard : Primitives::FlyoutShowMode::Transient);
            options.Position(winrt::Windows::Foundation::Point{
                bounds.X +
                    (flyout.Placement() == Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight ? bounds.Width : 0),
                bounds.Y + bounds.Height});
            flyout.ShowAt(target, options);
        }
        if (focus != focusRevision && !rows.empty()) {
            focusRevision = focus;
            auto& row = rows.begin()->second;
            if (row.check.Visibility() == Visibility::Visible)
                row.check.Focus(FocusState::Programmatic);
            else if (row.actions.Children().Size())
                row.actions.Children().GetAt(0).as<Button>().Focus(FocusState::Programmatic);
        }
        if (focusAfterAck) {
            auto target = focusAfterAck;
            focusAfterAck = nullptr;
            if (target.IsLoaded() && target.IsEnabled()) target.Focus(FocusState::Programmatic);
        }
    }
};
