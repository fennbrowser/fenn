#include "pch.h"
#include "../../bridge/WinUIPaneApi.h"
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <Microsoft.UI.Dispatching.Interop.h>
#include <map>
#include <deque>
#include <algorithm>
#include <set>
#include <vector>
#include <memory>
#include <objbase.h>
#include <cstdio>

using namespace winrt;
using namespace Microsoft::UI;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Hosting;

#include "Address.h"
#include "TabMenu.h"
#include "TabStrip.h"

namespace {
bool DiagnosticsEnabled() {
    static const bool enabled = GetEnvironmentVariableW(L"FIREFOX_WINUI_PANE_DIAGNOSTICS", nullptr, 0) != 0;
    return enabled;
}

struct PaneApplication : ApplicationT<PaneApplication, Markup::IXamlMetadataProvider> {
    XamlTypeInfo::XamlControlsXamlMetaDataProvider provider;
    Markup::IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const& name) {
        return provider.GetXamlType(name);
    }
    Markup::IXamlType GetXamlType(hstring const& name) { return provider.GetXamlType(name); }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return provider.GetXmlnsDefinitions(); }
};

// Process-wide: exactly one drag gesture can be active at a time, mirroring
// how DoDragDrop (the design this replaces) only ever let one run too.
// sourceHwnd doubles as the "is a gesture active at all" flag. Armed vs.
// Dragging is not duplicated here -- it's read directly off
// sourceStrip->drag.phase wherever it matters, so there is exactly one
// place a gesture's phase can ever disagree with itself. Defined before
// struct Pane (unlike the functions below) because Pane::Close() reads its
// fields directly, which needs it to be a complete type.
struct TabDragSession {
    HWND sourceHwnd{};
    NativeTabStrip* sourceStrip{};
    uint32_t tabId{};
    // Whichever pane's own strip is currently showing a live "make room"
    // preview: sourceStrip itself while still hovering the strip the
    // gesture started on, a different pane's own strip while hovering it
    // (ForeignDragOver), or null during a true tear-off (neither).
    NativeTabStrip* hoverStrip{};
    // The tear-off visual (TabDragImage.h) -- only created once the cursor
    // first clears every open pane's own StripScreenBounds(), and destroyed
    // again as soon as it re-enters one; see ShowOrMoveTearoff/
    // DestroyTearoffWindow below.
    HWND tearoffWindow{};
    HBITMAP tearoffBitmap{};
    int tearoffWidth{}, tearoffHeight{};
};

// Forward declarations: Pane::Create wires strip->armDragSession/
// tickDragSession to these, and Pane::Close aborts an in-progress session
// referencing this pane -- both need to be visible before struct Pane, and
// (aside from dragSession/TabDragSession, already complete above) are only
// fully defined once Pane itself is a complete type.
struct Pane;
TabDragSession& dragSession();
void ArmDragSession(Pane& pane, uint32_t id);
void AbortDragSession();
void TickDragSession();

struct Pane {
    HWND parent{};
    std::unique_ptr<NativeAddress> address;
    DesktopWindowXamlSource source{nullptr};
    Controls::ListView tabs{nullptr};
    Windows::Foundation::Collections::IObservableVector<Windows::Foundation::IInspectable> items{nullptr};
    std::vector<uint32_t> tabIds;
    std::map<uint32_t, hstring> tabTitles;
    std::map<uint32_t, uint32_t> tabCommands;
    NativeTabMenu tabMenu;
    std::unique_ptr<NativeTabStrip> strip;
    event_token selection{}, takeFocus{}, containers{};
    uint32_t pendingSelection{};
    hstring pendingSelectionSet;
    uint32_t pendingSelectionRevision{};
    uint32_t pendingNewTabs{};
    std::deque<uint32_t> pendingCloses;
    uint32_t inFlightClose{};
    uint32_t browserSelection{};
    bool syncing{};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    // This strip's own on-screen rectangle, for another window's drag to
    // hit-test a drop point against (Pane.cpp's TickDragSession, checked
    // against every open pane in state().panes). TransformToVisual(nullptr)
    // gives the TabView's position relative to this pane's own hosted root
    // (matching the technique already used for hit-testing screen points
    // elsewhere in this codebase, e.g. the native drag test's own
    // tab-to-screen-point resolution); ClientToScreen anchors that to this
    // pane's owning HWND.
    RECT StripScreenBounds() const {
        RECT bounds{};
        if (!strip) return bounds;
        auto topLeft = strip->view.TransformToVisual(nullptr).TransformPoint({0, 0});
        POINT origin{LONG(topLeft.X), LONG(topLeft.Y)};
        ClientToScreen(parent, &origin);
        bounds.left = origin.x;
        bounds.top = origin.y;
        bounds.right = origin.x + LONG(strip->view.ActualWidth());
        bounds.bottom = origin.y + LONG(strip->view.ActualHeight());
        return bounds;
    }

    void Close() {
        if (!source) return;
        // The tick-based session (unlike the old thread-blocking DoDragDrop
        // it replaced) can genuinely still be in progress when a window
        // closes -- abort it rather than leave a dangling sourceStrip/
        // hoverStrip pointer for the next tick to dereference.
        if (strip && (strip.get() == dragSession().sourceStrip || strip.get() == dragSession().hoverStrip))
            AbortDragSession();
        tabMenu.Close();
        lifetime.reset();
        if (tabs) {
            tabs.SelectionChanged(selection);
            tabs.ContainerContentChanging(containers);
        }
        source.TakeFocusRequested(takeFocus);
        source.Content(nullptr);
        address.reset();
        strip.reset();
        tabs = nullptr;
        items = nullptr;
        tabIds.clear();
        tabTitles.clear();
        source.Close();
        source = nullptr;
    }
    void UpdateRow(Controls::Primitives::SelectorItem const& row) {
        auto index = tabs.IndexFromContainer(row);
        if (index < 0 || size_t(index) >= tabIds.size()) return;
        auto found = tabTitles.find(tabIds[index]);
        if (found == tabTitles.end()) return;
        auto const& title = found->second;
        Automation::AutomationProperties::SetName(row, title);
        Automation::AutomationProperties::SetItemStatus(row, tabIds[index] == browserSelection ? L"Active tab" : L"");
        if (auto root = row.ContentTemplateRoot()) {
            auto grid = root.as<Controls::Grid>();
            auto label = grid.Children().GetAt(0).as<Controls::TextBlock>();
            label.Text(title);
            label.FontWeight(Windows::UI::Text::FontWeight{uint16_t(tabIds[index] == browserSelection ? 600 : 400)});
            Controls::ToolTipService::SetToolTip(label, box_value(title));
            Automation::AutomationProperties::SetName(grid.Children().GetAt(1), L"Close tab: " + title);
        }
    }
    void AttachMenu(Controls::Primitives::SelectorItem const& row, bool replace = false, uint32_t itemId = 0) {
        auto index = tabs.IndexFromContainer(row);
        auto id = itemId ? itemId : index >= 0 && size_t(index) < tabIds.size() ? tabIds[index] : 0;
        if (!id || !tabMenu.HasTab(id)) {
            row.ContextFlyout(nullptr);
            return;
        }
        if (!replace && row.ContextFlyout()) return;
        Controls::MenuFlyout menu;
        menu.Items().Append(NativeTabMenu::Placeholder());
        menu.Opening([this, alive = std::weak_ptr<int>(lifetime), weakRow = make_weak(row), weakMenu = make_weak(menu)](
                         auto const&, auto const&) {
            if (alive.expired()) return;
            auto row = weakRow.get();
            auto menu = weakMenu.get();
            if (!row || !menu) return;
            auto index = tabs.IndexFromContainer(row);
            auto id = index >= 0 && size_t(index) < tabIds.size() ? tabIds[index] : 0;
            tabMenu.Open(menu, id);
        });
        menu.Opened([this, alive = std::weak_ptr<int>(lifetime), weak = make_weak(menu)](auto const&, auto const&) {
            if (alive.expired()) return;
            // Focus the first enabled item after its presenter is loaded.
            if (weak.get() == tabMenu.active) tabMenu.FocusFirst();
        });
        menu.Closed([this, alive = std::weak_ptr<int>(lifetime), weakRow = make_weak(row), weak = make_weak(menu)](
                        auto const&, auto const&) {
            if (alive.expired()) return;
            if (weak.get() == tabMenu.active) tabMenu.Dismiss();
            // Keep fresh WinUI input state; the next opening synchronously
            // renders the current Firefox state without another browser exchange.
            if (auto row = weakRow.get(); row && row.ContextFlyout() == weak.get()) AttachMenu(row, true);
        });
        row.ContextFlyout(menu);
    }
    void QueueTabSelection(uint32_t activate = 0) {
        NativeTabMenu::Array selected;
        uint32_t active = activate;
        for (auto item : tabs.SelectedItems()) {
            auto id = unbox_value<uint32_t>(item);
            selected.Append(NativeTabMenu::Value::CreateNumberValue(id));
            if (!active || (!activate && id == browserSelection)) active = id;
        }
        if (!active) return;
        NativeTabMenu::Object event;
        event.SetNamedValue(L"kind", NativeTabMenu::Value::CreateStringValue(L"selection"));
        event.SetNamedValue(L"revision", NativeTabMenu::Value::CreateNumberValue(tabMenu.stateRevision));
        event.SetNamedValue(L"tab", NativeTabMenu::Value::CreateNumberValue(active));
        event.SetNamedValue(L"tabs", selected);
        event.SetNamedValue(L"control",
                            NativeTabMenu::Value::CreateBooleanValue((GetKeyState(VK_CONTROL) & 0x8000) != 0));
        event.SetNamedValue(L"shift", NativeTabMenu::Value::CreateBooleanValue((GetKeyState(VK_SHIFT) & 0x8000) != 0));
        pendingSelectionSet = event.Stringify();
        pendingSelectionRevision = tabMenu.stateRevision;
    }
    void Sync(const WinUITab* snapshot, uint32_t count, uint32_t selected) {
        if (strip) {
            browserSelection = selected;
            if (pendingSelectionRevision != tabMenu.stateRevision) pendingSelectionSet = L"";
            strip->Sync(snapshot, count, selected);
            return;
        }
        syncing = true;
        browserSelection = selected;
        try {
            std::map<uint32_t, hstring> titles;
            std::map<uint32_t, uint32_t> commands;
            bool reordered = count != tabIds.size();
            for (uint32_t i = 0; i < count; ++i) {
                titles.emplace(snapshot[i].id, snapshot[i].title);
                commands.emplace(snapshot[i].id, snapshot[i].commands);
                if (i >= tabIds.size() || tabIds[i] != snapshot[i].id) reordered = true;
            }
            for (auto it = tabMenu.tabModels.begin(); it != tabMenu.tabModels.end();) {
                if (!commands.count(it->first))
                    it = tabMenu.tabModels.erase(it);
                else
                    ++it;
            }
            // Revoke the open menu when its target, ordering or eligibility changes.
            if (tabMenu.tab &&
                (reordered || !commands.count(tabMenu.tab) || commands[tabMenu.tab] != tabCommands[tabMenu.tab]))
                tabMenu.Dismiss(true);
            tabCommands.swap(commands);
            tabTitles.swap(titles);
            // Let ListView generate and recycle its own containers. Keep unchanged
            // data items so ordinary title/selection updates preserve scroll and focus.
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t match = i;
                while (match < tabIds.size() && tabIds[match] != snapshot[i].id) ++match;
                if (match != i || match == tabIds.size()) {
                    if (match < tabIds.size()) {
                        items.RemoveAt(match);
                        tabIds.erase(tabIds.begin() + match);
                    }
                    tabIds.insert(tabIds.begin() + i, snapshot[i].id);
                    // Data-item equality must use stable IDs, not possibly identical
                    // titles. Boxed duplicate strings break UIA peer enumeration.
                    items.InsertAt(i, box_value(snapshot[i].id));
                }
            }
            while (tabIds.size() > count) {
                items.RemoveAtEnd();
                tabIds.pop_back();
            }
            for (uint32_t i = 0; i < count; ++i) {
                if (auto row = tabs.ContainerFromIndex(i).try_as<Controls::ListViewItem>()) {
                    AttachMenu(row);
                    UpdateRow(row);
                }
            }
            if (tabMenu.hasSelection) {
                tabs.SelectionMode(Controls::ListViewSelectionMode::Extended);
                if (pendingSelectionRevision != tabMenu.stateRevision) pendingSelectionSet = L"";
                if (pendingSelectionSet.empty()) {
                    for (uint32_t i = 0; i < count; ++i) {
                        auto item = items.GetAt(i);
                        uint32_t existing;
                        bool wanted = tabMenu.selectedTabs.count(snapshot[i].id) != 0;
                        bool present = tabs.SelectedItems().IndexOf(item, existing);
                        if (wanted && !present) tabs.SelectedItems().Append(item);
                        if (!wanted && present) tabs.SelectedItems().RemoveAt(existing);
                    }
                }
            } else {
                tabs.SelectionMode(Controls::ListViewSelectionMode::Single);
                int index = -1;
                for (uint32_t i = 0; i < count; ++i)
                    if (snapshot[i].id == selected) index = int(i);
                if (tabs.SelectedIndex() != index) {
                    tabs.SelectedIndex(index);
                    if (index >= 0) tabs.ScrollIntoView(items.GetAt(index));
                }
            }
        } catch (...) {
            syncing = false;
            throw;
        }
        syncing = false;
    }
    void QueueStripSelection(uint32_t id) {
        if (!tabMenu.HasTab(id)) return;
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        auto selected = control ? tabMenu.selectedTabs : std::set<uint32_t>{};
        if (control && selected.count(id) && id != browserSelection)
            selected.erase(id);
        else
            selected.insert(id);
        selected.insert(control ? browserSelection : id);
        NativeTabMenu::Array ids;
        for (auto value : selected) ids.Append(NativeTabMenu::Value::CreateNumberValue(value));
        NativeTabMenu::Object event;
        event.SetNamedValue(L"kind", NativeTabMenu::Value::CreateStringValue(L"selection"));
        event.SetNamedValue(L"revision", NativeTabMenu::Value::CreateNumberValue(tabMenu.stateRevision));
        event.SetNamedValue(L"tab",
                            NativeTabMenu::Value::CreateNumberValue(selected.count(id) ? id : browserSelection));
        event.SetNamedValue(L"tabs", ids);
        event.SetNamedValue(L"control", NativeTabMenu::Value::CreateBooleanValue(control));
        event.SetNamedValue(L"shift", NativeTabMenu::Value::CreateBooleanValue(shift));
        pendingSelectionSet = event.Stringify();
        pendingSelectionRevision = tabMenu.stateRevision;
    }
    void Create(HWND hwnd, bool tabStrip = false) {
        parent = hwnd;
        source = DesktopWindowXamlSource();
        source.Initialize(GetWindowIdFromWindow(hwnd));
        if (tabStrip) {
            strip = std::make_unique<NativeTabStrip>(tabMenu);
            strip->ownerHwnd = hwnd;
            strip->select = [this](uint32_t id) { QueueStripSelection(id); };
            strip->close = [this](uint32_t id) {
                if (id != inFlightClose &&
                    std::find(pendingCloses.begin(), pendingCloses.end(), id) == pendingCloses.end())
                    pendingCloses.push_back(id);
                pendingSelectionSet = L"";
            };
            strip->add = [this] {
                if (pendingNewTabs < 10000) ++pendingNewTabs;
                pendingSelectionSet = L"";
            };
            takeFocus = source.TakeFocusRequested([this](auto const&, auto const&) { SetFocus(parent); });
            source.Content(strip->root);
            strip->armDragSession = [this](uint32_t id) { ArmDragSession(*this, id); };
            strip->tickDragSession = TickDragSession;
            return;
        }
        auto panel = Markup::XamlReader::Load(
                         LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                Background="{ThemeResource SolidBackgroundFillColorBaseBrush}"><Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="*"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions></Grid>)")
                         .as<Controls::Grid>();
        panel.Padding({16, 16, 16, 16});

        panel.Loaded([](auto const& sender, auto const&) {
            if (!DiagnosticsEnabled()) return;
            auto element = sender.template as<FrameworkElement>();
            fprintf(stderr, "WinUIPane loaded width=%.1f height=%.1f rootWidth=%.1f rootHeight=%.1f\n",
                    element.ActualWidth(), element.ActualHeight(), element.XamlRoot().Size().Width,
                    element.XamlRoot().Size().Height);
            auto panel = element.as<Controls::Grid>();
            auto background = panel.Background().as<Media::SolidColorBrush>().Color();
            auto foreground =
                panel.Children().GetAt(0).as<Controls::TextBlock>().Foreground().as<Media::SolidColorBrush>().Color();
            fprintf(stderr, "WinUIPane theme=%d background=%u,%u,%u,%u foreground=%u,%u,%u,%u\n",
                    int(panel.ActualTheme()), background.A, background.R, background.G, background.B, foreground.A,
                    foreground.R, foreground.G, foreground.B);
        });
        Controls::TextBlock title;
        title.Text(L"Tabs");
        title.Margin({0, 0, 0, 12});
        tabs = Controls::ListView();
        items = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
        tabs.ItemsSource(items);
        tabs.ItemTemplate(
            Markup::XamlReader::Load(
                LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"><Grid><Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions><TextBlock VerticalAlignment="Center" TextTrimming="CharacterEllipsis" /><Button Grid.Column="1" Width="32" Height="32" MinWidth="0" Padding="0" Margin="8,0,0,0" ToolTipService.ToolTip="Close tab" AutomationProperties.AutomationId="FirefoxWinUICloseTab"><SymbolIcon Symbol="Cancel" /></Button></Grid></DataTemplate>)")
                .as<DataTemplate>());
        tabs.ItemContainerStyle(
            Markup::XamlReader::Load(
                LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="ListViewItem"><Setter Property="HorizontalContentAlignment" Value="Stretch"/></Style>)")
                .as<Style>());
        containers =
            tabs.ContainerContentChanging([this](auto const&, Controls::ContainerContentChangingEventArgs const& args) {
                if (args.InRecycleQueue()) return;
                auto row = args.ItemContainer();
                // The container may not have an index yet during realization.
                // The data item already carries its stable Firefox ID.
                AttachMenu(row, false, unbox_value<uint32_t>(args.Item()));
                UpdateRow(row);
                args.RegisterUpdateCallback([this, alive = std::weak_ptr<int>(lifetime)](
                                                auto const&, Controls::ContainerContentChangingEventArgs const& ready) {
                    if (alive.expired() || ready.InRecycleQueue()) return;
                    auto row = ready.ItemContainer();
                    auto grid = row.ContentTemplateRoot().as<Controls::Grid>();
                    // Resolve deferred work from the current snapshot, including
                    // title changes that do not replace the stable ListView item.
                    UpdateRow(row);
                    auto button = grid.Children().GetAt(1).as<Controls::Button>();
                    if (button.Tag()) return;
                    button.Tag(box_value(true));
                    button.Click([this, alive = std::weak_ptr<int>(lifetime), weak = make_weak(row)](auto const&,
                                                                                                     auto const&) {
                        if (alive.expired() || syncing) return;
                        if (auto row = weak.get()) {
                            auto index = tabs.IndexFromContainer(row);
                            if (index >= 0 && size_t(index) < tabIds.size()) {
                                auto id = tabIds[index];
                                if (id == inFlightClose) return;
                                if (std::find(pendingCloses.begin(), pendingCloses.end(), id) == pendingCloses.end())
                                    pendingCloses.push_back(id);
                                // Clicking a background close button must not select its row.
                                pendingSelection = 0;
                                pendingSelectionSet = L"";
                            }
                        }
                    });
                });
            });
        tabs.IsItemClickEnabled(true);
        tabs.ItemClick(
            [this, alive = std::weak_ptr<int>(lifetime)](auto const&, Controls::ItemClickEventArgs const& args) {
                if (alive.expired() || syncing || !tabMenu.hasSelection) return;
                try {
                    QueueTabSelection(unbox_value<uint32_t>(args.ClickedItem()));
                } catch (...) {}
            });
        tabs.SelectionMode(Controls::ListViewSelectionMode::Single);
        Automation::AutomationProperties::SetAutomationId(tabs, L"FirefoxWinUITabs");
        Automation::AutomationProperties::SetName(tabs, L"Tabs in this window");
        Controls::Grid::SetRow(tabs, 1);
        selection = tabs.SelectionChanged([this](auto const&, Controls::SelectionChangedEventArgs const& args) {
            if (syncing) return;
            try {
                if (tabMenu.hasSelection) {
                    uint32_t endpoint = 0;
                    if (GetKeyState(VK_SHIFT) & 0x8000) {
                        if (args.AddedItems().Size())
                            endpoint = unbox_value<uint32_t>(args.AddedItems().GetAt(args.AddedItems().Size() - 1));
                        else if (auto focused =
                                     Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(tabs.XamlRoot())
                                         .try_as<Controls::ListViewItem>()) {
                            auto index = tabs.IndexFromContainer(focused);
                            if (index >= 0 && size_t(index) < tabIds.size()) endpoint = tabIds[index];
                        }
                    }
                    QueueTabSelection(endpoint);
                } else {
                    auto index = tabs.SelectedIndex();
                    if (index >= 0 && size_t(index) < tabIds.size()) pendingSelection = tabIds[index];
                }
            } catch (...) {}
        });
        takeFocus = source.TakeFocusRequested([this](auto const&, auto const&) { SetFocus(parent); });
        panel.Children().Append(title);
        panel.Children().Append(tabs);
        auto newTab =
            Markup::XamlReader::Load(
                LR"x(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Width="32" Height="32" MinWidth="0" Padding="0" HorizontalAlignment="Left" Margin="0,12,0,0" ToolTipService.ToolTip="New tab (Ctrl+T)" AutomationProperties.Name="New tab" AutomationProperties.AutomationId="FirefoxWinUINewTab"><SymbolIcon Symbol="Add" /></Button>)x")
                .as<Controls::Button>();
        Controls::Grid::SetRow(newTab, 2);
        newTab.Click([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired() || syncing) return;
            // Bounded per-window queue, drained only by Firefox's event loop.
            if (pendingNewTabs < 10000) ++pendingNewTabs;
            pendingSelection = 0;
        });
        panel.Children().Append(newTab);
        address = std::make_unique<NativeAddress>();
        Controls::Grid::SetRow(address->root, 3);
        panel.Children().Append(address->root);
        source.Content(panel);
    }
};

TabDragSession& dragSession() {
    static TabDragSession session;
    return session;
}

// Intentionally process-lifetime storage: no WinRT destruction under DLL detach.
struct State {
    DWORD thread{};
    bool stopped{};
    bool attempted{};
    HRESULT initialization{E_PENDING};
    HANDLE activation{INVALID_HANDLE_VALUE};
    ULONG_PTR activationCookie{};
    Dispatching::DispatcherQueueController dispatcher{nullptr};
    Application app{nullptr};
    WindowsXamlManager manager{nullptr};
    std::map<HWND, std::unique_ptr<Pane>> panes;
};
State& state() {
    static auto value = new State;
    return *value;
}

void ArmDragSession(Pane& pane, uint32_t id) {
    auto& session = dragSession();
    session.sourceHwnd = pane.parent;
    session.sourceStrip = pane.strip.get();
    session.tabId = id;
    session.hoverStrip = pane.strip.get();
    session.tearoffWindow = nullptr;
    session.tearoffBitmap = nullptr;
    session.tearoffWidth = session.tearoffHeight = 0;
}

void DestroyTearoffWindow(TabDragSession& session) {
    if (session.tearoffBitmap) {
        DeleteObject(session.tearoffBitmap);
        session.tearoffBitmap = nullptr;
    }
    if (session.tearoffWindow) {
        DestroyWindow(session.tearoffWindow);
        session.tearoffWindow = nullptr;
    }
    session.tearoffWidth = session.tearoffHeight = 0;
}

// Tears the session down regardless of outcome -- release, cancel, a
// foreign pane accepting the drop, or (defensively) the source pane itself
// having been closed mid-gesture, which this tick-based design allows in a
// way the old, thread-blocking DoDragDrop call structurally couldn't.
void AbortDragSession() {
    auto& session = dragSession();
    if (session.hoverStrip && session.hoverStrip != session.sourceStrip) session.hoverStrip->EndForeignDragOver();
    DestroyTearoffWindow(session);
    if (session.sourceHwnd && GetCapture() == session.sourceHwnd) ReleaseCapture();
    session = TabDragSession{};
}

constexpr wchar_t kTearoffClassName[] = L"FirefoxWinUITabTearoff";

void EnsureTearoffClassRegistered() {
    static const ATOM atom = [] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kTearoffClassName;
        return RegisterClassExW(&wc);
    }();
    (void)atom;
}

// Renders `row`'s current appearance into the session's tear-off bitmap --
// once, the first tick of Dragging, since the popup now represents the
// dragged tab for the whole gesture (TickDragSession), not just once it
// leaves a strip. A coroutine, not a blocking call, for the same reason
// TabDragImage::RenderElementToBitmapAsync itself is one (see that file).
// Guards against the session having moved on (a new gesture) by the time it
// resumes. Hides `row` itself only once this actually completes, not before
// -- rendering it first is what makes the bitmap show the tab at all rather
// than a blank/transparent image, since RenderTargetBitmap captures what
// would actually be composited, including opacity.
winrt::fire_and_forget RenderTearoffAsync(Controls::TabViewItem row, HWND popup, std::weak_ptr<int> alive) {
    int width{}, height{};
    uint64_t bitmap{};
    try {
        bitmap = co_await TabDragImage::RenderElementToBitmapAsync(row, &width, &height);
    } catch (...) {
        co_return;
    }
    auto& session = dragSession();
    if (alive.expired() || !bitmap || session.tearoffWindow != popup) {
        if (bitmap) DeleteObject(HBITMAP(bitmap));
        co_return;
    }
    session.tearoffBitmap = HBITMAP(bitmap);
    session.tearoffWidth = width;
    session.tearoffHeight = height;
    row.Opacity(0);
}

// Creates (lazily, on the first tick of Dragging) and repositions the
// tear-off popup -- the sole visual for the dragged tab for the whole
// gesture now (TickDragSession), so this fires immediately once promoted,
// not just once a tear-off actually happens. A plain WS_POPUP this code
// fully owns -- no OLE registration, so none of the ancestor-chain/z-order
// problems that made an overlay window unusable for OLE's own purposes
// apply here: this window only ever needs to be visually on top, never
// hit-testable by anything (WS_EX_TRANSPARENT), so it can never interfere
// with the geometric hit-testing TickDragSession does against every pane's
// own StripScreenBounds().
void ShowOrMoveTearoff(TabDragSession& session, POINT screenPoint) {
    if (!session.tearoffWindow) {
        EnsureTearoffClassRegistered();
        session.tearoffWindow =
            CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kTearoffClassName,
                             L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!session.tearoffWindow) return;
        ShowWindow(session.tearoffWindow, SW_SHOWNOACTIVATE);
        SetWindowPos(session.tearoffWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (session.sourceStrip && session.sourceStrip->rows.count(session.tabId))
            RenderTearoffAsync(session.sourceStrip->rows.at(session.tabId), session.tearoffWindow,
                                std::weak_ptr<int>(session.sourceStrip->lifetime));
    }
    if (!session.tearoffBitmap) return;
    POINT topLeft{screenPoint.x - session.tearoffWidth / 2, screenPoint.y - session.tearoffHeight / 2};
    TabDragImage::PaintDragVisual(session.tearoffWindow, session.tearoffBitmap, session.tearoffWidth,
                                   session.tearoffHeight, topLeft);
}

// Resolves an ended gesture (button no longer physically down) against
// wherever the cursor actually is right now: a different pane's own strip
// (AcceptForeignDrop), still this strip's own (CommitReorder), or neither
// (CommitDetach) -- the same three outcomes DoDragDrop's own return value
// used to distinguish, now decided by this code's own geometry instead of
// asking OLE to resolve a target window. `hitPane`, like TickDragSession's
// own search, only ever names a DIFFERENT pane -- whether release still
// counts as "on the source strip" is `drag.detaching`, which TickDragSession
// only just updated (via UpdateDragVisual) for this exact point when
// `hitPane` is null.
void ResolveDragDrop(Pane& sourcePane, Pane* hitPane, POINT screenPoint) {
    auto& session = dragSession();
    auto* strip = session.sourceStrip;
    if (hitPane && hitPane->strip) {
        RECT bounds = hitPane->StripScreenBounds();
        Windows::Foundation::Point local{float(screenPoint.x - bounds.left), float(screenPoint.y - bounds.top)};
        hitPane->strip->AcceptForeignDrop(sourcePane.parent, session.tabId, local);
        strip->FinishDrag(session.tabId);
    } else if (strip->drag.detaching) {
        strip->CommitDetach(session.tabId);
    } else {
        RECT bounds = sourcePane.StripScreenBounds();
        Windows::Foundation::Point local{float(screenPoint.x - bounds.left), float(screenPoint.y - bounds.top)};
        strip->CommitReorder(session.tabId, local);
    }
    AbortDragSession();
}

// Pane.cpp's own Translate() hook calls this unconditionally on every
// message it pretranslates, and TabStrip.h's PointerMoved calls it directly
// too while already Dragging, as a lower-latency accelerant -- both routes
// converge here. Ground truth is GetCursorPos()/GetAsyncKeyState(), never
// the triggering message's own content, so it doesn't matter which one (or
// neither) actually drove a given call: the gesture keeps advancing as long
// as this keeps getting ticked at all, which raw ::SetCapture (TabStrip.h's
// PointerPressed) exists specifically to guarantee regardless of where the
// cursor physically is.
void TickDragSession() {
    auto& session = dragSession();
    if (!session.sourceHwnd) return;
    auto& panes = state().panes;
    auto srcIt = panes.find(session.sourceHwnd);
    if (srcIt == panes.end() || srcIt->second->strip.get() != session.sourceStrip) {
        // The source pane was closed mid-gesture -- nothing left to resolve into.
        AbortDragSession();
        return;
    }
    Pane& sourcePane = *srcIt->second;
    if (session.sourceStrip->drag.phase == NativeTabStrip::DragPhase::None) {
        // Ended some other way already (Escape via WinUI's own
        // PreviewKeyDown, if it happened to still fire) -- just clean up
        // the session-level state that hasn't caught up yet.
        AbortDragSession();
        return;
    }
    bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (session.sourceStrip->drag.phase == NativeTabStrip::DragPhase::Armed) {
        RECT bounds = sourcePane.StripScreenBounds();
        POINT screenPoint{};
        GetCursorPos(&screenPoint);
        Windows::Foundation::Point local{float(screenPoint.x - bounds.left), float(screenPoint.y - bounds.top)};
        if (session.sourceStrip->TryPromote(session.tabId, local)) return;  // Dragging now; next tick tracks it.
        if (!buttonDown) {
            session.sourceStrip->FinishDrag(session.tabId);
            AbortDragSession();
        }
        return;
    }
    // Dragging.
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        session.sourceStrip->CancelDrag();
        AbortDragSession();
        return;
    }
    POINT screenPoint{};
    GetCursorPos(&screenPoint);
    // A DIFFERENT pane's own strip claiming this point always wins,
    // regardless of the source strip's own vertical margin below --
    // otherwise two windows sitting side by side at the same screen height
    // would never resolve as a foreign hover at all (local Y relative to
    // the source alone can't tell "moved to a horizontally different
    // window" from "still over my own strip").
    Pane* hitPane = nullptr;
    for (auto& entry : panes) {
        if (!entry.second->strip || entry.second.get() == &sourcePane) continue;
        RECT bounds = entry.second->StripScreenBounds();
        if (PtInRect(&bounds, screenPoint)) {
            hitPane = entry.second.get();
            break;
        }
    }
    NativeTabStrip* newHover = hitPane ? hitPane->strip.get() : nullptr;
    if (!hitPane) {
        // Not over any other pane -- on-strip vs. tear-off is decided by
        // the source strip's own existing vertical-margin heuristic
        // (UpdateDragVisual's drag.detaching), which already tolerates
        // horizontal excess past the strip's own left/right edge (e.g.
        // dragging past the last tab) as still "on this strip," not a
        // tear-off -- exactly the move-to-end gesture this is meant to
        // support. UpdateDragVisual no longer touches the dragged row's own
        // visual at all (see its own comment) -- only the floating popup,
        // positioned below, ever represents the dragged tab now.
        RECT bounds = sourcePane.StripScreenBounds();
        Windows::Foundation::Point local{float(screenPoint.x - bounds.left), float(screenPoint.y - bounds.top)};
        session.sourceStrip->UpdateDragVisual(local);
        if (!session.sourceStrip->drag.detaching) newHover = session.sourceStrip;
    }
    if (session.hoverStrip != newHover && session.hoverStrip && session.hoverStrip != session.sourceStrip)
        session.hoverStrip->EndForeignDragOver();
    session.hoverStrip = newHover;
    // The floating popup is the sole visual for the dragged tab for the
    // whole gesture now, not just once it leaves a strip -- TabStrip.h's
    // TryPromote/UpdateDragVisual never move or show the real row anymore
    // (RenderTearoffAsync hides it once the popup's own snapshot is
    // actually ready). Keeping one mechanism instead of switching between
    // "real row" and "floating bitmap" at each hover transition is what
    // actually fixed the class of bugs that switch kept producing (a
    // duplicate visual, an inconsistent flush/elevated look depending on
    // which one currently owned the row, two independently-tuned clamps
    // disagreeing with each other) -- there's no transition left to get
    // wrong. `snapPane` is whichever pane's own strip the popup should
    // visually snap into right now (source or foreign); null means a true
    // tear-off, where it just follows the cursor directly instead.
    Pane* snapPane = hitPane ? hitPane : (newHover ? &sourcePane : nullptr);
    if (snapPane) {
        // Snap into that strip's own row band (vertically centered on it,
        // same as a real row) rather than centering on the raw cursor
        // point, so it reads as "part of this strip" the same way a real
        // row would. Horizontally it still tracks the cursor (clamped to
        // the strip's own width) -- the same direct cursor-following
        // UpdateDragVisual's own clamp math uses, not a grid-slot snap.
        RECT bounds = snapPane->StripScreenBounds();
        POINT anchor{screenPoint.x, bounds.top + (bounds.bottom - bounds.top) / 2};
        int halfWidth = session.tearoffWidth / 2;
        anchor.x = std::clamp(anchor.x, bounds.left + halfWidth, std::max(bounds.left + halfWidth, bounds.right - halfWidth));
        ShowOrMoveTearoff(session, anchor);
    } else {
        // True tear-off -- no strip to snap into, so the visual just
        // follows the cursor directly.
        ShowOrMoveTearoff(session, screenPoint);
    }
    if (hitPane) {
        session.sourceStrip->CloseDragGap();
        RECT bounds = hitPane->StripScreenBounds();
        Windows::Foundation::Point local{float(screenPoint.x - bounds.left), float(screenPoint.y - bounds.top)};
        hitPane->strip->ForeignDragOver(local);
    }
    if (!buttonDown) ResolveDragDrop(sourcePane, hitPane, screenPoint);
}

HRESULT Check(HWND hwnd = nullptr) {
    auto& s = state();
    if (s.thread && s.thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    if (s.stopped) return RO_E_CLOSED;
    if (hwnd) {
        DWORD process{};
        if (!IsWindow(hwnd)) return E_INVALIDARG;
        if (GetWindowThreadProcessId(hwnd, &process) != GetCurrentThreadId() || process != GetCurrentProcessId())
            return RPC_E_WRONG_THREAD;
    }
    return S_OK;
}

void Initialize() {
    auto& s = state();
    if (s.attempted) {
        check_hresult(s.initialization);
        return;
    }
    s.attempted = true;
    s.thread = GetCurrentThreadId();
    const char* phase = "apartment";
    try {
        APTTYPE apartment;
        APTTYPEQUALIFIER qualifier;
        check_hresult(CoGetApartmentType(&apartment, &qualifier));
        check_bool(apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA);
        phase = "activation-context";
        HMODULE module{};
        check_bool(
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&Initialize), &module));
        ACTCTXW context{sizeof(context)};
        context.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        context.hModule = module;
        context.lpResourceName = MAKEINTRESOURCEW(2);
        s.activation = CreateActCtxW(&context);
        check_bool(s.activation != INVALID_HANDLE_VALUE);
        check_bool(ActivateActCtx(s.activation, &s.activationCookie));
        // Never take shutdown ownership of a dispatcher created by another host.
        phase = "dispatcher";
        if (Dispatching::DispatcherQueue::GetForCurrentThread()) throw_hresult(E_UNEXPECTED);
        s.dispatcher = Dispatching::DispatcherQueueController::CreateOnCurrentThread();
        phase = "application";
        s.app = make<PaneApplication>();
        // COM teardown can unload XAML after its dispatcher has shut down.
        // Its module teardown requires process-exit semantics, like this host DLL.
        phase = "xaml-module-lifetime";
        HMODULE xaml{};
        check_bool(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"Microsoft.UI.Xaml.dll", &xaml));
        phase = "xaml-manager";
        s.manager = WindowsXamlManager::InitializeForCurrentThread();
        phase = "resources";
        s.app.Resources().MergedDictionaries().Append(Controls::XamlControlsResources());
        s.initialization = S_OK;
    } catch (...) {
        s.initialization = to_hresult();
        fprintf(stderr, "WinUIPane initialization phase=%s hr=0x%08X\n", phase, unsigned(s.initialization));
        throw;
    }
}

HRESULT UpdateImpl(HWND hwnd, const RECT* rect, BOOL visible, bool strip) noexcept {
    try {
        check_hresult(Check(hwnd));
        if (!hwnd || !rect || rect->right < rect->left || rect->bottom < rect->top) return E_INVALIDARG;
        auto& s = state();
        auto found = s.panes.find(hwnd);
        if (!visible && found == s.panes.end()) return S_OK;
        if (found != s.panes.end() && bool(found->second->strip) != strip) {
            found->second->Close();
            s.panes.erase(found);
            found = s.panes.end();
            if (!visible) return S_OK;
        }
        if (found == s.panes.end()) {
            Initialize();
            auto pane = std::make_unique<Pane>();
            try {
                pane->Create(hwnd, strip);
            } catch (...) {
                auto error = to_hresult();
                pane->Close();
                return error;
            }
            found = s.panes.emplace(hwnd, std::move(pane)).first;
        }
        auto site = found->second->source.SiteBridge();
        site.MoveAndResize({rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top});
        HWND child = GetWindowFromWindowId(site.WindowId());
        if (visible) {
            // Firefox's compositor is a sibling HWND covering the client area.
            check_bool(SetWindowPos(child, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
            site.Show();
        } else {
            site.Hide();
        }
        if (DiagnosticsEnabled()) {
            RECT actual{};
            GetClientRect(child, &actual);
            fprintf(stderr, "WinUIPane geometry requested=%ld,%ld,%ld,%ld actual=%ld,%ld visible=%d top=%d\n",
                    rect->left, rect->top, rect->right, rect->bottom, actual.right, actual.bottom,
                    IsWindowVisible(child), GetWindow(child, GW_HWNDPREV) == nullptr);
        }
        return S_OK;
    } catch (hresult_error const& error) {
        fwprintf(stderr, L"WinUIPane update hr=0x%08X message=%s\n", unsigned(error.code()), error.message().c_str());
        return error.code();
    } catch (...) {
        return to_hresult();
    }
}
HRESULT WINAPI Update(HWND hwnd, const RECT* rect, BOOL visible) noexcept {
    return UpdateImpl(hwnd, rect, visible, false);
}
HRESULT WINAPI UpdateTabStrip(HWND hwnd, const RECT* rect, BOOL visible) noexcept {
    return UpdateImpl(hwnd, rect, visible, true);
}
HRESULT WINAPI Clip(HWND hwnd, const RECT* exclusions, uint32_t count) noexcept {
    try {
        check_hresult(Check(hwnd));
        if (!hwnd || (count && !exclusions) || count > 1024) return E_INVALIDARG;
        for (uint32_t i = 0; i < count; ++i) {
            if (exclusions[i].right < exclusions[i].left || exclusions[i].bottom < exclusions[i].top)
                return E_INVALIDARG;
        }
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return count ? E_INVALIDARG : S_OK;
        HWND child = GetWindowFromWindowId(found->second->source.SiteBridge().WindowId());
        RECT bounds{};
        check_bool(GetClientRect(child, &bounds));
        HRGN region = CreateRectRgnIndirect(&bounds);
        if (!region) return E_OUTOFMEMORY;
        for (uint32_t i = 0; i < count; ++i) {
            RECT exclusion = exclusions[i];
            if (GetWindowLongPtrW(child, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) {
                exclusion.left = bounds.right - exclusions[i].right;
                exclusion.right = bounds.right - exclusions[i].left;
            }
            HRGN cut = CreateRectRgnIndirect(&exclusion);
            int result = cut ? CombineRgn(region, region, cut, RGN_DIFF) : ERROR;
            if (cut) DeleteObject(cut);
            if (result == ERROR) {
                DeleteObject(region);
                return E_FAIL;
            }
        }
        // Windows owns the region on success; it clips rendering and hit testing.
        if (!SetWindowRgn(child, region, TRUE)) {
            DeleteObject(region);
            return E_FAIL;
        }
        if (DiagnosticsEnabled())
            fprintf(stderr, "WinUIPane clip exclusions=%u visible=%d\n", count, IsWindowVisible(child));
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
HRESULT WINAPI SyncAddress(HWND hwnd, const wchar_t* input, wchar_t* output, uint32_t capacity) noexcept {
    try {
        check_hresult(Check(hwnd));
        if (!input || !output || capacity < 1 || capacity > 262144 || wcsnlen(input, 1048577) > 1048576)
            return E_INVALIDARG;
        output[0] = 0;
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto snapshot = NativeAddress::JObject::Parse(input);
        if (!found->second->address) return snapshot.GetNamedBoolean(L"enabled", false) ? E_NOTIMPL : S_OK;
        found->second->address->ObserveFocus(found->second->source.HasFocus() && GetForegroundWindow() == hwnd);
        auto action = found->second->address->Sync(snapshot);
        if (action.size() >= capacity) return E_INVALIDARG;
        wcscpy_s(output, capacity, action.c_str());
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
HRESULT WINAPI SyncTabMenu(HWND hwnd, const wchar_t* input, wchar_t* output, uint32_t capacity) noexcept {
    try {
        check_hresult(Check(hwnd));
        if (!input || !output || capacity < 1 || capacity > 262144 || wcsnlen(input, 1048577) > 1048576)
            return E_INVALIDARG;
        output[0] = 0;
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto snapshot = NativeTabMenu::Object::Parse(input);
        auto& pane = *found->second;
        if (snapshot.HasKey(L"strip")) {
            // Strip publication is a separate transaction: validate the envelope
            // before touching XAML, and never drain any queued input here.
            if (!pane.strip || snapshot.Size() != 3 || snapshot.GetNamedNumber(L"version") != 2 ||
                !snapshot.GetNamedBoolean(L"publishOnly", false))
                return E_INVALIDARG;
            pane.strip->Publish(snapshot.GetNamedObject(L"strip"));
            return S_OK;
        }
        auto result = pane.tabMenu.Sync(snapshot);
        // Deliver preceding selection before a queued menu opening can act on it.
        bool selectionResult = !snapshot.GetNamedBoolean(L"publishOnly", false) && !pane.pendingSelectionSet.empty();
        bool groupResult = !selectionResult && !snapshot.GetNamedBoolean(L"publishOnly", false) && pane.strip &&
                           !pane.strip->groupsButton.pending.empty();
        bool reorderResult = !selectionResult && !groupResult && !snapshot.GetNamedBoolean(L"publishOnly", false) &&
                              pane.strip && !pane.strip->pendingReorder.empty();
        if (selectionResult)
            result = pane.pendingSelectionSet;
        else if (groupResult)
            result = pane.strip->groupsButton.pending;
        else if (reorderResult)
            result = pane.strip->pendingReorder;
        if (result.size() >= capacity) return E_INVALIDARG;
        wcscpy_s(output, capacity, result.c_str());
        if (selectionResult)
            pane.pendingSelectionSet = L"";
        else if (groupResult)
            pane.strip->groupsButton.pending = L"";
        else if (reorderResult)
            pane.strip->pendingReorder = L"";
        else if (!result.empty())
            pane.tabMenu.events.pop_front();
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
HRESULT WINAPI SyncTabs(HWND hwnd, const WinUITab* tabs, uint32_t count, uint32_t selected, uint32_t* requested,
                        uint32_t* closeRequested, uint32_t* newRequested, uint32_t* contextId,
                        uint32_t* command) noexcept {
    try {
        check_hresult(Check(hwnd));
        if (!requested || !closeRequested || !newRequested || !contextId || !command) return E_POINTER;
        *requested = 0;
        *closeRequested = 0;
        *newRequested = 0;
        *contextId = 0;
        *command = 0;
        if (!hwnd || (count && !tabs) || count > 10000) return E_INVALIDARG;
        std::set<uint32_t> ids;
        for (uint32_t i = 0; i < count; ++i) {
            if (!tabs[i].id || !tabs[i].title || !ids.insert(tabs[i].id).second ||
                wcsnlen(tabs[i].title, 32769) > 32768 || (tabs[i].commands & ~31u) || (tabs[i].commands & 12u) == 12u)
                return E_INVALIDARG;
        }
        if (selected && !ids.count(selected)) return E_INVALIDARG;
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto& pane = *found->second;
        while (!pane.pendingCloses.empty() && !ids.count(pane.pendingCloses.front())) pane.pendingCloses.pop_front();
        auto close = pane.pendingCloses.empty() ? 0 : pane.pendingCloses.front();
        auto create = !close && pane.pendingNewTabs ? 1u : 0u;
        auto pending = close || create || pane.tabMenu.session ? 0 : pane.pendingSelection;
        // A newer Firefox selection takes precedence over a queued native action.
        if (selected != pane.browserSelection || !ids.count(pending)) pending = 0;
        pane.Sync(tabs, count, pending ? pending : selected);
        pane.browserSelection = pending ? pending : selected;
        pane.pendingSelection = 0;
        if (close) pane.pendingCloses.pop_front();
        pane.inFlightClose = close;
        if (create) --pane.pendingNewTabs;
        *newRequested = create;
        *closeRequested = close;
        *requested = pending;
        return S_OK;
    } catch (hresult_error const& error) {
        fwprintf(stderr, L"WinUIPane tabs hr=0x%08X message=%s\n", unsigned(error.code()), error.message().c_str());
        return error.code();
    } catch (...) {
        return to_hresult();
    }
}
HRESULT WINAPI Focus(HWND hwnd, BOOL backwards) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        found->second->source.NavigateFocus(XamlSourceFocusNavigationRequest(
            backwards ? XamlSourceFocusNavigationReason::Last : XamlSourceFocusNavigationReason::First));
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
HRESULT WINAPI Destroy(HWND hwnd) noexcept {
    try {
        check_hresult(Check());
        auto& panes = state().panes;
        auto found = panes.find(hwnd);
        if (found != panes.end()) {
            found->second->Close();
            panes.erase(found);
        }
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
BOOL WINAPI Translate(MSG* message) noexcept {
    try {
        if (!message || FAILED(Check())) return FALSE;
        // Message-agnostic: this is the ground-truth tick a process-wide
        // drag session's correctness depends on (Pane.cpp's TabDragSession/
        // TickDragSession), driven by every message this hook sees rather
        // than by parsing any one of them.
        TickDragSession();
        for (auto const& entry : state().panes) {
            auto& pane = *entry.second;
            HWND child = GetWindowFromWindowId(pane.source.SiteBridge().WindowId());
            bool ownsMessage = message->hwnd == child || IsChild(child, message->hwnd);
            // MenuFlyout can create a top-level popup outside the child HWND.
            // Use WinUI's explicit ownership graph, never all browser/thread HWNDs.
            if (!ownsMessage && (pane.tabMenu.active || pane.address || pane.strip)) {
                for (auto popup : pane.source.Content().XamlRoot().ContentIsland().Popups()) {
                    HWND popupWindow = GetWindowFromWindowId(popup.WindowId());
                    if (message->hwnd == popupWindow || IsChild(popupWindow, message->hwnd)) {
                        ownsMessage = true;
                        break;
                    }
                }
            }
            if (!ownsMessage) continue;
            if (message->message == WM_KEYDOWN) {
                const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (control && (message->wParam == 'L' || message->wParam == 'T' || message->wParam == 'W' ||
                                (pane.strip && (message->wParam == VK_TAB || message->wParam == VK_PRIOR ||
                                                message->wParam == VK_NEXT)))) {
                    SetFocus(pane.parent);
                    PostMessageW(pane.parent, message->message, message->wParam, message->lParam);
                    return TRUE;
                }
            }
            return ContentPreTranslateMessage(message);
        }
    } catch (...) {}
    return FALSE;
}
HRESULT WINAPI Shutdown() noexcept {
    try {
        check_hresult(Check());
        auto& s = state();
        s.stopped = true;
        for (auto& entry : s.panes) entry.second->Close();
        s.panes.clear();
        if (s.manager) {
            s.manager.Close();
            s.manager = nullptr;
        }
        if (s.dispatcher) {
            s.dispatcher.ShutdownQueue();
            s.dispatcher = nullptr;
        }
        s.app = nullptr;
        clear_factory_cache();
        if (s.activationCookie) {
            check_bool(DeactivateActCtx(0, s.activationCookie));
            s.activationCookie = 0;
        }
        if (s.activation != INVALID_HANDLE_VALUE) {
            ReleaseActCtx(s.activation);
            s.activation = INVALID_HANDLE_VALUE;
        }
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
}  // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneGetApi(uint32_t version, uint32_t size,
                                                                WinUIPaneApi* output) noexcept {
    if (!output) return E_POINTER;
    if (version != WINUI_PANE_ABI_VERSION || size != sizeof(WinUIPaneApi)) return E_NOINTERFACE;
    *output = {sizeof(WinUIPaneApi),
               WINUI_PANE_ABI_VERSION,
               Update,
               Focus,
               Destroy,
               Translate,
               Shutdown,
               SyncTabs,
               SyncAddress,
               Clip,
               SyncTabMenu,
               UpdateTabStrip};
    return S_OK;
}

#ifdef WINUI_PANE_TESTING
// Test-only inspection of the actual hosted content. Not exported by the shipped DLL.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestGetContent(HWND hwnd, void** output) noexcept {
    try {
        if (!output) return E_POINTER;
        *output = nullptr;
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        *output = detach_abi(found->second->source.Content());
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
// Test-only: drive the strip's TabItems()-order-derived reorder logic
// directly, as if a completed drag had already left TabItems() in the given
// state. Kept for the case a caller wants "the neighbor IS TabItems()'s
// current order" semantics without a real pointer gesture.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestCompleteReorder(HWND hwnd, uint32_t tabId) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        auto& strip = *found->second->strip;
        auto row = strip.rows.find(tabId);
        if (row == strip.rows.end()) return E_INVALIDARG;
        strip.CompleteReorder(row->second);
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
// Test-only: drive the explicit-target-index overload the hand-rolled
// pointer-driven drag actually uses (TabStrip.h), where TabItems()'s real
// order never changes during the gesture and the neighbor is resolved from
// an explicit index instead. A real OS drag gesture can't be triggered from
// outside actual pointer input, so this is the fastest way to exercise it.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestCompleteReorderAt(HWND hwnd, uint32_t tabId,
                                                                               uint32_t targetIndex) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        auto& strip = *found->second->strip;
        auto row = strip.rows.find(tabId);
        if (row == strip.rows.end()) return E_INVALIDARG;
        strip.CompleteReorder(row->second, targetIndex);
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
// Test-only: force the same drag-in-progress state a real gesture sets, for
// the same reason as above -- confirms Sync() ignores Firefox snapshots that
// land mid-gesture without needing a real drag to hold one open.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestSetDragging(HWND hwnd, BOOL value) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        found->second->strip->dragging = bool(value);
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
#endif
