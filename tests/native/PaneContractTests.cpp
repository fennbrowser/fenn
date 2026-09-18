#define NOMINMAX
#include "../../bridge/WinUIPaneApi.h"
#include <objbase.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <string>
#include <cmath>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <UIAutomation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

static void Require(bool value, const char* message) {
    if (!value) {
        fwprintf(stderr, L"FAIL: %hs\n", message);
        ExitProcess(1);
    }
}
static BOOL(WINAPI* translatePane)(MSG*) = nullptr;
static void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (translatePane && translatePane(&message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
// UIA must run off the XAML STA; the host keeps pumping provider requests.
static void WithTab(HWND hwnd, const wchar_t* id, const wchar_t* title, bool select = false, bool close = false,
                    int input = 0, int expectedMatches = 1, int matchIndex = 0) {
    fwprintf(stderr, L"UIA inspect id=%s title=%s select=%d close=%d input=%d\n", id, title, select, close, input);
    // Snapshot changes realize/recycle containers asynchronously. Let XAML finish
    // layout before querying a row's descendant accessibility properties.
    for (int i = 0; i < 20; ++i) {
        Pump();
        Sleep(10);
    }
    std::atomic<bool> done = false;
    std::thread client([&] {
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "UIA apartment");
        {
            ComPtr<IUIAutomation> automation;
            Require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&automation))),
                    "UIA client");
            ComPtr<IUIAutomationElement> root;
            Require(SUCCEEDED(automation->ElementFromHandle(hwnd, &root)), "UIA host");
            bool newTab = wcscmp(id, L"FirefoxWinUINewTab") == 0;
            VARIANT value{};
            value.vt = VT_BSTR;
            value.bstrVal = SysAllocString(id);
            ComPtr<IUIAutomationCondition> condition;
            Require(SUCCEEDED(automation->CreatePropertyCondition(
                        newTab ? UIA_AutomationIdPropertyId : UIA_NamePropertyId, value, &condition)),
                    "UIA identity condition");
            VariantClear(&value);
            VARIANT type{};
            type.vt = VT_I4;
            type.lVal = newTab ? UIA_ButtonControlTypeId : UIA_ListItemControlTypeId;
            ComPtr<IUIAutomationCondition> itemType, rowCondition;
            Require(SUCCEEDED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &itemType)),
                    "UIA row type");
            Require(SUCCEEDED(automation->CreateAndCondition(condition.Get(), itemType.Get(), &rowCondition)),
                    "UIA row condition");
            ComPtr<IUIAutomationElement> item;
            HRESULT findHr = S_OK;
            if (expectedMatches > 1) {
                ComPtr<IUIAutomationElementArray> matches;
                Require(SUCCEEDED(root->FindAll(TreeScope_Descendants, rowCondition.Get(), &matches)),
                        "enumerate identically named tab peers");
                int count{};
                Require(SUCCEEDED(matches->get_Length(&count)) && count == expectedMatches,
                        "identically named tabs have distinct automation peers");
                findHr = matches->GetElement(matchIndex, &item);
            } else {
                // WinUI may realize automation peers lazily on the first lookup.
                auto deadline = GetTickCount64() + 3000;
                do {
                    findHr = root->FindFirst(TreeScope_Descendants, rowCondition.Get(), &item);
                    if (item || FAILED(findHr)) break;
                    Sleep(20);
                } while (GetTickCount64() < deadline);
            }
            if (FAILED(findHr) || !item) {
                ComPtr<IUIAutomationElementArray> actualRows;
                if (SUCCEEDED(root->FindAll(TreeScope_Descendants, itemType.Get(), &actualRows))) {
                    int count{};
                    actualRows->get_Length(&count);
                    for (int i = 0; i < count; ++i) {
                        ComPtr<IUIAutomationElement> actual;
                        actualRows->GetElement(i, &actual);
                        BSTR text{};
                        actual->get_CurrentName(&text);
                        fwprintf(stderr, L"Actual UIA row %d: %s\n", i, text ? text : L"<null>");
                        SysFreeString(text);
                    }
                }
            }
            Require(SUCCEEDED(findHr) && item, "native tab exposed to accessibility");
            BSTR name{};
            Require(SUCCEEDED(item->get_CurrentName(&name)) && wcscmp(name, title) == 0,
                    "native tab title matches snapshot");
            SysFreeString(name);
            if (!newTab) {
                ComPtr<IUIAutomationSelectionItemPattern> pattern;
                Require(SUCCEEDED(item->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&pattern))),
                        "native selection accessibility pattern");
                BOOL selected{};
                Require(SUCCEEDED(pattern->get_CurrentIsSelected(&selected)), "accessible selection state");
            }
            if (close || newTab) {
                VARIANT buttonValue{};
                buttonValue.vt = VT_I4;
                buttonValue.lVal = UIA_ButtonControlTypeId;
                ComPtr<IUIAutomationCondition> buttonType;
                Require(
                    SUCCEEDED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, buttonValue, &buttonType)),
                    "close button condition");
                ComPtr<IUIAutomationElement> button;
                if (newTab)
                    button = item;
                else
                    Require(SUCCEEDED(item->FindFirst(TreeScope_Descendants, buttonType.Get(), &button)) && button,
                            "native close button exposed to accessibility");
                BSTR label{};
                auto nameHr = button->get_CurrentName(&label);
                fwprintf(stderr, L"UIA button label=%s\n", label ? label : L"<null>");
                Require(SUCCEEDED(nameHr) && std::wstring(label) == (newTab ? std::wstring(L"New tab")
                                                                            : std::wstring(L"Close tab: ") + title),
                        "close button accessible name");
                SysFreeString(label);
                ComPtr<IUIAutomationInvokePattern> invoke;
                Require(SUCCEEDED(button->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))),
                        "close invoke pattern");
                if (input == 2 || input == 3) {
                    Require(SUCCEEDED(button->SetFocus()), "keyboard focus native close button");
                    GUITHREADINFO info{sizeof(info)};
                    Require(GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, nullptr), &info) && info.hwndFocus,
                            "find focused XAML input window");
                    Require(PostMessageW(info.hwndFocus, WM_KEYDOWN, input == 3 ? VK_RETURN : VK_SPACE, 1) &&
                                PostMessageW(info.hwndFocus, WM_KEYUP, input == 3 ? VK_RETURN : VK_SPACE, 1),
                            "keyboard activate native close");
                } else {
                    Require(SUCCEEDED(invoke->Invoke()), "invoke native close button");
                }
            }
            if (select) {
                ComPtr<IUIAutomationSelectionItemPattern> pattern;
                Require(SUCCEEDED(item->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&pattern))),
                        "native selection accessibility pattern");
                auto result = pattern->Select();
                if (FAILED(result)) fprintf(stderr, "UIA Select hr=0x%08X\n", unsigned(result));
                Require(SUCCEEDED(result), "select native tab using accessibility");
            }
        }
        CoUninitialize();
        done = true;
    });
    auto deadline = GetTickCount64() + 15000;
    while (!done && GetTickCount64() < deadline) {
        Pump();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    Require(done, "UIA completed without deadlocking XAML");
    client.join();
    for (int i = 0; i < 10; ++i) {
        Pump();
        Sleep(10);
    }
}
static winrt::Microsoft::UI::Xaml::Controls::ListViewItem RealizedRow(
    winrt::Microsoft::UI::Xaml::Controls::ListView const& list, int32_t index) {
    using namespace winrt::Microsoft::UI::Xaml;
    for (int i = 0; i < 500; ++i) {
        list.UpdateLayout();
        auto row = list.ContainerFromIndex(index).try_as<Controls::ListViewItem>();
        if (row && row.IsLoaded() && row.ContentTemplateRoot() && row.ActualWidth() > 0 && row.ActualHeight() > 0)
            return row;
        if (index >= 0 && uint32_t(index) < list.Items().Size()) list.ScrollIntoView(list.Items().GetAt(index));
        Pump();
        Sleep(10);
    }
    fprintf(stderr, "Unrealized row index=%d items=%u list=%.1fx%.1f loaded=%d hostVisible=%d\n", index,
            list.Items().Size(), list.ActualWidth(), list.ActualHeight(), list.IsLoaded(),
            list.XamlRoot().IsHostVisible());
    auto missingRow = list.ContainerFromIndex(index).try_as<Controls::ListViewItem>();
    if (missingRow)
        fprintf(stderr, "Row loaded=%d template=%d size=%.1fx%.1f\n", missingRow.IsLoaded(),
                bool(missingRow.ContentTemplateRoot()), missingRow.ActualWidth(), missingRow.ActualHeight());
    Require(false, "native row is loaded and laid out before interaction");
    return nullptr;
}
static winrt::Windows::Data::Json::JsonObject MenuExchange(WinUIPaneApi& api, HWND hwnd,
                                                           wchar_t const* snapshot = L"{\"version\":2}") {
    std::vector<wchar_t> output(262144);
    Require(SUCCEEDED(api.syncTabMenu(hwnd, snapshot, output.data(), uint32_t(output.size()))),
            "menu protocol exchange");
    return output[0] ? winrt::Windows::Data::Json::JsonObject::Parse(output.data())
                     : winrt::Windows::Data::Json::JsonObject();
}
static uint32_t menuTestRevision = 0;
static winrt::Windows::Data::Json::JsonObject MenuState(std::vector<uint32_t> const& ids, bool nested = false) {
    using namespace winrt::Windows::Data::Json;
    auto snapshot = JsonObject::Parse(
        LR"({"version":2,"publishOnly":true,"state":{"revision":1,"models":[[{"kind":"command","id":"opaque-reload","label":"Reload Tab","enabled":true},{"kind":"command","id":"opaque-duplicate","label":"Duplicate Tab","enabled":false},{"kind":"command","id":"opaque-unpin","label":"Unpin Tab","enabled":true}]],"tabs":[]}})");
    auto state = snapshot.GetNamedObject(L"state");
    state.SetNamedValue(L"revision", JsonValue::CreateNumberValue(++menuTestRevision));
    for (auto id : ids) {
        JsonObject binding;
        binding.SetNamedValue(L"tab", JsonValue::CreateNumberValue(id));
        binding.SetNamedValue(L"model", JsonValue::CreateNumberValue(1));
        state.GetNamedArray(L"tabs").Append(binding);
    }
    if (nested) {
        auto models = state.GetNamedArray(L"models");
        auto sub = JsonObject::Parse(LR"({"kind":"submenu","label":"Actions","enabled":true})");
        sub.SetNamedValue(L"items", models.GetArrayAt(0));
        JsonArray outer;
        outer.Append(JsonObject::Parse(LR"({"kind":"separator"})"));
        outer.Append(sub);
        models.SetAt(0, outer);
    }
    return snapshot;
}
// Refresh the existing test fixture after publishing descriptions, as Firefox does.
static void PrepareFixtureMenus(WinUIPaneApi& api, HWND hwnd,
                                winrt::Microsoft::UI::Xaml::Controls::ListView const& list) {
    std::vector<uint32_t> ids;
    std::vector<std::wstring> titles;
    std::vector<WinUITab> tabs;
    for (uint32_t i = 0; i < list.Items().Size(); ++i) {
        ids.push_back(winrt::unbox_value<uint32_t>(list.Items().GetAt(i)));
        auto row = RealizedRow(list, i);
        titles.emplace_back(row.ContentTemplateRoot()
                                .as<winrt::Microsoft::UI::Xaml::Controls::Grid>()
                                .Children()
                                .GetAt(0)
                                .as<winrt::Microsoft::UI::Xaml::Controls::TextBlock>()
                                .Text());
    }
    for (uint32_t i = 0; i < ids.size(); ++i) tabs.push_back({ids[i], titles[i].c_str(), 7});
    MenuExchange(api, hwnd, MenuState(ids).Stringify().c_str());
    uint32_t selected{}, close{}, create{}, id{}, command{};
    Require(SUCCEEDED(api.syncTabs(hwnd, tabs.data(), uint32_t(tabs.size()), ids[list.SelectedIndex()], &selected,
                                   &close, &create, &id, &command)),
            "publish fixture menus before tab snapshot");
    Pump();
}
static void CheckContextMenu(HMODULE module, WinUIPaneApi& api, HWND hwnd) {
    using namespace winrt;
    using namespace winrt::Microsoft::UI::Xaml;
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Value = winrt::Windows::Data::Json::JsonValue;
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
    UIElement content{nullptr};
    Require(SUCCEEDED(inspect(hwnd, put_abi(content))), "inspect context menu host");
    auto list = content.as<Controls::Grid>().Children().GetAt(1).as<Controls::ListView>();
    WinUITab tabs[]{{71, L"Context first", 7}, {72, L"Context second", 27}};
    uint32_t selected{}, close{}, create{}, id{}, command{};
    auto settle = [] {
        for (int i = 0; i < 100; ++i) {
            Pump();
            Sleep(10);
        }
    };
    auto sync = [&](uint32_t count = 2) {
        fprintf(stderr, "Context sync count=%u\n", count);
        Require(SUCCEEDED(api.syncTabs(hwnd, tabs, count, 71, &selected, &close, &create, &id, &command)),
                "context tabs");
        settle();
    };
    SetForegroundWindow(hwnd);
    Require(SUCCEEDED(api.focus(hwnd, FALSE)), "activate test island before realizing rows");
    settle();  // Finish the new island's Loaded transition before inserting fixture rows.
    fprintf(stderr, "Context fixture hwnd=%p rootLoaded=%d\n", hwnd, content.as<FrameworkElement>().IsLoaded());
    auto current = MenuState({71, 72});
    MenuExchange(api, hwnd, current.Stringify().c_str());
    sync();
    Controls::MenuFlyout menu{nullptr};
    Object opened;
    auto open = [&](uint32_t count = 3) {
        fprintf(stderr, "Context open count=%u revision=%u\n", count, menuTestRevision);
        if (menu && menu.IsOpen()) menu.Hide();
        settle();
        while (MenuExchange(api, hwnd).Size()) {}
        auto row = RealizedRow(list, 1);
        menu = row.ContextFlyout().as<Controls::MenuFlyout>();
        SetForegroundWindow(hwnd);
        Require(SUCCEEDED(api.focus(hwnd, FALSE)), "enter owning island before opening menu");
        row.Focus(FocusState::Programmatic);
        settle();
        menu.XamlRoot(row.XamlRoot());
        bool shown = false;
        auto token = menu.Opened([&](auto const&, auto const&) { shown = true; });
        menu.ShowAt(row);
        for (int i = 0; i < 250 && !shown; ++i) {
            Pump();
            Sleep(20);
        }
        menu.Opened(token);
        Require(
            shown && menu.IsOpen() && menu.Items().Size() == count && menu.Items().GetAt(count - 1).ActualWidth() > 0,
            "first opening renders published entries before any protocol response");
        // Publication must leave the pending opening event queued.
        Require(!MenuExchange(api, hwnd, current.Stringify().c_str()).Size(), "publish-only never drains input");
        opened = MenuExchange(api, hwnd);
        while (opened.HasKey(L"kind") && opened.GetNamedString(L"kind") == L"dismiss") opened = MenuExchange(api, hwnd);
        Require(opened.GetNamedString(L"kind") == L"open" && opened.GetNamedNumber(L"tab") == 72 &&
                    opened.GetNamedNumber(L"revision") == current.GetNamedObject(L"state").GetNamedNumber(L"revision"),
                "opening retains target and published revision");
    };
    Controls::MenuFlyout retained{nullptr};
    Controls::MenuFlyoutItemBase retainedItem{nullptr};
    for (uint32_t index : {0u, 2u}) {
        open();
        Require(menu != retained && menu.Items().GetAt(0) != retainedItem, "each opening uses fresh native controls");
        retained = menu;
        retainedItem = menu.Items().GetAt(0);
        Require(!menu.Items().GetAt(1).IsEnabled(), "model disables native command");
        Require(menu.Items().GetAt(2).as<Controls::MenuFlyoutItem>().Text() == L"Unpin Tab", "Firefox supplies label");
        auto peer = Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(menu.Items().GetAt(index));
        peer.GetPattern(Automation::Peers::PatternInterface::Invoke)
            .as<Automation::Provider::IInvokeProvider>()
            .Invoke();
        Pump();
        auto event = MenuExchange(api, hwnd);
        Require(event.GetNamedString(L"kind") == L"activate" && event.GetNamedNumber(L"tab") == 72 &&
                    event.GetNamedNumber(L"session") == opened.GetNamedNumber(L"session") &&
                    event.GetNamedNumber(L"revision") == opened.GetNamedNumber(L"revision") &&
                    event.GetNamedString(L"id") == (index ? L"opaque-unpin" : L"opaque-reload"),
                "opaque action retains target/session/revision");
        Require(list.SelectedIndex() == 0, "context activation preserves background selection");
        auto next = MenuExchange(api, hwnd);
        Require(!next.HasKey(L"kind") || next.GetNamedString(L"kind") == L"dismiss", "activation consumed once");
    }
    current = MenuState({71, 72}, true);
    MenuExchange(api, hwnd, current.Stringify().c_str());
    open(2);
    Require(menu.Items().GetAt(0).try_as<Controls::MenuFlyoutSeparator>() != nullptr &&
                menu.Items().GetAt(1).as<Controls::MenuFlyoutSubItem>().Items().Size() == 3,
            "standard separator and submenu renderer");
    auto old = current;
    current = MenuState({71, 72});
    MenuExchange(api, hwnd, current.Stringify().c_str());
    settle();
    Require(!menu.IsOpen(), "changed state dismisses obsolete menu");
    open();
    MenuExchange(api, hwnd, old.Stringify().c_str());
    Require(menu.IsOpen() && menu.Items().Size() == 3, "late state cannot replace current controls");
    std::vector<wchar_t> output(262144);
    auto reject = [&](Object const& malformed) {
        Require(FAILED(api.syncTabMenu(hwnd, malformed.Stringify().c_str(), output.data(), uint32_t(output.size()))),
                "invalid state rejected atomically");
        Require(menu.IsOpen() && menu.Items().Size() == 3, "invalid state preserves current menu");
    };
    auto malformed = MenuState({71, 72});
    malformed.GetNamedObject(L"state").GetNamedArray(L"models").GetArrayAt(0).GetObjectAt(1).SetNamedValue(
        L"id", Value::CreateStringValue(L"opaque-reload"));
    reject(malformed);
    malformed = MenuState({71, 72});
    malformed.GetNamedObject(L"state").GetNamedArray(L"tabs").GetObjectAt(0).SetNamedValue(L"model",
                                                                                           Value::CreateNumberValue(2));
    reject(malformed);
    reject(MenuState({71, 71}));
    malformed = MenuState({71, 72});
    malformed.SetNamedValue(L"publishOnly", Value::CreateStringValue(L"true"));
    reject(malformed);
    Require(FAILED(api.syncTabMenu(hwnd, L"{broken", output.data(), uint32_t(output.size()))),
            "malformed JSON rejected");
    Require(FAILED(api.syncTabMenu(hwnd, L"{\"version\":3}", output.data(), uint32_t(output.size()))),
            "unknown protocol rejected");
    current = MenuState({71});
    MenuExchange(api, hwnd, current.Stringify().c_str());
    sync(1);
    Require(!menu.IsOpen(), "removed target dismisses menu");
    sync();
    Require(!RealizedRow(list, 1).ContextFlyout(), "unpublished target has no loading popup");
    current = MenuState({71, 72});
    MenuExchange(api, hwnd, current.Stringify().c_str());
    sync();
    open();
    menu.Hide();
    settle();
    while (MenuExchange(api, hwnd).Size()) {}
    current = MenuState({71, 72});
    for (auto binding : current.GetNamedObject(L"state").GetNamedArray(L"tabs"))
        binding.GetObject().SetNamedValue(L"selected", Value::CreateBooleanValue(true));
    MenuExchange(api, hwnd, current.Stringify().c_str());
    sync();
    Require(list.SelectionMode() == Controls::ListViewSelectionMode::Extended && list.SelectedItems().Size() == 2,
            "Firefox selected set uses native Extended selection independently of active tab");
    Require(Automation::AutomationProperties::GetItemStatus(RealizedRow(list, 0)) == L"Active tab" &&
                Automation::AutomationProperties::GetItemStatus(RealizedRow(list, 1)).empty(),
            "active tab remains separately accessible");
    list.SelectedItems().Clear();
    list.SelectedItems().Append(list.Items().GetAt(1));
    sync();
    auto selectionEvent = MenuExchange(api, hwnd);
    Require(selectionEvent.GetNamedString(L"kind") == L"selection" && selectionEvent.GetNamedNumber(L"tab") == 72 &&
                selectionEvent.GetNamedArray(L"tabs").Size() == 1 &&
                selectionEvent.GetNamedArray(L"tabs").GetNumberAt(0) == 72,
            "native selected set is copied and delivered once through the browser exchange");
    Require(!MenuExchange(api, hwnd).Size(), "selection request is consumed once");
    current = MenuState({71, 72});
    MenuExchange(api, hwnd, current.Stringify().c_str());
    sync();
    puts(
        "Menu protocol: first-open presentation, shared state, input isolation, opaque activation, revisions and "
        "malformed input passed");
}
static void CheckThemesAndLongList(HMODULE module, WinUIPaneApi& api, HWND hwnd) {
    using namespace winrt::Microsoft::UI::Xaml;
    for (int i = 0; i < 20; ++i) {
        Pump();
        Sleep(10);
    }
    Require(SUCCEEDED(api.focus(hwnd, FALSE)), "focus for theme inspection");
    Pump();
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
    Require(inspect != nullptr, "test build exposes content inspection");
    UIElement content{nullptr};
    Require(SUCCEEDED(inspect(hwnd, winrt::put_abi(content))), "inspect actual hosted content");
    auto root = content.as<Controls::Grid>();
    auto list = root.Children().GetAt(1).as<Controls::ListView>();
    PrepareFixtureMenus(api, hwnd, list);
    auto selectedRow = list.ContainerFromIndex(list.SelectedIndex()).as<Controls::ListViewItem>();
    Require(selectedRow.Focus(FocusState::Keyboard), "focus selected tab row");
    MSG tabKey{GetFocus(), WM_KEYDOWN, VK_TAB, 1};
    if (!api.translate(&tabKey)) {
        TranslateMessage(&tabKey);
        DispatchMessageW(&tabKey);
    }
    Pump();
    Require(Input::FocusManager::GetFocusedElement(root.XamlRoot()).try_as<Controls::Button>() != nullptr,
            "Tab reaches the native close button from the selected row");
    auto newTab = root.Children().GetAt(2).as<Controls::Button>();
    Require(Controls::ToolTipService::GetToolTip(newTab) != nullptr, "new-tab tooltip exists");
    Require(SUCCEEDED(api.focus(hwnd, TRUE)), "reverse island entry");
    Pump();
    Require(Input::FocusManager::GetFocusedElement(root.XamlRoot()) == newTab,
            "reverse island entry reaches new-tab button");
    auto original = root.RequestedTheme();
    auto contrast = [](auto foreground, auto background) {
        auto luminance = [](auto color) {
            auto linear = [](double c) {
                c /= 255;
                return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * linear(color.R) + 0.7152 * linear(color.G) + 0.0722 * linear(color.B);
        };
        auto a = luminance(foreground), b = luminance(background);
        return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
    };
    for (auto theme : {ElementTheme::Light, ElementTheme::Dark, ElementTheme::Light}) {
        root.RequestedTheme(theme);
        Pump();
        root.UpdateLayout();
        auto bg = root.Background().as<Media::SolidColorBrush>().Color();
        auto fg = root.Children().GetAt(0).as<Controls::TextBlock>().Foreground().as<Media::SolidColorBrush>().Color();
        fprintf(stderr, "Live theme=%d actual=%d background=%u,%u,%u foreground=%u,%u,%u contrast=%.2f\n", int(theme),
                int(root.ActualTheme()), bg.R, bg.G, bg.B, fg.R, fg.G, fg.B, contrast(fg, bg));
        auto row = RealizedRow(list, 0);
        auto rowGrid = row.ContentTemplateRoot().as<Controls::Grid>();
        auto label = rowGrid.Children().GetAt(0).as<Controls::TextBlock>();
        fwprintf(stderr, L"Rendered tab label: [%s] rowIndex=%d\n", label.Text().c_str(), list.IndexFromContainer(row));
        Require(label.Text() == L"Renamed tab", "stable-ID row realizes its visible title");
        auto button = rowGrid.Children().GetAt(1).as<Controls::Button>();
        auto buttonFg = button.Foreground().as<Media::SolidColorBrush>().Color();
        auto buttonBg = button.Background().as<Media::SolidColorBrush>().Color();
        // Composite the standard translucent button fill over its pane surface.
        auto composite = bg;
        composite.R = uint8_t((buttonBg.R * buttonBg.A + bg.R * (255 - buttonBg.A)) / 255);
        composite.G = uint8_t((buttonBg.G * buttonBg.A + bg.G * (255 - buttonBg.A)) / 255);
        composite.B = uint8_t((buttonBg.B * buttonBg.A + bg.B * (255 - buttonBg.A)) / 255);
        Require(contrast(buttonFg, composite) >= 3, "close icon remains readable after theme changes");
        auto newFg = newTab.Foreground().as<Media::SolidColorBrush>().Color();
        auto newBg = newTab.Background().as<Media::SolidColorBrush>().Color();
        auto newComposite = bg;
        newComposite.R = uint8_t((newBg.R * newBg.A + bg.R * (255 - newBg.A)) / 255);
        newComposite.G = uint8_t((newBg.G * newBg.A + bg.G * (255 - newBg.A)) / 255);
        newComposite.B = uint8_t((newBg.B * newBg.A + bg.B * (255 - newBg.A)) / 255);
        Require(contrast(newFg, newComposite) >= 4.5, "new-tab label readable in live themes");
        Require(root.ActualTheme() == theme && bg.A == 255 && contrast(fg, bg) >= 4.5,
                "live theme updates preserve opaque readable surface");
        auto menu = row.ContextFlyout().as<Controls::MenuFlyout>();
        SetForegroundWindow(hwnd);
        row.Focus(FocusState::Programmatic);
        for (int i = 0; i < 100; ++i) {
            Pump();
            Sleep(10);
        }
        bool opened = false;
        auto openedToken = menu.Opened([&](auto const&, auto const&) { opened = true; });
        menu.XamlRoot(row.XamlRoot());
        menu.ShowAt(row);
        for (int i = 0; i < 250 && !opened; ++i) {
            Pump();
            Sleep(20);
        }
        menu.Opened(openedToken);
        Require(opened, "menu rendered for theme inspection");
        auto event = MenuExchange(api, hwnd);
        while (event.HasKey(L"kind") && event.GetNamedString(L"kind") != L"open") event = MenuExchange(api, hwnd);
        Require(event.HasKey(L"kind"), "theme menu opening event");
        for (int i = 0; i < 300 && !menu.Items().GetAt(0).ActualWidth(); ++i) {
            Pump();
            Sleep(10);
        }
        for (auto base : menu.Items()) {
            auto item = base.as<Controls::MenuFlyoutItem>();
            fprintf(stderr, "Menu theme actual=%d root=%d width=%.1f height=%.1f open=%d\n", int(item.ActualTheme()),
                    int(root.ActualTheme()), item.ActualWidth(), item.ActualHeight(), menu.IsOpen());
            Require(item.ActualTheme() == root.ActualTheme() && item.Foreground() && item.ActualWidth() > 0 &&
                        item.ActualHeight() > 0,
                    "menu follows live root theme and realizes items");
        }
        menu.Hide();
        row.Focus(FocusState::Programmatic);
        for (int i = 0; i < 100; ++i) {
            Pump();
            Sleep(10);
        }
    }
    root.RequestedTheme(original);
    std::vector<std::wstring> titles;
    std::vector<WinUITab> tabs;
    for (uint32_t i = 0; i < 120; ++i)
        titles.push_back(L"Long tab title for scrolling and resizing " + std::to_wstring(i));
    for (uint32_t i = 0; i < titles.size(); ++i) tabs.push_back({i + 1, titles[i].c_str()});
    uint32_t requested{}, closeRequested{}, newRequested{}, contextId{}, command{};
    Require(SUCCEEDED(api.syncTabs(hwnd, tabs.data(), uint32_t(tabs.size()), 120, &requested, &closeRequested,
                                   &newRequested, &contextId, &command)),
            "long list snapshot");
    for (LONG width : {120L, 240L, 420L}) {
        RECT bounds{0, 0, width, 300};
        Require(SUCCEEDED(api.update(hwnd, &bounds, TRUE)), "resize long tab list");
        for (int i = 0; i < 20; ++i) {
            Pump();
            Sleep(10);
        }
        root.UpdateLayout();
        Require(list.ActualHeight() > 0 && list.ActualHeight() <= root.ActualHeight(), "finite scrolling viewport");
        list.ScrollIntoView(list.SelectedItem());
        root.UpdateLayout();
        auto row = list.ContainerFromIndex(119).as<Controls::ListViewItem>();
        auto button = row.ContentTemplateRoot().as<Controls::Grid>().Children().GetAt(1).as<Controls::Button>();
        auto boundsInList = button.TransformToVisual(list).TransformBounds(
            {0, 0, float(button.ActualWidth()), float(button.ActualHeight())});
        Require(boundsInList.X >= 0 && boundsInList.X + boundsInList.Width <= list.ActualWidth() + 1,
                "close button fits the narrow scrolling viewport");
        auto newBounds = newTab.TransformToVisual(root).TransformBounds(
            {0, 0, float(newTab.ActualWidth()), float(newTab.ActualHeight())});
        Require(newBounds.X >= 0 && newBounds.X + newBounds.Width <= root.ActualWidth() &&
                    newBounds.Y + newBounds.Height <= root.ActualHeight() && newTab.ActualHeight() >= 32,
                "new-tab button stays visible outside the scrolling list");
        fprintf(stderr, "Density width=%ld rowHeight=%.1f newButton=%.1fx%.1f padding=%.1f gap=%.1f\n", width,
                row.ActualHeight(), newTab.ActualWidth(), newTab.ActualHeight(), root.Padding().Left,
                newTab.Margin().Top);
        auto panel = list.ItemsPanelRoot().as<Controls::ItemsStackPanel>();
        Require(panel.LastVisibleIndex() == 119, "last selected tab is reachable in long list");
        fprintf(stderr, "Long list width=%ld visible=%d..%d viewport=%.1fx%.1f\n", width, panel.FirstVisibleIndex(),
                panel.LastVisibleIndex(), list.ActualWidth(), list.ActualHeight());
    }
    Require(
        SUCCEEDED(api.syncTabs(hwnd, nullptr, 0, 0, &requested, &closeRequested, &newRequested, &contextId, &command)),
        "clear long list");
    // Drain deferred container unloading before creating another test island.
    for (int i = 0; i < 100; ++i) {
        Pump();
        Sleep(10);
    }
}
static BOOL CALLBACK CountChild(HWND, LPARAM count) {
    ++*reinterpret_cast<unsigned*>(count);
    return TRUE;
}
void CheckAddress(WinUIPaneApi& api, HMODULE module, HWND hwnd) {
    using namespace winrt;
    using namespace winrt::Microsoft::UI::Xaml;
    std::vector<wchar_t> output(262144);
    const wchar_t* snapshot =
        LR"({"version":1,"enabled":true,"session":"native-test","revision":1,"sequence":1,"text":"initial","rows":[{"id":"a","title":"First suggestion"},{"id":"b","title":"Second suggestion"}]})";
    Require(FAILED(api.syncAddress(hwnd, L"{broken", output.data(), uint32_t(output.size()))),
            "reject malformed address JSON");
    Require(SUCCEEDED(api.syncAddress(hwnd, snapshot, output.data(), uint32_t(output.size()))),
            "accept native address snapshot");
    Pump();
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
    winrt::Windows::Foundation::IInspectable content;
    Require(SUCCEEDED(inspect(hwnd, put_abi(content))), "inspect address controls");
    auto address = content.as<Controls::Grid>().Children().GetAt(3).as<Controls::StackPanel>();
    auto field = address.Children().GetAt(0).as<Controls::Grid>().Children().GetAt(1).as<Controls::TextBox>();
    auto list = address.Children().GetAt(1).as<Controls::ListView>();
    Require(address.Visibility() == Visibility::Visible && field.Text() == L"initial" && list.Items().Size() == 2,
            "real TextBox and ListView receive snapshot");
    auto originalRow = list.Items().GetAt(0);
    const wchar_t* imageSnapshot =
        LR"({"version":1,"enabled":true,"session":"native-test","revision":1,"sequence":2,"text":"initial","selectedId":"b","rows":[{"id":"a","title":"First suggestion","sectionLabel":"Shortcuts","imageSize":32,"image":{"width":1,"height":1,"pixels":"0000ffff"}},{"id":"b","title":"Second suggestion"}]})";
    Require(SUCCEEDED(api.syncAddress(hwnd, imageSnapshot, output.data(), uint32_t(output.size()))),
            "accept decoded image");
    auto rowContent = originalRow.as<Controls::ListViewItem>().Content().as<Controls::StackPanel>();
    auto image = rowContent.Children()
                     .GetAt(1)
                     .as<Controls::Grid>()
                     .Children()
                     .GetAt(0)
                     .as<Controls::Grid>()
                     .Children()
                     .GetAt(1)
                     .as<Controls::Image>();
    Require(list.Items().GetAt(0) == originalRow && list.SelectedIndex() == 1 && image.Source() &&
                rowContent.Children().GetAt(0).as<Controls::TextBlock>().Text() == L"Shortcuts",
            "image and section update in place with selection intact");
    Require(
        FAILED(api.syncAddress(
            hwnd,
            LR"({"version":1,"enabled":true,"session":"native-test","revision":1,"text":"initial","rows":[{"id":"a","title":"bad","image":{"width":105,"height":1,"pixels":"00"}}]})",
            output.data(), uint32_t(output.size()))),
        "reject oversized image before mutation");
    Require(list.Items().GetAt(0) == originalRow && image.Source(), "malformed image leaves controls intact");
    const wchar_t* modesSnapshot =
        LR"({"version":1,"enabled":true,"session":"native-test","revision":1,"sequence":3,"text":"initial","rows":[{"id":"a","title":"First suggestion"},{"id":"b","title":"Second suggestion"}],"modes":{"generation":1,"applied":0,"active":false,"label":"Test engine","entries":[{"id":"engine","label":"Test engine","local":false,"symbol":"search","image":{"width":1,"height":1,"pixels":"0000ffff"}},{"id":"local","label":"History","local":true,"symbol":"history"},{"id":"settings","label":"Settings","local":true,"symbol":"settings"}]}})";
    Require(SUCCEEDED(api.syncAddress(hwnd, modesSnapshot, output.data(), uint32_t(output.size()))),
            "accept native mode model");
    auto modeButton =
        address.Children().GetAt(0).as<Controls::Grid>().Children().GetAt(0).as<Controls::DropDownButton>();
    auto hiddenMode = winrt::Windows::Data::Json::JsonObject::Parse(modesSnapshot);
    hiddenMode.SetNamedValue(L"modeButtonVisible", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(false));
    Require(SUCCEEDED(api.syncAddress(hwnd, hiddenMode.Stringify().c_str(), output.data(), uint32_t(output.size()))) &&
                modeButton.Visibility() == Visibility::Collapsed && field.Text() == L"initial",
            "Firefox hides the button without changing the address");
    Require(SUCCEEDED(api.syncAddress(hwnd, modesSnapshot, output.data(), uint32_t(output.size()))) &&
                modeButton.Visibility() == Visibility::Visible,
            "Firefox can restore the native search button");
    auto modeFlyout = modeButton.Flyout().as<Controls::MenuFlyout>();
    Require(modeButton.IsEnabled() && modeFlyout.Items().Size() == 5 &&
                modeFlyout.Items().GetAt(0).as<Controls::MenuFlyoutItem>().Icon().try_as<Controls::ImageIcon>(),
            "real dropdown, menu separators and decoded engine icon");
    Require(
        FAILED(api.syncAddress(
            hwnd,
            LR"({"version":1,"enabled":true,"session":"native-test","revision":1,"rows":[],"modes":{"generation":1,"active":false,"label":"Test","entries":[]}})",
            output.data(), uint32_t(output.size()))),
        "reject changed menu identities without new generation");
    const wchar_t* globalSnapshot =
        LR"({"version":1,"enabled":true,"session":"native-test","revision":1,"sequence":4,"text":"initial","rows":[{"id":"a","title":"First suggestion"}],"globalActions":[{"id":"global:action:0","resultId":"global","title":"Search with Fixture","hint":"Press Tab to select:","image":{"width":1,"height":1,"pixels":"0000ffff"}}]})";
    Require(SUCCEEDED(api.syncAddress(hwnd, globalSnapshot, output.data(), uint32_t(output.size()))),
            "accept separate global action");
    auto group = address.Children().GetAt(2).as<Controls::StackPanel>();
    auto actionButton = group.Children().GetAt(0).as<Controls::StackPanel>().Children().GetAt(1).as<Controls::Button>();
    Require(list.Items().Size() == 1 && actionButton.Content() &&
                Automation::AutomationProperties::GetName(actionButton) == L"Search with Fixture",
            "global action is a labeled Button outside the ListView");
    Require(SUCCEEDED(api.syncAddress(hwnd, globalSnapshot, output.data(), uint32_t(output.size()))) &&
                group.Children().GetAt(0).as<Controls::StackPanel>().Children().GetAt(1) == actionButton,
            "global action updates preserve button identity");
    field.Text(L"changed");
    Pump();
    Require(SUCCEEDED(api.syncAddress(hwnd, snapshot, output.data(), uint32_t(output.size()))), "drain native edit");
    fwprintf(stderr, L"Address drained action: %s\n", output.data());
    auto action = winrt::Windows::Data::Json::JsonObject::Parse(output.data());
    Require(action.GetNamedString(L"kind") == L"edit" && action.GetNamedString(L"text") == L"changed" &&
                action.GetNamedString(L"session") == L"native-test",
            "native edit is copied and session scoped");
    Require(field.Text() == L"changed", "old snapshot does not overwrite queued edit");
    Require(
        SUCCEEDED(api.syncAddress(hwnd, LR"({"version":1,"enabled":false})", output.data(), uint32_t(output.size()))),
        "disable address controls");
    Require(address.Visibility() == Visibility::Collapsed && list.Items().Size() == 0,
            "disable removes native suggestions");
    puts("Native address: real controls, bounded JSON, copied edit, feedback suppression and disable passed");
}

static void CheckTabStrip(HMODULE module, WinUIPaneApi& api, HWND host) {
    using namespace winrt;
    using namespace winrt::Microsoft::UI::Xaml;
    RECT rect{0, 0, 600, 48};
    Require(SUCCEEDED(api.updateTabStrip(host, &rect, TRUE)), "create strip presentation");
    ShowWindow(host, SW_SHOW);
    auto state = MenuState({11, 22});
    auto bindings = state.GetNamedObject(L"state").GetNamedArray(L"tabs");
    for (uint32_t i = 0; i < 2; ++i)
        bindings.GetObjectAt(i).SetNamedValue(L"selected",
                                              winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(i == 0));
    MenuExchange(api, host, state.Stringify().c_str());
    WinUITab tabs[]{{11, L"Same title"}, {22, L"Same title"}};
    uint32_t selected{}, close{}, create{}, context{}, command{};
    auto sync = [&] {
        Require(SUCCEEDED(api.syncTabs(host, tabs, 2, 11, &selected, &close, &create, &context, &command)),
                "sync strip snapshot");
    };
    sync();
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
    UIElement content{nullptr};
    Require(SUCCEEDED(inspect(host, put_abi(content))), "inspect strip");
    auto root = content.as<Controls::Grid>();
    auto view = root.Children().GetAt(0).as<Controls::TabView>();
    auto first = view.TabItems().GetAt(0).as<Controls::TabViewItem>();
    auto second = view.TabItems().GetAt(1).as<Controls::TabViewItem>();
    for (int i = 0; i < 300 && (!first.IsLoaded() || first.ActualWidth() == 0); ++i) {
        Pump();
        Sleep(10);
    }
    Require(first.IsLoaded() && first.ActualWidth() > 0 && first.ActualHeight() <= 48,
            "stock tab header fits reserved row");
    Require(root.Children().Size() == 1 && !first.Content() && !second.Content(),
            "strip has no address or browser content");
    Require(first != second && view.SelectedItem() == first, "duplicate titles preserve identity and active tab");
    view.SelectedItem(second);
    auto event = MenuExchange(api, host);
    Require(event.GetNamedString(L"kind") == L"selection" && event.GetNamedNumber(L"tab") == 22,
            "TabView selection queues browser intent");
    sync();
    Require(view.SelectedItem() == first, "Firefox snapshot owns active tab");
    tabs[1].title = L"Updated title";
    sync();
    Require(view.TabItems().GetAt(1) == second && second.Header()
                                                          .as<Controls::Grid>()
                                                          .Children()
                                                          .GetAt(2)
                                                          .as<Controls::StackPanel>()
                                                          .Children()
                                                          .GetAt(0)
                                                          .as<Controls::TextBlock>()
                                                          .Text() == L"Updated title",
            "title updates retain TabViewItem");
    // Equal sizing must be independent of title length, shrink with count,
    // stop at the native minimum, and grow again when space becomes available.
    auto settleWidths = [&] {
        for (int i = 0; i < 30; ++i) {
            Pump();
            root.UpdateLayout();
            Sleep(10);
        }
    };
    rect.right = 800;
    Require(SUCCEEDED(api.updateTabStrip(host, &rect, TRUE)), "resize strip for sizing checks");
    tabs[1].title = L"Y";
    sync();
    settleWidths();
    const auto spaciousWidth = first.ActualWidth();
    fprintf(stderr, "Initial tab widths: %.1f %.1f\n", spaciousWidth, second.ActualWidth());
    fprintf(stderr, "Root/view widths %.1f %.1f scale %.2f\n", root.ActualWidth(), view.ActualWidth(),
            root.XamlRoot().RasterizationScale());
    Require(std::abs(spaciousWidth - second.ActualWidth()) < 1 && spaciousWidth >= 200 && spaciousWidth <= 241,
            "short and long titles receive equal comfortable widths");
    WinUITab crowded[12]{{11, L"Same title"}, {22, L"Y"}};
    for (uint32_t i = 2; i < 12; ++i) crowded[i] = {100 + i, L"Another tab"};
    Require(SUCCEEDED(api.syncTabs(host, crowded, 4, 11, &selected, &close, &create, &context, &command)),
            "four-tab sizing snapshot");
    settleWidths();
    fprintf(stderr, "Four width actual %.1f Width %.1f Min %.1f Max %.1f align %d\n", first.ActualWidth(),
            first.Width(), first.MinWidth(), first.MaxWidth(), int(first.HorizontalAlignment()));
    Require(first.ActualWidth() < spaciousWidth - 20 && first.ActualWidth() >= 99, "tabs shrink as the strip fills");
    Require(SUCCEEDED(api.syncTabs(host, crowded, 12, 11, &selected, &close, &create, &context, &command)),
            "overflow sizing snapshot");
    settleWidths();
    fprintf(stderr, "Tab sizing: spacious=%.1f crowded=%.1f\n", spaciousWidth, first.ActualWidth());
    Require(first.ActualWidth() >= 99 && first.ActualWidth() <= 101, "tabs stop shrinking at WinUI's 100-unit minimum");
    sync();
    settleWidths();
    Require(std::abs(first.ActualWidth() - spaciousWidth) < 1, "tabs grow after other tabs close");
    // Drag-to-reorder: a real OS drag gesture can't be scripted here (WinUI's
    // gesture recognizer can't be triggered from outside actual pointer
    // input), so simulate what a live drag already leaves behind -- WinUI's
    // own optimistic move of TabItems() -- and drive the same completion
    // logic TabDragCompleted invokes via a test-only hook.
    auto completeReorder =
        reinterpret_cast<HRESULT(WINAPI*)(HWND, uint32_t)>(GetProcAddress(module, "WinUIPaneTestCompleteReorder"));
    Require(completeReorder != nullptr, "completed-drag test hook exists");
    uint32_t secondIndex{};
    Require(view.TabItems().IndexOf(second, secondIndex), "locate second tab for reorder simulation");
    view.TabItems().RemoveAt(secondIndex);
    view.TabItems().InsertAt(0, second);
    Require(view.TabItems().GetAt(0) == second && view.TabItems().GetAt(1) == first,
            "simulate WinUI's own live drag having already reordered TabItems()");
    Require(SUCCEEDED(completeReorder(host, 22)), "drive completed-drag logic for the tab now leading");
    auto reorderToFront = MenuExchange(api, host);
    Require(reorderToFront.GetNamedString(L"kind") == L"strip-reorder" &&
                reorderToFront.GetNamedNumber(L"id") == 22 && reorderToFront.GetNamedNumber(L"beforeId") == 11,
            "completed drag queues a validated request naming the real neighbor");
    Require(SUCCEEDED(completeReorder(host, 11)), "drive completed-drag logic for the tab now trailing");
    auto reorderToEnd = MenuExchange(api, host);
    Require(reorderToEnd.GetNamedString(L"kind") == L"strip-reorder" && reorderToEnd.GetNamedNumber(L"id") == 11 &&
                reorderToEnd.GetNamedNumber(L"beforeId") == 0,
            "a tab with no following row requests beforeId 0 (move to end)");
    Require(SUCCEEDED(completeReorder(host, 22)), "queue a reorder request for the drain-priority check");
    view.SelectedItem(second);
    auto priorityFirst = MenuExchange(api, host);
    Require(priorityFirst.GetNamedString(L"kind") == L"selection" && priorityFirst.GetNamedNumber(L"tab") == 22,
            "selection drains before a queued reorder");
    auto priorityReorder = MenuExchange(api, host);
    Require(priorityReorder.GetNamedString(L"kind") == L"strip-reorder",
            "queued reorder drains once the selection ahead of it is consumed");
    sync();
    settleWidths();
    Require(view.TabItems().GetAt(0) == first && view.TabItems().GetAt(1) == second,
            "Firefox's next snapshot reconciles the simulated drag back to its own order");
    auto setDragging = reinterpret_cast<HRESULT(WINAPI*)(HWND, BOOL)>(GetProcAddress(module, "WinUIPaneTestSetDragging"));
    Require(setDragging != nullptr, "dragging test hook exists");
    auto firstName = [&] {
        return first.Header()
            .as<Controls::Grid>()
            .Children()
            .GetAt(2)
            .as<Controls::StackPanel>()
            .Children()
            .GetAt(0)
            .as<Controls::TextBlock>()
            .Text();
    };
    Require(SUCCEEDED(setDragging(host, TRUE)), "simulate a drag in progress");
    tabs[0].title = L"Ignored while dragging";
    sync();
    Require(firstName() == L"Same title", "Sync() ignores a Firefox snapshot that lands mid-drag");
    Require(SUCCEEDED(setDragging(host, FALSE)), "end the simulated drag");
    sync();
    Require(firstName() == L"Ignored while dragging", "Sync() resumes once the drag ends");
    tabs[0].title = L"Same title";
    sync();
    auto richer =
        LR"({"version":2,"publishOnly":true,"strip":{"version":1,"revision":1,"rtl":false,"groupsLabel":"Tab groups","groups":[{"id":7,"name":"Work","label":"Work — Expanded","collapsed":false}],"rows":[{"id":11,"detail":"Work · Split 1 left","flags":1,"image":{"width":2,"height":2,"pixels":"0000ffff0000ffff0000ffff0000ffff"}},{"id":22,"detail":"Work · Split 1 right","flags":6,"image":null}]}})";
    MenuExchange(api, host, richer);
    sync();
    auto header = second.Header().as<Controls::Grid>();
    Require(header.Children().GetAt(0).as<Controls::Grid>().Children().GetAt(1).as<Controls::ProgressRing>().IsActive(),
            "Firefox loading state drives native progress");
    Require(header.Children().GetAt(3).as<Controls::Viewbox>().Child().as<Controls::SymbolIcon>().Symbol() ==
                Controls::Symbol::Mute,
            "Firefox mute state drives native icon");
    Require(first.Header().as<Controls::Grid>().Children().GetAt(1).Visibility() == Visibility::Visible,
            "pinned indicator visible");
    Require(
        header.Children().GetAt(2).as<Controls::StackPanel>().Children().GetAt(1).as<Controls::TextBlock>().Text() ==
            L"Work · Split 1 right",
        "linked group/split detail supplied by Firefox");
    auto firstFavicon = first.Header().as<Controls::Grid>().Children().GetAt(0).as<Controls::Grid>();
    Require(firstFavicon.Visibility() == Visibility::Visible &&
                firstFavicon.Children().GetAt(0).as<Controls::Image>().Visibility() == Visibility::Visible &&
                bool(firstFavicon.Children().GetAt(0).as<Controls::Image>().Source()),
            "decoded favicon is shown when not busy");
    // id 22 is busy (flags 6) with no favicon: the spinner shares the
    // favicon's slot, so the slot stays visible for the spinner even though
    // there is no icon to show yet.
    auto secondFavicon = header.Children().GetAt(0).as<Controls::Grid>();
    Require(secondFavicon.Visibility() == Visibility::Visible &&
                secondFavicon.Children().GetAt(0).as<Controls::Image>().Visibility() == Visibility::Collapsed &&
                !secondFavicon.Children().GetAt(0).as<Controls::Image>().Source(),
            "a busy tab with no favicon shows the spinner in the favicon's slot");
    auto notBusyNoIcon = winrt::Windows::Data::Json::JsonObject::Parse(richer);
    auto notBusyNoIconModel = notBusyNoIcon.GetNamedObject(L"strip");
    notBusyNoIconModel.SetNamedValue(L"revision", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(2));
    notBusyNoIconModel.GetNamedArray(L"rows").GetObjectAt(1).SetNamedValue(
        L"flags", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(4));
    MenuExchange(api, host, notBusyNoIcon.Stringify().c_str());
    sync();
    Require(secondFavicon.Visibility() == Visibility::Collapsed,
            "once loading ends, a tab with no favicon shows no icon at all");
    auto oversizedImage = winrt::Windows::Data::Json::JsonObject::Parse(richer);
    auto oversizedModel = oversizedImage.GetNamedObject(L"strip");
    oversizedModel.SetNamedValue(L"revision", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(3));
    oversizedModel.GetNamedArray(L"rows").GetObjectAt(0).GetNamedObject(L"image").SetNamedValue(
        L"width", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(1000));
    std::vector<wchar_t> imageRejectOutput(262144);
    Require(FAILED(api.syncTabMenu(host, oversizedImage.Stringify().c_str(), imageRejectOutput.data(),
                                   uint32_t(imageRejectOutput.size()))),
            "oversized favicon dimensions are rejected before native mutation");
    auto groups = view.TabStripHeader().as<Controls::DropDownButton>();
    Require(groups.Visibility() == Visibility::Visible, "group controls expose collapsed groups independently of tabs");
    auto groupMenu = groups.Flyout().as<Controls::MenuFlyout>();
    bool groupOpened = false;
    auto groupToken = groupMenu.Opened([&](auto const&, auto const&) { groupOpened = true; });
    groupMenu.ShowAt(groups);
    for (int i = 0; i < 300 && !groupOpened; ++i) {
        Pump();
        Sleep(10);
    }
    groupMenu.Opened(groupToken);
    Require(groupOpened, "group flyout opens");
    auto groupItem = groupMenu.Items().GetAt(0).as<Controls::ToggleMenuFlyoutItem>();
    auto peer = Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(groupItem);
    peer.GetPattern(Automation::Peers::PatternInterface::Toggle).as<Automation::Provider::IToggleProvider>().Toggle();
    event = MenuExchange(api, host);
    Require(event.GetNamedString(L"kind") == L"strip-group" && event.GetNamedNumber(L"group") == 7 &&
                event.GetNamedBoolean(L"collapsed"),
            "group toggle queues validated intent");
    Require(groupItem.IsChecked(), "group checkbox awaits Firefox acknowledgment");
    groupMenu.Hide();
    for (int i = 0; i < 50; ++i) {
        Pump();
        Sleep(10);
    }
    auto bad = winrt::Windows::Data::Json::JsonObject::Parse(richer);
    auto model = bad.GetNamedObject(L"strip");
    model.SetNamedValue(L"revision", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(3));
    model.GetNamedArray(L"rows").Append(model.GetNamedArray(L"rows").GetAt(0));
    std::vector<wchar_t> invalidOutput(262144);
    Require(
        FAILED(api.syncTabMenu(host, bad.Stringify().c_str(), invalidOutput.data(), uint32_t(invalidOutput.size()))),
        "duplicate presentation IDs rejected before native mutation");
    bad.SetNamedValue(L"version", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(99));
    Require(
        FAILED(api.syncTabMenu(host, bad.Stringify().c_str(), invalidOutput.data(), uint32_t(invalidOutput.size()))),
        "invalid outer strip envelope rejected");
    bad.SetNamedValue(L"version", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(2));
    sync();
    Require(second.Header() == header, "metadata updates retain header identity");
    for (auto theme : {ElementTheme::Light, ElementTheme::Dark, ElementTheme::Light}) {
        root.RequestedTheme(theme);
        Pump();
        Require(root.ActualTheme() == theme, "strip follows live theme");
    }
    auto flyout = second.ContextFlyout().as<Controls::MenuFlyout>();
    SetForegroundWindow(host);
    second.Focus(FocusState::Programmatic);
    for (int i = 0; i < 30; ++i) {
        Pump();
        root.UpdateLayout();
        Sleep(10);
    }
    bool opened = false;
    auto openedToken = flyout.Opened([&](auto const&, auto const&) { opened = true; });
    flyout.ShowAt(second);
    for (int i = 0; i < 300 && !opened; ++i) {
        Pump();
        Sleep(10);
    }
    flyout.Opened(openedToken);
    Require(opened && flyout.Items().Size() == 3, "strip reuses published menu before exchange");
    flyout.Hide();
    for (int i = 0; i < 500 && second.ContextFlyout() == flyout; ++i) {
        Pump();
        Sleep(10);
    }
    Require(second.ContextFlyout() != flyout, "strip opening gets fresh flyout");
    model.GetNamedArray(L"rows").RemoveAtEnd();
    model.SetNamedValue(L"rtl", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(true));
    MenuExchange(api, host, bad.Stringify().c_str());
    sync();
    Require(root.FlowDirection() == FlowDirection::RightToLeft, "native layout uses Firefox text direction");
    Require(SUCCEEDED(api.updateTabStrip(host, &rect, FALSE)), "hide strip");
    Require(SUCCEEDED(api.updateTabStrip(host, &rect, TRUE)), "restore strip");
    // Leave the group popup open: the caller destroys the island before its HWND.
    groupOpened = false;
    groupToken = groupMenu.Opened([&](auto const&, auto const&) { groupOpened = true; });
    groupMenu.ShowAt(groups);
    for (int i = 0; i < 300 && !groupOpened; ++i) {
        Pump();
        Sleep(10);
    }
    groupMenu.Opened(groupToken);
    Require(groupOpened, "group popup open before island teardown");
}

// A real OS drag gesture, via SendInput, against the same island-hosted
// TabView every other strip check uses. Originally added to reproduce the
// WinUI native-drag crash in isolation (see the tab strip layout notes)
// -- now that reordering is driven by Pane.cpp's own process-wide
// TabDragSession (the tab drag redesign), this doubles as
// the one real end-to-end exercise of the actual gesture, which no other
// test can drive (the WinUIPaneTestCompleteReorder*/WinUIPaneTestSetDragging
// hooks only drive the logic directly, never a real pointer sequence). Kept
// as its own entry point, not folded into CheckTabStrip, so a regression
// here can never take the rest of the suite down with it.
static void CheckTabStripDrag(HMODULE module, WinUIPaneApi& api, HWND host) {
    using namespace winrt;
    using namespace winrt::Microsoft::UI::Xaml;
    RECT rect{0, 0, 600, 48};
    Require(SUCCEEDED(api.updateTabStrip(host, &rect, TRUE)), "create strip presentation for drag test");
    ShowWindow(host, SW_SHOW);
    WinUITab tabs[]{{11, L"First tab"}, {22, L"Second tab"}};
    uint32_t selected{}, close{}, create{}, context{}, command{};
    Require(SUCCEEDED(api.syncTabs(host, tabs, 2, 11, &selected, &close, &create, &context, &command)),
            "sync strip snapshot for drag test");
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
    UIElement content{nullptr};
    Require(SUCCEEDED(inspect(host, put_abi(content))), "inspect strip for drag test");
    auto root = content.as<Controls::Grid>();
    auto view = root.Children().GetAt(0).as<Controls::TabView>();
    auto first = view.TabItems().GetAt(0).as<Controls::TabViewItem>();
    for (int i = 0; i < 300 && (!first.IsLoaded() || first.ActualWidth() == 0); ++i) {
        Pump();
        Sleep(10);
    }
    Require(first.IsLoaded() && first.ActualWidth() > 0, "tab realized before drag");
    SetForegroundWindow(host);
    for (int i = 0; i < 30; ++i) {
        Pump();
        root.UpdateLayout();
        Sleep(10);
    }
    auto topLeft = first.TransformToVisual(nullptr).TransformPoint({0, 0});
    POINT client{LONG(topLeft.X + first.ActualWidth() / 2), LONG(topLeft.Y + first.ActualHeight() / 2)};
    Require(ClientToScreen(host, &client) != 0, "resolve tab screen position");
    fprintf(stderr, "Drag test: dragging first tab from screen point (%ld,%ld)\n", client.x, client.y);
    // A single Pump()+Sleep() pair can race SendInput's delivery -- the
    // injected message may not have reached this thread's queue yet at the
    // moment Pump() drains it. Polling repeatedly (matching the pattern
    // already used elsewhere in this file for physical/UIA interactions,
    // e.g. waiting for a flyout to open) gives it many chances instead of
    // exactly one.
    auto settle = [&](int iterations = 30) {
        for (int i = 0; i < iterations; ++i) {
            Pump();
            Sleep(10);
        }
    };
    Require(SetCursorPos(client.x, client.y) != 0, "position cursor over first tab");
    settle();
    auto send = [](DWORD flags) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        SendInput(1, &input, sizeof(INPUT));
    };
    send(MOUSEEVENTF_LEFTDOWN);
    settle();
    // SetCursorPos teleports the cursor and generates a plain WM_MOUSEMOVE,
    // but WinUI's pointer pipeline (PointerRoutedEventArgs) is driven by
    // WM_POINTER* messages from the actual input-injection stack -- moving
    // the cursor via SetCursorPos alone never produced a single PointerMoved
    // once captured. SendInput with MOUSEEVENTF_ABSOLUTE gives the same
    // pixel-deterministic positioning (avoiding relative-mickey/sensitivity
    // ambiguity) while still going through real pointer input.
    auto moveTo = [&](LONG x, LONG y) {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        input.mi.dx = LONG((double(x - vx) * 65535) / vw);
        input.mi.dy = LONG((double(y - vy) * 65535) / vh);
        SendInput(1, &input, sizeof(INPUT));
    };
    // Nothing blocks this thread anymore -- Pane.cpp's TabDragSession
    // advances by polling GetCursorPos()/GetAsyncKeyState() on every Pump()
    // (Translate()'s own pretranslate hook ticks it unconditionally), not by
    // waiting for a message to reach some nested loop. Plain SendInput moves
    // the real cursor and updates that ground truth directly, so this drives
    // the whole gesture from a single thread with no SetCursorPos/
    // PostMessageW split and no second thread -- matching how the very
    // first, pre-OLE version of this test drove a real gesture. Comfortably
    // exceeds any tab width TabWidthMode="Equal" could produce for two tabs
    // in a 600-unit strip, so the cursor reliably ends up past the second
    // tab's midpoint regardless of the exact computed width.
    for (int step = 1; step <= 25; ++step) {
        moveTo(client.x + step * 20, client.y);
        settle(5);
    }
    send(MOUSEEVENTF_LEFTUP);
    settle(200);
    puts("Drag gesture completed without crashing");
    auto event = MenuExchange(api, host);
    Require(event.GetNamedString(L"kind", L"") == L"strip-reorder" && event.GetNamedNumber(L"id") == 11 &&
                event.GetNamedNumber(L"beforeId") == 0,
            "a real pointer drag past the last tab queues a validated move-to-end request");
}

// Detach-to-new-window. Releasing well outside every open pane's own strip
// bounds entirely (Pane.cpp's TickDragSession hit-tests screen points
// against each pane's own StripScreenBounds(), the tab drag
// redesign) means release resolves as "no pane's strip is under the
// cursor" -- CommitDetach queues strip-detach at the real release point,
// same as before.
static void CheckTabStripDetach(HMODULE module, WinUIPaneApi& api, HWND host) {
    using namespace winrt;
    using namespace winrt::Microsoft::UI::Xaml;
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
    auto settle = [](int iterations = 30) {
        for (int i = 0; i < iterations; ++i) {
            Pump();
            Sleep(10);
        }
    };
    auto send = [](DWORD flags) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        SendInput(1, &input, sizeof(INPUT));
    };
    auto moveTo = [](LONG x, LONG y) {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        input.mi.dx = LONG((double(x - vx) * 65535) / vw);
        input.mi.dy = LONG((double(y - vy) * 65535) / vh);
        SendInput(1, &input, sizeof(INPUT));
    };

    RECT rect{0, 0, 600, 48};
    Require(SUCCEEDED(api.updateTabStrip(host, &rect, TRUE)), "create strip presentation for detach test");
    ShowWindow(host, SW_SHOW);
    WinUITab tabs[]{{55, L"Detach tab"}};
    uint32_t selected{}, close{}, create{}, context{}, command{};
    Require(SUCCEEDED(api.syncTabs(host, tabs, 1, 55, &selected, &close, &create, &context, &command)),
            "sync strip snapshot for detach test");
    UIElement content{nullptr};
    Require(SUCCEEDED(inspect(host, put_abi(content))), "inspect strip for detach test");
    auto view = content.as<Controls::Grid>().Children().GetAt(0).as<Controls::TabView>();
    auto tab = view.TabItems().GetAt(0).as<Controls::TabViewItem>();
    for (int i = 0; i < 300 && (!tab.IsLoaded() || tab.ActualWidth() == 0); ++i) {
        Pump();
        Sleep(10);
    }
    Require(tab.IsLoaded() && tab.ActualWidth() > 0, "detach tab realized before drag");
    SetForegroundWindow(host);
    settle();

    auto topLeft = tab.TransformToVisual(nullptr).TransformPoint({0, 0});
    POINT client{LONG(topLeft.X + tab.ActualWidth() / 2), LONG(topLeft.Y + tab.ActualHeight() / 2)};
    Require(ClientToScreen(host, &client) != 0, "resolve detach tab screen position");
    RECT hostRect{};
    Require(GetWindowRect(host, &hostRect) != 0, "resolve host window rect for detach test");
    // Comfortably below host's own window entirely, not just below the
    // strip's own visual area within it -- registration is on the whole
    // top-level window, so only actually leaving that window's bounds
    // reaches "no target accepted it anywhere."
    POINT detachPoint{hostRect.left + 50, hostRect.bottom + 200};
    fprintf(stderr, "Detach test: dragging from (%ld,%ld) to (%ld,%ld), outside host rect (%ld,%ld,%ld,%ld)\n",
            client.x, client.y, detachPoint.x, detachPoint.y, hostRect.left, hostRect.top, hostRect.right,
            hostRect.bottom);

    Require(SetCursorPos(client.x, client.y) != 0, "position cursor over detach tab");
    settle();
    send(MOUSEEVENTF_LEFTDOWN);
    settle();
    // A single thread throughout -- see CheckTabStripDrag's own comment on
    // why nothing here needs to block.
    for (int step = 1; step <= 10; ++step) {
        LONG screenX = client.x + LONG(double(step) / 10 * (detachPoint.x - client.x));
        LONG screenY = client.y + LONG(double(step) / 10 * (detachPoint.y - client.y));
        moveTo(screenX, screenY);
        settle(5);
    }
    send(MOUSEEVENTF_LEFTUP);
    settle(200);
    puts("Detach gesture completed without crashing");
    auto detachEvent = MenuExchange(api, host);
    Require(detachEvent.GetNamedString(L"kind", L"") == L"strip-detach" && detachEvent.GetNamedNumber(L"id") == 55,
            "dragging a tab outside every open window's bounds queues a validated detach request");
    Require(detachEvent.GetNamedNumber(L"screenY") > double(hostRect.bottom),
            "the detach request carries a screen point below the host window it left");
}

// Cross-window adopt. A second, independent pane/strip in a real second
// HWND: Pane.cpp's TickDragSession hit-tests the real cursor position
// against every open pane's own StripScreenBounds() (the tab drag
// redesign), precisely so a drop landing on a DIFFERENT window's
// strip resolves there. Same real-pointer-gesture technique as
// CheckTabStripDrag above.
static void CheckTabStripCrossWindowAdopt(HMODULE module, WinUIPaneApi& api, HWND host) {
    using namespace winrt;
    using namespace winrt::Microsoft::UI::Xaml;
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
    auto settle = [](int iterations = 30) {
        for (int i = 0; i < iterations; ++i) {
            Pump();
            Sleep(10);
        }
    };
    auto send = [](DWORD flags) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        SendInput(1, &input, sizeof(INPUT));
    };
    auto moveTo = [](LONG x, LONG y) {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        input.mi.dx = LONG((double(x - vx) * 65535) / vw);
        input.mi.dy = LONG((double(y - vy) * 65535) / vh);
        SendInput(1, &input, sizeof(INPUT));
    };

    RECT rect{0, 0, 600, 48};
    Require(SUCCEEDED(api.updateTabStrip(host, &rect, TRUE)), "create strip presentation for adopt test");
    ShowWindow(host, SW_SHOW);
    WinUITab tabs[]{{33, L"Source tab"}};
    uint32_t selected{}, close{}, create{}, context{}, command{};
    Require(SUCCEEDED(api.syncTabs(host, tabs, 1, 33, &selected, &close, &create, &context, &command)),
            "sync first host's strip snapshot for adopt test");
    UIElement content{nullptr};
    Require(SUCCEEDED(inspect(host, put_abi(content))), "inspect first host's strip for adopt test");
    auto view = content.as<Controls::Grid>().Children().GetAt(0).as<Controls::TabView>();
    auto sourceTab = view.TabItems().GetAt(0).as<Controls::TabViewItem>();
    for (int i = 0; i < 300 && (!sourceTab.IsLoaded() || sourceTab.ActualWidth() == 0); ++i) {
        Pump();
        Sleep(10);
    }
    Require(sourceTab.IsLoaded() && sourceTab.ActualWidth() > 0, "source tab realized before adopt drag");

    HWND host2 = CreateWindowExW(0, L"STATIC", L"Cross-window drop target", WS_OVERLAPPEDWINDOW, 800, 0, 400, 200,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(host2 != nullptr, "second host for cross-window adopt test");
    RECT rect2{0, 0, 400, 48};
    Require(SUCCEEDED(api.updateTabStrip(host2, &rect2, TRUE)), "create strip presentation on second host");
    ShowWindow(host2, SW_SHOW);
    WinUITab tabs2[]{{44, L"Other window's tab"}};
    uint32_t selected2{}, close2{}, create2{}, context2{}, command2{};
    Require(SUCCEEDED(api.syncTabs(host2, tabs2, 1, 44, &selected2, &close2, &create2, &context2, &command2)),
            "sync second host's strip snapshot for adopt test");
    UIElement content2{nullptr};
    Require(SUCCEEDED(inspect(host2, put_abi(content2))), "inspect second host's strip");
    auto view2 = content2.as<Controls::Grid>().Children().GetAt(0).as<Controls::TabView>();
    auto destTab = view2.TabItems().GetAt(0).as<Controls::TabViewItem>();
    for (int i = 0; i < 300 && (!destTab.IsLoaded() || destTab.ActualWidth() == 0); ++i) {
        Pump();
        Sleep(10);
    }
    Require(destTab.IsLoaded() && destTab.ActualWidth() > 0, "destination tab realized before adopt drag");
    settle(30);

    auto sourceTopLeft = sourceTab.TransformToVisual(nullptr).TransformPoint({0, 0});
    POINT sourceClient{LONG(sourceTopLeft.X + sourceTab.ActualWidth() / 2),
                        LONG(sourceTopLeft.Y + sourceTab.ActualHeight() / 2)};
    Require(ClientToScreen(host, &sourceClient) != 0, "resolve source tab screen position");
    // Aimed at the destination tab's own left portion, not its center: with
    // only one tab in host2, approaching from the left keeps the cursor
    // before that tab's own midpoint (and so projects to index 0, including
    // that one row in the "make room" shift) for most of the approach,
    // which the preview-offset check below depends on landing on.
    auto destTopLeft = destTab.TransformToVisual(nullptr).TransformPoint({0, 0});
    POINT destPoint{LONG(destTopLeft.X + 5), LONG(destTopLeft.Y + destTab.ActualHeight() / 2)};
    Require(ClientToScreen(host2, &destPoint) != 0, "resolve second host's strip screen position");
    fprintf(stderr, "Adopt test: dragging from (%ld,%ld) toward (%ld,%ld)\n", sourceClient.x, sourceClient.y,
            destPoint.x, destPoint.y);

    SetForegroundWindow(host);
    settle();
    Require(SetCursorPos(sourceClient.x, sourceClient.y) != 0, "position cursor over source tab");
    settle();
    send(MOUSEEVENTF_LEFTDOWN);
    settle();
    // A single thread throughout -- see CheckTabStripDrag's own comment on
    // why nothing here needs to block. This thread IS the STA that owns the
    // XAML objects (nothing else ever blocked it to begin with), so
    // destTab's own RenderTransform can be read directly below with no
    // DispatcherQueue marshaling needed anymore.
    for (int step = 1; step <= 10; ++step) {
        LONG screenX = sourceClient.x + LONG(double(step) / 10 * (destPoint.x - sourceClient.x));
        LONG screenY = sourceClient.y + LONG(double(step) / 10 * (destPoint.y - sourceClient.y));
        moveTo(screenX, screenY);
        settle(5);
    }
    // Hover briefly before releasing -- both to give the make-room
    // Storyboard (135ms) time to actually reach its target, and to verify
    // the live preview (NativeTabStrip::ForeignDragOver) actually ran on
    // host2's own target, not just that the final drop resolved correctly.
    moveTo(destPoint.x, destPoint.y);
    settle(30);
    double previewOffset = destTab.RenderTransform().as<Media::TranslateTransform>().X();
    send(MOUSEEVENTF_LEFTUP);
    settle(200);
    puts("Cross-window adopt gesture completed without crashing");
    Require(previewOffset > 0.0,
            "hovering the dragged tab over the destination strip opens a live make-room preview gap there");
    auto adoptEvent = MenuExchange(api, host2);
    Require(adoptEvent.GetNamedString(L"kind", L"") == L"strip-adopt" && adoptEvent.GetNamedNumber(L"id") == 33 &&
                adoptEvent.GetNamedNumber(L"sourceHwnd") == double(reinterpret_cast<uintptr_t>(host)),
            "dropping a tab onto another window's strip queues a validated adopt request there");
    Require(SUCCEEDED(api.destroy(host2)), "destroy second host's pane");
    DestroyWindow(host2);
}

int wmain(int argc, wchar_t** argv) {
    Require(AddVectoredExceptionHandler(
                1,
                [](EXCEPTION_POINTERS* error) -> LONG {
                    if (error->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
                        fputs("FAIL: access violation, including COM/DLL teardown\n", stderr);
                        auto describe = [](void* address) {
                            HMODULE owner{};
                            wchar_t path[MAX_PATH]{};
                            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                   reinterpret_cast<LPCWSTR>(address), &owner))
                                GetModuleFileNameW(owner, path, MAX_PATH);
                            fwprintf(stderr, L"  %s + 0x%llx (%p)\n", path,
                                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                                     reinterpret_cast<uintptr_t>(owner)),
                                     address);
                        };
                        describe(error->ExceptionRecord->ExceptionAddress);
                        void* frames[32]{};
                        auto count = CaptureStackBackTrace(0, 32, frames, nullptr);
                        for (USHORT i = 0; i < count; ++i) describe(frames[i]);
                        ExitProcess(1);
                    }
                    return EXCEPTION_CONTINUE_SEARCH;
                }) != nullptr,
            "install teardown exception guard");
    Require(argc == 2 || (argc == 3 && (wcscmp(argv[2], L"--inspect") == 0 || wcscmp(argv[2], L"--address") == 0 ||
                                        wcscmp(argv[2], L"--menu") == 0 || wcscmp(argv[2], L"--strip") == 0 ||
                                        wcscmp(argv[2], L"--drag") == 0)),
            "DLL path and optional --inspect required");
    Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "STA initialization");
    HMODULE module = LoadLibraryExW(
        argv[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    Require(module != nullptr, "load pane DLL");
    auto get = reinterpret_cast<WinUIPaneGetApiFn>(GetProcAddress(module, "WinUIPaneGetApi"));
    Require(get != nullptr, "get export");
    WinUIPaneApi api;
    memset(&api, 0xA5, sizeof(api));
    const auto original = api;
    Require(get(1, sizeof(api), &api) == E_NOINTERFACE, "reject old unclipped ABI");
    Require(get(999, sizeof(api), &api) == E_NOINTERFACE, "reject version");
    Require(memcmp(&original, &api, sizeof(api)) == 0, "leave rejected output untouched");
    Require(get(WINUI_PANE_ABI_VERSION - 1, sizeof(api), &api) == E_NOINTERFACE,
            "reject prior ABI without native selection support");
    Require(get(WINUI_PANE_ABI_VERSION, sizeof(api) - 1, &api) == E_NOINTERFACE, "reject size");
    Require(get(WINUI_PANE_ABI_VERSION, sizeof(api), nullptr) == E_POINTER, "reject null output");
    Require(SUCCEEDED(get(WINUI_PANE_ABI_VERSION, sizeof(api), &api)), "ABI handshake");
    translatePane = api.translate;
    Require((GetProcAddress(module, "WinUIPaneTestGetContent") != nullptr) == (argc == 3),
            "inspection export exists only in the test DLL");
    RECT rect{0, 0, 240, 300};
    Require(api.update(nullptr, &rect, TRUE) == E_INVALIDARG, "reject null HWND");
    HWND first = CreateWindowExW(0, L"STATIC", L"Pane contract test", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(first != nullptr, "first host");
    HWND compositor = CreateWindowExW(0, L"STATIC", L"Simulated compositor", WS_CHILD | WS_VISIBLE, 0, 0, 640, 480,
                                      first, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(compositor != nullptr, "compositor sibling");
    Require(api.update(first, nullptr, TRUE) == E_INVALIDARG, "reject null geometry");
    Require(SUCCEEDED(api.update(first, &rect, TRUE)), "first island");
    if (argc == 3 && wcscmp(argv[2], L"--strip") == 0) {
        CheckTabStrip(module, api, first);
        Require(SUCCEEDED(api.destroy(first)), "destroy strip before host");
        DestroyWindow(first);
        winrt::clear_factory_cache();
        Require(SUCCEEDED(api.shutdown()), "strip clean shutdown");
        CoUninitialize();
        puts("CHECKS PASSED: TabView layout, stable IDs, selection, menus, theme, hide/show, teardown");
        return 0;
    }
    if (argc == 3 && wcscmp(argv[2], L"--drag") == 0) {
        CheckTabStripDrag(module, api, first);
        CheckTabStripDetach(module, api, first);
        CheckTabStripCrossWindowAdopt(module, api, first);
        Require(SUCCEEDED(api.destroy(first)), "destroy drag test island");
        DestroyWindow(first);
        winrt::clear_factory_cache();
        Require(SUCCEEDED(api.shutdown()), "drag test clean shutdown");
        CoUninitialize();
        puts("CHECKS PASSED: drag gesture");
        return 0;
    }
    if (argc == 3 && wcscmp(argv[2], L"--menu") == 0) {
        ShowWindow(first, SW_SHOW);
        Pump();
        CheckContextMenu(module, api, first);
        uint32_t selected{}, close{}, create{}, id{}, command{};
        WinUITab themeTabs[]{{11, L"Renamed tab"}, {22, L"Second tab"}};
        Require(SUCCEEDED(api.syncTabs(first, themeTabs, 2, 11, &selected, &close, &create, &id, &command)),
                "theme fixture");
        CheckThemesAndLongList(module, api, first);
        for (int cycle = 0; cycle < 20; ++cycle) {
            HWND second = CreateWindowExW(0, L"STATIC", L"Menu lifecycle host", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480,
                                          nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            Require(second && SUCCEEDED(api.update(second, &rect, TRUE)), "menu secondary island");
            if (!cycle) {
                ShowWindow(second, SW_SHOW);
                Pump();
                CheckContextMenu(module, api, second);
            }
            Require(SUCCEEDED(api.destroy(second)), "menu secondary island teardown");
            DestroyWindow(second);
            Pump();
        }
        Require(SUCCEEDED(api.destroy(first)), "focused menu teardown");
        DestroyWindow(first);
        Require(SUCCEEDED(api.shutdown()), "focused menu shutdown");
        FreeLibrary(module);
        CoUninitialize();
        puts("Menu checks passed; clean process exit still required");
        return 0;
    }
    uint32_t requested = 99, closeRequested = 0, newRequested = 0, contextId = 0, command = 0;
    WinUITab tabs[]{{11, L"First tab"}, {22, L"Unicode tab \u65e5\u672c\u8a9e"}};
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)) &&
            !requested,
        "initial tabs do not emit user selection");
    tabs[0].title = L"Renamed tab";
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 22, &requested, &closeRequested, &newRequested, &contextId, &command)) &&
            !requested,
        "title and Firefox selection updates do not echo");
    WinUITab reordered[]{{22, L"Moved tab"}, {11, L"Renamed tab"}};
    Require(SUCCEEDED(api.syncTabs(first, reordered, 2, 22, &requested, &closeRequested, &newRequested, &contextId,
                                   &command)),
            "reorder tabs");
    Require(SUCCEEDED(api.syncTabs(first, reordered, 1, 22, &requested, &closeRequested, &newRequested, &contextId,
                                   &command)),
            "close tab");
    Require(api.syncTabs(first, tabs, 2, 99, &requested, &closeRequested, &newRequested, &contextId, &command) ==
                E_INVALIDARG,
            "reject missing selected tab");
    Require(api.syncTabs(first, nullptr, 2, 0, &requested, &closeRequested, &newRequested, &contextId, &command) ==
                E_INVALIDARG,
            "reject null snapshot");
    WinUITab duplicate[]{{11, L"One"}, {11, L"Two"}};
    Require(api.syncTabs(first, duplicate, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command) ==
                E_INVALIDARG,
            "reject duplicate identities");
    Require(
        SUCCEEDED(api.syncTabs(first, nullptr, 0, 0, &requested, &closeRequested, &newRequested, &contextId, &command)),
        "empty list");
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)),
        "restore tabs");
    HWND island = GetWindow(first, GW_CHILD);
    Require(island != nullptr && island != compositor, "island above compositor sibling");
    ShowWindow(first, SW_SHOWNOACTIVATE);
    for (int i = 0; i < 20; ++i) {
        Pump();
        Sleep(10);
    }

    if (argc == 3 && wcscmp(argv[2], L"--address") == 0) {
        try {
            CheckAddress(api, module, first);
        } catch (winrt::hresult_error const& error) {
            fwprintf(stderr, L"Address test HRESULT 0x%08X: %s\n", unsigned(error.code()), error.message().c_str());
            return 1;
        }
        Require(SUCCEEDED(api.destroy(first)), "address destroy");
        DestroyWindow(first);
        winrt::clear_factory_cache();
        Require(SUCCEEDED(api.shutdown()), "address shutdown");
        CoUninitialize();
        return 0;
    }
    Require(SUCCEEDED(api.focus(first, FALSE)), "focus native tab list");
    Pump();
    auto key = [&](WPARAM value) {
        MSG message{GetFocus(), WM_KEYDOWN, value, 1};
        if (!api.translate(&message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        message.message = WM_KEYUP;
        if (!api.translate(&message)) DispatchMessageW(&message);
        Pump();
    };
    key(VK_DOWN);
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)) &&
            requested == 22,
        "native selection reaches the Firefox bridge");
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 22, &requested, &closeRequested, &newRequested, &contextId, &command)) &&
            !requested,
        "selection request consumed exactly once");

    key(VK_UP);
    auto staleHr =
        api.syncTabs(first, tabs + 1, 1, 22, &requested, &closeRequested, &newRequested, &contextId, &command);
    Require(SUCCEEDED(staleHr) && !requested, "closed tab discards queued native selection");
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)),
        "restore after stale selection");
    WithTab(first, tabs[1].title, tabs[1].title, true);
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)) &&
            requested == 22,
        "accessibility selection reaches browser bridge");
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)),
        "restore selection before close");
    WithTab(first, tabs[1].title, tabs[1].title, false, true);
    for (int i = 0; i < 200; ++i) {
        Require(SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId,
                                       &command)) &&
                    !requested,
                "background close does not select its row");
        if (closeRequested) break;
        Pump();
        Sleep(10);
    }
    Require(closeRequested == 22, "background close reaches queue after asynchronous UIA invocation");
    WithTab(first, tabs[1].title, tabs[1].title, false, true);
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)) &&
            !closeRequested,
        "in-flight repeated close ignored and row retained until Firefox removes it");
    WithTab(first, tabs[1].title, tabs[1].title, false, true);
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 1, 11, &requested, &closeRequested, &newRequested, &contextId, &command)) &&
            !closeRequested,
        "stale close discarded when Firefox already removed the tab");
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)),
        "restore close fixtures");
    WithTab(first, tabs[0].title, tabs[0].title, false, true);
    WithTab(first, tabs[1].title, tabs[1].title, false, true);
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)) &&
            closeRequested == 11,
        "first queued close retained");
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)) &&
            closeRequested == 22,
        "second queued close retained");
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)),
        "acknowledge queued closes");
    for (int input : {2}) {
        WithTab(first, tabs[1].title, tabs[1].title, false, true, input);
        auto closeHr =
            api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command);
        fprintf(stderr, "Close input=%d hr=%08X selection=%u close=%u\n", input, unsigned(closeHr), requested,
                closeRequested);
        Require(SUCCEEDED(closeHr) && closeRequested == 22 && !requested, "keyboard background close does not select");
    }
    Require(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, nullptr, &contextId, &command) == E_POINTER,
            "reject null new-tab output");
    for (int input : {0, 2, 3}) {
        WithTab(first, L"FirefoxWinUINewTab", L"New tab", false, false, input);
        Require(SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId,
                                       &command)) &&
                    newRequested == 1 && !requested && !closeRequested,
                "UIA/Space/Enter requests new tab without inventing rows");
        Require(SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId,
                                       &command)) &&
                    !newRequested,
                "new-tab request consumed exactly once");
    }
    WithTab(first, L"FirefoxWinUINewTab", L"New tab");
    WithTab(first, L"FirefoxWinUINewTab", L"New tab");
    for (int i = 0; i < 2; ++i)
        Require(SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId,
                                       &command)) &&
                    newRequested == 1,
                "repeated new-tab requests retained");
    WinUITab duplicates[]{{11, L"New tab"}, {22, L"New tab"}};
    Require(SUCCEEDED(api.syncTabs(first, duplicates, 2, 11, &requested, &closeRequested, &newRequested, &contextId,
                                   &command)),
            "identical title snapshot");
    WithTab(first, L"New tab", L"New tab", false, true, 0, 2, 1);
    Require(SUCCEEDED(api.syncTabs(first, duplicates, 2, 11, &requested, &closeRequested, &newRequested, &contextId,
                                   &command)) &&
                closeRequested == 22 && !requested,
            "close second identically named tab by stable identity");
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)),
        "restore titles after duplicate regression");
    if (argc == 3) {
        CheckContextMenu(module, api, first);
        Require(SUCCEEDED(
                    api.syncTabs(first, tabs, 2, 11, &requested, &closeRequested, &newRequested, &contextId, &command)),
                "restore after context tests");
        CheckThemesAndLongList(module, api, first);
    }
    Require(SUCCEEDED(api.update(first, &rect, TRUE)), "restore geometry after theme checks");
    ShowWindow(first, SW_HIDE);
    Require(SetWindowPos(compositor, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE),
            "simulate compositor restacking");
    Require(SUCCEEDED(api.update(first, &rect, TRUE)), "restore island stacking");
    Require(GetWindow(first, GW_CHILD) == island, "island remains above restacked compositor");
    RECT cuts[]{{0, 0, 30, 100}, {100, 100, 200, 200}};
    Require(SUCCEEDED(api.clip(first, cuts, 2)), "clip overlapping overlays");
    Pump();
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    Require(GetWindowRgn(island, region) == COMPLEXREGION, "nonrectangular island region");
    Require(!PtInRegion(region, 10, 10) && !PtInRegion(region, 150, 150), "covered areas excluded");
    Require(PtInRegion(region, 50, 50) && PtInRegion(region, 230, 250), "uncovered areas preserved");
    Require(api.clip(first, nullptr, 1) == E_INVALIDARG, "reject null exclusions");
    RECT full{0, 0, 240, 300};
    Require(SUCCEEDED(api.clip(first, &full, 1)), "fully covered pane");
    Require(GetWindowRgn(island, region) == NULLREGION, "full cover has empty region");
    Require(SUCCEEDED(api.clip(first, nullptr, 0)), "restore full pane");
    Require(GetWindowRgn(island, region) == SIMPLEREGION && PtInRegion(region, 10, 10), "restored region");
    DeleteObject(region);
    unsigned children{};
    EnumChildWindows(first, CountChild, reinterpret_cast<LPARAM>(&children));
    Require(children > 0, "real native island child");
    HRESULT otherThread = S_OK;
    std::thread worker([&] { otherThread = api.update(first, &rect, TRUE); });
    worker.join();
    Require(otherThread == RPC_E_WRONG_THREAD, "reject wrong-thread update");
    std::thread clipWorker([&] { otherThread = api.clip(first, nullptr, 0); });
    clipWorker.join();
    Require(otherThread == RPC_E_WRONG_THREAD, "reject wrong thread");
    if (argc == 3) CheckAddress(api, module, first);
    for (int cycle = 0; cycle < 20; ++cycle) {
        HWND second = CreateWindowExW(0, L"STATIC", L"Second pane host", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr,
                                      nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(second != nullptr, "second host");
        Require(SUCCEEDED(api.update(second, &rect, TRUE)), "create second island");
        WinUITab other[]{{11, L"Independent window"}};
        Require(SUCCEEDED(api.syncTabs(second, other, 1, 11, &requested, &closeRequested, &newRequested, &contextId,
                                       &command)) &&
                    !requested,
                "independent window can reuse tab IDs");
        if (cycle == 0) {
            ShowWindow(second, SW_SHOWNOACTIVATE);
            Pump();
            WithTab(second, L"FirefoxWinUINewTab", L"New tab");
            Require(SUCCEEDED(api.syncTabs(first, nullptr, 0, 0, &requested, &closeRequested, &newRequested, &contextId,
                                           &command)) &&
                        !newRequested,
                    "second-window new-tab action cannot leak to first window");
            Require(SUCCEEDED(api.syncTabs(second, other, 1, 11, &requested, &closeRequested, &newRequested, &contextId,
                                           &command)) &&
                        newRequested == 1,
                    "new-tab request reaches its owning window");
        }
        Require(SUCCEEDED(api.clip(second, cuts, 2)), "second window clipping");
        HRGN firstRegion = CreateRectRgn(0, 0, 0, 0);
        Require(GetWindowRgn(island, firstRegion) == SIMPLEREGION && PtInRegion(firstRegion, 10, 10),
                "second window clipping leaves first unchanged");
        DeleteObject(firstRegion);
        Pump();
        RECT resized{10, 20, 210, 380};
        Require(SUCCEEDED(api.update(second, &resized, TRUE)), "resize second island");
        Require(SUCCEEDED(api.update(second, &resized, FALSE)), "hide second island");
        Require(SUCCEEDED(api.update(second, &resized, TRUE)), "show second island");
        if (cycle == 0 && argc == 3) {
            using namespace winrt::Microsoft::UI::Xaml;
            auto inspect =
                reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
            UIElement content{nullptr};
            Require(SUCCEEDED(inspect(second, winrt::put_abi(content))), "inspect shutdown-menu host");
            auto list = content.as<Controls::Grid>().Children().GetAt(1).as<Controls::ListView>();
            PrepareFixtureMenus(api, second, list);
            auto row = RealizedRow(list, 0);
            SetForegroundWindow(second);
            row.Focus(FocusState::Programmatic);
            for (int i = 0; i < 100; ++i) {
                Pump();
                Sleep(10);
            }
            auto menu = row.ContextFlyout().as<Controls::MenuFlyout>();
            bool opened = false;
            auto token = menu.Opened([&](auto const&, auto const&) { opened = true; });
            menu.ShowAt(row);
            for (int i = 0; i < 250 && !opened; ++i) {
                Pump();
                Sleep(20);
            }
            menu.Opened(token);
            Require(opened, "open menu before island teardown");
        }
        Require(SUCCEEDED(api.destroy(second)), "destroy before HWND, including an open menu");
        Require(SUCCEEDED(api.destroy(second)), "idempotent destroy");
        DestroyWindow(second);
        Require(SUCCEEDED(api.update(first, &rect, TRUE)), "first island survives second close");
        Pump();
    }
    Require(SUCCEEDED(api.destroy(first)), "destroy first island");
    DestroyWindow(first);
    winrt::clear_factory_cache();
    Require(SUCCEEDED(api.shutdown()), "dispatcher shutdown");
    Require(api.update(nullptr, &rect, TRUE) == RO_E_CLOSED, "reject after shutdown");
    CoUninitialize();
    puts(
        "CHECKS PASSED (process exit still required): ABI rejection, child HWND stacking above compositor, thread "
        "affinity, partial clipping/restoration, tab snapshots, keyboard selection, stale selection rejection, UIA "
        "selection and accessible titles, native close via UIA/keyboard, close queue and stale IDs, new tab via "
        "UIA/Space/Enter and per-window queue, 20 two-window lifecycles, shutdown");
    return 0;
}
