#pragma once
#include "JsonValidation.h"
#include "shell_core.h"

// Actual WinUI editing controls only; Firefox owns group state and commands.
struct NativeGroupHeader {
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Value = winrt::Windows::Data::Json::JsonValue;
    ShellCore* core{};
    uint32_t opening{}, revision{};
    bool applying{}, nameDirty{}, colorDirty{}, creation{}, escaped{};
    Microsoft::UI::Xaml::Controls::Button anchor{nullptr};
    Microsoft::UI::Xaml::Controls::Flyout flyout{nullptr};
    Microsoft::UI::Xaml::Controls::TextBox name{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox color{nullptr};
    std::vector<Microsoft::UI::Xaml::Controls::Button> buttons;
    std::function<void(winrt::hstring const&)> dispatch;
    std::function<void()> releaseFocus, afterClose;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0), openingLifetime;
    ~NativeGroupHeader() {
        lifetime.reset();
        Close();
    }
    static uint32_t Id(Object const& m, wchar_t const* key) {
        auto n = NativeJson::RequireRevision(m, key);
        if (!n || n > UINT32_MAX) winrt::throw_hresult(E_INVALIDARG);
        return uint32_t(n);
    }
    static Object Read(Object const& m) {
        if (m.HasKey(L"closed")) {
            Id(m, L"closed");
            if (m.Size() != 1 && !(m.Size() == 2 && m.GetNamedBoolean(L"fallback", false)))
                winrt::throw_hresult(E_INVALIDARG);
            return m;
        }
        Id(m, L"session");
        Id(m, L"revision");
        NativeJson::RequireLength(m.GetNamedString(L"name"), 4096);
        for (auto key : {L"title", L"nameLabel", L"placeholder", L"colorLabel", L"done"}) {
            auto s = m.GetNamedString(key);
            NativeJson::RequireLength(s, 512);
            if (s.empty()) winrt::throw_hresult(E_INVALIDARG);
        }
        std::set<winrt::hstring> colors;
        auto options = m.GetNamedArray(L"colors");
        if (options.Size() != 9) winrt::throw_hresult(E_INVALIDARG);
        for (auto v : options) {
            auto o = v.GetObject();
            auto key = o.GetNamedString(L"key"), label = o.GetNamedString(L"label");
            if (key.empty() || key.size() > 16 || label.empty() || !colors.insert(key).second)
                winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(label, 512);
        }
        if (!colors.count(m.GetNamedString(L"color"))) winrt::throw_hresult(E_INVALIDARG);
        std::set<uint32_t> ids;
        auto commands = m.GetNamedArray(L"commands");
        if (commands.Size() > 7) winrt::throw_hresult(E_INVALIDARG);
        for (auto v : commands) {
            auto o = v.GetObject();
            auto id = Id(o, L"id");
            if (id < 4 || id > 10 || !ids.insert(id).second) winrt::throw_hresult(E_INVALIDARG);
            auto key = o.GetNamedString(L"key"), label = o.GetNamedString(L"label");
            if (key.empty() || key.size() > 32 || label.empty()) winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(label, 512);
            o.GetNamedBoolean(L"enabled");
        }
        if (m.GetNamedBoolean(L"creation", false)) {
            auto label = m.GetNamedString(L"cancel");
            NativeJson::RequireLength(label, 512);
            if (label.empty() || commands.Size()) winrt::throw_hresult(E_INVALIDARG);
        }
        if (m.HasKey(L"more")) {
            auto s = m.GetNamedString(L"more");
            if (s.empty()) winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(s, 512);
        }
        if (m.Stringify().size() > 16384) winrt::throw_hresult(E_INVALIDARG);
        return m;
    }
    static std::vector<uint32_t> Actions(Object const& m) {
        std::vector<uint32_t> ids{1, 2, 3};
        for (auto v : m.GetNamedArray(L"commands")) {
            auto o = v.GetObject();
            if (o.GetNamedBoolean(L"enabled")) ids.push_back(Id(o, L"id"));
        }
        if (m.HasKey(L"more")) ids.push_back(20);
        if (m.GetNamedBoolean(L"creation", false)) ids.push_back(21);
        return ids;
    }
    void Close(bool cancel = true) {
        opening = revision = 0;
        openingLifetime.reset();
        afterClose = {};
        if (cancel && core) shell_core_group_header_dismiss(core);
        if (flyout) flyout.Hide();
        flyout = nullptr;
        name = nullptr;
        color = nullptr;
        anchor = nullptr;
        buttons.clear();
        nameDirty = colorDirty = creation = escaped = false;
    }
    uint32_t Open(Microsoft::UI::Xaml::Controls::Button const& source) {
        Close();
        uint32_t id{};
        if (!core || shell_core_group_header_open(core, &id) != SHELL_STATUS_OK || !id) return 0;
        opening = id;
        anchor = source;
        openingLifetime = std::make_shared<int>(0);
        return id;
    }
    uint32_t Request(Microsoft::UI::Xaml::Controls::Button const& source, uint32_t request, uint32_t group,
                     uint32_t window) {
        uint32_t id{};
        winrt::check_hresult(shell_core_group_header_request(core, request, group, window, &id) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        if (!id || id == opening) return 0;
        Close(false);
        opening = id;
        anchor = source;
        openingLifetime = std::make_shared<int>(0);
        return id;
    }
    void Send(uint32_t id, bool retained = false, winrt::hstring const& value = {}) {
        if (!opening || !dispatch) return;
        bool accepted{};
        if (shell_core_group_header_activate(core, opening, revision, id, retained, &accepted) != SHELL_STATUS_OK ||
            !accepted)
            return;
        Object action;
        action.SetNamedValue(L"kind", Value::CreateStringValue(L"group-header"));
        action.SetNamedValue(L"session", Value::CreateNumberValue(opening));
        action.SetNamedValue(L"revision", Value::CreateNumberValue(revision));
        action.SetNamedValue(L"command", Value::CreateNumberValue(id));
        if (id == 3 || id == 21) {
            action.SetNamedValue(L"name", Value::CreateStringValue(name.Text()));
            action.SetNamedValue(L"color", Value::CreateStringValue(Color()));
        }
        auto local = anchor.TransformToVisual(nullptr).TransformBounds(
            {0, 0, float(anchor.ActualWidth()), float(anchor.ActualHeight())});
        auto screen = anchor.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
        action.SetNamedValue(L"x", Value::CreateNumberValue(screen.X));
        action.SetNamedValue(L"y", Value::CreateNumberValue(screen.Y));
        action.SetNamedValue(L"width", Value::CreateNumberValue(screen.Width));
        action.SetNamedValue(L"height", Value::CreateNumberValue(screen.Height));
        if (retained) {
            action.SetNamedValue(L"value", Value::CreateStringValue(value));
            for (auto button : buttons) button.IsEnabled(false);
            dispatch(action.Stringify());
            return;
        }
        auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
        afterClose = [this, alive, current, payload = action.Stringify()] {
            if (!alive.expired() && !current.expired() && dispatch) {
                if (releaseFocus) releaseFocus();
                dispatch(payload);
            }
        };
        flyout.Hide();
    }
    winrt::hstring Color() const {
        return color && color.SelectedItem()
                   ? color.SelectedItem().as<Microsoft::UI::Xaml::Controls::ComboBoxItem>().Tag().as<winrt::hstring>()
                   : L"";
    }
    void Apply(Object const& raw) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        const auto m = Read(raw);
        if (m.HasKey(L"closed")) {
            if (Id(m, L"closed") == opening) {
                const bool fallback = m.GetNamedBoolean(L"fallback", false);
                Close();
                if (fallback && releaseFocus) releaseFocus();
            }
            return;
        }
        if (flyout && creation != m.GetNamedBoolean(L"creation", false)) winrt::throw_hresult(E_INVALIDARG);
        auto ids = Actions(m);
        auto fp = m.Stringify();
        bool changed{};
        winrt::check_hresult(shell_core_group_header_resolve(core, Id(m, L"session"), Id(m, L"revision"), ids.data(),
                                                             ids.size(), reinterpret_cast<uint16_t const*>(fp.c_str()),
                                                             fp.size(), &changed) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        if (!changed) return;
        winrt::check_hresult(shell_core_group_header_accept(core, Id(m, L"session"), Id(m, L"revision"), ids.data(),
                                                            ids.size(), reinterpret_cast<uint16_t const*>(fp.c_str()),
                                                            fp.size(), &changed) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        if (!changed || Id(m, L"session") != opening) return;
        revision = Id(m, L"revision");
        creation = m.GetNamedBoolean(L"creation", false);
        auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
        applying = true;
        if (!flyout) {
            flyout = Flyout();
            flyout.ShouldConstrainToRootBounds(false);
            if (creation)
                flyout.Placement(anchor.FlowDirection() == FlowDirection::RightToLeft
                                     ? Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight
                                     : Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
            StackPanel panel;
            panel.Width(300);
            panel.Spacing(8);
            Automation::AutomationProperties::SetAutomationId(panel, L"FennGroupHeader");
            TextBlock title;
            title.Text(m.GetNamedString(L"title"));
            title.FontSize(18);
            title.FontWeight({600});
            panel.Children().Append(title);
            name = TextBox();
            name.MaxLength(4096);
            name.Header(winrt::box_value(m.GetNamedString(L"nameLabel")));
            name.PlaceholderText(m.GetNamedString(L"placeholder"));
            Automation::AutomationProperties::SetAutomationId(name, L"FennGroupHeader-name");
            Automation::AutomationProperties::SetName(name, m.GetNamedString(L"nameLabel"));
            panel.Children().Append(name);
            color = ComboBox();
            color.HorizontalAlignment(HorizontalAlignment::Stretch);
            color.Header(winrt::box_value(m.GetNamedString(L"colorLabel")));
            Automation::AutomationProperties::SetAutomationId(color, L"FennGroupHeader-color");
            Automation::AutomationProperties::SetName(color, m.GetNamedString(L"colorLabel"));
            for (auto v : m.GetNamedArray(L"colors")) {
                auto o = v.GetObject();
                ComboBoxItem item;
                item.Content(winrt::box_value(o.GetNamedString(L"label")));
                item.Tag(winrt::box_value(o.GetNamedString(L"key")));
                Automation::AutomationProperties::SetAutomationId(item,
                                                                  L"FennGroupHeader-color-" + o.GetNamedString(L"key"));
                color.Items().Append(item);
            }
            panel.Children().Append(color);
            name.TextChanged([this, alive, current](auto const&, auto const&) {
                if (!alive.expired() && !current.expired() && !applying) {
                    nameDirty = true;
                    Send(1, true, name.Text());
                }
            });
            name.KeyDown([this, alive, current](auto const&, auto const& args) {
                if (!alive.expired() && !current.expired() && args.Key() == Windows::System::VirtualKey::Enter) {
                    args.Handled(true);
                    Send(3);
                }
            });
            color.SelectionChanged([this, alive, current](auto const&, auto const&) {
                if (!alive.expired() && !current.expired() && !applying) {
                    colorDirty = true;
                    Send(2, true, Color());
                }
            });
            auto button = [&](uint32_t id, winrt::hstring const& key, winrt::hstring const& label) {
                auto b =
                    Markup::XamlReader::Load(
                        LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" HorizontalAlignment="Stretch" HorizontalContentAlignment="Left"/>)")
                        .as<Button>();
                b.Content(winrt::box_value(label));
                b.Tag(winrt::box_value(id));
                Automation::AutomationProperties::SetAutomationId(b, L"FennGroupHeader-" + key);
                b.Click([this, alive, current, id](auto const&, auto const&) {
                    if (!alive.expired() && !current.expired()) Send(id);
                });
                buttons.push_back(b);
                panel.Children().Append(b);
            };
            for (auto v : m.GetNamedArray(L"commands")) {
                auto o = v.GetObject();
                button(Id(o, L"id"), o.GetNamedString(L"key"), o.GetNamedString(L"label"));
            }
            panel.PreviewKeyDown([this, alive, current](auto const&, auto const& args) {
                if (!alive.expired() && !current.expired() && creation &&
                    args.Key() == Windows::System::VirtualKey::Escape && !color.IsDropDownOpen())
                    escaped = true;
            });
            if (creation) button(21, L"cancel", m.GetNamedString(L"cancel"));
            if (m.HasKey(L"more")) button(20, L"more", m.GetNamedString(L"more"));
            button(3, L"done", m.GetNamedString(L"done"));
            ScrollViewer scroll;
            scroll.MaxHeight(600);
            scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            scroll.Content(panel);
            flyout.Content(scroll);
            flyout.Opened([this, alive, current](auto const&, auto const&) {
                if (!alive.expired() && !current.expired()) {
                    name.Focus(FocusState::Programmatic);
                    name.SelectAll();
                }
            });
            const auto session = opening;
            flyout.Closed([this, alive, session](auto const&, auto const&) {
                if (alive.expired() || opening != session) return;
                auto send = std::move(afterClose);
                const auto lastRevision = revision;
                opening = revision = 0;
                shell_core_group_header_dismiss(core);
                if (send)
                    anchor.DispatcherQueue().TryEnqueue(std::move(send));
                else if (dispatch) {
                    Object a;
                    a.SetNamedValue(L"kind", Value::CreateStringValue(L"group-header"));
                    a.SetNamedValue(L"session", Value::CreateNumberValue(session));
                    a.SetNamedValue(L"command", Value::CreateNumberValue(0));
                    a.SetNamedValue(L"cancel", Value::CreateBooleanValue(creation && escaped));
                    a.SetNamedValue(L"revision", Value::CreateNumberValue(lastRevision));
                    a.SetNamedValue(L"name", Value::CreateStringValue(name.Text()));
                    a.SetNamedValue(L"color", Value::CreateStringValue(Color()));
                    dispatch(a.Stringify());
                }
                flyout = nullptr;
                name = nullptr;
                color = nullptr;
                anchor = nullptr;
                buttons.clear();
            });
        }
        if (nameDirty && name.Text() == m.GetNamedString(L"name")) nameDirty = false;
        if (!nameDirty) name.Text(m.GetNamedString(L"name"));
        if (colorDirty && Color() == m.GetNamedString(L"color")) colorDirty = false;
        if (!colorDirty)
            for (auto v : color.Items())
                if (v.as<ComboBoxItem>().Tag().as<winrt::hstring>() == m.GetNamedString(L"color"))
                    color.SelectedItem(v);
        for (auto b : buttons) {
            auto id = winrt::unbox_value<uint32_t>(b.Tag());
            b.IsEnabled(std::find(ids.begin(), ids.end(), id) != ids.end() && !nameDirty && !colorDirty);
            for (auto v : m.GetNamedArray(L"commands")) {
                auto c = v.GetObject();
                if (Id(c, L"id") == id) b.Content(winrt::box_value(c.GetNamedString(L"label")));
            }
        }
        applying = false;
        if (!flyout.IsOpen()) flyout.ShowAt(anchor);
        // Preserve newer TextBox edits while acknowledging earlier publications.
        if (nameDirty) Send(1, true, name.Text());
        if (colorDirty) Send(2, true, Color());
    }
};
