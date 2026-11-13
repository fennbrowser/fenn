#pragma once
#include "JsonValidation.h"
#include "OmnibarIcons.h"
#include "shell_core.h"

// WinUI objects and screen placement only. ShellCore owns opening/action
// identity; nsContextMenu owns every browser command and target.
struct NativePageContextMenu {
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Array = winrt::Windows::Data::Json::JsonArray;
    using Value = winrt::Windows::Data::Json::JsonValue;
    using Button = Microsoft::UI::Xaml::Controls::Button;
    using Item = Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase;
    using Items = winrt::Windows::Foundation::Collections::IVector<Item>;
    ShellCore* core{};
    uint32_t opening{}, revision{};
    bool fallbackQueued{};
    Microsoft::UI::Xaml::Controls::Primitives::FlyoutBase flyout{nullptr};
    Button anchor{nullptr};
    std::map<uint32_t, Microsoft::UI::Xaml::Controls::Control> rootControls;
    winrt::hstring rootLayout;
    std::map<uint32_t, Item> controls;
    std::map<uint32_t, winrt::hstring> kinds;
    std::map<uint32_t, winrt::hstring> imageKeys;
    std::map<uint32_t, std::shared_ptr<bool>> submenuRequests;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0), openingLifetime;
    std::function<void(Object const&, Button const&)> dispatch;
    std::function<void()> afterClose;
    ~NativePageContextMenu() {
        lifetime.reset();
        Close();
    }
    static uint32_t Id(Object const& m, wchar_t const* key) {
        auto n = NativeJson::RequireRevision(m, key);
        if (!n || n > UINT32_MAX) winrt::throw_hresult(E_INVALIDARG);
        return uint32_t(n);
    }
    static void Validate(Array const& entries, std::set<uint32_t>& ids, size_t& count, size_t& pixels,
                         unsigned depth = 0) {
        if (depth > 6 || (count += entries.Size()) > 256) winrt::throw_hresult(E_INVALIDARG);
        for (auto value : entries) {
            auto row = value.GetObject();
            if (row.GetNamedBoolean(L"separator", false)) continue;
            if (!ids.insert(Id(row, L"id")).second) winrt::throw_hresult(E_INVALIDARG);
            auto label = row.GetNamedString(L"label");
            if (label.empty()) winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(label, 2048);
            NativeJson::RequireLength(row.GetNamedString(L"key"), 256);
            NativeJson::RequireLength(row.GetNamedString(L"accessKey"), 16);
            row.GetNamedBoolean(L"enabled");
            row.GetNamedBoolean(L"checked");
            row.GetNamedBoolean(L"checkable");
            if (row.GetNamedBoolean(L"radio") && !row.GetNamedBoolean(L"checkable")) winrt::throw_hresult(E_INVALIDARG);
            row.GetNamedBoolean(L"loaded");
            NativeJson::RequireLength(row.GetNamedString(L"group"), 256);
            auto glyph = row.GetNamedString(L"glyph");
            if (glyph != L"" && glyph != L"back" && glyph != L"forward" && glyph != L"reload" && glyph != L"stop" &&
                glyph != L"bookmark")
                winrt::throw_hresult(E_INVALIDARG);
            if (row.GetNamedBoolean(L"primary", false) &&
                (depth || glyph.empty() || row.HasKey(L"children") || row.GetNamedBoolean(L"checkable")))
                winrt::throw_hresult(E_INVALIDARG);
            NativeJson::ValidateImage(row, pixels, {16, 4096});
            if (row.HasKey(L"children")) Validate(row.GetNamedArray(L"children"), ids, count, pixels, depth + 1);
        }
    }
    static Object Read(Object const& snapshot) {
        auto m = snapshot.GetNamedObject(L"navigation", Object()).GetNamedObject(L"pageContextMenu", Object());
        m.GetNamedBoolean(L"enabled", false);
        if (m.HasKey(L"closed") && NativeJson::RequireRevision(m, L"closed") > UINT32_MAX)
            winrt::throw_hresult(E_INVALIDARG);
        if (m.HasKey(L"fallback") && NativeJson::RequireRevision(m, L"fallback") > UINT32_MAX)
            winrt::throw_hresult(E_INVALIDARG);
        if (!m.HasKey(L"request")) return m;
        Id(m, L"request");
        if (m.GetNamedString(L"source") != L"page-context") winrt::throw_hresult(E_INVALIDARG);
        if (!m.HasKey(L"session")) return m;
        Id(m, L"session");
        Id(m, L"revision");
        for (auto key : {L"pointX", L"pointY"}) {
            auto n = m.GetNamedNumber(key);
            if (!std::isfinite(n) || std::floor(n) != n || n < INT32_MIN || n > INT32_MAX)
                winrt::throw_hresult(E_INVALIDARG);
        }
        std::set<uint32_t> ids;
        size_t count{}, pixels{};
        auto items = m.GetNamedArray(L"items");
        Validate(items, ids, count, pixels);
        std::set<winrt::hstring> primary;
        for (auto value : items) {
            auto row = value.GetObject();
            if (row.GetNamedBoolean(L"primary", false)) {
                auto glyph = row.GetNamedString(L"glyph");
                if (!primary.insert(glyph == L"stop" ? winrt::hstring(L"reload") : glyph).second || primary.size() > 4)
                    winrt::throw_hresult(E_INVALIDARG);
            }
        }
        if (m.Stringify().size() > 65536) winrt::throw_hresult(E_INVALIDARG);
        return m;
    }
    static void Actions(Array const& entries, std::vector<uint32_t>& ids, bool allowed = true) {
        for (auto v : entries) {
            auto row = v.GetObject();
            if (row.GetNamedBoolean(L"separator", false)) continue;
            bool enabled = allowed && row.GetNamedBoolean(L"enabled");
            if (enabled) ids.push_back(Id(row, L"id"));
            if (row.HasKey(L"children")) Actions(row.GetNamedArray(L"children"), ids, enabled);
        }
    }
    bool Resolve(Object const& m) {
        if (!m.HasKey(L"session")) return false;
        std::vector<uint32_t> ids;
        Actions(m.GetNamedArray(L"items"), ids);
        auto fp = m.Stringify();
        bool changed{};
        winrt::check_hresult(shell_core_page_context_menu_resolve(
                                 core, Id(m, L"session"), Id(m, L"revision"), ids.data(), ids.size(),
                                 reinterpret_cast<uint16_t const*>(fp.c_str()), fp.size(), &changed) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        return changed;
    }
    void LightDismiss() {
        if (flyout) flyout.Hide();
    }
    void Close(bool cancel = true) {
        opening = revision = 0;
        fallbackQueued = false;
        openingLifetime.reset();
        afterClose = {};
        controls.clear();
        kinds.clear();
        imageKeys.clear();
        submenuRequests.clear();
        rootControls.clear();
        rootLayout = L"";
        if (cancel && core) shell_core_page_context_menu_dismiss(core);
        if (flyout) flyout.Hide();
        flyout = nullptr;
        anchor = nullptr;
    }
    void Send(uint32_t id, bool submenu, bool middle = false, bool keyboard = false) {
        if (!opening || !dispatch) return;
        bool accepted{};
        auto status = shell_core_page_context_menu_activate(core, opening, revision, id, submenu, &accepted);
        if (GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0))
            fprintf(stderr,
                    "WinUIPane page-context action opening=%u revision=%u id=%u submenu=%d status=%u accepted=%d\n",
                    opening, revision, id, submenu, unsigned(status), accepted);
        if (status != SHELL_STATUS_OK || !accepted) return;
        Object message;
        message.SetNamedValue(
            L"command", Value::CreateStringValue(std::wstring(submenu  ? L"page-context:submenu:"
                                                              : middle ? L"page-context:middle:"
                                                                       : L"page-context:invoke:") +
                                                 std::to_wstring(opening) + L":" + std::to_wstring(revision) + L":" +
                                                 std::to_wstring(id)));
        auto modifiers = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) | ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) |
                         ((GetKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u) | ((GetKeyState(VK_LWIN) & 0x8000) ? 8u : 0u);
        message.SetNamedValue(L"modifiers", Value::CreateNumberValue(modifiers));
        message.SetNamedValue(L"keyboard", Value::CreateBooleanValue(keyboard));
        if (submenu) {
            dispatch(message, anchor);
            return;
        }
        auto source = anchor;
        auto send = dispatch;
        auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
        afterClose = [message, source, send, alive, current] {
            if (!alive.expired() && !current.expired()) send(message, source);
        };
        flyout.Hide();
    }
    void Forget(Array const& entries) {
        for (auto value : entries) {
            auto row = value.GetObject();
            if (row.GetNamedBoolean(L"separator", false)) continue;
            auto id = Id(row, L"id");
            controls.erase(id);
            kinds.erase(id);
            imageKeys.erase(id);
            submenuRequests.erase(id);
            if (row.HasKey(L"children")) Forget(row.GetNamedArray(L"children"));
        }
    }
    void Populate(Array const& entries, Items const& target, std::set<uint32_t>& present, uint32_t parent = 0) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        std::vector<Item> ordered;
        auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
        uint32_t diagnosticId{};
        const char* phase = "rows";
        try {
            for (auto value : entries) {
                auto row = value.GetObject();
                if (row.GetNamedBoolean(L"separator", false)) {
                    auto index = uint32_t(ordered.size());
                    ordered.push_back(index < target.Size() && target.GetAt(index).try_as<MenuFlyoutSeparator>()
                                          ? target.GetAt(index)
                                          : MenuFlyoutSeparator());
                    continue;
                }
                const auto id = Id(row, L"id");
                present.insert(id);
                const bool submenu = row.HasKey(L"children"), check = row.GetNamedBoolean(L"checkable");
                const bool radio = row.GetNamedBoolean(L"radio");
                winrt::hstring kind = submenu ? L"submenu" : radio ? L"radio" : check ? L"toggle" : L"item";
                if (!controls.count(id) || kinds[id] != kind) {
                    Item item{nullptr};
                    if (submenu) {
                        MenuFlyoutSubItem sub;
                        auto requested = std::make_shared<bool>(false);
                        submenuRequests[id] = requested;
                        auto request = [this, alive, current, id, requested] {
                            if (!alive.expired() && !current.expired() && !*requested) {
                                *requested = true;
                                Send(id, true);
                            }
                        };
                        sub.GotFocus([request](auto const&, auto const&) { request(); });
                        item = sub;
                    } else {
                        auto keyboard = std::make_shared<bool>(false);
                        auto invoke = [this, alive, current, id, keyboard](auto const&, auto const&) {
                            if (!alive.expired() && !current.expired()) Send(id, false, false, *keyboard);
                        };
                        if (radio) {
                            RadioMenuFlyoutItem b;
                            b.Click(invoke);
                            item = b;
                        } else if (check) {
                            ToggleMenuFlyoutItem b;
                            b.Click(invoke);
                            item = b;
                        } else {
                            MenuFlyoutItem b;
                            b.Click(invoke);
                            item = b;
                        }
                        item.PreviewKeyDown([keyboard](auto const&, auto const&) { *keyboard = true; });
                        item.PointerPressed([keyboard](auto const&, auto const&) { *keyboard = false; });
                        item.AddHandler(UIElement::PointerReleasedEvent(),
                                        winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                                            [this, alive, current, id](auto const&, auto const& args) {
                                                if (!alive.expired() && !current.expired() &&
                                                    args.GetCurrentPoint(nullptr).Properties().PointerUpdateKind() ==
                                                        Microsoft::UI::Input::PointerUpdateKind::MiddleButtonReleased) {
                                                    args.Handled(true);
                                                    Send(id, false, true);
                                                }
                                            })),
                                        true);
                    }
                    controls.insert_or_assign(id, item);
                    kinds[id] = kind;
                    imageKeys.erase(id);
                    Automation::AutomationProperties::SetAutomationId(item, L"FennPageContext-" + std::to_wstring(id));
                }
                diagnosticId = id;
                phase = "properties";
                auto item = controls.at(id);
                item.IsEnabled(row.GetNamedBoolean(L"enabled"));
                item.AccessKey(row.GetNamedString(L"accessKey"));
                if (auto sub = item.try_as<MenuFlyoutSubItem>()) {
                    sub.Text(row.GetNamedString(L"label"));
                    auto children = row.GetNamedArray(L"children");
                    Populate(children, sub.Items(), present, id);
                    if (!sub.Items().Size() && !row.GetNamedBoolean(L"loaded")) {
                        MenuFlyoutItem pending;
                        pending.Text(L"…");
                        pending.IsEnabled(false);
                        auto requested = submenuRequests.at(id);
                        pending.Loaded([this, alive, current, id, requested](auto const&, auto const&) {
                            if (!alive.expired() && !current.expired() && !*requested) {
                                *requested = true;
                                Send(id, true);
                            }
                        });
                        sub.Items().Append(pending);
                    }
                } else if (auto radioItem = item.try_as<RadioMenuFlyoutItem>()) {
                    radioItem.Text(row.GetNamedString(L"label"));
                    radioItem.GroupName(L"page-" + std::to_wstring(opening) + L"-" + std::to_wstring(parent) + L"-" +
                                        row.GetNamedString(L"group"));
                    radioItem.IsChecked(row.GetNamedBoolean(L"checked"));
                } else if (auto toggle = item.try_as<ToggleMenuFlyoutItem>()) {
                    toggle.Text(row.GetNamedString(L"label"));
                    toggle.IsChecked(row.GetNamedBoolean(L"checked"));
                } else
                    item.as<MenuFlyoutItem>().Text(row.GetNamedString(L"label"));
                phase = "icon";
                auto image = row.GetNamedValue(L"image", Value::CreateNullValue());
                auto glyph = row.GetNamedString(L"glyph");
                auto imageKey = image.Stringify() + glyph;
                if (imageKeys[id] != imageKey) {
                    imageKeys[id] = imageKey;
                    IconElement icon{nullptr};
                    if (image.ValueType() == winrt::Windows::Data::Json::JsonValueType::Object) {
                        ImageIcon picture;
                        picture.Source(NativeJson::DecodeImage(image.GetObject()));
                        icon = picture;
                    } else if (!glyph.empty()) {
                        FontIcon vector;
                        vector.FontSize(14);
                        vector.Glyph(glyph == L"back"      ? L"\xE72B"
                                     : glyph == L"forward" ? L"\xE72A"
                                     : glyph == L"reload"  ? L"\xE72C"
                                     : glyph == L"stop"    ? L"\xE711"
                                                           : L"\xE734");
                        icon = vector;
                    }
                    if (icon)
                        Automation::AutomationProperties::SetAccessibilityView(
                            icon, Automation::Peers::AccessibilityView::Raw);
                    if (auto sub = item.try_as<MenuFlyoutSubItem>())
                        sub.Icon(icon);
                    else if (auto toggle = item.try_as<ToggleMenuFlyoutItem>())
                        toggle.Icon(icon);
                    else
                        item.as<MenuFlyoutItem>().Icon(icon);
                }
                ordered.push_back(item);
            }
            // Metadata updates keep every control attached. WinUI can retain the
            // old logical parent after Items.Clear, so structural changes require
            // fresh controls for this level and its descendants.
            phase = "membership";
            bool same = target.Size() == ordered.size();
            for (uint32_t i = 0; same && i < ordered.size(); i++) same = target.GetAt(i) == ordered[i];
            if (!same) {
                if (target.Size()) {
                    target.Clear();
                    Forget(entries);
                    Populate(entries, target, present, parent);
                } else
                    for (auto const& item : ordered) target.Append(item);
            }
        } catch (winrt::hresult_error const& error) {
            if (GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0))
                fprintf(stderr, "WinUIPane page-context populate parent=%u id=%u phase=%s hr=%08x\n", parent,
                        diagnosticId, phase, unsigned(error.code().value));
            throw;
        }
    }
    void PopulateCommands(Array const& entries, Microsoft::UI::Xaml::Controls::CommandBarFlyout const& bar,
                          std::set<uint32_t>& present) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        std::wstring layout;
        for (auto value : entries) {
            auto row = value.GetObject();
            layout += row.GetNamedBoolean(L"separator", false)
                          ? L"/"
                          : std::to_wstring(Id(row, L"id")) + (row.GetNamedBoolean(L"primary", false) ? L"p"
                                                               : row.HasKey(L"children")              ? L"m"
                                                               : row.GetNamedBoolean(L"checkable")    ? L"t"
                                                                                                      : L"b");
            layout += L",";
        }
        bool rebuild = rootLayout != layout;
        if (rebuild) {
            bar.PrimaryCommands().Clear();
            bar.SecondaryCommands().Clear();
            rootControls.clear();
            controls.clear();
            kinds.clear();
            imageKeys.clear();
            submenuRequests.clear();
            rootLayout = layout;
        }
        auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
        bool secondary = false;
        for (uint32_t i = 0; i < entries.Size(); i++) {
            auto row = entries.GetAt(i).GetObject();
            if (row.GetNamedBoolean(L"separator", false)) {
                bool follows = false;
                for (uint32_t n = i + 1; n < entries.Size(); n++) {
                    auto next = entries.GetAt(n).GetObject();
                    if (!next.GetNamedBoolean(L"separator", false) && !next.GetNamedBoolean(L"primary", false)) {
                        follows = true;
                        break;
                    }
                }
                if (rebuild && secondary && follows) bar.SecondaryCommands().Append(AppBarSeparator());
                continue;
            }
            auto id = Id(row, L"id");
            present.insert(id);
            bool primary = row.GetNamedBoolean(L"primary", false), submenu = row.HasKey(L"children"),
                 check = row.GetNamedBoolean(L"checkable");
            if (!primary) secondary = true;
            if (!rootControls.count(id)) {
                Control control{nullptr};
                if (check) {
                    AppBarToggleButton button;
                    control = button;
                } else {
                    AppBarButton button;
                    control = button;
                }
                if (submenu) {
                    MenuFlyout children;
                    children.ShouldConstrainToRootBounds(false);
                    auto requested = std::make_shared<bool>(false);
                    auto request = [this, alive, current, id, requested] {
                        if (!alive.expired() && !current.expired() && !*requested) {
                            *requested = true;
                            Send(id, true);
                        }
                    };
                    children.Opening([request](auto const&, auto const&) { request(); });
                    control.GotFocus([request](auto const&, auto const&) { request(); });
                    control.as<AppBarButton>().Flyout(children);
                } else {
                    auto keyboard = std::make_shared<bool>(false);
                    control.as<Primitives::ButtonBase>().Click(
                        [this, alive, current, id, keyboard](auto const&, auto const&) {
                            if (!alive.expired() && !current.expired()) Send(id, false, false, *keyboard);
                        });
                    control.PreviewKeyDown([keyboard](auto const&, auto const&) { *keyboard = true; });
                    control.PointerPressed([keyboard](auto const&, auto const&) { *keyboard = false; });
                    control.AddHandler(UIElement::PointerReleasedEvent(),
                                       winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                                           [this, alive, current, id](auto const&, auto const& args) {
                                               if (!alive.expired() && !current.expired() &&
                                                   args.GetCurrentPoint(nullptr).Properties().PointerUpdateKind() ==
                                                       Microsoft::UI::Input::PointerUpdateKind::MiddleButtonReleased) {
                                                   args.Handled(true);
                                                   Send(id, false, true);
                                               }
                                           })),
                                       true);
                }
                Automation::AutomationProperties::SetAutomationId(control, L"FennPageContext-" + std::to_wstring(id));
                rootControls.insert_or_assign(id, control);
                (primary ? bar.PrimaryCommands() : bar.SecondaryCommands()).Append(control.as<ICommandBarElement>());
            }
            auto control = rootControls.at(id);
            auto label = row.GetNamedString(L"label");
            control.IsEnabled(row.GetNamedBoolean(L"enabled"));
            control.AccessKey(row.GetNamedString(L"accessKey"));
            Automation::AutomationProperties::SetName(control, label);
            ToolTipService::SetToolTip(control, winrt::box_value(label));
            if (auto toggle = control.try_as<AppBarToggleButton>()) {
                toggle.Label(label);
                toggle.IsChecked(row.GetNamedBoolean(L"checked"));
            } else {
                auto button = control.as<AppBarButton>();
                button.Label(label);
                if (primary) {
                    button.LabelPosition(CommandBarLabelPosition::Collapsed);
                    button.IsCompact(true);
                }
            }
            auto image = row.GetNamedValue(L"image", Value::CreateNullValue());
            auto glyph = row.GetNamedString(L"glyph");
            auto imageKey = image.Stringify() + glyph;
            if (imageKeys[id] != imageKey) {
                imageKeys[id] = imageKey;
                IconElement icon{nullptr};
                if (image.ValueType() == winrt::Windows::Data::Json::JsonValueType::Object) {
                    ImageIcon picture;
                    picture.Source(NativeJson::DecodeImage(image.GetObject()));
                    icon = picture;
                } else if (!glyph.empty()) {
                    FontIcon vector;
                    vector.FontSize(14);
                    vector.Glyph(glyph == L"back"      ? L"\xE72B"
                                 : glyph == L"forward" ? L"\xE72A"
                                 : glyph == L"reload"  ? L"\xE72C"
                                 : glyph == L"stop"    ? L"\xE711"
                                                       : L"\xE734");
                    icon = vector;
                }
                if (icon)
                    Automation::AutomationProperties::SetAccessibilityView(icon,
                                                                           Automation::Peers::AccessibilityView::Raw);
                if (auto toggle = control.try_as<AppBarToggleButton>())
                    toggle.Icon(icon);
                else
                    control.as<AppBarButton>().Icon(icon);
            }
            if (submenu) {
                auto children = control.as<AppBarButton>().Flyout().as<MenuFlyout>();
                Populate(row.GetNamedArray(L"children"), children.Items(), present, id);
                if (!children.Items().Size() && !row.GetNamedBoolean(L"loaded")) {
                    MenuFlyoutItem pending;
                    pending.Text(L"…");
                    pending.IsEnabled(false);
                    children.Items().Append(pending);
                }
            }
        }
    }
    void Apply(Object const& m, bool visible, Button const& source, bool publish) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        if (!visible || !m.GetNamedBoolean(L"enabled", false) || !m.HasKey(L"request")) {
            Close();
            return;
        }
        if (opening && m.GetNamedNumber(L"closed", 0) == opening) {
            Close();
            if (m.HasKey(L"session")) return;
        }
        if (opening && m.GetNamedNumber(L"fallback", 0) == opening) {
            if (fallbackQueued) return;
            fallbackQueued = true;
            Object message;
            message.SetNamedValue(L"command",
                                  Value::CreateStringValue(L"page-context:fallback:" + std::to_wstring(opening)));
            auto send = dispatch;
            auto button = anchor;
            auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
            afterClose = [message, send, button, alive, current] {
                if (!alive.expired() && !current.expired()) send(message, button);
            };
            if (flyout)
                flyout.Hide();
            else {
                opening = revision = 0;
                shell_core_page_context_menu_dismiss(core);
                auto callback = std::move(afterClose);
                button.DispatcherQueue().TryEnqueue(std::move(callback));
            }
            return;
        }
        if (!m.HasKey(L"session")) {
            auto fp = m.GetNamedString(L"source");
            uint32_t session{};
            winrt::check_hresult(shell_core_page_context_menu_request(core, Id(m, L"request"),
                                                                      reinterpret_cast<uint16_t const*>(fp.c_str()),
                                                                      fp.size(), &session) == SHELL_STATUS_OK
                                     ? S_OK
                                     : E_INVALIDARG);
            if (!session || session == opening) return;
            Close(false);
            opening = session;
            anchor = source;
            openingLifetime = std::make_shared<int>(0);
            Object request;
            request.SetNamedValue(
                L"command", Value::CreateStringValue(L"page-context:open:" + std::to_wstring(Id(m, L"request")) + L":" +
                                                     std::to_wstring(session)));
            dispatch(request, anchor);
            return;
        }
        if (!publish || !opening || Id(m, L"session") != opening) return;
        std::vector<uint32_t> ids;
        Actions(m.GetNamedArray(L"items"), ids);
        auto fp = m.Stringify();
        bool changed{};
        winrt::check_hresult(shell_core_page_context_menu_accept(
                                 core, opening, Id(m, L"revision"), ids.data(), ids.size(),
                                 reinterpret_cast<uint16_t const*>(fp.c_str()), fp.size(), &changed) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        if (!changed) return;
        revision = Id(m, L"revision");
        const bool fresh = !flyout;
        auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
        if (fresh) {
            bool quick = false;
            for (auto value : m.GetNamedArray(L"items")) quick |= value.GetObject().GetNamedBoolean(L"primary", false);
            if (quick) {
                CommandBarFlyout bar;
                bar.AlwaysExpanded(true);
                flyout = bar;
            } else
                flyout = MenuFlyout();
            flyout.ShouldConstrainToRootBounds(false);
            const auto session = opening;
            flyout.Closed([this, alive, current, session](auto const&, auto const&) {
                if (alive.expired() || current.expired() || opening != session) return;
                auto callback = std::move(afterClose);
                opening = revision = 0;
                shell_core_page_context_menu_dismiss(core);
                if (callback)
                    anchor.DispatcherQueue().TryEnqueue(std::move(callback));
                else if (dispatch) {
                    Object message;
                    message.SetNamedValue(
                        L"command", Value::CreateStringValue(L"page-context:dismiss:" + std::to_wstring(session)));
                    dispatch(message, anchor);
                }
            });
        }
        std::set<uint32_t> present;
        if (auto bar = flyout.try_as<CommandBarFlyout>())
            PopulateCommands(m.GetNamedArray(L"items"), bar, present);
        else
            Populate(m.GetNamedArray(L"items"), flyout.as<MenuFlyout>().Items(), present);
        for (auto i = controls.begin(); i != controls.end();)
            if (!present.count(i->first)) {
                kinds.erase(i->first);
                imageKeys.erase(i->first);
                submenuRequests.erase(i->first);
                i = controls.erase(i);
            } else
                ++i;
        if (fresh) {
            auto target = source.XamlRoot().Content().as<FrameworkElement>();
            auto point = source.XamlRoot().ContentIsland().CoordinateConverter().ConvertScreenToLocal(
                winrt::Windows::Graphics::PointInt32{int32_t(m.GetNamedNumber(L"pointX")),
                                                     int32_t(m.GetNamedNumber(L"pointY"))});
            point = target.TransformToVisual(nullptr).Inverse().TransformPoint(point);
            Primitives::FlyoutShowOptions placement;
            placement.Position(point);
            if (flyout.try_as<CommandBarFlyout>())
                placement.Placement(Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
            placement.ShowMode(Primitives::FlyoutShowMode::Standard);
            flyout.ShowAt(target, placement);
        }
    }
};
