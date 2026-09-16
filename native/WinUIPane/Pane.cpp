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

    void Close() {
        if (!source) return;
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
        if (selectionResult)
            result = pane.pendingSelectionSet;
        else if (groupResult)
            result = pane.strip->groupsButton.pending;
        if (result.size() >= capacity) return E_INVALIDARG;
        wcscpy_s(output, capacity, result.c_str());
        if (selectionResult)
            pane.pendingSelectionSet = L"";
        else if (groupResult)
            pane.strip->groupsButton.pending = L"";
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
#endif
