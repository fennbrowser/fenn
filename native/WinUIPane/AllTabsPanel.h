#pragma once
#include "JsonValidation.h"
#include "shell_core.h"

// WinUI objects only. Firefox supplies the rows and owns every browser command.
// List items are opaque data IDs; only realized containers own row controls.
struct NativeAllTabsPanel {
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Value = winrt::Windows::Data::Json::JsonValue;
    using Point = winrt::Windows::Foundation::Point;
    ShellCore* core{};
    HWND ownerHwnd{};
    winrt::hstring AutomationId(winrt::hstring const& local) const {
        return local + L"-" + winrt::to_hstring(reinterpret_cast<uintptr_t>(ownerHwnd)) + L"-" +
               winrt::to_hstring(opening);
    }
    uint32_t opening{}, revision{}, epoch{}, dropRow{}, middleRow{};
    bool dropBefore{}, suppressClick{}, forceClosing{}, closing{};
    winrt::hstring commandFingerprint;
    Microsoft::UI::Xaml::Controls::Button anchor{nullptr};
    Microsoft::UI::Xaml::Controls::Flyout flyout{nullptr};
    Microsoft::UI::Xaml::Controls::ListView list{nullptr};
    Microsoft::UI::Xaml::Controls::Button search{nullptr}, back{nullptr};
    Microsoft::UI::Xaml::Controls::DropDownButton more{nullptr};
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> items{
        winrt::single_threaded_observable_vector<winrt::Windows::Foundation::IInspectable>()};
    std::map<uint32_t, Object> rows, groupModels;
    std::map<uint32_t, winrt::weak_ref<Microsoft::UI::Xaml::Controls::ListViewItem>> realized;
    std::map<uint32_t, Microsoft::UI::Xaml::Controls::Border> markers;
    std::map<uint32_t, winrt::hstring> rendered;
    struct RowIcon {
        uint32_t tab{};
        Microsoft::UI::Xaml::Controls::Image image;
        Microsoft::UI::Xaml::Controls::FontIcon fallback;
        winrt::hstring pixels;
    };
    std::map<uint32_t, RowIcon> icons;
    struct RowVisual {
        Microsoft::UI::Xaml::Controls::Grid root;
        Microsoft::UI::Xaml::Controls::TextBlock text;
        Microsoft::UI::Xaml::Controls::FontIcon check, muteGlyph;
        Microsoft::UI::Xaml::Controls::Button close{nullptr}, mute{nullptr};
    };
    std::map<uint32_t, RowVisual> visuals;
    std::function<void(Object const&)> dispatch;
    std::function<void()> releaseFocus, cancelDrag;
    std::function<bool(uint32_t, uint32_t, Microsoft::UI::Xaml::UIElement)> armDrag;
    std::function<bool()> dragging, deferOutside;
    std::function<Microsoft::UI::Xaml::Media::ImageSource(uint32_t)> image;
    std::function<Microsoft::UI::Xaml::Controls::MenuFlyout(uint32_t)> context;
    std::function<void(uint32_t)> groupContext;
    std::function<void()> afterClose;
    std::vector<Microsoft::UI::Xaml::Controls::MenuFlyout> openMenus;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0), openingLifetime;
    ~NativeAllTabsPanel() {
        lifetime.reset();
        Close();
    }
    static uint32_t Id(Object const& o, wchar_t const* key) {
        auto n = NativeJson::RequireRevision(o, key);
        if (!n || n > UINT32_MAX) winrt::throw_hresult(E_INVALIDARG);
        return uint32_t(n);
    }
    static Object Read(Object const& o) {
        if (o.HasKey(L"closed")) {
            Id(o, L"closed");
            if (o.Size() != 1 && !(o.Size() == 2 && o.GetNamedBoolean(L"fallback", false)))
                winrt::throw_hresult(E_INVALIDARG);
            return o;
        }
        Id(o, L"session");
        Id(o, L"revision");
        Id(o, L"epoch");
        o.GetNamedBoolean(L"hidden");
        for (auto key : {L"title", L"search", L"more", L"duplicates", L"hiddenLabel", L"back", L"viewAll",
                         L"newContainer", L"manageContainers", L"addContainer", L"groupsLabel"})
            NativeJson::RequireLength(o.GetNamedString(key), 512);
        size_t pixels = 0;
        std::set<uint32_t> ids;
        auto entries = o.GetNamedArray(L"rows");
        if (entries.Size() > 2048) winrt::throw_hresult(E_INVALIDARG);
        for (auto v : entries) {
            auto row = v.GetObject();
            NativeJson::ValidateImage(row, pixels, {32, 16384});
            if (!ids.insert(Id(row, L"id")).second) winrt::throw_hresult(E_INVALIDARG);
            auto kind = row.GetNamedString(L"kind");
            if (kind != L"tab" && kind != L"group") winrt::throw_hresult(E_INVALIDARG);
            if (kind == L"tab")
                Id(row, L"tab");
            else if (row.GetNamedNumber(L"tab") != 0)
                winrt::throw_hresult(E_INVALIDARG);
            auto group = row.GetNamedNumber(L"group");
            if (!std::isfinite(group) || group < 0 || group > UINT32_MAX || floor(group) != group)
                winrt::throw_hresult(E_INVALIDARG);
            for (auto key : {L"label", L"container", L"close", L"mute"})
                NativeJson::RequireLength(row.GetNamedString(key), 512);
            NativeJson::RequireLength(row.GetNamedString(L"detail"), 2048);
            NativeJson::RequireLength(row.GetNamedString(L"status", L""), 512);
            if (row.GetNamedString(L"label").empty()) winrt::throw_hresult(E_INVALIDARG);
            for (auto key : {L"selected", L"audio", L"muted", L"pinned", L"draggable", L"droppable"})
                row.GetNamedBoolean(key);
        }
        std::set<uint32_t> containers;
        auto options = o.GetNamedArray(L"containers");
        if (options.Size() > 128) winrt::throw_hresult(E_INVALIDARG);
        for (auto v : options) {
            auto c = v.GetObject();
            if (!containers.insert(Id(c, L"id")).second) winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(c.GetNamedString(L"label"), 512);
        }
        auto validateContext = [](Object const& row) {
            auto entries = row.GetNamedArray(L"context");
            if (entries.Size() > 7) winrt::throw_hresult(E_INVALIDARG);
            std::set<uint32_t> commands;
            for (auto v : entries) {
                auto c = v.GetObject();
                auto id = Id(c, L"command");
                if (id > 7 || !commands.insert(id).second) winrt::throw_hresult(E_INVALIDARG);
                NativeJson::RequireLength(c.GetNamedString(L"label"), 512);
                c.GetNamedBoolean(L"enabled");
            }
        };
        for (auto v : entries) {
            auto row = v.GetObject();
            if (row.GetNamedString(L"kind") == L"group") validateContext(row);
        }
        auto groups = o.GetNamedArray(L"groups");
        if (groups.Size() > 256) winrt::throw_hresult(E_INVALIDARG);
        std::set<uint32_t> groupIds;
        for (auto v : groups) {
            auto row = v.GetObject();
            Id(row, L"generation");
            if (!groupIds.insert(Id(row, L"id")).second) winrt::throw_hresult(E_INVALIDARG);
            NativeJson::RequireLength(row.GetNamedString(L"label"), 512);
            row.GetNamedBoolean(L"saved");
            validateContext(row);
        }
        if (o.Stringify().size() > 1048576) winrt::throw_hresult(E_INVALIDARG);
        return o;
    }
    static std::vector<uint32_t> Actions() { return {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}; }
    uint32_t Open(Microsoft::UI::Xaml::Controls::Button const& source) {
        if (opening) {
            LightDismiss();
            return 0;
        }
        uint32_t s{};
        if (!core || shell_core_all_tabs_panel_open(core, &s) != SHELL_STATUS_OK || !s) return 0;
        forceClosing = closing = false;
        opening = s;
        anchor = source;
        openingLifetime = std::make_shared<int>(0);
        return s;
    }
    void ObserveMenu(Microsoft::UI::Xaml::Controls::MenuFlyout const& menu) {
        auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
        auto weak = winrt::make_weak(menu);
        menu.Opening([this, alive, current, weak](auto const&, auto const&) {
            if (alive.expired() || current.expired()) return;
            if (auto child = weak.get();
                child && std::find(openMenus.begin(), openMenus.end(), child) == openMenus.end())
                openMenus.push_back(child);
        });
        menu.Closed([this, alive, current, weak](auto const&, auto const&) {
            if (alive.expired() || current.expired()) return;
            if (auto child = weak.get()) std::erase(openMenus, child);
        });
    }
    void HideMenus() {
        auto menus = std::exchange(openMenus, {});
        for (auto i = menus.rbegin(); i != menus.rend(); ++i) i->Hide();
    }
    void ResetPreview() {
        dropRow = 0;
        for (auto const& [id, marker] : markers) marker.Visibility(Microsoft::UI::Xaml::Visibility::Collapsed);
    }
    void Close() {
        forceClosing = closing = true;
        if (cancelDrag && dragging && dragging()) cancelDrag();
        opening = revision = epoch = 0;
        HideMenus();
        commandFingerprint = L"";
        openingLifetime.reset();
        afterClose = {};
        ResetPreview();
        rows.clear();
        groupModels.clear();
        realized.clear();
        markers.clear();
        rendered.clear();
        icons.clear();
        visuals.clear();
        if (list) list.ItemsSource(nullptr);
        items.Clear();
        if (core) shell_core_all_tabs_panel_dismiss(core);
        if (flyout) flyout.Hide();
        flyout = nullptr;
        list = nullptr;
        anchor = nullptr;
    }
    void LightDismiss(bool force = false) {
        if (!force && ((dragging && dragging()) || (deferOutside && deferOutside()))) return;
        if (force) {
            forceClosing = closing = true;
            if (cancelDrag) cancelDrag();
        }
        if (flyout)
            flyout.Hide();
        else if (opening) {
            auto a = Message(0);
            if (dispatch) dispatch(a);
            Close();
        }
    }
    Object Message(uint32_t operation, uint32_t row = 0) const {
        Object a;
        a.SetNamedValue(L"kind", Value::CreateStringValue(L"all-tabs"));
        a.SetNamedValue(L"menuSession", Value::CreateNumberValue(opening));
        a.SetNamedValue(L"revision", Value::CreateNumberValue(revision));
        a.SetNamedValue(L"epoch", Value::CreateNumberValue(epoch));
        a.SetNamedValue(L"operation", Value::CreateNumberValue(operation));
        a.SetNamedValue(L"row", Value::CreateNumberValue(row));
        return a;
    }
    bool Send(Object const& action, bool retained) {
        if (!opening || !dispatch || Id(action, L"revision") != revision || Id(action, L"epoch") != epoch) return false;
        bool accepted{};
        if (shell_core_all_tabs_panel_activate(core, opening, revision, Id(action, L"operation"), retained,
                                               &accepted) != SHELL_STATUS_OK ||
            !accepted)
            return false;
        if (retained)
            dispatch(action);
        else {
            auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
            afterClose = [this, action, alive, current] {
                if (alive.expired() || current.expired()) return;
                if (releaseFocus) releaseFocus();
                if (dispatch) dispatch(action);
            };
            HideMenus();
            if (flyout) flyout.Hide();
        }
        return true;
    }
    void Send(uint32_t op, uint32_t row = 0) {
        if (GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0))
            fprintf(stderr, "AllTabs send session=%u revision=%u op=%u row=%u\n", opening, revision, op, row);
        Send(Message(op, row),
             op != 1 && op != 5 && op != 7 && op != 10 && op != 11 && op != 13 && op != 14 && op != 15);
    }
    static winrt::Windows::Foundation::Rect ScreenBounds(Microsoft::UI::Xaml::FrameworkElement const& element) {
        auto local = element.TransformToVisual(nullptr).TransformBounds(
            {0, 0, float(element.ActualWidth()), float(element.ActualHeight())});
        auto screen = element.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
        return {float(screen.X), float(screen.Y), float(screen.Width), float(screen.Height)};
    }
    bool Contains(POINT p) const {
        if (closing || !opening || !list || !flyout || !list.IsLoaded()) return false;
        auto b = ScreenBounds(list);
        return p.x >= b.X && p.x < b.X + b.Width && p.y >= b.Y && p.y < b.Y + b.Height;
    }
    static Microsoft::UI::Xaml::Controls::ScrollViewer Scroller(Microsoft::UI::Xaml::DependencyObject const& node) {
        using namespace Microsoft::UI::Xaml;
        if (auto s = node.try_as<Controls::ScrollViewer>()) return s;
        for (int i = 0; i < Media::VisualTreeHelper::GetChildrenCount(node); ++i)
            if (auto s = Scroller(Media::VisualTreeHelper::GetChild(node, i))) return s;
        return nullptr;
    }
    bool Preview(POINT p) {
        using namespace Microsoft::UI::Xaml;
        ResetPreview();
        if (!Contains(p)) return false;
        auto bounds = ScreenBounds(list);
        if (auto scroll = Scroller(list); scroll && (p.y < bounds.Y + 24 || p.y > bounds.Y + bounds.Height - 24)) {
            auto target =
                std::clamp(scroll.VerticalOffset() + (p.y < bounds.Y + 24 ? -10 : 10), 0.0, scroll.ScrollableHeight());
            scroll.ChangeView(nullptr, winrt::box_value(target).as<winrt::Windows::Foundation::IReference<double>>(),
                              nullptr, true);
        }
        std::vector<ShellAllTabsRow> measured;
        for (auto const& [id, weak] : realized)
            if (auto item = weak.get(); item && item.IsLoaded() && rows.count(id) &&
                                        winrt::unbox_value<uint32_t>(list.ItemFromContainer(item)) == id) {
                auto r = ScreenBounds(item);
                if (!rows.at(id).GetNamedBoolean(L"droppable")) {
                    if (p.y >= r.Y && p.y < r.Y + r.Height) return false;
                    continue;
                }
                measured.push_back({id, r.Y, r.Height});
            }
        if (shell_core_all_tabs_panel_project(core, measured.data(), measured.size(), p.y, bounds.Y,
                                              bounds.Y + bounds.Height, &dropRow, &dropBefore) != SHELL_STATUS_OK)
            return false;
        if (dropRow && markers.count(dropRow)) {
            auto mark = markers.at(dropRow);
            mark.VerticalAlignment(dropBefore ? VerticalAlignment::Top : VerticalAlignment::Bottom);
            mark.Visibility(Visibility::Visible);
        }
        return dropRow != 0;
    }
    bool Drop(HWND source, uint32_t session, uint32_t tab, uint32_t group) {
        if (!dropRow) return false;
        auto action = Message(4, dropRow);
        action.SetNamedValue(L"sourceHwnd", Value::CreateNumberValue(double(reinterpret_cast<uintptr_t>(source))));
        action.SetNamedValue(L"sourceSession", Value::CreateNumberValue(session));
        action.SetNamedValue(L"sourceTab", Value::CreateNumberValue(tab));
        action.SetNamedValue(L"sourceGroup", Value::CreateNumberValue(group));
        action.SetNamedValue(L"before", Value::CreateBooleanValue(dropBefore));
        return Send(action, true);
    }
    Microsoft::UI::Xaml::Controls::MenuFlyout GroupMenu(Object const&, uint32_t id) {
        using namespace Microsoft::UI::Xaml;
        using namespace Controls;
        MenuFlyout menu;
        MenuFlyoutItem pending;
        pending.Text(L"…");
        pending.IsEnabled(false);
        menu.Items().Append(pending);
        menu.Opening([this, id, weak = winrt::make_weak(menu), alive = std::weak_ptr<int>(lifetime),
                      current = std::weak_ptr<int>(openingLifetime)](auto const&, auto const&) {
            if (alive.expired() || current.expired()) return;
            auto menu = weak.get();
            if (!menu) return;
            menu.Items().Clear();
            auto row = rows.count(id) ? rows.at(id) : (groupModels.count(id) ? groupModels.at(id) : Object{nullptr});
            if (!row) return;
            for (auto v : row.GetNamedArray(L"context")) {
                auto c = v.GetObject();
                MenuFlyoutItem item;
                item.Text(c.GetNamedString(L"label"));
                item.IsEnabled(c.GetNamedBoolean(L"enabled"));
                Automation::AutomationProperties::SetAutomationId(
                    item, AutomationId(L"FennAllTabs-group-context-" + winrt::to_hstring(id) + L"-" +
                                       winrt::to_hstring(Id(c, L"command"))));
                item.Click([this, id, command = Id(c, L"command"), commandEpoch = epoch, alive, current](auto const&,
                                                                                                         auto const&) {
                    if (alive.expired() || current.expired() || commandEpoch != epoch) return;
                    auto a = Message(14, id);
                    a.SetNamedValue(L"groupCommand", Value::CreateNumberValue(command));
                    Send(a, false);
                });
                menu.Items().Append(item);
            }
        });
        ObserveMenu(menu);
        return menu;
    }
    void RefreshIcon(uint32_t id) {
        using namespace Microsoft::UI::Xaml;
        if (!icons.count(id) || !rows.count(id)) return;
        auto& icon = icons.at(id);
        auto row = rows.at(id);
        Media::ImageSource source{nullptr};
        if (row.HasKey(L"image") &&
            row.GetNamedValue(L"image").ValueType() == winrt::Windows::Data::Json::JsonValueType::Object) {
            auto data = row.GetNamedObject(L"image");
            auto pixels = data.GetNamedString(L"pixels");
            if (pixels != icon.pixels) {
                icon.pixels = pixels;
                source = NativeJson::DecodeImage(data);
            } else
                source = icon.image.Source();
        } else {
            icon.pixels = L"";
            if (image) source = image(icon.tab);
        }
        if (icon.image.Source() != source) icon.image.Source(source);
        icon.image.Visibility(source ? Visibility::Visible : Visibility::Collapsed);
        icon.fallback.Visibility(source ? Visibility::Collapsed : Visibility::Visible);
    }
    void RefreshTabImage(uint32_t tab) {
        for (auto const& [id, icon] : icons)
            if (icon.tab == tab) RefreshIcon(id);
    }
    void Populate(Microsoft::UI::Xaml::Controls::ListViewItem const& item, uint32_t id) {
        using namespace Microsoft::UI::Xaml;
        using namespace Controls;
        if (!rows.count(id)) return;
        auto row = rows.at(id);
        auto grid = item.ContentTemplateRoot().try_as<Grid>();
        if (!grid) return;
        const auto fingerprint = row.Stringify();
        const auto kind = row.GetNamedString(L"kind"), label = row.GetNamedString(L"label"),
                   container = row.GetNamedString(L"container");
        const bool bound = visuals.count(id) && visuals.at(id).root == grid;
        if (!bound) {
            grid.Children().Clear();
            grid.ColumnDefinitions().Clear();
            grid.MinHeight(32);
            grid.ColumnSpacing(8);
            for (auto width :
                 {GridLengthHelper::FromPixels(32), GridLengthHelper::FromValueAndType(1, GridUnitType::Star),
                  GridLengthHelper::Auto(), GridLengthHelper::Auto()}) {
                ColumnDefinition c;
                c.Width(width);
                grid.ColumnDefinitions().Append(c);
            }
            RowVisual visual;
            visual.root = grid;
            visual.text.TextTrimming(TextTrimming::CharacterEllipsis);
            visual.text.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(visual.text, 1);
            grid.Children().Append(visual.text);
            Grid leading;
            leading.Width(32);
            leading.ColumnSpacing(4);
            for (auto width : {12.0, 16.0}) {
                ColumnDefinition column;
                column.Width(GridLengthHelper::FromPixels(width));
                leading.ColumnDefinitions().Append(column);
            }
            visual.check.Glyph(L"\xE73E");
            visual.check.FontSize(12);
            leading.Children().Append(visual.check);
            if (kind == L"tab") {
                RowIcon icon;
                icon.tab = Id(row, L"tab");
                icon.image.Width(16);
                icon.image.Height(16);
                icon.fallback.Glyph(L"\xE774");
                icon.fallback.FontSize(16);
                Grid::SetColumn(icon.image, 1);
                Grid::SetColumn(icon.fallback, 1);
                leading.Children().Append(icon.image);
                leading.Children().Append(icon.fallback);
                icons.insert_or_assign(id, icon);
            } else {
                FontIcon icon;
                icon.Glyph(L"\xE8F1");
                icon.FontSize(16);
                Grid::SetColumn(icon, 1);
                leading.Children().Append(icon);
            }
            grid.Children().Append(leading);
            auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
            auto button = [&](uint32_t op, wchar_t const* glyph, int column) {
                auto b =
                    Markup::XamlReader::Load(
                        LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" Width="28" Height="28" MinWidth="0" MinHeight="0" Padding="0"/>)")
                        .as<Button>();
                FontIcon icon;
                icon.FontSize(12);
                icon.Glyph(glyph);
                b.Content(icon);
                Automation::AutomationProperties::SetAutomationId(
                    b, AutomationId(L"FennAllTabs-" + winrt::to_hstring(op) + L"-" + winrt::to_hstring(id)));
                Grid::SetColumn(b, column);
                b.Click([this, alive, current, id, op](auto const&, auto const&) {
                    if (!alive.expired() && !current.expired()) Send(op, id);
                });
                grid.Children().Append(b);
                return b;
            };
            if (kind == L"tab") {
                visual.mute = button(3, L"\xE767", 2);
                visual.muteGlyph = visual.mute.Content().as<FontIcon>();
                visual.close = button(2, L"\xE711", 3);
                if (context) item.ContextFlyout(context(Id(row, L"tab")));
            } else
                item.ContextFlyout(GroupMenu(row, id));
            auto marker =
                Markup::XamlReader::Load(
                    LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Height="2" Background="{ThemeResource AccentFillColorDefaultBrush}" IsHitTestVisible="False" Visibility="Collapsed"/>)")
                    .as<Border>();
            Grid::SetColumnSpan(marker, 4);
            grid.Children().Append(marker);
            markers[id] = marker;
            visuals.insert_or_assign(id, visual);
        }
        realized[id] = winrt::make_weak(item);
        if (!bound || rendered[id] != fingerprint) {
            auto& visual = visuals.at(id);
            visual.text.Text(label + (container.empty() ? L"" : L" · " + container));
            visual.text.FontWeight({uint16_t(kind == L"group" ? 600 : 400)});
            visual.check.Visibility(row.GetNamedBoolean(L"selected") ? Visibility::Visible : Visibility::Collapsed);
            grid.Margin({kind == L"tab" && row.GetNamedNumber(L"group") ? 12.0 : 0.0, 0, 0, 0});
            Automation::AutomationProperties::SetName(item, label + (container.empty() ? L"" : L", " + container));
            Automation::AutomationProperties::SetHelpText(item, row.GetNamedString(L"detail"));
            Automation::AutomationProperties::SetAutomationId(
                item, AutomationId(L"FennAllTabs-row-" + winrt::to_hstring(id)));
            Automation::AutomationProperties::SetItemStatus(item, row.GetNamedString(L"status", L""));
            if (kind == L"tab") {
                const bool audio = row.GetNamedBoolean(L"audio"), pinned = row.GetNamedBoolean(L"pinned");
                auto focused = Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(item.XamlRoot());
                if ((!audio && focused == visual.mute) || (pinned && focused == visual.close))
                    item.Focus(FocusState::Programmatic);
                visual.mute.Visibility(audio ? Visibility::Visible : Visibility::Collapsed);
                visual.close.Visibility(pinned ? Visibility::Collapsed : Visibility::Visible);
                visual.muteGlyph.Glyph(row.GetNamedBoolean(L"muted") ? L"\xE74F" : L"\xE767");
                Automation::AutomationProperties::SetName(visual.close, row.GetNamedString(L"close"));
                Automation::AutomationProperties::SetName(visual.mute, row.GetNamedString(L"mute"));
                ToolTipService::SetToolTip(visual.close, winrt::box_value(row.GetNamedString(L"close")));
                ToolTipService::SetToolTip(visual.mute, winrt::box_value(row.GetNamedString(L"mute")));
            }
            rendered[id] = fingerprint;
        }
        RefreshIcon(id);
    }
    void Apply(Object const& input) {
        using namespace Microsoft::UI::Xaml;
        using namespace Controls;
        auto model = Read(input);
        if (model.HasKey(L"closed")) {
            if (Id(model, L"closed") == opening) {
                const bool fallback = model.GetNamedBoolean(L"fallback", false);
                Close();
                if (fallback && releaseFocus) releaseFocus();
            }
            return;
        }
        if (Id(model, L"session") != opening) return;
        auto ids = Actions();
        auto fingerprint = model.Stringify();
        bool changed{};
        winrt::check_hresult(shell_core_all_tabs_panel_accept(core, opening, Id(model, L"revision"), ids.data(),
                                                              ids.size(),
                                                              reinterpret_cast<uint16_t const*>(fingerprint.c_str()),
                                                              fingerprint.size(), &changed) == SHELL_STATUS_OK
                                 ? S_OK
                                 : E_INVALIDARG);
        if (!changed) return;
        if (epoch && epoch != Id(model, L"epoch")) HideMenus();
        revision = Id(model, L"revision");
        epoch = Id(model, L"epoch");
        std::vector<uint32_t> order;
        std::map<uint32_t, Object> next;
        for (auto v : model.GetNamedArray(L"rows")) {
            auto row = v.GetObject();
            auto id = Id(row, L"id");
            order.push_back(id);
            next.emplace(id, row);
        }
        rows = std::move(next);
        groupModels.clear();
        for (auto v : model.GetNamedArray(L"groups")) {
            auto row = v.GetObject();
            groupModels.emplace(Id(row, L"id"), row);
        }
        auto alive = std::weak_ptr<int>(lifetime), current = std::weak_ptr<int>(openingLifetime);
        const auto commandEpoch = epoch;
        bool first = !flyout;
        if (first) {
            flyout = Flyout();
            flyout.XamlRoot(anchor.XamlRoot());
            flyout.OverlayInputPassThroughElement(anchor.XamlRoot().Content());
            flyout.ShouldConstrainToRootBounds(false);
            flyout.Placement(anchor.FlowDirection() == FlowDirection::RightToLeft
                                 ? Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight
                                 : Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
            StackPanel panel;
            panel.Width(360);
            panel.Spacing(4);
            Automation::AutomationProperties::SetAutomationId(panel, AutomationId(L"FennAllTabsPanel"));
            StackPanel header;
            header.Orientation(Orientation::Horizontal);
            header.Spacing(4);
            panel.Children().Append(header);
            back = Button();
            Automation::AutomationProperties::SetAutomationId(back, AutomationId(L"FennAllTabs-back"));
            back.Click([this, alive, current](auto const&, auto const&) {
                if (!alive.expired() && !current.expired()) Send(9);
            });
            header.Children().Append(back);
            search = Button();
            search.Click([this, alive, current](auto const&, auto const&) {
                if (!alive.expired() && !current.expired()) Send(5);
            });
            header.Children().Append(search);
            more = DropDownButton();
            header.Children().Append(more);
            list = ListView();
            list.MaxHeight(480);
            list.MinHeight(36);
            list.SelectionMode(ListViewSelectionMode::Single);
            list.IsItemClickEnabled(true);
            list.ItemsSource(items);
            list.ItemTemplate(
                Markup::XamlReader::Load(
                    LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"><Grid HorizontalAlignment="Stretch"/></DataTemplate>)")
                    .as<DataTemplate>());
            list.ItemContainerStyle(
                Markup::XamlReader::Load(
                    LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="ListViewItem"><Setter Property="HorizontalContentAlignment" Value="Stretch"/><Setter Property="Padding" Value="8,2"/></Style>)")
                    .as<Style>());
            Automation::AutomationProperties::SetAutomationId(list, AutomationId(L"FennAllTabsList"));
            panel.Children().Append(list);
            flyout.Content(panel);
            auto populate = [this, alive, current](auto const&, auto const& args) {
                if (alive.expired() || current.expired() || args.InRecycleQueue()) return;
                Populate(args.ItemContainer().template as<ListViewItem>(), winrt::unbox_value<uint32_t>(args.Item()));
            };
            list.ContainerContentChanging([this, alive, current, populate](auto const& sender, auto const& args) {
                if (alive.expired() || current.expired()) return;
                if (args.InRecycleQueue()) {
                    for (auto i = realized.begin(); i != realized.end();)
                        if (i->second.get() == args.ItemContainer()) {
                            markers.erase(i->first);
                            rendered.erase(i->first);
                            icons.erase(i->first);
                            visuals.erase(i->first);
                            i = realized.erase(i);
                        } else
                            ++i;
                    return;
                }
                if (args.ItemContainer().ContentTemplateRoot())
                    populate(sender, args);
                else
                    args.RegisterUpdateCallback(
                        1,
                        winrt::Windows::Foundation::TypedEventHandler<ListViewBase, ContainerContentChangingEventArgs>(
                            populate));
            });
            // Suppress only the pointer click following a promoted drag. A
            // later keyboard activation is a fresh native interaction.
            list.PreviewKeyDown([this, alive, current](auto const&, auto const& args) {
                if (alive.expired() || current.expired()) return;
                // Standard WinUI Application hosting has no pane message-pump
                // filter. Cancel on the routed key, not a timer's key-state poll
                // which can miss a complete press between drag ticks.
                if (args.Key() == winrt::Windows::System::VirtualKey::Escape && dragging && dragging() && cancelDrag) {
                    cancelDrag();
                    args.Handled(true);
                    return;
                }
                if ((args.Key() == winrt::Windows::System::VirtualKey::Enter ||
                     args.Key() == winrt::Windows::System::VirtualKey::Space) &&
                    (!dragging || !dragging()))
                    suppressClick = false;
            });
            list.ItemClick([this, alive, current](auto const&, auto const& args) {
                if (!alive.expired() && !current.expired()) {
                    if (GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0))
                        fprintf(stderr, "AllTabs item-click row=%u suppressed=%d\n",
                                winrt::unbox_value<uint32_t>(args.ClickedItem()), suppressClick);
                    if (!suppressClick) Send(1, winrt::unbox_value<uint32_t>(args.ClickedItem()));
                }
            });
            list.AddHandler(
                UIElement::PointerPressedEvent(),
                winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler([this, alive, current](
                                                                                     auto const&, auto const& args) {
                    if (alive.expired() || current.expired()) return;
                    suppressClick = false;
                    middleRow = 0;
                    auto node = args.OriginalSource().template try_as<DependencyObject>();
                    ListViewItem item{nullptr};
                    while (node && node != list) {
                        if (node.template try_as<Primitives::ButtonBase>() &&
                            !args.GetCurrentPoint(nullptr).Properties().IsMiddleButtonPressed())
                            return;
                        if (auto found = node.template try_as<ListViewItem>()) {
                            item = found;
                            break;
                        }
                        node = Media::VisualTreeHelper::GetParent(node);
                    }
                    if (!item) return;
                    auto id = winrt::unbox_value<uint32_t>(list.ItemFromContainer(item));
                    if (!rows.count(id)) return;
                    auto row = rows.at(id);
                    if (GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0))
                        fprintf(stderr, "AllTabs press session=%u row=%u tab=%u draggable=%d\n", opening, id,
                                uint32_t(row.GetNamedNumber(L"tab")), row.GetNamedBoolean(L"draggable"));
                    if (args.GetCurrentPoint(nullptr).Properties().IsMiddleButtonPressed()) middleRow = id;
                    if (args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed() &&
                        row.GetNamedBoolean(L"draggable") && armDrag)
                        armDrag(uint32_t(row.GetNamedNumber(L"tab")),
                                row.GetNamedString(L"kind") == L"group" ? uint32_t(row.GetNamedNumber(L"group")) : 0,
                                item);
                })),
                true);
            list.AddHandler(UIElement::PointerReleasedEvent(),
                            winrt::box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                                [this, alive, current](auto const&, auto const& args) {
                                    if (alive.expired() || current.expired() ||
                                        args.GetCurrentPoint(nullptr).Properties().PointerUpdateKind() !=
                                            Microsoft::UI::Input::PointerUpdateKind::MiddleButtonReleased)
                                        return;
                                    auto id = std::exchange(middleRow, 0u);
                                    if (!id || !rows.count(id)) return;
                                    auto node = args.OriginalSource().template try_as<DependencyObject>();
                                    while (node && node != list) {
                                        if (auto item = node.template try_as<ListViewItem>()) {
                                            if (winrt::unbox_value<uint32_t>(list.ItemFromContainer(item)) == id) {
                                                args.Handled(true);
                                                Send(rows.at(id).GetNamedString(L"kind") == L"group" ? 12 : 2, id);
                                            }
                                            break;
                                        }
                                        node = Media::VisualTreeHelper::GetParent(node);
                                    }
                                })),
                            true);
            list.PointerCanceled([this, alive](auto const&, auto const&) {
                if (!alive.expired()) middleRow = 0;
            });
            flyout.Closing([this, alive, current](auto const&, auto const& args) {
                if (alive.expired() || current.expired()) return;
                if (!forceClosing && ((dragging && dragging()) || (deferOutside && deferOutside())))
                    args.Cancel(true);
                else
                    closing = true;
            });
            flyout.Closed([this, alive, session = opening](auto const&, auto const&) {
                if (alive.expired() || opening != session) return;
                if (cancelDrag) cancelDrag();
                HideMenus();
                auto message = Message(0);
                auto send = std::move(afterClose);
                afterClose = {};
                opening = 0;
                shell_core_all_tabs_panel_dismiss(core);
                if (send)
                    anchor.DispatcherQueue().TryEnqueue(std::move(send));
                else if (dispatch)
                    dispatch(message);
                rows.clear();
                groupModels.clear();
                realized.clear();
                markers.clear();
                rendered.clear();
                icons.clear();
                visuals.clear();
                if (list) list.ItemsSource(nullptr);
                items.Clear();
                flyout = nullptr;
                list = nullptr;
                anchor = nullptr;
            });
        }
        search.Content(winrt::box_value(model.GetNamedString(L"search")));
        Automation::AutomationProperties::SetAutomationId(search, AutomationId(L"FennAllTabs-search"));
        back.Content(winrt::box_value(model.GetNamedString(L"back")));
        back.Visibility(model.GetNamedBoolean(L"hidden") ? Visibility::Visible : Visibility::Collapsed);
        more.Content(winrt::box_value(model.GetNamedString(L"more")));
        Automation::AutomationProperties::SetAutomationId(more, AutomationId(L"FennAllTabs-more"));
        Automation::AutomationProperties::SetName(list, model.GetNamedString(L"title"));
        Automation::AutomationProperties::SetName(flyout, model.GetNamedString(L"title"));
        Object commandModel;
        for (auto key : {L"epoch", L"duplicates", L"hiddenLabel", L"viewAll", L"newContainer", L"manageContainers",
                         L"addContainer", L"groupsLabel", L"containers", L"groups"})
            commandModel.SetNamedValue(key, model.GetNamedValue(key));
        auto commandState = commandModel.Stringify();
        if (first || commandState != commandFingerprint) {
            commandFingerprint = commandState;
            MenuFlyout commands;
            auto command = [&](uint32_t op, winrt::hstring const& label) {
                if (label.empty()) return;
                MenuFlyoutItem item;
                item.Text(label);
                Automation::AutomationProperties::SetAutomationId(
                    item, AutomationId(L"FennAllTabs-command-" + winrt::to_hstring(op)));
                item.Click([this, alive, current, op, commandEpoch](auto const&, auto const&) {
                    if (!alive.expired() && !current.expired() && epoch == commandEpoch) Send(op);
                });
                commands.Items().Append(item);
            };
            command(6, model.GetNamedString(L"duplicates"));
            command(8, model.GetNamedString(L"hiddenLabel"));
            auto containers = model.GetNamedArray(L"containers");
            if (containers.Size()) {
                MenuFlyoutSubItem sub;
                sub.Text(model.GetNamedString(L"newContainer"));
                Automation::AutomationProperties::SetAutomationId(sub, AutomationId(L"FennAllTabs-containers"));
                for (auto v : containers) {
                    auto c = v.GetObject();
                    MenuFlyoutItem item;
                    item.Text(c.GetNamedString(L"label"));
                    Automation::AutomationProperties::SetAutomationId(
                        item, AutomationId(L"FennAllTabs-container-" + winrt::to_hstring(Id(c, L"id"))));
                    item.Click([this, alive, current, commandEpoch, id = Id(c, L"id")](auto const&, auto const&) {
                        if (alive.expired() || current.expired() || epoch != commandEpoch) return;
                        auto a = Message(10);
                        a.SetNamedValue(L"container", Value::CreateNumberValue(id));
                        Send(a, false);
                    });
                    sub.Items().Append(item);
                }
                commands.Items().Append(sub);
                command(15, model.GetNamedString(L"addContainer"));
                command(11, model.GetNamedString(L"manageContainers"));
            }
            auto groups = model.GetNamedArray(L"groups");
            if (groups.Size()) {
                MenuFlyoutSubItem sub;
                sub.Text(model.GetNamedString(L"groupsLabel"));
                Automation::AutomationProperties::SetAutomationId(sub, AutomationId(L"FennAllTabs-groups"));
                for (auto v : groups) {
                    auto g = v.GetObject();
                    MenuFlyoutItem item;
                    item.Text(g.GetNamedString(L"label"));
                    Automation::AutomationProperties::SetAutomationId(
                        item, AutomationId(L"FennAllTabs-group-" + winrt::to_hstring(Id(g, L"id"))));
                    item.ContextFlyout(GroupMenu(g, Id(g, L"id")));
                    item.Click([this, alive, current, commandEpoch, id = Id(g, L"id")](auto const&, auto const&) {
                        if (!alive.expired() && !current.expired() && epoch == commandEpoch) Send(13, id);
                    });
                    sub.Items().Append(item);
                }
                commands.Items().Append(sub);
            }
            command(7, model.GetNamedString(L"viewAll"));
            ObserveMenu(commands);
            more.Flyout(commands);
        }
        for (uint32_t i = 0; i < order.size(); ++i) {
            uint32_t found{};
            auto value = winrt::box_value(order[i]);
            if (i < items.Size() && winrt::unbox_value<uint32_t>(items.GetAt(i)) == order[i]) continue;
            for (found = i; found < items.Size(); ++found)
                if (winrt::unbox_value<uint32_t>(items.GetAt(found)) == order[i]) break;
            if (found < items.Size()) items.RemoveAt(found);
            items.InsertAt(i, value);
        }
        while (items.Size() > order.size()) items.RemoveAtEnd();
        for (auto i = realized.begin(); i != realized.end();)
            if (!rows.count(i->first)) {
                markers.erase(i->first);
                rendered.erase(i->first);
                icons.erase(i->first);
                visuals.erase(i->first);
                i = realized.erase(i);
            } else {
                if (auto item = i->second.get()) Populate(item, i->first);
                ++i;
            }
        if (first) {
            flyout.Opened([this, alive, current](auto const&, auto const&) {
                if (alive.expired() || current.expired()) return;
                for (uint32_t i = 0; i < items.Size(); ++i)
                    if (rows.at(winrt::unbox_value<uint32_t>(items.GetAt(i))).GetNamedBoolean(L"selected")) {
                        list.SelectedIndex(i);
                        list.ScrollIntoView(items.GetAt(i));
                        break;
                    }
                list.Focus(FocusState::Programmatic);
            });
            flyout.ShowAt(anchor);
        }
    }
};
