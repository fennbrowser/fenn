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
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>
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
// Only inspect this test thread's popups; an unrelated running browser can
// legitimately own a preview with the same window class.
static std::vector<HWND> DragPreviewWindows() {
    std::vector<HWND> windows;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM data) -> BOOL {
        wchar_t name[128]{};
        GetClassNameW(hwnd, name, 128);
        if (wcscmp(name, L"FirefoxWinUITabTearoff") == 0)
            reinterpret_cast<std::vector<HWND>*>(data)->push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}
static bool nestedDragCreateObserved{};
static LRESULT CALLBACK DragCreateProbe(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_CREATEWND && !nestedDragCreateObserved) {
        wchar_t name[128]{};
        GetClassNameW(HWND(w), name, 128);
        if (wcscmp(name, L"FirefoxWinUITabTearoff") == 0) {
            nestedDragCreateObserved = true;
            // Simulate message-hook reentry before CreateWindowEx has returned
            // its HWND to the session, as native callbacks are allowed to do.
            MSG message{HWND(w), WM_NULL};
            if (translatePane) translatePane(&message);
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
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
// Two conditions, two waits. An island's content tree goes live asynchronously
// once its host window becomes visible, and nothing can realize a container
// before that; whether the row then realizes is a separate question with its
// own, much shorter, answer. Measured on this machine with
// `PaneContractTests.exe ... --inspect`: the process's first island goes live
// 78-125ms after creation, a later island 359-625ms, and rarely -- about one
// long-running process in six -- 9.7-13.4s, with every observable state
// (IsHostVisible, the child window's visibility, its region) identical in the
// fast and slow cases. Covering both conditions with one ~5s budget is what
// turned that outlier into an intermittent failure blamed on hide/show.
static void AwaitLiveTree(winrt::Microsoft::UI::Xaml::FrameworkElement const& element) {
    auto started = GetTickCount64();
    while (!element.IsLoaded() && GetTickCount64() - started < 30000) {
        Pump();
        Sleep(5);
    }
    auto waited = GetTickCount64() - started;
    if (waited > 1000)
        fprintf(stderr, "Island content tree went live after %llums\n", static_cast<unsigned long long>(waited));
    Require(element.IsLoaded(), "the island's content tree goes live");
}
static winrt::Microsoft::UI::Xaml::Controls::ListViewItem RealizedRow(
    winrt::Microsoft::UI::Xaml::Controls::ListView const& list, int32_t index) {
    using namespace winrt::Microsoft::UI::Xaml;
    AwaitLiveTree(list);
    for (int i = 0; i < 500; ++i) {
        list.UpdateLayout();
        auto row = list.ContainerFromIndex(index).try_as<Controls::ListViewItem>();
        if (row && row.IsLoaded() && row.ContentTemplateRoot() && row.ActualWidth() > 0 && row.ActualHeight() > 0)
            return row;
        if (index >= 0 && uint32_t(index) < list.Items().Size()) list.ScrollIntoView(list.Items().GetAt(index));
        Pump();
        Sleep(10);
    }
    bool rootLoaded = false, hasParent = false;
    try {
        if (auto content = list.XamlRoot().Content().try_as<FrameworkElement>()) rootLoaded = content.IsLoaded();
        hasParent = list.Parent() != nullptr;
    } catch (...) {
    }
    fprintf(stderr,
            "Unrealized row index=%d items=%u list=%.1fx%.1f loaded=%d hostVisible=%d rootLoaded=%d parent=%d\n",
            index, list.Items().Size(), list.ActualWidth(), list.ActualHeight(), list.IsLoaded(),
            list.XamlRoot().IsHostVisible(), rootLoaded, hasParent);
    auto missingRow = list.ContainerFromIndex(index).try_as<Controls::ListViewItem>();
    if (missingRow)
        fprintf(stderr, "Row loaded=%d template=%d size=%.1fx%.1f\n", missingRow.IsLoaded(),
                bool(missingRow.ContentTemplateRoot()), missingRow.ActualWidth(), missingRow.ActualHeight());
    Require(false, "native row is loaded and laid out before interaction");
    return nullptr;
}
// Since ABI v12 this is a publication with no reply. Every native intent,
// including the strip's own gestures, arrives through drainRequests instead,
// so there is nothing to read back here.
static void MenuPublish(WinUIPaneApi& api, HWND hwnd,
                         wchar_t const* snapshot = L"{\"version\":2}") {
    auto result = api.syncTabMenu(hwnd, snapshot);
    if (FAILED(result)) fwprintf(stderr, L"Publication HRESULT %08x: %ls\n", unsigned(result), snapshot);
    Require(SUCCEEDED(result), "menu protocol publication");
}
static uint32_t menuTestRevision = 0;
// Since ABI v11 the recurring publication carries identities and the selected
// set only. No description of any tab's commands appears here.
static winrt::Windows::Data::Json::JsonObject MenuState(std::vector<uint32_t> const& ids) {
    using namespace winrt::Windows::Data::Json;
    auto snapshot = JsonObject::Parse(LR"({"version":2,"publishOnly":true,"state":{"revision":1,"tabs":[]}})");
    auto state = snapshot.GetNamedObject(L"state");
    state.SetNamedValue(L"revision", JsonValue::CreateNumberValue(++menuTestRevision));
    for (auto id : ids) {
        JsonObject binding;
        binding.SetNamedValue(L"tab", JsonValue::CreateNumberValue(id));
        state.GetNamedArray(L"tabs").Append(binding);
    }
    return snapshot;
}
// The on-demand answer to one opening, bound to its session/tab/revision.
static winrt::Windows::Data::Json::JsonObject MenuDescription(double session, uint32_t tab, double revision,
                                                              bool nested = false) {
    using namespace winrt::Windows::Data::Json;
    auto items = JsonArray::Parse(
        LR"([{"kind":"command","id":"opaque-reload","label":"Reload Tab","enabled":true},{"kind":"command","id":"opaque-duplicate","label":"Duplicate Tab","enabled":false},{"kind":"command","id":"opaque-unpin","label":"Unpin Tab","enabled":true}])");
    if (nested) {
        auto sub = JsonObject::Parse(LR"({"kind":"submenu","label":"Actions","enabled":true})");
        sub.SetNamedValue(L"items", items);
        JsonArray outer;
        outer.Append(JsonObject::Parse(LR"({"kind":"separator"})"));
        outer.Append(sub);
        items = outer;
    }
    JsonObject menu;
    menu.SetNamedValue(L"session", JsonValue::CreateNumberValue(session));
    menu.SetNamedValue(L"tab", JsonValue::CreateNumberValue(tab));
    menu.SetNamedValue(L"revision", JsonValue::CreateNumberValue(revision));
    menu.SetNamedValue(L"items", items);
    JsonObject snapshot;
    snapshot.SetNamedValue(L"version", JsonValue::CreateNumberValue(2));
    snapshot.SetNamedValue(L"publishOnly", JsonValue::CreateBooleanValue(true));
    snapshot.SetNamedValue(L"menu", menu);
    return snapshot;
}
// The v11 request path, as the browser drives it: take one bounded batch, then
// close it with explicit outcomes. Returns the envelope, empty when there was
// nothing to take.
static winrt::Windows::Data::Json::JsonObject DrainRequests(WinUIPaneApi& api, HWND hwnd) {
    std::vector<wchar_t> output(262144);
    output[0] = 0;
    Require(SUCCEEDED(api.drainRequests(hwnd, output.data(), uint32_t(output.size()))), "drain native requests");
    return output[0] ? winrt::Windows::Data::Json::JsonObject::Parse(output.data())
                     : winrt::Windows::Data::Json::JsonObject();
}
static void CompleteRequests(WinUIPaneApi& api, HWND hwnd, winrt::Windows::Data::Json::JsonObject const& batch,
                             uint32_t outcome = 1, bool blocked = false) {
    using namespace winrt::Windows::Data::Json;
    JsonArray outcomes;
    if (batch.Size()) {
        for (auto value : batch.GetNamedArray(L"requests")) {
            JsonObject entry;
            entry.SetNamedValue(L"id", JsonValue::CreateNumberValue(value.GetObject().GetNamedNumber(L"id")));
            entry.SetNamedValue(L"outcome", JsonValue::CreateNumberValue(outcome));
            outcomes.Append(entry);
        }
    }
    JsonObject message;
    message.SetNamedValue(L"version", JsonValue::CreateNumberValue(1));
    message.SetNamedValue(L"generation",
                          JsonValue::CreateNumberValue(batch.Size() ? batch.GetNamedNumber(L"generation") : 0));
    message.SetNamedValue(L"outcomes", outcomes);
    message.SetNamedValue(L"blocked", JsonValue::CreateBooleanValue(blocked));
    Require(SUCCEEDED(api.completeRequests(hwnd, message.Stringify().c_str())), "complete native requests");
}
// Drain until a request of `kind` appears (1 select-tab, 2 menu transition),
// completing every batch as the browser would. Returns an empty object if none
// arrives, so a caller can assert absence as well as presence.
static winrt::Windows::Data::Json::JsonObject NextRequest(WinUIPaneApi& api, HWND hwnd, uint32_t kind,
                                                          wchar_t const* eventKind = nullptr, int attempts = 40, uint32_t outcome = 1) {
    using namespace winrt::Windows::Data::Json;
    for (int i = 0; i < attempts; ++i) {
        auto batch = DrainRequests(api, hwnd);
        JsonObject found;
        if (batch.Size()) {
            for (auto value : batch.GetNamedArray(L"requests")) {
                auto request = value.GetObject();
                if (request.GetNamedNumber(L"kind") != kind) continue;
                auto action = request.GetNamedObject(L"action");
                if (eventKind && action.GetNamedString(L"kind") != eventKind) continue;
                found = request;
            }
            CompleteRequests(api, hwnd, batch, outcome);
        }
        if (found.Size()) return found;
        Pump();
        Sleep(5);
    }
    return JsonObject();
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
    MenuPublish(api, hwnd, MenuState(ids).Stringify().c_str());
    uint32_t selected{};
    Require(SUCCEEDED(api.syncTabs(hwnd, tabs.data(), uint32_t(tabs.size()), ids[list.SelectedIndex()], &selected)),
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
    uint32_t selected{};
    auto settle = [] {
        for (int i = 0; i < 100; ++i) {
            Pump();
            Sleep(10);
        }
    };
    auto sync = [&](uint32_t count = 2) {
        Require(SUCCEEDED(api.syncTabs(hwnd, tabs, count, 71, &selected)),
                "context tabs");
        settle();
    };
    SetForegroundWindow(hwnd);
    Require(SUCCEEDED(api.focus(hwnd, FALSE)), "activate test island before realizing rows");
    settle();  // Finish the new island's Loaded transition before inserting fixture rows.
    auto current = MenuState({71, 72});
    MenuPublish(api, hwnd, current.Stringify().c_str());
    sync();
    Controls::MenuFlyout menu{nullptr};
    Object opened;
    // Opening a menu: the row shows a deliberately non-actionable pending state
    // and asks for that one tab's description; the answer is what makes any
    // command appear. `count` is how many entries the answer produces.
    //
    // Every opening asks again, including a reopen of the same row: Firefox is
    // the only authority on a tab's commands, and a description can go stale
    // from state no publication carries.
    auto open = [&](uint32_t count = 3, bool nested = false, bool describe = true) {
        fprintf(stderr, "Context open count=%u revision=%u\n", count, menuTestRevision);
        if (menu && menu.IsOpen()) menu.Hide();
        settle();
        while (DrainRequests(api, hwnd).Size()) {
        }
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
        Require(shown && menu.IsOpen(), "the menu opens without waiting for a browser exchange");
        // Whatever is displayed before the description arrives must not be
        // actionable: one disabled row, no commands. This holds for a reopen
        // too, which is what makes a stale description impossible.
        Require(menu.Items().Size() == 1 && !menu.Items().GetAt(0).IsEnabled(),
                "an opening menu shows a non-actionable pending state, never stale commands");
        auto request = NextRequest(api, hwnd, 2, L"open");
        opened = request.Size() ? request.GetNamedObject(L"action") : Object();
        Require(opened.Size() && opened.GetNamedNumber(L"tab") == 72 &&
                    opened.GetNamedNumber(L"revision") ==
                        current.GetNamedObject(L"state").GetNamedNumber(L"revision"),
                "opening retains target and published revision");
        if (!describe) return;
        auto description = MenuDescription(opened.GetNamedNumber(L"session"), 72, opened.GetNamedNumber(L"revision"), nested);
        if (!nested) description.GetNamedObject(L"menu").GetNamedArray(L"items").GetObjectAt(1).SetNamedValue(
            L"checked", Value::CreateBooleanValue(true));
        if (!nested) description.GetNamedObject(L"menu").GetNamedArray(L"items").GetObjectAt(0).SetNamedValue(
            L"image", Object::Parse(LR"({"width":2,"height":2,"pixels":"0000ffff00000000000000000000ffff"})"));
        MenuPublish(api, hwnd, description.Stringify().c_str());
        settle();
        Require(menu.IsOpen() && menu.Items().Size() == count && menu.Items().GetAt(count - 1).ActualWidth() > 0,
                "the authoritative description replaces the pending state in place");
        Require(nested || bool(menu.Items().GetAt(0).as<Controls::MenuFlyoutItem>().Icon().try_as<Controls::ImageIcon>().Source()),
                "menu uses a decoded image icon, never a font fallback glyph");
        Require(nested || menu.Items().GetAt(1).as<Controls::ToggleMenuFlyoutItem>().IsChecked(),
                "authoritative checked state is rendered by a native toggle menu item");
    };
    Controls::MenuFlyout retained{nullptr};
    Controls::MenuFlyoutItemBase retainedItem{nullptr};
    for (uint32_t index : {0u, 2u}) {
        open();
        Require(menu != retained && menu.Items().GetAt(0) != retainedItem, "each opening uses fresh native controls");
        retained = menu;
        retainedItem = menu.Items().GetAt(0);
        Require(!menu.Items().GetAt(1).IsEnabled(), "the description disables a native command");
        Require(menu.Items().GetAt(2).as<Controls::MenuFlyoutItem>().Text() == L"Unpin Tab", "Firefox supplies label");
        auto peer = Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(menu.Items().GetAt(index));
        peer.GetPattern(Automation::Peers::PatternInterface::Invoke)
            .as<Automation::Provider::IInvokeProvider>()
            .Invoke();
        Pump();
        auto request = NextRequest(api, hwnd, 2, L"activate");
        auto event = request.Size() ? request.GetNamedObject(L"action") : Object();
        Require(event.Size() && event.GetNamedNumber(L"tab") == 72 &&
                    event.GetNamedNumber(L"session") == opened.GetNamedNumber(L"session") &&
                    event.GetNamedNumber(L"revision") == opened.GetNamedNumber(L"revision") &&
                    event.GetNamedString(L"id") == (index ? L"opaque-unpin" : L"opaque-reload"),
                "opaque action retains target/session/revision");
        Require(list.SelectedIndex() == 0, "context activation preserves background selection");
        Require(!NextRequest(api, hwnd, 2, L"activate", 3).Size(), "activation is delivered once");
    }
    // A new published context invalidates every cached description, so the next
    // opening asks again and can be answered differently.
    current = MenuState({71, 72});
    MenuPublish(api, hwnd, current.Stringify().c_str());
    sync();
    open(2, true);
    Require(menu.Items().GetAt(0).try_as<Controls::MenuFlyoutSeparator>() != nullptr &&
                menu.Items().GetAt(1).as<Controls::MenuFlyoutSubItem>().Items().Size() == 3,
            "standard separator and submenu renderer");
    auto old = current;
    current = MenuState({71, 72});
    MenuPublish(api, hwnd, current.Stringify().c_str());
    settle();
    Require(!menu.IsOpen(), "changed state dismisses obsolete menu");
    open();
    MenuPublish(api, hwnd, old.Stringify().c_str());
    Require(menu.IsOpen() && menu.Items().Size() == 3, "late state cannot replace current controls");
    // A description that no longer names the open request must not be rendered.
    auto session = opened.GetNamedNumber(L"session");
    auto revision = opened.GetNamedNumber(L"revision");
    for (auto stale : {MenuDescription(session + 5, 72, revision), MenuDescription(session, 71, revision),
                       MenuDescription(session, 72, revision + 5)}) {
        MenuPublish(api, hwnd, stale.Stringify().c_str());
        settle();
        Require(menu.IsOpen() && menu.Items().Size() == 3,
                "a description for another session, tab or revision is discarded");
    }
    auto reject = [&](Object const& malformed, char const* what) {
        Require(FAILED(api.syncTabMenu(hwnd, malformed.Stringify().c_str())),
                "invalid state rejected atomically");
        fprintf(stderr, "Reject %s open=%d items=%u\n", what, menu.IsOpen(), menu.Items().Size());
        Require(menu.IsOpen() && menu.Items().Size() == 3, "invalid state preserves current menu");
    };
    auto malformed = MenuDescription(session, 72, revision);
    malformed.GetNamedObject(L"menu").GetNamedArray(L"items").GetObjectAt(1).SetNamedValue(
        L"id", Value::CreateStringValue(L"opaque-reload"));
    reject(malformed, "duplicate command id in a description");
    auto mixedEnvelope = MenuState({71, 72});
    mixedEnvelope.SetNamedValue(L"menu", malformed.GetNamedObject(L"menu"));
    reject(mixedEnvelope, "new publication plus malformed description leaves old session intact");
    reject(MenuState({71, 71}), "duplicate tab id in a publication");
    malformed = MenuState({71, 72});
    malformed.SetNamedValue(L"publishOnly", Value::CreateStringValue(L"true"));
    reject(malformed, "publishOnly of the wrong type");
    Require(FAILED(api.syncTabMenu(hwnd, L"{broken")), "malformed JSON rejected");
    Require(FAILED(api.syncTabMenu(hwnd, L"{\"version\":3}")), "unknown protocol rejected");
    // An opening whose description never arrives stays non-actionable, and the
    // menu can be dismissed and reopened without leaving stale rows behind.
    current = MenuState({71, 72});
    MenuPublish(api, hwnd, current.Stringify().c_str());
    sync();
    open(0, false, /*describe=*/false);
    Require(menu.Items().Size() == 1 && !menu.Items().GetAt(0).IsEnabled(),
            "an unanswered opening never becomes actionable");
    menu.Hide();
    settle();
    while (DrainRequests(api, hwnd).Size()) {
    }
    current = MenuState({71});
    MenuPublish(api, hwnd, current.Stringify().c_str());
    sync(1);
    Require(!menu.IsOpen(), "removed target dismisses menu");
    sync();
    Require(!RealizedRow(list, 1).ContextFlyout(), "unpublished target has no loading popup");
    current = MenuState({71, 72});
    MenuPublish(api, hwnd, current.Stringify().c_str());
    sync();
    open();
    menu.Hide();
    settle();
    while (DrainRequests(api, hwnd).Size()) {
    }
    current = MenuState({71, 72});
    for (auto binding : current.GetNamedObject(L"state").GetNamedArray(L"tabs"))
        binding.GetObject().SetNamedValue(L"selected", Value::CreateBooleanValue(true));
    MenuPublish(api, hwnd, current.Stringify().c_str());
    sync();
    Require(list.SelectionMode() == Controls::ListViewSelectionMode::Extended && list.SelectedItems().Size() == 2,
            "Firefox selected set uses native Extended selection independently of active tab");
    Require(Automation::AutomationProperties::GetItemStatus(RealizedRow(list, 0)) == L"Active tab" &&
                Automation::AutomationProperties::GetItemStatus(RealizedRow(list, 1)).empty(),
            "active tab remains separately accessible");
    list.SelectedItems().Clear();
    list.SelectedItems().Append(list.Items().GetAt(1));
    settle();
    // Selection now travels on the v11 request path, not on syncTabs and not on
    // the menu exchange: exactly one executor.
    auto selectionRequest = NextRequest(api, hwnd, 1);
    auto selectionEvent = selectionRequest.Size() ? selectionRequest.GetNamedObject(L"action") : Object();
    Require(selectionEvent.Size() && selectionEvent.GetNamedString(L"kind") == L"selection" &&
                selectionEvent.GetNamedNumber(L"tab") == 72 && selectionEvent.GetNamedArray(L"tabs").Size() == 1 &&
                selectionEvent.GetNamedArray(L"tabs").GetNumberAt(0) == 72,
            "native selected set is copied and delivered once through the request path");
    Require(!NextRequest(api, hwnd, 1, nullptr, 3).Size(), "selection request is delivered once");
    {
        uint32_t reserved{}, c{}, n{}, i{}, cmd{};
        WinUITab pair[]{{71, L"Context first", 7}, {72, L"Context second", 27}};
        Require(SUCCEEDED(api.syncTabs(hwnd, pair, 2, 71, &reserved)), "snapshot after selection");
        Require(c == 0 && n == 0 && i == 0 && cmd == 0, "syncTabs no longer drains selection");
    }
    current = MenuState({71, 72});
    MenuPublish(api, hwnd, current.Stringify().c_str());
    sync();
    puts(
        "Menu protocol: pending-state opening, on-demand descriptions bound to their request, stale-answer "
        "rejection, opaque activation, revisions and malformed input passed");
}
// The v11 request path and the incremental presentation model, both owned by
// the Rust shell core linked into this DLL. This is the direct evidence that
// the core is actually driving the pane rather than merely being linked.
static void CheckShellCoreRequests(HMODULE module, WinUIPaneApi& api, HWND hwnd) {
    using namespace winrt;
    using namespace winrt::Microsoft::UI::Xaml;
    using Object = winrt::Windows::Data::Json::JsonObject;
    using Value = winrt::Windows::Data::Json::JsonValue;
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
    UIElement content{nullptr};
    Require(SUCCEEDED(inspect(hwnd, put_abi(content))), "inspect request-path host");
    auto list = content.as<Controls::Grid>().Children().GetAt(1).as<Controls::ListView>();
    uint32_t revision{};
    WinUITab tabs[]{{101, L"Core first", 7}, {102, L"Core second", 7}, {103, L"Core third", 7}};
    MenuPublish(api, hwnd, MenuState({101, 102, 103}).Stringify().c_str());
    Require(SUCCEEDED(api.syncTabs(hwnd, tabs, 3, 101, &revision)),
            "seed the presentation model");
    Require(revision != 0, "a snapshot reports the model revision it produced");
    Pump();

    // One notification per endpoint. The sink is called on the transition out
    // of idle and not again until the host drains.
    static int notifications = 0;
    notifications = 0;
    Require(SUCCEEDED(api.setNotifySink(
                hwnd, [](void*) -> BOOL { ++notifications; return TRUE; }, nullptr)),
            "register the drain wakeup");
    for (uint32_t index : {1u, 2u, 1u}) {
        list.SelectedIndex(int(index));
        Pump();
    }
    Require(notifications == 1, "repeated input accumulates work without reposting the wakeup");

    auto batch = DrainRequests(api, hwnd);
    Require(batch.Size() && batch.GetNamedNumber(L"version") == 1, "the drain returns a versioned envelope");
    auto requests = batch.GetNamedArray(L"requests");
    // Selection is a replaceable value, so three clicks collapse to the newest
    // one -- explicitly superseded, not silently dropped.
    Require(requests.Size() == 1 && requests.GetObjectAt(0).GetNamedNumber(L"kind") == 1 &&
                requests.GetObjectAt(0).GetNamedObject(L"action").GetNamedNumber(L"tab") == 102,
            "the newest selection supersedes the queued ones");
    auto requestId = requests.GetObjectAt(0).GetNamedNumber(L"id");
    // A drain already open is refused rather than nested.
    Require(!DrainRequests(api, hwnd).Size(), "a second drain is refused while one is open");
    CompleteRequests(api, hwnd, batch);
    Require(!DrainRequests(api, hwnd).Size(), "an empty queue drains nothing");

    // Replay: the same completion a second time must not be accepted, and the
    // generation must match the live endpoint.
    Object replay;
    Object outcome;
    outcome.SetNamedValue(L"id", Value::CreateNumberValue(requestId));
    outcome.SetNamedValue(L"outcome", Value::CreateNumberValue(1));
    winrt::Windows::Data::Json::JsonArray outcomes;
    outcomes.Append(outcome);
    replay.SetNamedValue(L"version", Value::CreateNumberValue(1));
    replay.SetNamedValue(L"generation", Value::CreateNumberValue(batch.GetNamedNumber(L"generation") + 99));
    replay.SetNamedValue(L"outcomes", outcomes);
    Require(api.completeRequests(hwnd, replay.Stringify().c_str()) == E_NOT_VALID_STATE,
            "a completion for another endpoint generation is rejected");

    // Blocking: the host says it cannot execute, and nothing is posted until it
    // says otherwise.
    notifications = 0;
    Object blockMessage;
    blockMessage.SetNamedValue(L"version", Value::CreateNumberValue(1));
    blockMessage.SetNamedValue(L"generation", Value::CreateNumberValue(0));
    blockMessage.SetNamedValue(L"outcomes", winrt::Windows::Data::Json::JsonArray());
    blockMessage.SetNamedValue(L"blocked", Value::CreateBooleanValue(true));
    Require(SUCCEEDED(api.completeRequests(hwnd, blockMessage.Stringify().c_str())), "block execution");
    list.SelectedIndex(2);
    Pump();
    Require(notifications == 0, "a blocked endpoint never reposts its wakeup");
    Require(!DrainRequests(api, hwnd).Size(), "a blocked endpoint refuses to hand out work");
    blockMessage.SetNamedValue(L"blocked", Value::CreateBooleanValue(false));
    Require(SUCCEEDED(api.completeRequests(hwnd, blockMessage.Stringify().c_str())), "unblock execution");
    Require(notifications == 1, "unblocking arranges exactly one wakeup for the work that waited");
    auto resumed = DrainRequests(api, hwnd);
    Require(resumed.Size() && resumed.GetNamedArray(L"requests").Size() == 1,
            "the work queued while blocked is still there, and is delivered once");
    CompleteRequests(api, hwnd, resumed);

    // Incremental presentation. One title changes; nothing else is touched, and
    // the revision advances so the next delta has a base.
    auto delta = [&](wchar_t const* json) {
        uint32_t next = 0;
        auto hr = api.applyTabDelta(hwnd, json, &next);
        return std::pair<HRESULT, uint32_t>{hr, next};
    };
    auto before = revision;
    auto title = std::wstring(LR"({"version":1,"base":)") + std::to_wstring(before) +
                 LR"(,"titles":[{"id":102,"title":"Core renamed"}]})";
    auto applied = delta(title.c_str());
    Require(SUCCEEDED(applied.first) && applied.second > before, "a title delta applies and advances the revision");
    // The same delta replayed names a revision the model has moved past.
    Require(delta(title.c_str()).first == E_NOT_VALID_STATE,
            "a delta against a stale base is a gap, answered with a snapshot rather than a retry");
    auto selection = std::wstring(LR"({"version":1,"base":)") + std::to_wstring(applied.second) +
                     LR"(,"selected":103,"titles":[]})";
    auto selected = delta(selection.c_str());
    Require(SUCCEEDED(selected.first), "a selection delta applies");
    Pump();
    Require(list.SelectedIndex() == 2, "the delta moved the native selection without a snapshot");
    auto unknown = std::wstring(LR"({"version":1,"base":)") + std::to_wstring(selected.second) +
                   LR"(,"titles":[{"id":999,"title":"absent"}]})";
    Require(delta(unknown.c_str()).first == E_NOT_VALID_STATE, "a delta for an unknown tab is refused");

    Require(SUCCEEDED(api.setNotifySink(hwnd, nullptr, nullptr)), "clear the drain wakeup");
    notifications = 0;
    list.SelectedIndex(0);
    Pump();
    Require(notifications == 0, "a cleared sink is never called");
    // Work queued with no sink must not be stranded: registering one again
    // schedules the wakeup that could not be delivered.
    Require(SUCCEEDED(api.setNotifySink(
                hwnd, [](void*) -> BOOL { ++notifications; return TRUE; }, nullptr)),
            "re-register the drain wakeup");
    Require(notifications == 1, "a late sink picks up the work that could not be announced");
    CompleteRequests(api, hwnd, DrainRequests(api, hwnd));
    Require(SUCCEEDED(api.setNotifySink(hwnd, nullptr, nullptr)), "leave the fixture without a sink");

    // Last, because it deliberately poisons this endpoint's core: a panic is
    // contained and reported rather than unwound into C++, and the pane keeps
    // working through its snapshot-only fallback.
    if (auto panic = reinterpret_cast<HRESULT(WINAPI*)(HWND)>(GetProcAddress(module, "WinUIPaneTestCorePanic"))) {
        Require(panic(hwnd) == E_FAIL, "a Rust panic is contained and reported as a failure");
        Require(SUCCEEDED(api.syncTabs(hwnd, tabs, 3, 101, &revision)),
                "the pane still accepts snapshots after a contained panic");
        uint32_t next = 0;
        Require(api.applyTabDelta(hwnd, LR"({"version":1,"base":1,"titles":[]})", &next) == E_NOT_VALID_STATE,
                "a poisoned core refuses deltas instead of pretending to apply them");
    }
    puts("Shell core: one wakeup per endpoint, superseding selection, blocked/unblocked execution, bounded batches, "
         "replay rejection, incremental deltas, gap detection and panic containment passed");
}

// Exercise OS-delivered Tab navigation before observing address edits.
static void CheckTabKeyboardNavigation(HMODULE module, WinUIPaneApi& api, HWND hwnd) {
    using namespace winrt::Microsoft::UI::Xaml;
    SetForegroundWindow(hwnd);
    for (int attempt = 0; attempt < 100 && GetForegroundWindow() != hwnd; ++attempt) {
        Pump();
        Sleep(10);
    }
    Require(GetForegroundWindow() == hwnd, "tab keyboard test owns foreground before injecting input");
    Require(SUCCEEDED(api.focus(hwnd, FALSE)), "focus island for Tab navigation");
    Pump();
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
    UIElement content{nullptr};
    Require(inspect && SUCCEEDED(inspect(hwnd, winrt::put_abi(content))), "inspect Tab navigation host");
    auto root = content.as<Controls::Grid>();
    auto list = root.Children().GetAt(1).as<Controls::ListView>();
    auto row = RealizedRow(list, list.SelectedIndex());
    Require(row.Focus(FocusState::Keyboard), "focus selected tab row");
    auto close = row.ContentTemplateRoot().as<Controls::Grid>().Children().GetAt(1).as<Controls::Button>();
    bool released = false;
    Input::KeyEventHandler releaseHandler = [&](auto const&, Input::KeyRoutedEventArgs const& event) {
        if (event.Key() == winrt::Windows::System::VirtualKey::Tab) released = true;
    };
    auto boxedReleaseHandler = winrt::box_value(releaseHandler);
    root.AddHandler(UIElement::KeyUpEvent(), boxedReleaseHandler, true);
    INPUT keys[2]{};
    keys[0].type = keys[1].type = INPUT_KEYBOARD;
    keys[0].ki.wVk = keys[1].ki.wVk = VK_TAB;
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    auto sent = SendInput(2, keys, sizeof(INPUT));
    if (sent != 2) SendInput(1, &keys[1], sizeof(INPUT));
    Require(sent == 2, "inject complete Tab press and release");
    for (int attempt = 0; attempt < 100; ++attempt) {
        Pump();
        if (Input::FocusManager::GetFocusedElement(root.XamlRoot()) == close &&
            released) break;
        Sleep(10);
    }
    root.RemoveHandler(UIElement::KeyUpEvent(), boxedReleaseHandler);
    Require(Input::FocusManager::GetFocusedElement(root.XamlRoot()) == close && released,
            "Tab reaches the selected row's native close button and the key is released");
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
        auto request = NextRequest(api, hwnd, 2, L"open");
        Require(request.Size(), "theme menu opening request");
        auto action = request.GetNamedObject(L"action");
        // Answer the opening so there are real command rows to inspect.
        MenuPublish(api, hwnd,
                     MenuDescription(action.GetNamedNumber(L"session"), uint32_t(action.GetNamedNumber(L"tab")),
                                     action.GetNamedNumber(L"revision"))
                         .Stringify()
                         .c_str());
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
    uint32_t requested{};
    Require(SUCCEEDED(api.syncTabs(hwnd, tabs.data(), uint32_t(tabs.size()), 120, &requested)),
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
        SUCCEEDED(api.syncTabs(hwnd, nullptr, 0, 0, &requested)),
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
    Require(FAILED(api.syncAddress(hwnd, L"{broken")),
            "reject malformed address JSON");
    Require(SUCCEEDED(api.syncAddress(hwnd, snapshot)),
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
    Require(SUCCEEDED(api.syncAddress(hwnd, imageSnapshot)),
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
            LR"({"version":1,"enabled":true,"session":"native-test","revision":1,"text":"initial","rows":[{"id":"a","title":"bad","image":{"width":105,"height":1,"pixels":"00"}}]})")),
        "reject oversized image before mutation");
    Require(list.Items().GetAt(0) == originalRow && image.Source(), "malformed image leaves controls intact");
    const wchar_t* modesSnapshot =
        LR"({"version":1,"enabled":true,"session":"native-test","revision":1,"sequence":3,"text":"initial","rows":[{"id":"a","title":"First suggestion"},{"id":"b","title":"Second suggestion"}],"modes":{"generation":1,"applied":0,"active":false,"label":"Test engine","entries":[{"id":"engine","label":"Test engine","local":false,"symbol":"search","image":{"width":1,"height":1,"pixels":"0000ffff"}},{"id":"local","label":"History","local":true,"symbol":"history"},{"id":"settings","label":"Settings","local":true,"symbol":"settings"}]}})";
    Require(SUCCEEDED(api.syncAddress(hwnd, modesSnapshot)),
            "accept native mode model");
    auto modeButton =
        address.Children().GetAt(0).as<Controls::Grid>().Children().GetAt(0).as<Controls::DropDownButton>();
    auto hiddenMode = winrt::Windows::Data::Json::JsonObject::Parse(modesSnapshot);
    hiddenMode.SetNamedValue(L"modeButtonVisible", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(false));
    Require(SUCCEEDED(api.syncAddress(hwnd, hiddenMode.Stringify().c_str())) &&
                modeButton.Visibility() == Visibility::Collapsed && field.Text() == L"initial",
            "Firefox hides the button without changing the address");
    Require(SUCCEEDED(api.syncAddress(hwnd, modesSnapshot)) &&
                modeButton.Visibility() == Visibility::Visible,
            "Firefox can restore the native search button");
    auto modeFlyout = modeButton.Flyout().as<Controls::MenuFlyout>();
    Require(modeButton.IsEnabled() && modeFlyout.Items().Size() == 5 &&
                modeFlyout.Items().GetAt(0).as<Controls::MenuFlyoutItem>().Icon().try_as<Controls::ImageIcon>(),
            "real dropdown, menu separators and decoded engine icon");
    Require(
        FAILED(api.syncAddress(
            hwnd,
            LR"({"version":1,"enabled":true,"session":"native-test","revision":1,"rows":[],"modes":{"generation":1,"active":false,"label":"Test","entries":[]}})")),
        "reject changed menu identities without new generation");
    const wchar_t* globalSnapshot =
        LR"({"version":1,"enabled":true,"session":"native-test","revision":1,"sequence":4,"text":"initial","rows":[{"id":"a","title":"First suggestion"}],"globalActions":[{"id":"global:action:0","resultId":"global","title":"Search with Fixture","hint":"Press Tab to select:","image":{"width":1,"height":1,"pixels":"0000ffff"}}]})";
    Require(SUCCEEDED(api.syncAddress(hwnd, globalSnapshot)),
            "accept separate global action");
    auto group = address.Children().GetAt(2).as<Controls::StackPanel>();
    auto actionButton = group.Children().GetAt(0).as<Controls::StackPanel>().Children().GetAt(1).as<Controls::Button>();
    Require(list.Items().Size() == 1 && actionButton.Content() &&
                Automation::AutomationProperties::GetName(actionButton) == L"Search with Fixture",
            "global action is a labeled Button outside the ListView");
    Require(SUCCEEDED(api.syncAddress(hwnd, globalSnapshot)) &&
                group.Children().GetAt(0).as<Controls::StackPanel>().Children().GetAt(1) == actionButton,
            "global action updates preserve button identity");
    field.Text(L"changed");
    Pump();
    // A publication produced before the edit predates it, so while the edit is
    // still outstanding it must not be applied -- otherwise the field snaps
    // back to the text the browser knew about when it published.
    Require(SUCCEEDED(api.syncAddress(hwnd, snapshot)),
            "an older publication is accepted while an edit is outstanding");
    Require(field.Text() == L"changed", "old snapshot does not overwrite an outstanding edit");
    // Since the address intents joined the request path, the edit wakes the
    // host itself rather than waiting to be collected by the next publication.
    auto editRequest = NextRequest(api, hwnd, 6, L"edit");
    Require(editRequest.Size() != 0, "native edit reaches the request queue");
    auto edit = editRequest.GetNamedObject(L"action");
    Require(edit.GetNamedString(L"text") == L"changed" &&
                edit.GetNamedString(L"session") == L"native-test",
            "native edit is copied and session scoped");
    // Once the host has answered it, publications apply again: the browser is
    // now the authority on what the field should say.
    Require(SUCCEEDED(api.syncAddress(hwnd, snapshot)),
            "publications apply again once the edit has been answered");
    Require(field.Text() == L"initial", "an answered edit stops holding off publications");
    // Fill the real pane queue with non-coalescing UIA button invocations.
    // A rejected edit must survive until completion frees queue capacity.
    Require(SUCCEEDED(api.syncAddress(hwnd, modesSnapshot)), "restore modes before queue saturation");
    NextRequest(api, hwnd, 6, nullptr, 2);
    field.Text(L"previous admitted edit");
    // TextChanged is asynchronous. Tab navigation can leave a chain of
    // deferred dispatch work before it. Sleep(10) between pump passes delays
    // each link, accumulating seconds even when new messages are ready.
    // Wait for messages as well as the deadline; keep waiting for this exact
    // edit rather than treating an unrelated notification as delivery.
    winrt::Windows::Data::Json::JsonObject heldEdit;
    const auto editDeadline = GetTickCount64() + 10000;
    while (!heldEdit.Size() && GetTickCount64() < editDeadline) {
        Pump();
        auto batch = DrainRequests(api, hwnd);
        if (batch.Size()) {
            for (auto value : batch.GetNamedArray(L"requests")) {
                auto request = value.GetObject();
                if (request.GetNamedNumber(L"kind") != 6) continue;
                auto action = request.GetNamedObject(L"action");
                if (action.GetNamedString(L"kind") == L"edit" &&
                    action.GetNamedString(L"text") == L"previous admitted edit") heldEdit = batch;
            }
            if (!heldEdit.Size()) CompleteRequests(api, hwnd, batch);
        }
        if (!heldEdit.Size()) {
            Require(MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE) != WAIT_FAILED,
                    "wait for address edit messages");
        }
    }
    Require(heldEdit.Size() != 0, "receive and hold the previous admitted edit before filling the queue (10 second deadline)");
    auto newTab = content.as<Controls::Grid>().Children().GetAt(2).as<Controls::Button>();
    auto invoke = Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(newTab)
                      .GetPattern(Automation::Peers::PatternInterface::Invoke)
                      .as<Automation::Provider::IInvokeProvider>();
    for (int i = 0; i < 64; ++i) invoke.Invoke();
    Pump();
    auto modeInvoke = Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(
                          modeButton.Flyout().as<Controls::MenuFlyout>().Items().GetAt(0))
                          .GetPattern(Automation::Peers::PatternInterface::Invoke)
                          .as<Automation::Provider::IInvokeProvider>();
    modeInvoke.Invoke();
    Pump();
    field.Text(L"edit while full");
    Pump();
    Require(SUCCEEDED(api.syncAddress(hwnd, snapshot)), "publish while edit admission is refused");
    Require(field.Text() == L"edit while full", "queue refusal cannot overwrite visible input");
    CompleteRequests(api, hwnd, heldEdit);
    Require(SUCCEEDED(api.syncAddress(hwnd, snapshot)), "publish after the previous edit completes");
    Require(field.Text() == L"edit while full", "old completion preserves the newer unadmitted edit");
    auto firstFullBatch = DrainRequests(api, hwnd);
    Require(firstFullBatch.Size() && firstFullBatch.GetNamedArray(L"requests").Size() == 16, "full queue yields a bounded batch");
    for (auto value : firstFullBatch.GetNamedArray(L"requests"))
        Require(value.GetObject().GetNamedNumber(L"kind") == 4, "queue was filled by actual new-tab actions");
    CompleteRequests(api, hwnd, firstFullBatch);
    auto retried = NextRequest(api, hwnd, 6, L"edit");
    Require(retried.Size() && retried.GetNamedObject(L"action").GetNamedString(L"text") == L"edit while full",
            "completion retries an unadmitted edit without requiring another keystroke or snapshot");
    Require(!NextRequest(api, hwnd, 6, L"edit", 3).Size(), "recovered edit is delivered once");
    Require(!NextRequest(api, hwnd, 7, nullptr, 3).Size(), "refused mode action is not replayed");
    // UIA menu invocation can also produce delayed caret notifications. Answer
    // those legitimate requests before testing whether publication is stuck.
    for (int i = 0; i < 10 && field.Text() != L"initial"; ++i) {
        auto batch = DrainRequests(api, hwnd);
        if (batch.Size()) CompleteRequests(api, hwnd, batch);
        Require(SUCCEEDED(api.syncAddress(hwnd, snapshot)), "publish after admission recovery");
        Pump();
    }
    Require(field.Text() == L"initial", "rejected mode and recovered edit leave no phantom staging slot");
    for (wchar_t character : {L'a', L'"', wchar_t(1)}) {
        std::wstring text(32768, character);
        field.Text(text);
        Pump();
        Require(field.Text() == text, "real TextBox retains the maximum-length input");
        Require(SUCCEEDED(api.syncAddress(hwnd, snapshot)), "publish while maximum edit is outstanding");
        Require(field.Text() == text, "old publication preserves maximum-length input");
        auto request = NextRequest(api, hwnd, 6, L"edit");
        Require(request.Size(), "maximum-length native edit reaches the queue");
        Require(request.GetNamedObject(L"action").GetNamedString(L"text") == text,
                "maximum-length edit survives JSON encoding and the drain buffer intact");
        Require(SUCCEEDED(api.syncAddress(hwnd, snapshot)), "publish after maximum edit completion");
        Require(field.Text() == L"initial", "maximum-length completion releases staged state");
    }
    for (int i = 0; i < 64; ++i) invoke.Invoke();
    Pump();
    field.Text(L"cancelled before admission");
    Pump();
    Require(
        SUCCEEDED(api.syncAddress(hwnd, LR"({"version":1,"enabled":false})")),
        "disable address controls with an unadmitted edit");
    Require(!NextRequest(api, hwnd, 6, L"edit", 8).Size(), "disabling cancels the unadmitted edit");
    Require(address.Visibility() == Visibility::Collapsed && list.Items().Size() == 0,
            "disable removes native suggestions");
    puts("Native address: real controls, bounded JSON, copied edit, feedback suppression and disable passed");
}

static void CheckTabStrip(HMODULE module, WinUIPaneApi& api, HWND host) {
    using namespace winrt;
    using namespace winrt::Microsoft::UI::Xaml;
    RECT rect{0, 0, 600, 40};
    const auto stripResult = api.updateTabStrip(host, &rect, TRUE);
    if (FAILED(stripResult)) fwprintf(stderr, L"Strip creation HRESULT: %08x\n", unsigned(stripResult));
    Require(SUCCEEDED(stripResult), "create strip presentation");
    ShowWindow(host, SW_SHOW);
    auto state = MenuState({11, 22});
    auto bindings = state.GetNamedObject(L"state").GetNamedArray(L"tabs");
    for (uint32_t i = 0; i < 2; ++i)
        bindings.GetObjectAt(i).SetNamedValue(L"selected",
                                              winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(i == 0));
    MenuPublish(api, host, state.Stringify().c_str());
    WinUITab tabs[]{{11, L"Same title"}, {22, L"Same title"}};
    uint32_t selected{};
    auto sync = [&] {
        Require(SUCCEEDED(api.syncTabs(host, tabs, 2, 11, &selected)),
                "sync strip snapshot");
    };
    sync();
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
    UIElement content{nullptr};
    Require(SUCCEEDED(inspect(host, put_abi(content))), "inspect strip");
    auto root = content.as<Controls::Grid>();
    auto view = root.Children().GetAt(0).as<Controls::TabView>();
    auto groupIndicator = root.Children().GetAt(2).as<Controls::Border>();
    Require(groupIndicator.Visibility() == Visibility::Collapsed && !groupIndicator.IsHitTestVisible() &&
            groupIndicator.BorderThickness().Left == 2 && groupIndicator.BorderBrush(),
            "group dwell visual starts hidden, themed and does not intercept input");
    auto first = view.TabItems().GetAt(0).as<Controls::TabViewItem>();
    auto second = view.TabItems().GetAt(1).as<Controls::TabViewItem>();
    for (int i = 0; i < 300 && (!first.IsLoaded() || first.ActualWidth() == 0); ++i) {
        Pump();
        Sleep(10);
    }
    Require(first.IsLoaded() && first.ActualWidth() > 0 && first.ActualHeight() == 32,
            "stock tab header fits reserved row");
    Require(view.ActualHeight() == 40 && root.ActualHeight() == view.ActualHeight(),
            "reservation fits the unchanged stock TabView without an empty bottom band");
    Require(root.Children().Size() == 6 && !first.Content() && !second.Content(),
            "strip has no address or browser content");
    Require(root.ColumnDefinitions().Size() == 3 && root.ColumnDefinitions().GetAt(2).Width().Value > 0,
            "system caption buttons reserve their SDK inset");
    Require(view.ActualWidth() + root.ColumnDefinitions().GetAt(0).Width().Value +
                root.ColumnDefinitions().GetAt(2).Width().Value <= root.ActualWidth() + 1,
            "tabs do not overlap the system caption inset");
    auto systemCaption = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(
        winrt::Microsoft::UI::GetWindowIdFromWindow(host)).TitleBar();
    Require(systemCaption.ExtendsContentIntoTitleBar() &&
                systemCaption.PreferredHeightOption() == winrt::Microsoft::UI::Windowing::TitleBarHeightOption::Standard &&
                GetPropW(host, WINUI_PANE_SYSTEM_CAPTION_PROPERTY),
            "Windows owns the visible native strip caption");
    Require(systemCaption.Height() <= root.ActualHeight() * root.XamlRoot().RasterizationScale(),
            "system caption controls stay inside the tab strip reservation");
    for (int height : {32, 48, 40}) {
        rect.bottom = height;
        Require(SUCCEEDED(api.updateTabStrip(host, &rect, TRUE)), "change strip height");
        const auto expected = height == 48 ? winrt::Microsoft::UI::Windowing::TitleBarHeightOption::Tall
                                          : winrt::Microsoft::UI::Windowing::TitleBarHeightOption::Standard;
        for (int i = 0; i < 100; ++i) {
            Pump();
            if (view.ActualHeight() == height && systemCaption.PreferredHeightOption() == expected) break;
            Sleep(10);
        }
        Require(root.ActualHeight() == height && view.ActualHeight() == height && first.ActualHeight() == 32,
                "all strip layouts preserve tab height without a bottom band");
        Require(systemCaption.PreferredHeightOption() == expected, "caption size follows strip layout");
        for (uint32_t index : {3u, 4u}) {
            auto line = root.Children().GetAt(index).as<Controls::Border>();
            Require(line.Visibility() == (height == 40 ? Visibility::Visible : Visibility::Collapsed) &&
                        !line.IsHitTestVisible() && line.Height() == 1 && line.Background(),
                    "caption inset bottom border is decorative and only visible in the 40-DIP layout");
        }
        Require(view.TabItems().GetAt(0) == first && view.SelectedItem() == first,
                "height changes preserve tab identity and selection");
    }
    auto mergePreview = root.Children().GetAt(5).as<Controls::Border>();
    Require(!mergePreview.IsHitTestVisible() && mergePreview.Visibility() == Visibility::Collapsed,
            "merge feedback is hidden and cannot capture input while idle");
    Require(first != second && view.SelectedItem() == first, "duplicate titles preserve identity and active tab");
    view.SelectedItem(second);
    auto selectionRequest = NextRequest(api, host, 1);
    auto event = selectionRequest.Size() ? selectionRequest.GetNamedObject(L"action")
                                         : winrt::Windows::Data::Json::JsonObject();
    Require(event.Size() && event.GetNamedString(L"kind") == L"selection" && event.GetNamedNumber(L"tab") == 22,
            "TabView selection queues browser intent on the request path");
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
    Require(SUCCEEDED(api.syncTabs(host, crowded, 4, 11, &selected)),
            "four-tab sizing snapshot");
    settleWidths();
    fprintf(stderr, "Four width actual %.1f Width %.1f Min %.1f Max %.1f align %d\n", first.ActualWidth(),
            first.Width(), first.MinWidth(), first.MaxWidth(), int(first.HorizontalAlignment()));
    Require(first.ActualWidth() < spaciousWidth - 20 && first.ActualWidth() >= 99, "tabs shrink as the strip fills");
    Require(SUCCEEDED(api.syncTabs(host, crowded, 12, 11, &selected)),
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
    // Since ABI v12 a completed gesture is a request on the shared queue; it
    // wakes the host itself rather than waiting to be read back by the next
    // publication.
    auto reorderToFront = NextRequest(api, host, 5, L"strip-reorder");
    Require(reorderToFront.Size(), "completed drag reaches the request queue");
    auto frontAction = reorderToFront.GetNamedObject(L"action");
    Require(frontAction.GetNamedNumber(L"id") == 22 && frontAction.GetNamedNumber(L"beforeId") == 11,
            "completed drag queues a validated request naming the real neighbor");
    Require(SUCCEEDED(completeReorder(host, 11)), "drive completed-drag logic for the tab now trailing");
    auto reorderToEnd = NextRequest(api, host, 5, L"strip-reorder").GetNamedObject(L"action");
    Require(reorderToEnd.Size() && reorderToEnd.GetNamedNumber(L"id") == 11 &&
                reorderToEnd.GetNamedNumber(L"beforeId") == 0,
            "a tab with no following row requests beforeId 0 (move to end)");
    Require(SUCCEEDED(completeReorder(host, 22)), "queue a reorder request alongside a selection");
    view.SelectedItem(second);
    // Both now travel on the one queue, and the order the user produced them
    // in is the order Firefox sees: the gesture that moved the tab, then the
    // selection that followed it. There is no second transport for either to
    // overtake the other on, and neither appears twice.
    std::vector<uint32_t> observed;
    for (int i = 0; i < 40 && observed.size() < 2; ++i) {
        auto batch = DrainRequests(api, host);
        if (batch.Size()) {
            for (auto value : batch.GetNamedArray(L"requests")) {
                auto kind = uint32_t(value.GetObject().GetNamedNumber(L"kind"));
                if (kind == 1 || kind == 5) observed.push_back(kind);
            }
            CompleteRequests(api, host, batch);
        }
        Pump();
        Sleep(5);
    }
    Require(observed == std::vector<uint32_t>({5, 1}),
            "the gesture and the selection that followed it drain once each, in order");
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
    MenuPublish(api, host, richer);
    sync();
    auto header = second.Header().as<Controls::Grid>();
    Require(header.Children().GetAt(0).as<Controls::Grid>().Children().GetAt(1).as<Controls::ProgressRing>().IsActive(),
            "Firefox loading state drives native progress");
    Require(header.Children().GetAt(3).as<Controls::Viewbox>().Child().as<Controls::SymbolIcon>().Symbol() ==
                Controls::Symbol::Mute,
            "Firefox mute state drives native icon");
    Require(first.Header().as<Controls::Grid>().Children().GetAt(1).Visibility() == Visibility::Visible,
            "pinned indicator visible");
    Require(header.Children().GetAt(2).as<Controls::StackPanel>().Children().Size() == 1,
        "tab face contains only its title, without a group/split subtitle");
    Require(std::wstring(unbox_value<hstring>(Controls::ToolTipService::GetToolTip(second))).find(L"Work · Split 1 right") != std::wstring::npos,
        "group/split detail remains in the native hover tooltip");
    Require(std::wstring(Automation::AutomationProperties::GetHelpText(second)).find(L"Work · Split 1 right") != std::wstring::npos,
        "group/split detail remains accessible");
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
    MenuPublish(api, host, notBusyNoIcon.Stringify().c_str());
    sync();
    Require(secondFavicon.Visibility() == Visibility::Collapsed,
            "once loading ends, a tab with no favicon shows no icon at all");
    auto oversizedImage = winrt::Windows::Data::Json::JsonObject::Parse(richer);
    auto oversizedModel = oversizedImage.GetNamedObject(L"strip");
    oversizedModel.SetNamedValue(L"revision", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(3));
    oversizedModel.GetNamedArray(L"rows").GetObjectAt(0).GetNamedObject(L"image").SetNamedValue(
        L"width", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(1000));
    Require(FAILED(api.syncTabMenu(host, oversizedImage.Stringify().c_str())),
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
    // Since ABI v12 the toggle is a request on the shared queue, not something
    // read back from the next publication: it wakes the host on its own.
    auto groupRequest = NextRequest(api, host, 5, L"strip-group");
    Require(groupRequest.Size(), "group toggle reaches the request queue");
    auto groupAction = groupRequest.GetNamedObject(L"action");
    Require(groupAction.GetNamedNumber(L"group") == 7 && groupAction.GetNamedBoolean(L"collapsed"),
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
    Require(FAILED(api.syncTabMenu(host, bad.Stringify().c_str())),
            "duplicate presentation IDs rejected before native mutation");
    bad.SetNamedValue(L"version", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(99));
    Require(FAILED(api.syncTabMenu(host, bad.Stringify().c_str())),
            "invalid outer strip envelope rejected");
    bad.SetNamedValue(L"version", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(2));
    sync();
    Require(second.Header() == header, "metadata updates retain header identity");
    // The metadata delta must update immediately, without a full tab Sync,
    // and omission must retain the original decoded Image and group flyout.
    auto firstIcon = first.Header().as<Controls::Grid>().Children().GetAt(0).as<Controls::Grid>()
                         .Children().GetAt(0).as<Controls::Image>();
    auto retainedImage = firstIcon.Source();
    auto retainedFlyout = groups.Flyout();
    auto metadataDelta = LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":2,"revision":3,"containerDot":true,"rows":[{"id":11,"flags":3,"containerName":"Personal","containerColor":4281839103}],"removed":[]}})";
    MenuPublish(api, host, metadataDelta);
    Require(firstIcon.Source() == retainedImage && retainedImage, "an omitted image preserves the decoded source");
    Require(groups.Flyout() == retainedFlyout, "a row delta preserves group controls");
    Require(first.Header().as<Controls::Grid>().Children().GetAt(0).as<Controls::Grid>()
                .Children().GetAt(1).as<Controls::ProgressRing>().IsActive(), "metadata delta applies without full Sync");
    MenuPublish(api, host, metadataDelta);  // Identical retry is harmless.
    auto container = first.Header().as<Controls::Grid>().Children().GetAt(4).as<Controls::Border>();
    Require(container.Visibility() == Visibility::Visible &&
                container.Background().as<Media::SolidColorBrush>().Color() == winrt::Windows::UI::Color{255, 55, 173, 255},
            "container delta paints the authoritative Firefox color");
    Require(std::wstring_view(Automation::AutomationProperties::GetHelpText(first)).find(L"Personal") != std::wstring_view::npos,
            "container name is accessible independently of color");
    for (auto invalid : {
        LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":3,"revision":4,"rows":[{"id":11,"containerColor":-1}],"removed":[]}})",
        LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":3,"revision":4,"rows":[{"id":11,"flags":"bad"}],"removed":[]}})",
        LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":3,"revision":4,"rows":[{"id":11,"detail":null}],"removed":[]}})",
        LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":2,"revision":4,"rows":[],"removed":[]}})",
        LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":3,"revision":4,"rows":[{"id":11,"flags":0},{"id":999,"flags":0}],"removed":[]}})",
        LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":3,"revision":4,"rows":[{"id":11,"flags":0},{"id":11,"flags":1}],"removed":[]}})",
        LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":3,"revision":4,"rows":[],"removed":[999]}})"}) {
        Require(FAILED(api.syncTabMenu(host, invalid)), "invalid delta is rejected atomically");
        Require(firstIcon.Source() == retainedImage, "rejected delta preserves the image");
        Require(first.Header().as<Controls::Grid>().Children().GetAt(0).as<Controls::Grid>()
                    .Children().GetAt(1).as<Controls::ProgressRing>().IsActive(), "rejected delta preserves prior flags");
    }
    MenuPublish(api, host, LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":3,"revision":4,"rows":[{"id":11,"image":null,"containerName":"","containerColor":0}],"removed":[]}})");
    Require(container.Visibility() == Visibility::Collapsed, "cleared container removes its indicator in place");
    Require(!firstIcon.Source(), "explicit null clears a retained image");
    for (auto theme : {ElementTheme::Light, ElementTheme::Dark, ElementTheme::Light}) {
        root.RequestedTheme(theme);
        Pump();
        Require(root.ActualTheme() == theme, "strip follows live theme");
        Require(root.Background().as<Media::SolidColorBrush>().Color().A == 0 &&
                    systemCaption.ButtonBackgroundColor().Value().A == 0 &&
                    systemCaption.ButtonInactiveBackgroundColor().Value().A == 0,
                "strip and caption backgrounds reveal the island backdrop across theme changes");
        const auto captionTheme = theme == ElementTheme::Dark
            ? winrt::Microsoft::UI::Windowing::TitleBarTheme::Dark : winrt::Microsoft::UI::Windowing::TitleBarTheme::Light;
        for (int i = 0; i < 100 && systemCaption.PreferredTheme() != captionTheme; ++i) {
            Pump();
            Sleep(10);
        }
        if (systemCaption.PreferredTheme() != captionTheme)
            fprintf(stderr, "Caption theme mismatch: root=%d requested=%d caption=%d expected=%d loaded=%d\n",
                int(root.ActualTheme()), int(theme), int(systemCaption.PreferredTheme()), int(captionTheme), root.IsLoaded());
        Require(systemCaption.PreferredTheme() == captionTheme,
                "Windows caption follows live strip theme without overriding hover colors");
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
    // A strip row's menu takes the same on-demand path as the sidebar's: it
    // opens on a non-actionable pending row and asks for that one tab's
    // description, which is what fills it.
    Require(opened && flyout.Items().Size() == 1 && !flyout.Items().GetAt(0).IsEnabled(),
            "strip opening shows a non-actionable pending state");
    {
        auto request = NextRequest(api, host, 2, L"open");
        Require(request.Size(), "strip opening asks for this tab's description");
        auto action = request.GetNamedObject(L"action");
        MenuPublish(api, host,
                     MenuDescription(action.GetNamedNumber(L"session"), uint32_t(action.GetNamedNumber(L"tab")),
                                     action.GetNamedNumber(L"revision"))
                         .Stringify()
                         .c_str());
        for (int i = 0; i < 100 && flyout.Items().Size() != 3; ++i) {
            Pump();
            Sleep(10);
        }
        Require(flyout.Items().Size() == 3, "the description fills the strip row's menu in place");
    }
    flyout.Hide();
    for (int i = 0; i < 500 && second.ContextFlyout() == flyout; ++i) {
        Pump();
        Sleep(10);
    }
    Require(second.ContextFlyout() != flyout, "strip opening gets fresh flyout");
    model.GetNamedArray(L"rows").RemoveAtEnd();
    model.SetNamedValue(L"revision", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(5));
    model.SetNamedValue(L"rtl", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(true));
    MenuPublish(api, host, bad.Stringify().c_str());
    sync();
    Require(root.FlowDirection() == FlowDirection::RightToLeft, "native layout uses Firefox text direction");
    Require(SUCCEEDED(api.updateTabStrip(host, &rect, FALSE)), "hide strip");
    Require(!GetPropW(host, WINUI_PANE_SYSTEM_CAPTION_PROPERTY), "hidden strip releases caption ownership");
    Require(SUCCEEDED(api.updateTabStrip(host, &rect, TRUE)), "restore strip");
    Require(GetPropW(host, WINUI_PANE_SYSTEM_CAPTION_PROPERTY) != nullptr, "shown strip reacquires caption ownership");
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

static void CheckInlineGroups(HMODULE module, WinUIPaneApi& api, HWND host, bool checkMotion) {
    using namespace winrt;
    using namespace winrt::Microsoft::UI::Xaml;
    RECT rect{0, 0, 1200, 40};
    SetWindowPos(host, nullptr, 0, 0, 1200, 300, SWP_NOMOVE | SWP_NOZORDER);
    Require(SUCCEEDED(api.updateTabStrip(host, &rect, TRUE)), "create inline strip");
    ShowWindow(host, SW_SHOW);
    SetForegroundWindow(host);
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
    UIElement content{nullptr};
    Require(SUCCEEDED(inspect(host,put_abi(content))), "inspect inline strip");
    auto root = content.as<Controls::Grid>();
    auto view = root.Children().GetAt(0).as<Controls::TabView>();
    // Finish island activation before inserting the grouped source.
    AwaitLiveTree(root);
    for (int i=0;i<20;++i) { Pump(); Sleep(10); }
    MenuPublish(api, host, MenuState({41, 42}).Stringify().c_str());
    auto full = LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":0,"revision":1,"rtl":false,"groupsLabel":"Groups","groups":[{"id":7,"name":"Research","label":"Collapse Research","collapsed":false,"color":4281558681},{"id":9,"name":"Empty","label":"Expand Empty","collapsed":true}],"rows":[{"id":41,"detail":"","flags":0},{"id":42,"detail":"","flags":0}],"layout":[{"group":0,"tab":41},{"group":7,"tab":0},{"group":7,"tab":42},{"group":9,"tab":0}]}})";
    MenuPublish(api, host, full);
    WinUITab tabs[]{{41,L"Home"},{42,L"Research page"}};
    uint32_t selected{};
    Require(SUCCEEDED(api.syncTabs(host,tabs,2,41,&selected)), "sync grouped tabs");
    auto settle = [&] { for (int i=0;i<20;++i) { Pump(); Sleep(10); } root.UpdateLayout(); };
    std::function<Controls::Button(DependencyObject const&, hstring const&)> find;
    find = [&](DependencyObject const& node, hstring const& id) -> Controls::Button {
        if (auto button = node.try_as<Controls::Button>(); button &&
            (Automation::AutomationProperties::GetAutomationId(button) == id || button.Name() == id)) return button;
        for (int i=0;i<Media::VisualTreeHelper::GetChildrenCount(node);++i)
            if (auto found = find(Media::VisualTreeHelper::GetChild(node,i),id)) return found;
        return nullptr;
    };
    for (int i=0;i<1500 && !root.IsLoaded();++i) { Pump(); Sleep(10); }
    Require(root.IsLoaded(), "recreated inline island has loaded");
    for (int i=0;i<500 && !find(view,L"FirefoxWinUIInlineGroup-9");++i) { Pump(); Sleep(10); }
    settle();
    auto header = find(view,L"FirefoxWinUIInlineGroup-7");
    auto ambientLayer = root.Children().GetAt(1).as<Controls::Canvas>();
    Require(!ambientLayer.IsHitTestVisible() && Automation::AutomationProperties::GetAccessibilityView(ambientLayer) ==
            Automation::Peers::AccessibilityView::Raw, "ambient decoration cannot capture input or enter the control tree");
    auto ambientOutput = Hosting::ElementCompositionPreview::GetElementChildVisual(ambientLayer)
        .as<winrt::Microsoft::UI::Composition::SpriteVisual>();
    auto ambientMask = ambientOutput.Brush().as<winrt::Microsoft::UI::Composition::CompositionMaskBrush>();
    auto occludedSurface = ambientMask.Source().as<winrt::Microsoft::UI::Composition::CompositionSurfaceBrush>()
        .Surface().as<winrt::Microsoft::UI::Composition::CompositionVisualSurface>();
    auto occluded = occludedSurface.SourceVisual().as<winrt::Microsoft::UI::Composition::SpriteVisual>();
    auto faceMaskBrush = occluded.Brush().as<winrt::Microsoft::UI::Composition::CompositionMaskBrush>();
    auto faceSurface = faceMaskBrush.Mask().as<winrt::Microsoft::UI::Composition::CompositionSurfaceBrush>()
        .Surface().as<winrt::Microsoft::UI::Composition::CompositionVisualSurface>();
    auto faceRoot = faceSurface.SourceVisual().as<winrt::Microsoft::UI::Composition::ContainerVisual>();
    Require(faceRoot.Comment() == L"tab-face-occlusion" && faceSurface.SourceSize().x > 0,
        "face occlusion uses a bounded Composition surface after diffusion");
    auto ambientSurface = faceMaskBrush.Source().as<winrt::Microsoft::UI::Composition::CompositionSurfaceBrush>()
        .Surface().as<winrt::Microsoft::UI::Composition::CompositionVisualSurface>();
    auto ambient = ambientSurface.SourceVisual().as<winrt::Microsoft::UI::Composition::ContainerVisual>();
    auto edgeFade = ambientMask.Mask().as<winrt::Microsoft::UI::Composition::CompositionLinearGradientBrush>();
    Require(edgeFade.ColorStops().Size() == 4 && edgeFade.ColorStops().GetAt(1).Color().A == 255 &&
        edgeFade.ColorStops().GetAt(2).Color().A == 255, "post-diffusion edge mask preserves interior illumination");
    auto diffusion = ambient.as<winrt::Microsoft::UI::Composition::LayerVisual>();
    Require(diffusion.Effect() && diffusion.Size().x > 0 && diffusion.Size().y > 0 &&
        diffusion.Opacity() == .9f, "ambient-only diffusion has an active effect and viewport-sized surface");
    auto bottomGlow = ambient.Children().First().Current().as<winrt::Microsoft::UI::Composition::ContainerVisual>();
    auto upperIterator = ambient.Children().First(); upperIterator.MoveNext();
    auto topGlow = upperIterator.Current().as<winrt::Microsoft::UI::Composition::ContainerVisual>();
    printf("Ambient fields: bottom=%d top=%d live=%d\n", bottomGlow.Children().Count(), topGlow.Children().Count(), checkMotion);
    fflush(stdout);
    // The initial grouped-source insertion may not have realized its members
    // yet. Assert complete field geometry in the loaded motion fixture below.
    auto addButton = find(view,L"AddButton");
    Require(bool(addButton), "grouped strip has the stock add button");
    const auto groupedAddY = addButton.TransformToVisual(root).TransformPoint({0,0}).Y;
    std::map<uint32_t, float> groupedRowY;
    for (auto item : view.TabItems()) {
        auto row = item.as<Controls::TabViewItem>();
        groupedRowY[unbox_value<uint32_t>(row.Tag())] = row.TransformToVisual(root).TransformPoint({0,0}).Y;
    }
    std::function<void(DependencyObject)> checkHeaderFocus = [&](DependencyObject node) {
        if (auto header = node.try_as<Controls::ListViewHeaderItem>()) {
            printf("Group container: width=%.1f height=%.1f tabstop=%d content=%d\n",
                header.ActualWidth(), header.ActualHeight(), header.IsTabStop(), bool(header.Content()));
            Require(!header.IsTabStop(), "group containers cannot take focus away from actual disclosure buttons or tabs");
            Require(header.ActualHeight() <= 32, "group header cannot inflate the tab row");
            auto model = header.Content().as<winrt::Windows::Foundation::Collections::IObservableMap<hstring, winrt::Windows::Foundation::IInspectable>>();
            if (!model.Lookup(L"Header")) Require(header.ActualWidth() == 24, "retain the original horizontal gap");
        }
        for (int child = 0; child < Media::VisualTreeHelper::GetChildrenCount(node); ++child)
            checkHeaderFocus(Media::VisualTreeHelper::GetChild(node, child));
    };
    checkHeaderFocus(view);
    printf("Inline layout: items=%u width=%.1f height=%.1f header=%d empty=%d\n", view.TabItems().Size(),
           view.ActualWidth(), view.ActualHeight(), bool(header), bool(find(view,L"FirefoxWinUIInlineGroup-9")));
    fflush(stdout);
    Require(bool(header) && bool(find(view,L"FirefoxWinUIInlineGroup-9")), "inline and empty group headers exist");
    auto capsule = header.Content().as<Controls::Grid>();
    auto marker = capsule.Children().GetAt(0).as<Shapes::Rectangle>();
    Require(!capsule.IsHitTestVisible() && unbox_value<uint32_t>(header.Tag()) == 7,
        "whole group button is an identified input target for future dragging");
    Require(capsule.Children().GetAt(1).as<Controls::Grid>().Children().GetAt(0).as<Controls::FontIcon>().Glyph() == L"\uE76C", "expanded group arrow points toward its tabs");
    Require(marker.Visibility() == Visibility::Visible, "Firefox group color marker is visible");
    auto accent = header.Resources().ThemeDictionaries().Lookup(box_value(L"Default")).as<ResourceDictionary>()
        .Lookup(box_value(L"InlineGroupColorBrush")).as<Media::SolidColorBrush>();
    Require(accent.Color() == winrt::Windows::UI::Color{255,51,102,153}, "group accent comes from Firefox ARGB");
    auto first = view.TabItems().GetAt(0);
    for (auto theme : {ElementTheme::Light, ElementTheme::Dark, ElementTheme::Light}) {
        root.RequestedTheme(theme);
        settle();
        Require(find(view,L"FirefoxWinUIInlineGroup-7") == header, "theme retains inline header");
        Require(header.ActualHeight() > 0 && header.ActualHeight() <= 40 && view.ActualHeight() <= 40, "inline group fits reserved strip height");
        Require(header.ActualWidth() == 32 && header.ActualHeight() == 32, "capsule retains a compact 32-DIP input target");
        auto painted = marker.Fill().as<Media::SolidColorBrush>().Color();
        if (winrt::Windows::UI::ViewManagement::AccessibilitySettings().HighContrast()) {
            const auto system = GetSysColor(COLOR_WINDOWTEXT);
            Require(painted == winrt::Windows::UI::Color{255,GetRValue(system),GetGValue(system),GetBValue(system)}, "high contrast marker uses system text color");
        } else Require(painted == accent.Color(), "light/dark marker uses Firefox accent");
    }
    auto peer = Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(header);
    peer.GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
    auto request = NextRequest(api,host,5,L"strip-group");
    Require(request.Size() && request.GetNamedObject(L"action").GetNamedNumber(L"group") == 7, "inline header uses shared group command");
    auto invalid = LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":1,"revision":2,"rows":[],"removed":[42]}})";
    Require(FAILED(api.syncTabMenu(host,invalid)), "membership delta requires matching layout");
    for (double badColor : {-1.0, 4294967296.0, 1.5}) {
        auto bad = winrt::Windows::Data::Json::JsonObject::Parse(full);
        auto strip = bad.GetNamedObject(L"strip");
        strip.SetNamedValue(L"base", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(1));
        strip.SetNamedValue(L"revision", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(2));
        strip.GetNamedArray(L"groups").GetObjectAt(0).SetNamedValue(L"color", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(badColor));
        Require(FAILED(api.syncTabMenu(host,bad.Stringify().c_str())), "invalid group ARGB rejected atomically");
        Require(accent.Color() == winrt::Windows::UI::Color{255,51,102,153}, "invalid color leaves retained marker unchanged");
    }
    auto collapsed = LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":1,"revision":2,"rtl":true,"rows":[],"removed":[42],"groups":[{"id":7,"name":"Renamed","label":"Expand Renamed","collapsed":true},{"id":9,"name":"Empty","label":"Expand Empty","collapsed":true}],"layout":[{"group":0,"tab":41},{"group":7,"tab":0},{"group":9,"tab":0}]}})";
    auto collapseData = winrt::Windows::Data::Json::JsonObject::Parse(collapsed);
    collapseData.GetNamedObject(L"strip").GetNamedArray(L"groups").GetObjectAt(0).SetNamedValue(L"headerText",
        winrt::Windows::Data::Json::JsonValue::CreateStringValue(L"A deliberately long named group for trimming"));
    collapseData.GetNamedObject(L"strip").SetNamedValue(L"rtl", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(false));
    MenuPublish(api,host,collapseData.Stringify().c_str());
    Require(SUCCEEDED(api.syncTabs(host,tabs,1,41,&selected)), "sync hidden group member");
    settle();
    Require(marker.Visibility() == Visibility::Collapsed, "legacy group metadata without color hides marker");
    auto groupText = capsule.Children().GetAt(2).as<Controls::TextBlock>();
    Require(header.ActualWidth() > 32 && header.ActualWidth() <= 154 && groupText.Visibility() == Visibility::Visible &&
        groupText.Text() == L"A deliberately long named group for trimming" && groupText.TextTrimming() == TextTrimming::CharacterEllipsis &&
        capsule.Children().GetAt(1).Visibility() == Visibility::Visible &&
        capsule.Children().GetAt(1).as<Controls::Grid>().Children().GetAt(0).as<Controls::FontIcon>().Glyph() == L"\uE70D", "named group retains the arrow beside bounded text");
    for (auto direction : {FlowDirection::LeftToRight, FlowDirection::RightToLeft}) {
        root.FlowDirection(direction); settle();
        auto rotor=capsule.Children().GetAt(1).as<Controls::Grid>();
        const auto arrowBounds=rotor.TransformToVisual(nullptr).TransformBounds({0,0,float(rotor.ActualWidth()),float(rotor.ActualHeight())});
        const auto textBounds=groupText.TransformToVisual(nullptr).TransformBounds({0,0,float(groupText.ActualWidth()),float(groupText.ActualHeight())});
        Require(direction==FlowDirection::LeftToRight ? arrowBounds.X>=textBounds.X+textBounds.Width : arrowBounds.X+arrowBounds.Width<=textBounds.X,
            "named group arrow sits on the tab-facing side in LTR and RTL");
        Require(rotor.ActualWidth()==24 && groupText.ActualWidth()>0,"long name leaves room for the arrow");
    }
    Require(view.TabItems().Size() == 1 && view.TabItems().GetAt(0) == first && view.SelectedItem() == first, "collapse retains real active tab");
    Require(find(view,L"FirefoxWinUIInlineGroup-7") == header && Automation::AutomationProperties::GetName(header) == L"Expand Renamed", "empty renamed group retains its button");
    MenuPublish(api,host,LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":2,"revision":3,"rtl":true,"rows":[],"removed":[],"groups":[{"id":7,"name":"Unnamed group","headerText":"","label":"Expand unnamed group","collapsed":true},{"id":9,"name":"Empty","label":"Expand Empty","collapsed":true}]}})");
    settle();
    Require(find(view,L"FirefoxWinUIInlineGroup-7") == header && header.ActualWidth() == 32 &&
        groupText.Visibility() == Visibility::Collapsed && capsule.Children().GetAt(1).Visibility() == Visibility::Visible,
        "clearing the name restores the arrow on the retained button");
    Require(root.FlowDirection() == FlowDirection::RightToLeft, "inline groups inherit RTL");
    // The grouped source is also used for an entirely ungrouped strip. Moving
    // its first tab changes the run identity and reparents the retained rows.
    for (uint32_t revision = 4; revision <= 8; ++revision) {
        const bool reversed = revision % 2;
        auto publication = winrt::Windows::Data::Json::JsonObject::Parse(
            reversed
                ? LR"({"version":2,"publishOnly":true,"strip":{"version":2,"rtl":false,"groups":[],"rows":[{"id":42,"detail":"","flags":0},{"id":41,"detail":"","flags":0}],"layout":[{"group":0,"tab":42},{"group":0,"tab":41}]}})"
                : LR"({"version":2,"publishOnly":true,"strip":{"version":2,"rtl":false,"groups":[],"rows":[{"id":41,"detail":"","flags":0},{"id":42,"detail":"","flags":0}],"layout":[{"group":0,"tab":41},{"group":0,"tab":42}]}})");
        auto strip = publication.GetNamedObject(L"strip");
        strip.SetNamedValue(L"base", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(0));
        strip.SetNamedValue(L"groupsLabel", winrt::Windows::Data::Json::JsonValue::CreateStringValue(L"Groups"));
        strip.SetNamedValue(L"revision", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(revision));
        MenuPublish(api, host, publication.Stringify().c_str());
        WinUITab ordered[]{{reversed ? 42u : 41u,L"One"},{reversed ? 41u : 42u,L"Two"}};
        Require(SUCCEEDED(api.syncTabs(host,ordered,2,42,&selected)), "publish two-tab round trip");
        settle();
        Require(std::abs(addButton.TransformToVisual(root).TransformPoint({0,0}).Y - groupedAddY) < .5,
            "group presence cannot move the add button vertically");
        for (auto item : view.TabItems()) {
            auto row = item.as<Controls::TabViewItem>();
            Require(std::abs(row.TransformToVisual(root).TransformPoint({0,0}).Y - groupedRowY.at(unbox_value<uint32_t>(row.Tag()))) < .5,
                "group presence cannot move tabs vertically");
            const bool active = unbox_value<uint32_t>(row.Tag()) == 42;
            if (row.IsSelected() != active)
                fprintf(stderr, "Round trip revision=%u tab=%u selected=%d expected=%d index=%d\n",
                       revision, unbox_value<uint32_t>(row.Tag()), row.IsSelected(), active, view.SelectedIndex());
            Require((view.SelectedItem() == row) == active, "round trip retains TabView selection");
            Require(row.IsSelected() == active, "round trip retains the row's visual selection");
        }
    }
    if (checkMotion) {
        auto motion = reinterpret_cast<HRESULT(WINAPI*)(HWND,uint32_t,uint32_t*)>(GetProcAddress(module,"WinUIPaneTestGroupMotion"));
        Require(motion != nullptr, "group motion lifecycle hook exists");
        auto motionState = [&](uint32_t revision, bool collapsed) {
            auto state = winrt::Windows::Data::Json::JsonObject::Parse(LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":0,"revision":9,"rtl":false,"groupsLabel":"Groups","groups":[{"id":7,"name":"Research","label":"Research","collapsed":false}],"rows":[{"id":41,"detail":"","flags":0},{"id":42,"detail":"","flags":0}],"layout":[{"group":7,"tab":0},{"group":7,"tab":41},{"group":7,"tab":42}]}})");
            auto strip = state.GetNamedObject(L"strip");
            strip.SetNamedValue(L"revision", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(revision));
            if (collapsed) {
                strip.GetNamedArray(L"groups").GetObjectAt(0).SetNamedValue(L"collapsed", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(true));
                strip.GetNamedArray(L"rows").RemoveAt(0);
                strip.GetNamedArray(L"layout").RemoveAt(1);
            }
            MenuPublish(api,host,state.Stringify().c_str());
            Require(SUCCEEDED(api.syncTabs(host,tabs + (collapsed ? 1 : 0),collapsed ? 1 : 2,42,&selected)), "publish motion fixture");
        };
        motionState(9,false); settle();
        Require(view.TabItems().GetAt(0).as<Controls::TabViewItem>().IsLoaded(), "motion starts with loaded tab visuals");
        motionState(10,true);
        uint32_t ghosts{};
        Require(SUCCEEDED(motion(host,0,&ghosts)), "inspect outgoing visual lifetime");
        if (winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled())
            Require(ghosts == 1, "only the disappearing background tab gets a visual copy");
        Require(view.TabItems().Size() == 1 && unbox_value<uint32_t>(view.SelectedItem().as<Controls::TabViewItem>().Tag()) == 42,
            "animation preserves the real selected tab and excludes outgoing visuals from tab navigation");
        Require(SUCCEEDED(motion(host,1,&ghosts)) && ghosts == 0, "cancellation releases departing visuals");
        motionState(11,false); settle();
        Require(SUCCEEDED(motion(host,2,&ghosts)), "simulate reduced-motion preference");
        motionState(12,true);
        Require(SUCCEEDED(motion(host,0,&ghosts)) && ghosts == 0, "reduced motion installs final layout without visual copies");
        auto reducedArrow = find(view,L"FirefoxWinUIInlineGroup-7").Content().as<Controls::Grid>().Children().GetAt(1).as<Controls::Grid>();
        auto reducedIcon = reducedArrow.Children().GetAt(0).as<Controls::FontIcon>();
        Require(reducedIcon.Glyph() == L"\uE70D" && reducedArrow.RenderTransform().as<Media::RotateTransform>().Angle() == 0,
            "reduced motion installs the final arrow direction immediately");
        Require(SUCCEEDED(motion(host,3,&ghosts)), "restore normal motion policy");
        settle();
        // An active-only group can toggle without any tab-list publication.
        MenuPublish(api,host,LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":12,"revision":13,"rows":[],"removed":[],"groups":[{"id":7,"name":"Research","label":"Research","collapsed":false}]}})");
        bool turned = false;
        for (int i=0;i<12;++i) {
            Pump(); Sleep(5);
            const auto angle = reducedArrow.RenderTransform().as<Media::RotateTransform>().Angle();
            turned |= angle > 0 && angle < 90;
        }
        if (winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled())
            Require(turned, "active-only group arrow rotates without a tab-list change");
        settle();
        Require(reducedIcon.Glyph() == L"\uE76C" && reducedArrow.RenderTransform().as<Media::RotateTransform>().Angle() == 0,
            "arrow finishes pointing toward the expanded group");
        // Collapsing every member reparents the retained header. Its disclosure
        // animation must survive that transient Unloaded/Loaded pair.
        MenuPublish(api,host,LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":0,"revision":14,"rtl":false,"groupsLabel":"Groups","groups":[{"id":7,"name":"Research","headerText":"Research","label":"Research","collapsed":false}],"rows":[{"id":41,"detail":"","flags":0},{"id":42,"detail":"","flags":0}],"layout":[{"group":7,"tab":0},{"group":7,"tab":41},{"group":0,"tab":42}]}})");
        Require(SUCCEEDED(api.syncTabs(host,tabs,2,42,&selected)), "prepare empty-group rotation fixture");
        settle();
        MenuPublish(api,host,LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":0,"revision":15,"rtl":false,"groupsLabel":"Groups","groups":[{"id":7,"name":"Research","headerText":"Research","label":"Research","collapsed":true}],"rows":[{"id":42,"detail":"","flags":0}],"layout":[{"group":7,"tab":0},{"group":0,"tab":42}]}})");
        Require(SUCCEEDED(api.syncTabs(host,tabs+1,1,42,&selected)), "collapse every group member");
        turned=false;
        for (int i=0;i<12;++i) {
            Pump(); Sleep(5);
            auto button=find(view,L"FirefoxWinUIInlineGroup-7");
            if (!button || !button.IsLoaded()) continue;
            auto rotor=button.Content().as<Controls::Grid>().Children().GetAt(1).as<Controls::Grid>();
            const auto angle=rotor.RenderTransform().as<Media::RotateTransform>().Angle();
            turned |= angle<0 && angle>-90;
        }
        if (winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled())
            Require(turned,"empty-group arrow rotates down after header reattachment");
        settle();
        motionState(16,false); settle();
        auto dragged=view.TabItems().GetAt(0).as<Controls::TabViewItem>();
        auto draggedActive=view.TabItems().GetAt(1).as<Controls::TabViewItem>();
        auto borrowedContent=dragged.Header().as<UIElement>();
        auto activeContent=draggedActive.Header();
        const auto input=borrowedContent.IsHitTestVisible();
        const auto accessibility=Automation::AutomationProperties::GetAccessibilityView(borrowedContent);
        const auto automationId=Automation::AutomationProperties::GetAutomationId(borrowedContent);
        for (bool early : {true,false}) {
            Require(SUCCEEDED(motion(host,4,&ghosts)),"start temporary drag collapse with shared slide animation");
            Require(dragged.Visibility()==Visibility::Collapsed && draggedActive.Visibility()==Visibility::Collapsed,
                "drag collapse removes background and active rows from layout immediately");
            if (winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled()) {
                Require(ghosts==2 && !dragged.Header() && !draggedActive.Header(),"drag collapse slides both retained rows");
                Require(!borrowedContent.IsHitTestVisible() && Automation::AutomationProperties::GetAccessibilityView(borrowedContent)==Automation::Peers::AccessibilityView::Raw,
                    "borrowed drag visuals are excluded from input and accessibility");
            }
            if (!early) settle();
            Require(SUCCEEDED(motion(host,5,&ghosts)) && ghosts==0,"drop or cancellation clears drag-collapse visuals");
            settle();
            Require(dragged.Header()==borrowedContent && draggedActive.Header()==activeContent &&
                dragged.Visibility()==Visibility::Visible && draggedActive.Visibility()==Visibility::Visible,
                "animation completion and early cancellation restore exact tab content");
            Require(borrowedContent.IsHitTestVisible()==input && Automation::AutomationProperties::GetAccessibilityView(borrowedContent)==accessibility &&
                Automation::AutomationProperties::GetAutomationId(borrowedContent)==automationId,
                "restored tabs retain their input and automation properties");
            Require(view.SelectedItem()==draggedActive,"drag animation preserves active tab selection");
        }
        Require(SUCCEEDED(motion(host,4,&ghosts)),"start slide before incremental title update");
        Require(SUCCEEDED(motion(host,6,&ghosts)) && ghosts==0 && dragged.Header()==borrowedContent,
            "incremental title update returns borrowed content before editing it");
        Require(Automation::AutomationProperties::GetName(dragged)==L"Updated during drag",
            "title updates remain live during temporary collapse");
        Require(SUCCEEDED(motion(host,5,&ghosts)),"restore group after incremental update");
        settle();
        Require(SUCCEEDED(motion(host,2,&ghosts)),"disable temporary drag motion");
        Require(SUCCEEDED(motion(host,4,&ghosts)) && ghosts==0 && dragged.Header()==borrowedContent,
            "reduced-motion drag collapse keeps original content without visual copies");
        Require(SUCCEEDED(motion(host,5,&ghosts)),"restore reduced-motion drag collapse");
        Require(SUCCEEDED(motion(host,3,&ghosts)),"restore animation preference after drag checks");
        // Publish independent colors without changing group or tab membership.
        auto colored = winrt::Windows::Data::Json::JsonObject::Parse(LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":16,"revision":17,"rows":[{"id":41,"containerName":"Personal","containerColor":4289934591}],"removed":[],"groups":[{"id":7,"name":"Research","label":"Research","collapsed":false,"color":4281558681}]}})");
        MenuPublish(api,host,colored.Stringify().c_str()); settle();
        Require(bottomGlow.Children().Count() == 1 && topGlow.Children().Count() == 1,
            "two grouped tabs share one bottom field and a container adds an independent top field");
        Require(edgeFade.ColorStops().GetAt(0).Color().A == 0 || edgeFade.ColorStops().GetAt(3).Color().A == 0,
            "non-overflowing glow reaches zero alpha at the new-tab boundary");
        winrt::Microsoft::UI::Composition::ContainerVisual spill{nullptr};
        for (auto visual : ambient.Children()) if (visual.Comment() == L"ambient-color-spill")
            spill = visual.as<winrt::Microsoft::UI::Composition::ContainerVisual>();
        Require(spill && spill.Children().Count() == 1, "container corners do not add a central body cast");
        for (auto field : spill.Children()) {
            const bool isGroup = field.Comment() == L"group-spill:7";
            auto lobes = field.as<winrt::Microsoft::UI::Composition::SpriteVisual>();
            Require(lobes.Children().Count() == (isGroup ? 2 : 1),
                "group spill has overlapping lobes while container spill follows its own tab");
            for (auto lobe : lobes.Children()) {
                auto brush = lobe.as<winrt::Microsoft::UI::Composition::SpriteVisual>().Brush()
                    .as<winrt::Microsoft::UI::Composition::CompositionRadialGradientBrush>();
                Require(brush.EllipseRadius().y > 1 && (isGroup ? brush.EllipseCenter().y > 1 : brush.EllipseCenter().y < 0),
                    "weak casts reach across the tab from opposite edges");
                for (auto stop : brush.ColorStops()) Require(stop.Color().A <= 8, "body spill never becomes a strong tint");
            }
        }
        auto groupVisual = bottomGlow.Children().First().Current().as<winrt::Microsoft::UI::Composition::SpriteVisual>();
        auto containerVisual = topGlow.Children().First().Current().as<winrt::Microsoft::UI::Composition::SpriteVisual>();
        auto a = view.TabItems().GetAt(0).as<Controls::TabViewItem>();
        auto b = view.TabItems().GetAt(1).as<Controls::TabViewItem>();
        auto ar = a.TransformToVisual(ambientLayer).TransformBounds({0,0,float(a.ActualWidth()),float(a.ActualHeight())});
        auto br = b.TransformToVisual(ambientLayer).TransformBounds({0,0,float(b.ActualWidth()),float(b.ActualHeight())});
        auto nativeSeparator = [&](Controls::TabViewItem const& row) {
            auto part = Media::VisualTreeHelper::GetChild(row, 0).as<FrameworkElement>()
                .FindName(L"TabSeparator").try_as<Controls::Border>();
            Require(bool(part), "stock tab template exposes its separator");
            return part;
        };
        auto separatorA = nativeSeparator(a);
        auto separatorColor = separatorA.Background().as<Media::SolidColorBrush>().Color();
        Require(separatorColor.A == 170 && separatorColor.R == ((4289934591u >> 16) & 255) &&
            separatorColor.G == ((4289934591u >> 8) & 255) && separatorColor.B == (4289934591u & 255),
            "unselected container tints its stock trailing divider with its own color");
        auto faceShape = [&](uint32_t id) {
            for (auto visual : faceRoot.Children()) {
                if (visual.Comment() != L"tab-face:" + winrt::to_hstring(id)) continue;
                return visual.as<winrt::Microsoft::UI::Composition::ShapeVisual>().Shapes().GetAt(0)
                    .as<winrt::Microsoft::UI::Composition::CompositionSpriteShape>();
            }
            return winrt::Microsoft::UI::Composition::CompositionSpriteShape{nullptr};
        };
        Require(faceRoot.Children().Count() == 3 && faceShape(41) && faceShape(42),
            "occlusion retains one contour per visible tab and an exterior");
        auto faceFill = faceShape(41).FillBrush().as<winrt::Microsoft::UI::Composition::CompositionLinearGradientBrush>();
        Require(faceFill.ColorStops().GetAt(0).Color().A == 255 &&
            faceFill.ColorStops().GetAt(2).Color().A < 32 && faceFill.ColorStops().GetAt(5).Color().A == 255 &&
            std::abs(faceFill.StartPoint().y - ar.Y) < 1 && std::abs(faceFill.EndPoint().y - ar.Y - ar.Height) < 1,
            "post-blur face mask preserves edge light and attenuates the title region in tab coordinates");
        auto glowHeader = find(view,L"FirefoxWinUIInlineGroup-7");
        auto hr = glowHeader.TransformToVisual(ambientLayer).TransformBounds(
            {0,0,float(glowHeader.ActualWidth()),float(glowHeader.ActualHeight())});
        winrt::Windows::Foundation::Numerics::float4 joinedFace{};
        faceShape(41).Properties().TryGetVector4(L"FaceBounds", joinedFace);
        printf("Header join: header=%.1f+%.1f tab=%.1f+%.1f mask=%.1f+%.1f\n", hr.X, hr.Width, ar.X, ar.Width, joinedFace.x, joinedFace.z);
        fflush(stdout);
        Require(faceShape(41).Properties().TryGetVector4(L"FaceBounds", joinedFace) ==
            winrt::Microsoft::UI::Composition::CompositionGetValueStatus::Succeeded &&
            std::abs(joinedFace.x - hr.X) < 1 && std::abs(joinedFace.x + joinedFace.z - ar.X - ar.Width) < 1,
            "header and first inactive tab share a continuous face mask without a fabricated inner edge");
        Require(SUCCEEDED(api.syncTabs(host,tabs,2,41,&selected)), "activate the first grouped member for mask inspection");
        settle();
        winrt::Windows::Foundation::Numerics::float4 selectedGroupedFace{};
        Require(faceShape(41).Properties().TryGetVector4(L"FaceBounds", selectedGroupedFace) ==
            winrt::Microsoft::UI::Composition::CompositionGetValueStatus::Succeeded &&
            std::abs(selectedGroupedFace.x - ar.X) < 1 && std::abs(selectedGroupedFace.z - ar.Width) < 1,
            "selected first member keeps its separate native contour beside the group header");
        Require(SUCCEEDED(api.syncTabs(host,tabs,2,42,&selected)), "restore the second grouped member after mask inspection");
        settle();
        Require(std::abs(groupVisual.Offset().x - (hr.X - 18)) < 1 &&
            std::abs(groupVisual.Size().x - (br.X + br.Width - hr.X + 36)) < 1 &&
            std::abs(containerVisual.Size().x - ar.Width) < 1,
            "bottom field includes the disclosure button and both tabs; container corners follow the full tab");
        winrt::Microsoft::UI::Composition::SpriteVisual groupCenter{nullptr};
        for (auto child : groupVisual.Children())
            if (child.Comment() == L"group-center") groupCenter = child.as<winrt::Microsoft::UI::Composition::SpriteVisual>();
        Require(groupVisual.Children().Count() == 3 && groupCenter &&
            std::abs(groupCenter.Size().x - (groupVisual.Size().x - 48)) < 1 && groupCenter.Offset().x == 24,
            "rounded group caps taper only within 24 DIP of the outer corners");
        auto lowerBrush = groupCenter.Brush().as<winrt::Microsoft::UI::Composition::CompositionRadialGradientBrush>();
        Require(containerVisual.Children().Count() == 1 && !containerVisual.Brush() &&
            std::abs(containerVisual.Offset().x + containerVisual.Size().x / 2 - (ar.X + ar.Width / 2)) < 1,
            "container glow has no central fill");
        auto cornerShape = containerVisual.Children().First().Current().as<winrt::Microsoft::UI::Composition::ShapeVisual>();
        Require(cornerShape.Shapes().Size() == 2, "each container has two independent open corner strokes");
        auto cornerStroke = cornerShape.Shapes().GetAt(0).as<winrt::Microsoft::UI::Composition::CompositionSpriteShape>();
        auto upperBrush = cornerStroke.StrokeBrush().as<winrt::Microsoft::UI::Composition::CompositionRadialGradientBrush>();
        Require(std::abs(containerVisual.Offset().y - ar.Y) < 1 && ambientLayer.Clip().Rect().Y == 0 &&
            !cornerStroke.FillBrush() && cornerStroke.StrokeThickness() == 3 &&
            upperBrush.EllipseRadius().x <= 28 && upperBrush.EllipseRadius().y <= 16 &&
            upperBrush.ColorStops().GetAt(3).Color().A == 0,
            "corner strokes fade locally, leaving the top center and lower edge unpainted");
        Require(lowerBrush.EllipseCenter().y == 1 && lowerBrush.EllipseRadius().x == 1.f &&
            upperBrush.EllipseCenter().y >= 0 &&
            lowerBrush.ColorStops().GetAt(0).Color() != upperBrush.ColorStops().GetAt(0).Color(),
            "combined state retains two independently colored opposite-edge gradients");
        auto dot = a.Header().as<Controls::Grid>().Children().GetAt(4).as<Controls::Border>();
        Require(dot.Visibility() == Visibility::Collapsed && topGlow.Children().Count() == 1,
            "container glow defaults on and dot defaults off");
        auto rimCanvas = ambientLayer.Children().GetAt(0).as<Controls::Canvas>();
        Require(!rimCanvas.IsHitTestVisible() && Automation::AutomationProperties::GetAccessibilityView(rimCanvas) ==
            Automation::Peers::AccessibilityView::Raw, "surface reflections are decorative only");
        auto rimRoot = Hosting::ElementCompositionPreview::GetElementChildVisual(rimCanvas)
            .as<winrt::Microsoft::UI::Composition::ContainerVisual>();
        auto rim = [&](wchar_t const* name) {
            for (auto visual : rimRoot.Children())
                if (visual.Comment() == name) return visual.as<winrt::Microsoft::UI::Composition::SpriteVisual>();
            return winrt::Microsoft::UI::Composition::SpriteVisual{nullptr};
        };
        Require(rim(L"active-group-reflection").IsVisible() &&
            (!rim(L"active-container-reflection") || !rim(L"active-container-reflection").IsVisible()),
            "only the active tab's own glow sources produce surface highlights");
        auto groupRim = rim(L"active-group-reflection");
        auto sharedRim = rim(L"group-shared-reflection");
        Require(sharedRim && rim(L"group-outer-reflection"), "group has shared and outer surface reflections");
        unsigned outerCount = 0, sharedCount = 0;
        for (auto visual : rimRoot.Children()) {
            if (visual.Comment() == L"group-outer-reflection") ++outerCount;
            if (visual.Comment() == L"group-shared-reflection") ++sharedCount;
        }
        Require(outerCount == 2 && sharedCount == 1, "two group ends and exactly one reflection per internal seam");
        auto sharedBrush = sharedRim.Brush().as<winrt::Microsoft::UI::Composition::CompositionLinearGradientBrush>();
        auto outerBrush = rim(L"group-outer-reflection").Brush()
            .as<winrt::Microsoft::UI::Composition::CompositionLinearGradientBrush>();
        Require(sharedBrush.ColorStops().GetAt(4).Color().A < outerBrush.ColorStops().GetAt(4).Color().A &&
            sharedBrush.ColorStops().GetAt(2).Color().A == 0 && sharedBrush.ColorStops().GetAt(1).Color().A > 0,
            "shared seam has weaker group light, a quiet middle and neighboring container contribution");
        auto rimShape = groupRim.Children().First().Current().as<winrt::Microsoft::UI::Composition::ShapeVisual>();
        auto stroke = rimShape.Shapes().GetAt(0).as<winrt::Microsoft::UI::Composition::CompositionSpriteShape>();
        auto rimGeometry = stroke.Geometry().as<winrt::Microsoft::UI::Composition::CompositionPathGeometry>();
        auto rimBrush = stroke.StrokeBrush().as<winrt::Microsoft::UI::Composition::CompositionLinearGradientBrush>();
        Require(rimGeometry.Path() && stroke.StrokeThickness() == 1.25f &&
            std::abs(groupRim.Size().x - (br.Width + 8)) < 1 && std::abs(groupRim.Offset().x - (br.X - 4)) < 1 &&
            rimBrush.StartPoint().y == br.Height - 12 &&
            rimBrush.ColorStops().GetAt(0).Color().A == 0 && rimBrush.ColorStops().GetAt(4).Color().A == 185,
            "group reflection flares outside the attached tab and fades up its lower sides");
        // Glow geometry follows live controls immediately, without its own easing.
        auto ambientRefresh = reinterpret_cast<HRESULT(WINAPI*)(HWND)>(
            GetProcAddress(module,"WinUIPaneTestRefreshAmbient"));
        Require(ambientRefresh != nullptr, "ambient refresh inspection available");
        auto refreshAmbient = [&] {
            Require(ambientRefresh(host) == S_OK, "stationary glow has no frame observer");
            Require(!groupVisual.ImplicitAnimations(), "group light has no independent transition");
            for (auto field : spill.Children()) Require(!field.ImplicitAnimations(), "spill has no independent transition");
            for (auto field : topGlow.Children()) Require(!field.ImplicitAnimations(), "container light has no independent transition");
        };
        refreshAmbient();
        auto aTransform = a.RenderTransform().as<Media::TranslateTransform>();
        auto bTransform = b.RenderTransform().as<Media::TranslateTransform>();
        const auto originalWidth = groupVisual.Size().x;
        aTransform.X(48); bTransform.X(48);
        refreshAmbient();
        Require(groupVisual.Size().x > originalWidth + 40, "group light immediately reaches moved tabs");
        aTransform.X(24); bTransform.X(72);
        refreshAmbient();
        Require(groupVisual.Size().x > originalWidth + 64 && bottomGlow.Children().First().Current() == groupVisual,
            "rapid geometry changes update the retained light immediately");
        auto setAmbientDragging = reinterpret_cast<HRESULT(WINAPI*)(HWND,BOOL)>(
            GetProcAddress(module,"WinUIPaneTestSetDragging"));
        Require(setAmbientDragging && SUCCEEDED(setAmbientDragging(host,TRUE)), "start direct manipulation");
        Require(ambientRefresh(host) == S_FALSE && !groupVisual.ImplicitAnimations(),
            "dragging only observes the existing gesture");
        aTransform.X(40); bTransform.X(40);
        Require(ambientRefresh(host) == S_FALSE && !groupVisual.ImplicitAnimations(),
            "live drag coordinates never launch decorative easing");
        Require(SUCCEEDED(setAmbientDragging(host,FALSE)), "end direct manipulation");
        aTransform.X(0); bTransform.X(0); refreshAmbient();
        for (int i=0;i<50;++i) { Pump(); Sleep(10); }
        auto settledFace = faceShape(41).Geometry().as<winrt::Microsoft::UI::Composition::CompositionPathGeometry>().Path();
        refreshAmbient();
        Require(faceShape(41).Geometry().as<winrt::Microsoft::UI::Composition::CompositionPathGeometry>().Path() == settledFace,
            "unchanged layout retains mask geometry without rebuilding the visual surface");
        puts("CHECKS PASSED: immediate glow geometry, retained visuals and bounded drag observation");
        auto toggleIndicators = [&](uint32_t revision, bool glow, bool showDot) {
            auto update = winrt::Windows::Data::Json::JsonObject::Parse(LR"({"version":2,"publishOnly":true,"strip":{"version":2,"rows":[],"removed":[]}})");
            auto strip = update.GetNamedObject(L"strip");
            using Value = winrt::Windows::Data::Json::JsonValue;
            strip.SetNamedValue(L"base", Value::CreateNumberValue(revision - 1));
            strip.SetNamedValue(L"revision", Value::CreateNumberValue(revision));
            strip.SetNamedValue(L"containerGlow", Value::CreateBooleanValue(glow));
            strip.SetNamedValue(L"containerDot", Value::CreateBooleanValue(showDot));
            MenuPublish(api,host,update.Stringify().c_str()); settle();
            Require(topGlow.Children().Count() == (glow ? 1 : 0) &&
                dot.Visibility() == (showDot ? Visibility::Visible : Visibility::Collapsed) &&
                bottomGlow.Children().Count() == 1,
                "live container options apply independently without hiding the group glow");
            Require(spill.Children().Count() == 1, "container option never introduces a body cast");
            Require(glow ? separatorA.Background().as<Media::SolidColorBrush>().Color().A == 170 :
                bool(separatorA.GetBindingExpression(Controls::Border::BackgroundProperty())),
                "disabling glow restores a live theme binding on the native divider");
            auto currentSeam = rim(L"group-shared-reflection").Brush()
                .as<winrt::Microsoft::UI::Composition::CompositionLinearGradientBrush>();
            Require(currentSeam.ColorStops().GetAt(1).Color().A == (glow ? 20 : 0),
                "container option also removes its shared-seam contribution");
        };
        toggleIndicators(18, false, true);
        toggleIndicators(19, true, true);
        toggleIndicators(20, false, false);
        toggleIndicators(21, true, false);
        MenuPublish(api,host,LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":21,"revision":22,"rows":[],"removed":[],"groups":[],"layout":[{"group":0,"tab":41},{"group":0,"tab":42}]}})");
        Require(SUCCEEDED(api.syncTabs(host,tabs,2,42,&selected)), "ungroup colored container tabs");
        settle();
        Require(bottomGlow.Children().Count() == 0 && topGlow.Children().Count() == 1,
            "container glow survives tab realization after ungrouping");
        Require(!rim(L"group-outer-reflection") && !rim(L"group-shared-reflection"),
            "ungrouping releases group boundary reflections");
        MenuPublish(api,host,LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":22,"revision":23,"rows":[{"id":42,"containerName":"Other identity, same color","containerColor":4289934591}],"removed":[]}})");
        settle();
        a = view.TabItems().GetAt(0).as<Controls::TabViewItem>();
        b = view.TabItems().GetAt(1).as<Controls::TabViewItem>();
        ar = a.TransformToVisual(ambientLayer).TransformBounds({0,0,float(a.ActualWidth()),float(a.ActualHeight())});
        br = b.TransformToVisual(ambientLayer).TransformBounds({0,0,float(b.ActualWidth()),float(b.ActualHeight())});
        auto joined = topGlow.Children().First().Current().as<winrt::Microsoft::UI::Composition::SpriteVisual>();
        Require(topGlow.Children().Count() == 2 && joined.Children().Count() == 1,
            "same-color neighboring containers retain individual corner strokes");
        auto upperRim = rim(L"active-container-reflection");
        Require(upperRim && upperRim.IsVisible() && std::abs(upperRim.Size().x - br.Width) < 1 && upperRim.Children().Count() == 1 &&
            std::abs(upperRim.Offset().y - br.Y) < 1 && !rim(L"active-group-reflection").IsVisible(),
            "ungrouped active container has corner highlights and no bottom highlight");
        auto activeClip = diffusion.Clip().as<winrt::Microsoft::UI::Composition::CompositionGeometricClip>();
        Require(activeClip.Geometry().as<winrt::Microsoft::UI::Composition::CompositionPathGeometry>().Path() != nullptr,
            "the active tab cuts a notch in the final diffused glow rather than changing the per-tab fields");
        Require(SUCCEEDED(api.syncTabs(host,tabs,2,41,&selected)), "activate the first container tab");
        settle();
        Require(diffusion.Clip() != activeClip && topGlow.Children().First().Current() == joined,
            "active-tab clipping follows selection without replacing the container glow");
        Require(std::abs(upperRim.Offset().x + upperRim.Size().x / 2 - (ar.X + ar.Width / 2)) < 1,
            "the retained surface highlight follows active-tab changes");
        Require(bool(nativeSeparator(a).GetBindingExpression(Controls::Border::BackgroundProperty())),
            "activating a container restores its themed divider");
        winrt::Windows::Foundation::Numerics::float4 selectedFace{};
        Require(faceShape(41).Properties().TryGetVector4(L"FaceBounds", selectedFace) ==
            winrt::Microsoft::UI::Composition::CompositionGetValueStatus::Succeeded && std::abs(selectedFace.x - ar.X) < 1 &&
            std::abs(selectedFace.z - ar.Width) < 1,
            "ungrouped selected tab retains its separate native contour");
        activeClip = diffusion.Clip().as<winrt::Microsoft::UI::Composition::CompositionGeometricClip>();
        Require(SUCCEEDED(api.syncTabs(host,tabs,2,42,&selected)), "restore the second active tab");
        settle();
        Require(diffusion.Clip() != activeClip, "moving activation restores the former active tab's upper spill");
        MenuPublish(api,host,LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":23,"revision":24,"rows":[{"id":42,"containerColor":4281558681}],"removed":[]}})");
        settle();
        Require(topGlow.Children().Count() == 2, "different colors retain two independently composited fields");
        float leftEnd = 0, rightStart = 0;
        for (auto visual : topGlow.Children()) {
            Require(visual.as<winrt::Microsoft::UI::Composition::SpriteVisual>().Children().Count() == 1,
                "neighboring color fields retain their own corner strokes");
            if (visual.Comment() == L"container:41") leftEnd = visual.Offset().x + visual.Size().x;
            if (visual.Comment() == L"container:42") rightStart = visual.Offset().x;
        }
        Require(std::abs(leftEnd - (ar.X + ar.Width)) < 1 && std::abs(rightStart - br.X) < 1,
            "different-color corner fields track their own tab boundaries");
        MenuPublish(api,host,LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":24,"revision":25,"rows":[{"id":42,"containerName":"","containerColor":0}],"removed":[]}})");
        settle();
        auto isolated = topGlow.Children().First().Current().as<winrt::Microsoft::UI::Composition::SpriteVisual>();
        Require(topGlow.Children().Count() == 1 && isolated.Children().Count() == 1 &&
            std::abs(isolated.Size().x - ar.Width) < 1,
            "removing a neighbor container leaves the remaining corner glow unchanged");
        // Clear both color sources in the next full snapshot.
        motionState(26,false); settle();
        Require(bottomGlow.Children().Count() == 0 && topGlow.Children().Count() == 0,
            "removing color and container metadata releases both decorations");
        Require(spill.Children().Count() == 0, "clearing source metadata also removes all weak color spill");
        Require(!rim(L"active-container-reflection").IsVisible() && !rim(L"active-group-reflection").IsVisible(),
            "clearing glow sources also hides their surface reflections");
        puts("CHECKS PASSED: group motion cancellation, reduced motion and navigation exclusion");
    }
    puts("CHECKS PASSED: retained inline groups, disclosure, themes, RTL and atomic layout validation");
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
static void CheckAttachedTabAppearance(winrt::Microsoft::UI::Xaml::Controls::TabViewItem const& row) {
    using namespace winrt::Microsoft::UI::Xaml;
    std::function<FrameworkElement(DependencyObject const&)> find = [&](DependencyObject const& node) -> FrameworkElement {
        if (auto element = node.try_as<FrameworkElement>(); element && element.Name() == L"TabDragVisualContainer") return element;
        for (int i=0; i<Media::VisualTreeHelper::GetChildrenCount(node); ++i)
            if (auto found = find(Media::VisualTreeHelper::GetChild(node,i))) return found;
        return nullptr;
    };
    auto floating = find(row);
    Require(floating && floating.Visibility() == Visibility::Collapsed, "released or cancelled tab restores the attached template state");
    auto root = Media::VisualTreeHelper::GetChild(row,0).as<FrameworkElement>();
    auto container = root.FindName(L"TabContainer").as<Controls::Grid>();
    auto corners = container.CornerRadius();
    Require(corners.BottomLeft == 0 && corners.BottomRight == 0 &&
            container.GetBindingExpression(Controls::Grid::CornerRadiusProperty()),
            "capture restores the attached bottom corners and their live template binding");
}

// Inspect pixels from the actual floating HWND's bitmap, not a substitute row.
static void CheckFloatingDragBitmap(HMODULE module, bool dark = false) {
    auto copy = reinterpret_cast<HRESULT(WINAPI*)(HBITMAP*)>(GetProcAddress(module, "WinUIPaneTestCopyDragBitmap"));
    Require(copy != nullptr, "drag bitmap inspection available");
    HBITMAP bitmap{};
    for (int i=0; i<300 && !bitmap; ++i) {
        Require(SUCCEEDED(copy(&bitmap)), "copy drag bitmap");
        if (!bitmap) { Pump(); Sleep(10); }
    }
    Require(bitmap != nullptr, "floating drag bitmap rendered");
    DIBSECTION dib{};
    Require(GetObjectW(bitmap, sizeof(dib), &dib) == sizeof(dib) && dib.dsBm.bmBits, "drag bitmap pixels available");
    const int width = dib.dsBm.bmWidth, height = dib.dsBm.bmHeight;
    auto alpha = [&](int x, int y) {
        const int scan = dib.dsBmih.biHeight < 0 ? y : height - 1 - y;
        return static_cast<unsigned char*>(dib.dsBm.bmBits)[scan * dib.dsBm.bmWidthBytes + x * 4 + 3];
    };
    // RenderTargetBitmap can include transparent padding at non-unit island
    // scales. Measure the painted body rather than assuming it fills the bitmap.
    int top=0, bottom=height-1, left=0, right=width-1;
    while (top < height && alpha(width/2,top) < 200) ++top;
    while (bottom > top && alpha(width/2,bottom) < 200) --bottom;
    Require(bottom-top > 12, "floating tab has a filled vertical body");
    const int middle = (top+bottom)/2;
    while (left < width && alpha(left,middle) < 200) ++left;
    while (right > left && alpha(right,middle) < 200) --right;
    Require(right-left > 16, "floating tab has an opaque body");
    // Save the same pixels for visual review alongside the automated alpha checks.
    BITMAPV4HEADER header{};
    header.bV4Size = sizeof(header);
    header.bV4Width = width;
    header.bV4Height = dib.dsBmih.biHeight;
    header.bV4Planes = 1;
    header.bV4BitCount = 32;
    header.bV4V4Compression = BI_BITFIELDS;
    header.bV4RedMask = 0x00ff0000;
    header.bV4GreenMask = 0x0000ff00;
    header.bV4BlueMask = 0x000000ff;
    header.bV4AlphaMask = 0xff000000;
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4d42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(header);
    fileHeader.bfSize = fileHeader.bfOffBits + dib.dsBm.bmWidthBytes * height;
    FILE* output{};
    if (fopen_s(&output, dark ? "artifacts/setup/tab-floating-dark.bmp" : "artifacts/setup/tab-floating-light.bmp", "wb") == 0) {
        fwrite(&fileHeader,sizeof(fileHeader),1,output);
        fwrite(&header,sizeof(header),1,output);
        fwrite(dib.dsBm.bmBits,dib.dsBm.bmWidthBytes,height,output);
        fclose(output);
    }
    fprintf(stderr, "Floating corner alpha: %u, %u\n", alpha(left+1,bottom-1), alpha(right-1,bottom-1));
    Require(alpha(left,bottom) == 0 && alpha(right,bottom) == 0 &&
            alpha(left+1,bottom-1) <= 16 && alpha(right-1,bottom-1) <= 16,
            "floating bitmap has rounded bottom corners without attached tab feet");
    DeleteObject(bitmap);
}

static void CheckWindowContinuation(HMODULE module, WinUIPaneApi& api, HWND host) {
    using namespace winrt;
    using namespace winrt::Microsoft::UI::Xaml;
    RECT rect{0,0,600,48};
    Require(SUCCEEDED(api.updateTabStrip(host,&rect,TRUE)), "create continuation strip");
    ShowWindow(host,SW_SHOW);
    SetForegroundWindow(host);
    auto continuation = reinterpret_cast<HRESULT(WINAPI*)(HWND,uint32_t,uint32_t)>(GetProcAddress(module,"WinUIPaneTestWindowContinuation"));
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND,void**)>(GetProcAddress(module,"WinUIPaneTestGetContent"));
    Require(continuation && inspect, "continuation inspection available");
    WinUITab tabs[]{{41,L"Existing tab"},{42,L"Second tab"},{77,L"Merged tab"}};
    uint32_t selected{};
    Require(SUCCEEDED(api.syncTabs(host,tabs,2,41,&selected)), "initial destination tabs");
    Require(SUCCEEDED(continuation(host,77,0)), "Firefox identifies the merged tab before its snapshot");
    Require(SUCCEEDED(api.syncTabs(host,tabs,3,77,&selected)), "publish adopted tab");
    UIElement content{nullptr};
    Require(SUCCEEDED(inspect(host,put_abi(content))), "inspect continuation strip");
    auto root = content.as<Controls::Grid>();
    auto view = root.Children().GetAt(0).as<Controls::TabView>();
    auto row = view.TabItems().GetAt(2).as<Controls::TabViewItem>();
    Require(row.Opacity() == 0, "adopted tab is hidden before its first layout frame");
    Require(SUCCEEDED(continuation(host,77,2)), "cancel before drag continuation starts");
    Require(row.Opacity() == 1, "cancelled pending continuation restores the adopted row");
    for (int i=0;i<40;++i) { Pump(); root.UpdateLayout(); Sleep(10); }
    CheckAttachedTabAppearance(row);
    Require(SUCCEEDED(continuation(host,77,0)), "prepare held continuation");
    Require(SUCCEEDED(api.syncTabs(host,tabs,3,77,&selected)), "hide retained adopted row during pending continuation");
    Require(row.Opacity() == 0, "retained adopted row is hidden before capture");
    // Hold the button over blank content, then continue into the destination
    // strip at a point different from the adopted row's real layout slot.
    POINT cursor{200,180};
    ClientToScreen(host,&cursor);
    SetCursorPos(cursor.x,cursor.y);
    INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
    SendInput(1,&input,sizeof(input));
    for (int i=0;i<10;++i) { Pump(); Sleep(10); }
    cursor = {200,24}; ClientToScreen(host,&cursor); SetCursorPos(cursor.x,cursor.y);
    Require(SUCCEEDED(continuation(host,77,1)), "take real window-drag continuation");
    Require(row.Opacity() == 0, "capture never reveals the adopted tab in its layout slot");
    CheckFloatingDragBitmap(module);
    Require(row.Opacity() == 0, "floating image owns continued drag appearance");
    input.mi.dwFlags=MOUSEEVENTF_LEFTUP; SendInput(1,&input,sizeof(input));
    for (int i=0;i<40;++i) { Pump(); Sleep(10); }
    NextRequest(api,host,5,L"strip-reorder");
    Require(row.Opacity() == 1, "release restores the real adopted tab");
    CheckAttachedTabAppearance(row);
    puts("CHECKS PASSED: merged tab stays hidden through layout/capture, pending cancel restores it");
}

// Query UIA through its client interface on an MTA, as WithTab does.
// Calling a XAML peer directly returns island-local bounds, before the
// hosting provider converts them to physical screen coordinates.
static RECT TabScreenBounds(HWND window, wchar_t const* id) {
    RECT bounds{};
    std::atomic<bool> done = false;
    std::thread client([&] {
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "bounds UIA apartment");
        {
            ComPtr<IUIAutomation> automation;
            Require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&automation))), "bounds UIA client");
            ComPtr<IUIAutomationElement> root, row;
            Require(SUCCEEDED(automation->ElementFromHandle(window, &root)), "bounds UIA host");
            VARIANT value{};
            value.vt = VT_BSTR;
            value.bstrVal = SysAllocString(id);
            ComPtr<IUIAutomationCondition> condition;
            Require(SUCCEEDED(automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, &condition)),
                    "bounds UIA condition");
            VariantClear(&value);
            for (int attempt = 0; attempt < 100 && !row; ++attempt) {
                Require(SUCCEEDED(root->FindFirst(TreeScope_Descendants, condition.Get(), &row)), "query row through UIA");
                if (!row) Sleep(20);
            }
            if (!row) fwprintf(stderr, L"Missing UIA row: %ls\n", id);
            Require(bool(row), "find dragged row through UIA");
            Require(SUCCEEDED(row->get_CurrentBoundingRectangle(&bounds)), "physical UIA row bounds");
        }
        CoUninitialize();
        done = true;
    });
    auto deadline = GetTickCount64() + 15000;
    while (!done && GetTickCount64() < deadline) {
        Pump();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    Require(done, "bounds UIA completes without deadlock");
    client.join();
    Require(bounds.right > bounds.left && bounds.bottom > bounds.top, "UIA reports nonempty screen bounds");
    return bounds;
}

static void CheckTabGroupHeaderShift(HMODULE module, WinUIPaneApi& api, HWND host) {
    using namespace winrt::Microsoft::UI::Xaml;
    auto settle = [] { for (int i=0;i<40;++i) { Pump(); Sleep(10); } };
    RECT rect{0,0,1000,40};
    SetWindowPos(host,nullptr,200,200,1040,400,SWP_NOZORDER);
    Require(SUCCEEDED(api.updateTabStrip(host,&rect,TRUE)), "group gap strip geometry");
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module,"WinUIPaneTestGetContent"));
    UIElement content{nullptr};
    Require(SUCCEEDED(inspect(host,winrt::put_abi(content))),"inspect group gap strip");
    auto view=content.as<Controls::Grid>().Children().GetAt(0).as<Controls::TabView>();
    SetForegroundWindow(host);
    for (bool rtl : {false,true}) {
        auto state = winrt::Windows::Data::Json::JsonObject::Parse(LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":0,"revision":70,"rtl":false,"groupsLabel":"Groups","groups":[{"id":7,"name":"Research","label":"Research","collapsed":false}],"rows":[{"id":11,"detail":"","flags":0},{"id":22,"detail":"","flags":0},{"id":33,"detail":"","flags":0}],"layout":[{"group":0,"tab":11},{"group":7,"tab":0},{"group":7,"tab":22},{"group":0,"tab":33}]}})");
        state.GetNamedObject(L"strip").SetNamedValue(L"rtl",winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(rtl));
        state.GetNamedObject(L"strip").SetNamedValue(L"revision",winrt::Windows::Data::Json::JsonValue::CreateNumberValue(rtl ? 71 : 70));
        MenuPublish(api,host,state.Stringify().c_str());
        WinUITab tabs[]{{11,L"Before"},{22,L"Group member"},{33,L"After"}};
        uint32_t selected{};
        Require(SUCCEEDED(api.syncTabs(host,tabs,3,11,&selected)), "group gap tabs");
        settle();
        for (bool backward : {false,true}) {
            auto source=TabScreenBounds(host,backward ? L"FirefoxWinUITab-33" : L"FirefoxWinUITab-11");
            auto member=TabScreenBounds(host,L"FirefoxWinUITab-22");
            auto header=TabScreenBounds(host,L"FirefoxWinUIInlineGroup-7");
            SetCursorPos((source.left+source.right)/2,(source.top+source.bottom)/2);
            settle();
            INPUT mouse{}; mouse.type=INPUT_MOUSE; mouse.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
            Require(SendInput(1,&mouse,sizeof(mouse))==1,"press tab for group gap");
            settle();
            auto target = backward != rtl ? member.left+16 : member.right-16;
            SetCursorPos(target,(member.top+member.bottom)/2);
            settle();
            auto sourceWidth=view.TabItems().GetAt(backward ? 2 : 0).as<Controls::TabViewItem>().ActualWidth()
                * content.XamlRoot().RasterizationScale();
            LONG headerDelta{},memberDelta{};
            for (int attempt=0;attempt<20;++attempt) {
                auto shiftedHeader=TabScreenBounds(host,L"FirefoxWinUIInlineGroup-7");
                auto shiftedMember=TabScreenBounds(host,L"FirefoxWinUITab-22");
                headerDelta=shiftedHeader.left-header.left;
                memberDelta=shiftedMember.left-member.left;
                if (std::abs(headerDelta-memberDelta)<=2 && std::abs(std::abs(headerDelta)-sourceWidth)<=2) break;
                settle();
            }
            fprintf(stderr,"Group gap rtl=%d backward=%d header=%ld member=%ld width=%ld\n",rtl,backward,headerDelta,memberDelta,source.right-source.left);
            Require(std::abs(headerDelta-memberDelta)<=2 && std::abs(headerDelta)>20,
                "physical tab drag shifts the retained group button with its member");
            Require(std::abs(std::abs(headerDelta)-sourceWidth)<=2,
                "group gap transfers tab width without consuming header space");
            INPUT escape[2]{};
            escape[0].type=escape[1].type=INPUT_KEYBOARD;
            escape[0].ki.wVk=escape[1].ki.wVk=VK_ESCAPE;
            escape[1].ki.dwFlags=KEYEVENTF_KEYUP;
            SendInput(1,escape,sizeof(INPUT));
            settle();
            SendInput(1,escape+1,sizeof(INPUT));
            mouse.mi.dwFlags=MOUSEEVENTF_LEFTUP; SendInput(1,&mouse,sizeof(mouse));
            settle();
            auto restored=TabScreenBounds(host,L"FirefoxWinUIInlineGroup-7");
            Require(std::abs(restored.left-header.left)<=2,"Escape restores group header geometry");
        }
    }
}

static void CheckTabStripDrag(HMODULE module, WinUIPaneApi& api, HWND host, bool reject = false, bool inactive = false, bool rearm = false, bool grouping = false) {
    using namespace winrt;
    using namespace winrt::Microsoft::UI::Xaml;
    RECT rect{0, 0, 600, 48};
    Require(SUCCEEDED(api.updateTabStrip(host, &rect, TRUE)), "create strip presentation for drag test");
    SetWindowPos(host, nullptr, 300, 200, 640, 480, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(host, SW_SHOW);
    WinUITab tabs[]{{11, L"First tab"}, {22, L"Second tab"}};
    uint32_t selected{};
    Require(SUCCEEDED(api.syncTabs(host, tabs, 2, inactive ? 22 : 11, &selected)),
            "sync strip snapshot for drag test");
    auto inspect = reinterpret_cast<HRESULT(WINAPI*)(HWND, void**)>(GetProcAddress(module, "WinUIPaneTestGetContent"));
    UIElement content{nullptr};
    Require(SUCCEEDED(inspect(host, put_abi(content))), "inspect strip for drag test");
    auto root = content.as<Controls::Grid>();
    root.RequestedTheme(reject ? ElementTheme::Dark : ElementTheme::Light);
    auto view = root.Children().GetAt(0).as<Controls::TabView>();
    if (grouping) {
        MenuPublish(api,host,LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":0,"revision":50,"rtl":false,"groupsLabel":"Groups","groups":[],"windowDragSession":1,"rows":[{"id":11,"detail":"","flags":0,"groupSource":true},{"id":22,"detail":"","flags":0,"groupTarget":true}],"layout":[{"group":0,"tab":11},{"group":0,"tab":22}]}})");
    }
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
    auto firstBounds = TabScreenBounds(host, L"FirefoxWinUITab-11");
    auto lastBounds = TabScreenBounds(host, L"FirefoxWinUITab-22");
    POINT client{(firstBounds.left + firstBounds.right) / 2, (firstBounds.top + firstBounds.bottom) / 2};
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
    HHOOK createProbe{};
    if (rearm) {
        nestedDragCreateObserved = false;
        createProbe = SetWindowsHookExW(WH_CBT, DragCreateProbe, nullptr, GetCurrentThreadId());
        Require(createProbe != nullptr, "install nested drag-window creation probe");
    }
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
    // Leave horizontally on both sides, then re-enter without releasing.
    // Re-entry must recover the reorder gap and produce a reorder, not detach.
    RECT hostBounds{};
    Require(GetWindowRect(host, &hostBounds), "reorder host bounds");
    for (LONG x : {hostBounds.left - 120, hostBounds.right + 120}) {
        moveTo(x, client.y);
        settle();
    }
    // Finish over the target for grouping, or at the end for a reorder.
    const LONG endX = grouping ? (lastBounds.left + lastBounds.right) / 2 : lastBounds.right - 8;
    for (int step = 1; step <= 25; ++step) {
        moveTo(client.x + (endX - client.x) * step / 25, client.y);
        settle(5);
    }
    if (grouping) settle(100);
    CheckFloatingDragBitmap(module, reject);
    if (rearm) {
        UnhookWindowsHookEx(createProbe);
        Require(nestedDragCreateObserved, "creation exercised a reentrant message-hook tick");
        auto popups = DragPreviewWindows();
        Require(popups.size() == 1, "reentrant creation leaves exactly one owned drag popup");
        const auto popup = popups.front();
        auto restart = reinterpret_cast<HRESULT(WINAPI*)(HWND,uint32_t)>(GetProcAddress(module,"WinUIPaneTestRearmDrag"));
        Require(restart && SUCCEEDED(restart(host,22)), "new press replaces a still-live drag session");
        Require(!IsWindow(popup) && DragPreviewWindows().empty(),
                "rearming destroys the old popup instead of losing its handle");
        Require(first.Opacity() == 1, "rearming restores the previous source tab");
        send(MOUSEEVENTF_LEFTUP);
        settle();
        Require(DragPreviewWindows().empty(), "replacement click leaves no floating tab");
        return;
    }
    send(MOUSEEVENTF_LEFTUP);
    settle(200);
    Require(DragPreviewWindows().empty(), "release destroys the floating drag HWND");
    puts("Drag gesture completed without crashing");
    CheckAttachedTabAppearance(first);
    if (grouping) {
        auto action = NextRequest(api,host,5,L"strip-create-group").GetNamedObject(L"action");
        Require(action.GetNamedNumber(L"id") == 11 && action.GetNamedNumber(L"targetId") == 22, "physical dwell creates group");
        MenuPublish(api,host,LR"({"version":2,"publishOnly":true,"strip":{"version":2,"base":50,"revision":51,"rows":[],"removed":[],"groups":[{"id":7,"name":"New group","label":"New group","collapsed":false}],"layout":[{"group":7,"tab":0},{"group":7,"tab":11},{"group":7,"tab":22}]}})");
        Require(SUCCEEDED(api.syncTabs(host,tabs,2,11,&selected)), "Firefox confirms created group");
        settle(100);
        // Raw capture can deprive TabViewItem of PointerReleased. Re-enter the
        // retained source after grouping, without another press or selection.
        auto groupedBounds = TabScreenBounds(host, L"FirefoxWinUITab-11");
        moveTo(groupedBounds.left + 16, (groupedBounds.top + groupedBounds.bottom) / 2);
        settle();
        moveTo(groupedBounds.right - 48, (groupedBounds.top + groupedBounds.bottom) / 2);
        settle();
        CheckAttachedTabAppearance(first);
        auto templateRoot = Media::VisualTreeHelper::GetChild(first,0).as<FrameworkElement>();
        auto background = templateRoot.FindName(L"SelectedBackgroundPath").as<FrameworkElement>();
        Require(first.IsSelected() && background.Visibility() == Visibility::Visible && background.Opacity() == 1,
                "grouped active tab immediately has its attached selected background");
        return;
    }
    auto event = NextRequest(api, host, 5, L"strip-reorder",40,reject ? 2 : 1).GetNamedObject(L"action");
    Require(event.Size() && event.GetNamedNumber(L"id") == 11 && event.GetNamedNumber(L"beforeId") == 0,
            "a real pointer drag past the last tab queues a validated move-to-end request");
    if (reject) {
        Require(first.Opacity() == 1 && first.RenderTransform().as<Media::TranslateTransform>().X() == 0,
                "rejected reorder restores the source slot without a settle animation");
        return;
    }
    const auto pendingOffset = first.RenderTransform().as<Media::TranslateTransform>().X();
    Require(first.Opacity() == 1 && pendingOffset > first.ActualWidth() / 2,
            "released tab is revealed at the destination while awaiting Firefox");
    Require(SUCCEEDED(api.syncTabs(host,tabs,2,11,&selected)), "unrelated old-order snapshot while reorder is pending");
    Require(first.RenderTransform().as<Media::TranslateTransform>().X() == pendingOffset,
            "old-order snapshot does not flash the released tab at its original slot");
    WinUITab confirmed[]{{22,L"Second tab"},{11,L"First tab"}};
    Require(SUCCEEDED(api.syncTabs(host,confirmed,2,11,&selected)), "Firefox confirms move to end");
    Require(view.TabItems().GetAt(1) == first && first.RenderTransform().as<Media::TranslateTransform>().X() == 0,
            "confirmed order replaces the temporary offset without a settle animation");
}

// Detach-to-new-window. Releasing well outside every open pane's own strip
// bounds entirely (Pane.cpp's TickDragSession hit-tests screen points
// against each pane's own StripScreenBounds(), the tab drag
// redesign) means release resolves as "no pane's strip is under the
// cursor" -- CommitDetach queues strip-detach at the real release point,
// same as before.
static void CheckTabStripDetach(HMODULE module, WinUIPaneApi& api, HWND host, bool reject = false, int horizontal = 0) {
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
    if (horizontal) SetWindowPos(host, nullptr, 300, 200, 640, 480, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(host, SW_SHOW);
    WinUITab tabs[]{{55, L"Detach tab"}};
    uint32_t selected{};
    Require(SUCCEEDED(api.syncTabs(host, tabs, 1, 55, &selected)),
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

    auto rowBounds = TabScreenBounds(host, L"FirefoxWinUITab-55");
    POINT client{(rowBounds.left + rowBounds.right) / 2, (rowBounds.top + rowBounds.bottom) / 2};
    RECT hostRect{};
    Require(GetWindowRect(host, &hostRect) != 0, "resolve host window rect for detach test");
    // Comfortably below host's own window entirely, not just below the
    // strip's own visual area within it -- registration is on the whole
    // top-level window, so only actually leaving that window's bounds
    // reaches "no target accepted it anywhere."
    POINT detachPoint{hostRect.left + 50, hostRect.bottom + 200};
    if (horizontal) detachPoint = {horizontal < 0 ? hostRect.left - 120 : hostRect.right + 120, client.y};
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
    CheckFloatingDragBitmap(module);
    if (horizontal) {
        RECT popup{};
        POINT cursor{};
        Require(GetWindowRect(FindWindowW(L"FirefoxWinUITabTearoff", nullptr), &popup) && GetCursorPos(&cursor),
                "read live floating tab and pointer bounds");
        Require(std::abs((popup.left + popup.right) / 2 - cursor.x) <= 2,
                "horizontal exit releases the visual clamp and follows the pointer");
    }
    send(MOUSEEVENTF_LEFTUP);
    settle(200);
    puts("Detach gesture completed without crashing");
    Require(tab.Opacity() == 0, "detach release keeps the real source tab hidden");
    CheckAttachedTabAppearance(tab);
    Require(SUCCEEDED(api.syncTabs(host,tabs,1,55,&selected)), "old source snapshot during detach");
    Require(tab.Opacity() == 0, "a departing last tab is not mistaken for a confirmed move to end");
    auto detachEvent = NextRequest(api, host, 5, L"strip-detach",40,reject ? 2 : 1).GetNamedObject(L"action");
    Require(tab.Opacity() == (reject ? 1 : 0), "detach outcome restores rejected tabs but preserves accepted handoff");
    Require(SUCCEEDED(api.syncTabs(host,nullptr,0,0,&selected)), "source removal confirms detach");
    Require(detachEvent.Size() && detachEvent.GetNamedNumber(L"id") == 55,
            "dragging a tab outside every open window's bounds queues a validated detach request");
    if (horizontal) {
        const auto x = detachEvent.GetNamedNumber(L"screenX");
        Require(horizontal < 0 ? x < hostRect.left : x > hostRect.right,
                "horizontal exit detaches beyond the window edge at strip height");
    } else {
        Require(detachEvent.GetNamedNumber(L"screenY") > double(hostRect.bottom),
                "the detach request carries a screen point below the host window it left");
    }
}

// Cross-window adopt. A second, independent pane/strip in a real second
// HWND: Pane.cpp's TickDragSession hit-tests the real cursor position
// against every open pane's own StripScreenBounds() (the tab drag
// redesign), precisely so a drop landing on a DIFFERENT window's
// strip resolves there. Same real-pointer-gesture technique as
// CheckTabStripDrag above.
enum class AdoptScenario { Drop, RejectDrop, CloseAfterDrop, CloseSourceAfterDrop, CloseTarget, CloseSource, ChangeThenClose, ChangeThenDrop, Offset };

static void CheckTabStripCrossWindowAdopt(HMODULE module, WinUIPaneApi& api, HWND host,
                                         AdoptScenario scenario = AdoptScenario::Drop, float sourceScale = 1, float targetScale = 1) {
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

    const bool scaled = sourceScale != 1 || targetScale != 1;
    auto setScale = [&](HWND window, float scale) {
        auto apply = reinterpret_cast<HRESULT(WINAPI*)(HWND, float)>(GetProcAddress(module, "WinUIPaneTestSetIslandScale"));
        Require(apply && SUCCEEDED(apply(window, scale)), "set real island rasterization scale");
    };
    RECT rect = scenario == AdoptScenario::Offset ? RECT{40, 90, 600, scaled ? 186 : 138} : RECT{0, 0, 600, 48};
    Require(SUCCEEDED(api.updateTabStrip(host, &rect, TRUE)), "create strip presentation for adopt test");
    setScale(host, sourceScale);
    ShowWindow(host, SW_SHOW);
    WinUITab tabs[]{{33, L"Source tab"}};
    uint32_t selected{};
    Require(SUCCEEDED(api.syncTabs(host, tabs, 1, 33, &selected)),
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

    HWND host2 = CreateWindowExW(0, L"STATIC", L"Cross-window drop target", WS_OVERLAPPEDWINDOW, 680, 0, 820, 260,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(host2 != nullptr, "second host for cross-window adopt test");
    RECT rect2 = scenario == AdoptScenario::Offset ? RECT{20, 70, 780, scaled ? 166 : 118} : RECT{0, 0, 400, 48};
    Require(SUCCEEDED(api.updateTabStrip(host2, &rect2, TRUE)), "create strip presentation on second host");
    setScale(host2, targetScale);
    ShowWindow(host2, SW_SHOW);
    WinUITab tabs2[]{{44, L"Other window's tab"}, {55, L"Removed destination tab"}, {66, L"Last destination tab"}};
    const bool changeTarget = scenario == AdoptScenario::ChangeThenClose || scenario == AdoptScenario::ChangeThenDrop;
    const uint32_t targetCount = changeTarget || scaled ? 3 : 1;
    uint32_t selected2{};
    Require(SUCCEEDED(api.syncTabs(host2, tabs2, targetCount, 44, &selected2)),
            "sync second host's strip snapshot for adopt test");
    UIElement content2{nullptr};
    Require(SUCCEEDED(inspect(host2, put_abi(content2))), "inspect second host's strip");
    auto view2 = content2.as<Controls::Grid>().Children().GetAt(0).as<Controls::TabView>();
    auto destTab = view2.TabItems().GetAt(scaled ? 1 : 0).as<Controls::TabViewItem>();
    for (int i = 0; i < 300 && (!destTab.IsLoaded() || destTab.ActualWidth() == 0); ++i) {
        Pump();
        Sleep(10);
    }
    Require(destTab.IsLoaded() && destTab.ActualWidth() > 0, "destination tab realized before adopt drag");
    auto overlap = reinterpret_cast<HRESULT(WINAPI*)(HWND, HWND, uint32_t)>(GetProcAddress(module, "WinUIPaneTestTransferOverlap"));
    Require(overlap && SUCCEEDED(overlap(host, host2, 33)),
            "old adoption rejection cannot reveal a newer source departure; latest rejection restores it");
    settle(30);

    auto sourceBounds = TabScreenBounds(host, L"FirefoxWinUITab-33");
    POINT sourceClient{(sourceBounds.left + sourceBounds.right) / 2, (sourceBounds.top + sourceBounds.bottom) / 2};
    auto destBounds = TabScreenBounds(host2, scaled ? L"FirefoxWinUITab-55" : L"FirefoxWinUITab-44");
    if (scaled) {
        fprintf(stderr, "Island scales source=%.2f target=%.2f\n", view.XamlRoot().RasterizationScale(), view2.XamlRoot().RasterizationScale());
        Require(std::abs(view.XamlRoot().RasterizationScale() - sourceScale) < .01 &&
                std::abs(view2.XamlRoot().RasterizationScale() - targetScale) < .01, "WinUI applied both distinct scales");
    }
    POINT destPoint{destBounds.left + (scaled ? (destBounds.right - destBounds.left) * 2 / 5 : 5), (destBounds.top + destBounds.bottom) / 2};
    fprintf(stderr, "Adopt test: dragging from (%ld,%ld) toward (%ld,%ld)\n", sourceClient.x, sourceClient.y,
            destPoint.x, destPoint.y);

    // Foreground the source before injecting the actual tab gesture.
    SetWindowPos(host, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(host);
    settle();
    for (int attempt = 0; attempt < 30 && GetForegroundWindow() != host; ++attempt) {
        SetForegroundWindow(host);
        settle(10);
    }
    Require(GetForegroundWindow() == host, "source window owns foreground before injecting drag input");
    Require(SetCursorPos(sourceClient.x, sourceClient.y) != 0, "position cursor over source tab");
    settle();
    send(MOUSEEVENTF_LEFTDOWN);
    settle();
    Require(sourceTab.Opacity() == 1, "a stationary press does not promote an offset or scaled drag");
    if (scaled) {
        // Change both host and island positions after the gesture starts.
        // The next hit test must use the live placement, not a cached origin.
        SetWindowPos(host2, nullptr, 680, 100, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        OffsetRect(&rect2, 9, 13);
        Require(SUCCEEDED(api.updateTabStrip(host2, &rect2, TRUE)), "move target island during drag");
        settle();
        destBounds = TabScreenBounds(host2, L"FirefoxWinUITab-55");
        destPoint = {(destBounds.left * 3 + destBounds.right * 2) / 5,
                     (destBounds.top + destBounds.bottom) / 2};
    }
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
    Require(previewOffset > 0.0, "fixture entered a real foreign drag preview before mutation");
    // Wait for the asynchronous drag bitmap: opacity 0 proves the actual
    // source gesture promoted and its popup owns the visual.
    for (int i = 0; i < 300 && sourceTab.Opacity() != 0; ++i) settle(1);
    Require(sourceTab.Opacity() == 0, "fixture has a hidden source row before destination mutation");
    if (changeTarget) {
        WinUITab changed[]{{66, L"Moved destination tab"}, {77, L"New destination tab"},
                           {88, L"Replacement destination tab"}};
        // Closing covers shrinkage; dropping covers a same-count replacement
        // and reorder, which a count-only invalidation would miss.
        uint32_t changedCount = scenario == AdoptScenario::ChangeThenDrop ? 3 : 2;
        Require(SUCCEEDED(api.syncTabs(host2, changed, changedCount, 66, &selected2)),
                "destination removes, replaces and reorders tabs while foreign drag is held");
    }
    if (scenario == AdoptScenario::CloseSource) {
        Require(SUCCEEDED(api.destroy(host)), "source pane closes during foreign hover");
        send(MOUSEEVENTF_LEFTUP);
        settle();
        Require(destTab.RenderTransform().as<Media::TranslateTransform>().X() == 0,
                "closing source resets the surviving destination preview");
        Require(!NextRequest(api, host2, 5).Size(), "source close does not commit a foreign adopt");
        Require(SUCCEEDED(api.destroy(host2)), "destroy target after source cancellation");
        DestroyWindow(host2);
        puts("CHECKS PASSED: source teardown clears the foreign preview");
        return;
    }
    if (scenario == AdoptScenario::CloseTarget || scenario == AdoptScenario::ChangeThenClose) {
        // Do not pump between mutation and close: teardown must not index the
        // old three-row preview through the new two-row collection.
        HRESULT closed = api.destroy(host2);
        fprintf(stderr, "Destination teardown during drag: hr=0x%08lx sourceOpacity=%.1f\n",
                static_cast<unsigned long>(closed), sourceTab.Opacity());
        send(MOUSEEVENTF_LEFTUP);
        Require(SUCCEEDED(closed), "destination with active foreign preview tears down without stale indices");
        DestroyWindow(host2);
        settle();
        Require(sourceTab.Opacity() == 1, "closing the hovered destination restores the surviving source row");
        CheckAttachedTabAppearance(sourceTab);
        WinUITab refreshed[]{{33, L"Source after cancelled drag"}, {88, L"New source tab"}};
        Require(SUCCEEDED(api.syncTabs(host, refreshed, 2, 33, &selected)),
                "source accepts snapshot after destination disappears");
        Require(view.TabItems().Size() == 2, "destination close clears source dragging guard");
        Require(!NextRequest(api, host, 5).Size(), "destination close does not commit reorder or detach");
        puts("CHECKS PASSED: destination teardown cancels the live source gesture");
        return;
    }
    if (scenario == AdoptScenario::ChangeThenDrop) {
        settle(30);
        // Old cached rows, including the removed first row, must be reset.
        Require(destTab.RenderTransform().as<Media::TranslateTransform>().X() == 0,
                "removed destination row has no surviving preview transform");
        auto current = view2.TabItems().GetAt(0).as<Controls::TabViewItem>();
        Require(winrt::unbox_value<uint32_t>(current.Tag()) == 66, "destination order changed during hover");
        Require(current.RenderTransform().as<Media::TranslateTransform>().X() > 0,
                "foreign preview is rebuilt for the new destination order");
    }
    CheckFloatingDragBitmap(module);
    send(MOUSEEVENTF_LEFTUP);
    settle(200);
    puts("Cross-window adopt gesture completed without crashing");
    Require(previewOffset > 0.0,
            "hovering the dragged tab over the destination strip opens a live make-room preview gap there");
    // The adopt is queued on the destination window's own endpoint, so it is
    // drained there -- this is also what proves the two endpoints' queues are
    // independent rather than one shared list.
    Require(sourceTab.Opacity() == 0, "foreign drop does not reveal the original source slot");
    CheckAttachedTabAppearance(sourceTab);
    Require(SUCCEEDED(api.syncTabs(host,tabs,1,33,&selected)), "old source snapshot during adoption");
    Require(sourceTab.Opacity() == 0, "old source snapshot preserves pending departure");
    if (!changeTarget && !scaled) {
        Require(destTab.RenderTransform().as<Media::TranslateTransform>().X() > 0,
                "destination gap survives release until adoption is published");
        Require(SUCCEEDED(api.syncTabs(host2,tabs2,targetCount,44,&selected2)), "old destination snapshot during adoption");
        Require(destTab.RenderTransform().as<Media::TranslateTransform>().X() > 0,
                "old destination snapshot does not close the pending gap");
    }
    if (scenario == AdoptScenario::CloseAfterDrop) {
        Require(SUCCEEDED(api.destroy(host2)), "destination closes before adoption outcome");
        Require(sourceTab.Opacity() == 1, "closing pending destination restores source tab");
        DestroyWindow(host2);
        return;
    }
    if (scenario == AdoptScenario::CloseSourceAfterDrop) {
        Require(SUCCEEDED(api.destroy(host)), "source closes before adoption outcome");
        Require(NextRequest(api,host2,5,L"strip-adopt",40,2).Size(), "pending outcome survives source teardown");
        Require(destTab.RenderTransform().as<Media::TranslateTransform>().X() == 0, "failed adoption clears gap after source teardown");
        Require(SUCCEEDED(api.destroy(host2)), "destroy pending target");
        DestroyWindow(host2);
        return;
    }
    auto adoptRequest = NextRequest(api, host2, 5, L"strip-adopt",40,scenario == AdoptScenario::RejectDrop ? 2 : 1);
    Require(adoptRequest.Size(), "adopt reaches the destination window's request queue");
    Require(adoptRequest.GetNamedNumber(L"tab") == 0,
            "an incoming adopt names no tab in the destination window's own id space");
    auto adoptEvent = adoptRequest.GetNamedObject(L"action");
    Require(adoptEvent.GetNamedNumber(L"id") == 33 &&
                adoptEvent.GetNamedNumber(L"sourceHwnd") == double(reinterpret_cast<uintptr_t>(host)),
            "dropping a tab onto another window's strip queues a validated adopt request there");
    if (scaled) Require(adoptEvent.GetNamedNumber(L"index") == 1, "scaled drop resolves the UIA-targeted second tab gap");
    if (scenario == AdoptScenario::ChangeThenDrop)
        Require(adoptEvent.GetNamedNumber(L"index") == 0, "drop uses the refreshed destination insertion index");
    if (scenario == AdoptScenario::RejectDrop) {
        Require(sourceTab.Opacity() == 1, "rejected adoption restores source visibility");
        Require(destTab.RenderTransform().as<Media::TranslateTransform>().X() == 0, "rejected adoption closes destination gap");
    } else {
        Require(sourceTab.Opacity() == 0, "accepted adoption waits for source removal");
        WinUITab adopted[]{{77,L"Adopted tab"},{44,L"Other window's tab"}};
        Require(SUCCEEDED(api.syncTabs(host2,adopted,2,77,&selected2)), "destination publishes adopted tab");
        Require(destTab.RenderTransform().as<Media::TranslateTransform>().X() == 0, "adopted layout replaces preview gap");
        Require(SUCCEEDED(api.syncTabs(host,nullptr,0,0,&selected)), "source publishes departure");
    }
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
                                        wcscmp(argv[2], L"--menu") == 0 || wcscmp(argv[2], L"--strip") == 0 || wcscmp(argv[2], L"--inline") == 0 ||
                                        wcscmp(argv[2], L"--drag") == 0 || wcscmp(argv[2], L"--reorder") == 0 || wcscmp(argv[2], L"--transfers") == 0 || wcscmp(argv[2], L"--continuation") == 0 || wcscmp(argv[2], L"--drag-close") == 0 ||
                                        wcscmp(argv[2], L"--drag-change") == 0 || wcscmp(argv[2], L"--drag-offset") == 0 || wcscmp(argv[2], L"--drag-scale") == 0 || wcscmp(argv[2], L"--group-drop") == 0)),
            "DLL path and optional --inspect required");
    // Before the DLL loads, because the pane reads this once. Without it the
    // request path's stages are counted but never timed, and the assertions
    // below would be asserting on a switched-off instrument.
    Require(SetEnvironmentVariableW(L"FIREFOX_WINUI_PANE_WORK", L"1"), "enable native work accounting");
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
    Require((GetProcAddress(module, "WinUIPaneTestGetGeometryWork") != nullptr) == (argc == 3),
            "geometry-work export exists only in the test DLL");
    Require((GetProcAddress(module, "WinUIPaneTestGetEntryCalls") != nullptr) == (argc == 3),
            "entry-call export exists only in the test DLL");
    Require((GetProcAddress(module, "WinUIPaneTestGetStageCounter") != nullptr) == (argc == 3),
            "stage-counter export exists only in the test DLL");
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
    if (argc == 3 && (wcscmp(argv[2], L"--strip") == 0 || wcscmp(argv[2], L"--inline") == 0)) {
        if (wcscmp(argv[2], L"--strip") == 0) {
        CheckTabStrip(module, api, first);
        Require(SUCCEEDED(api.destroy(first)), "destroy strip before host");
        Require(!GetPropW(first, WINUI_PANE_SYSTEM_CAPTION_PROPERTY), "destroy releases native caption ownership");
        Require(SUCCEEDED(api.update(first, &rect, TRUE)), "recreate island for inline group checks");
        }
        CheckInlineGroups(module, api, first, wcscmp(argv[2], L"--inline") == 0);
        Require(SUCCEEDED(api.destroy(first)), "destroy inline groups before host");
        DestroyWindow(first);
        winrt::clear_factory_cache();
        Require(SUCCEEDED(api.shutdown()), "strip clean shutdown");
        CoUninitialize();
        puts("CHECKS PASSED: TabView layout, stable IDs, selection, menus, theme, hide/show, teardown");
        return 0;
    }
    if (argc == 3 && (wcscmp(argv[2], L"--drag") == 0 || wcscmp(argv[2], L"--reorder") == 0 || wcscmp(argv[2], L"--transfers") == 0 || wcscmp(argv[2], L"--continuation") == 0 || wcscmp(argv[2], L"--drag-close") == 0 ||
                      wcscmp(argv[2], L"--drag-change") == 0 || wcscmp(argv[2], L"--drag-offset") == 0 || wcscmp(argv[2], L"--drag-scale") == 0 || wcscmp(argv[2], L"--group-drop") == 0)) {
        auto dragTicks = [&] {
            auto counts = reinterpret_cast<HRESULT(WINAPI*)(const wchar_t*, uint64_t*)>(
                GetProcAddress(module, "WinUIPaneTestGetEntryCalls"));
            Require(counts != nullptr, "entry-call counters available");
            uint64_t value = 0;
            Require(SUCCEEDED(counts(L"dragTick", &value)), "read drag tick count");
            return value;
        };
        if (wcscmp(argv[2], L"--group-drop") == 0) {
            CheckTabStripDrag(module, api, first, false, false, false, true);
            CheckTabGroupHeaderShift(module, api, first);
        } else if (wcscmp(argv[2], L"--continuation") == 0) {
            CheckWindowContinuation(module, api, first);
        } else if (wcscmp(argv[2], L"--transfers") == 0) {
            CheckTabStripDetach(module, api, first, false, -1);
            CheckTabStripDetach(module, api, first, false, 1);
            CheckTabStripDetach(module, api, first);
            CheckTabStripDetach(module, api, first, true);
            CheckTabStripCrossWindowAdopt(module, api, first);
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::RejectDrop);
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::CloseAfterDrop);
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::CloseSourceAfterDrop);
        } else if (wcscmp(argv[2], L"--reorder") == 0) {
            CheckTabStripDrag(module, api, first);
            CheckTabStripDrag(module, api, first, true);
            CheckTabStripDrag(module, api, first, false, true);
            CheckTabStripDrag(module, api, first, true, true);
            CheckTabStripDrag(module, api, first, false, false, true);
            auto groupSlide = reinterpret_cast<HRESULT(WINAPI*)(HWND,uint32_t)>(GetProcAddress(module,"WinUIPaneTestGroupSlide"));
            Require(groupSlide && SUCCEEDED(groupSlide(first,0)), "start a real slide with grouping dwell pending");
            Require(SUCCEEDED(groupSlide(first,1)), "expired dwell leaves the running storyboard and reorder destination intact");
            HRESULT settled = E_PENDING;
            for (int i = 0; i < 100 && settled == E_PENDING; ++i) {
                Pump(); Sleep(10);
                settled = groupSlide(first,2);
            }
            Require(SUCCEEDED(settled), "after sliding, group only at the actual tab position without resetting its offset");
        } else if (wcscmp(argv[2], L"--drag-scale") == 0) {
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::Offset, 1.25f, 1.5f);
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::Offset, 1.5f, 2.f);
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::Offset, 2.f, 1.25f);
        } else if (wcscmp(argv[2], L"--drag-offset") == 0) {
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::Offset);
        } else if (wcscmp(argv[2], L"--drag-close") == 0) {
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::CloseTarget);
        } else if (wcscmp(argv[2], L"--drag-change") == 0) {
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::ChangeThenClose);
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::ChangeThenDrop);
        } else {
            CheckTabStripDrag(module, api, first);
            CheckTabStripDetach(module, api, first, false, -1);
            CheckTabStripDetach(module, api, first, false, 1);
            CheckTabStripDetach(module, api, first);
            CheckTabStripCrossWindowAdopt(module, api, first);
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::CloseTarget);
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::ChangeThenClose);
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::ChangeThenDrop);
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::Offset);
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::Offset, 1.25f, 1.5f);
            CheckTabStripCrossWindowAdopt(module, api, first, AdoptScenario::CloseSource);
        }
        // Section 8: no session work after a gesture completes. Every window
        // message still reaches TickDragSession, so the only way to show that
        // a finished drag costs nothing is to keep the messages coming and
        // require the count of ticks that had a session to advance to stand
        // still. A tick that returns on the pointer test is not counted, which
        // is what makes this assertion mean something.
        auto ticksAfterGestures = dragTicks();
        Require(ticksAfterGestures > 0, "the completed gestures did tick a live session");
        for (int i = 0; i < 40; ++i) {
            Require(PostMessageW(first, WM_NULL, 0, 0), "keep the message pump busy after the gesture");
            Pump();
            Sleep(5);
        }
        Require(dragTicks() == ticksAfterGestures, "a completed drag leaves no per-message session work behind");
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
        uint32_t selected{};
        WinUITab themeTabs[]{{11, L"Renamed tab"}, {22, L"Second tab"}};
        Require(SUCCEEDED(api.syncTabs(first, themeTabs, 2, 11, &selected)),
                "theme fixture");
        CheckThemesAndLongList(module, api, first);
        {
            // Its own endpoint: the panic check deliberately poisons a core and
            // drops the pane to its snapshot-only fallback, which must not leak
            // into the other fixtures.
            HWND core = CreateWindowExW(0, L"STATIC", L"Shell core host", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr,
                                        nullptr, GetModuleHandleW(nullptr), nullptr);
            Require(core && SUCCEEDED(api.update(core, &rect, TRUE)), "shell core island");
            ShowWindow(core, SW_SHOW);
            Pump();
            CheckShellCoreRequests(module, api, core);
            // That check queues a request while the endpoint is blocked and
            // drains it after unblocking. Its wait is the host holding the
            // queue, not the pane being slow to wake it, so it must be
            // classified apart from the stage's own timing rather than
            // averaged into it.
            {
                auto stageCounter = reinterpret_cast<HRESULT(WINAPI*)(const wchar_t*, const wchar_t*, uint64_t*)>(
                    GetProcAddress(module, "WinUIPaneTestGetStageCounter"));
                Require(stageCounter != nullptr, "stage counters available");
                uint64_t held = 0;
                Require(SUCCEEDED(stageCounter(L"wakeup", L"blocked", &held)), "read blocked wakeup samples");
                Require(held > 0, "a request the host held while blocked is not counted as wakeup latency");
                uint64_t ignored = 0;
                Require(stageCounter(L"noSuchStage", L"n", &ignored) == E_INVALIDARG, "reject an unknown stage");
                Require(stageCounter(L"wakeup", L"noSuchCounter", &ignored) == E_INVALIDARG,
                        "reject an unknown counter");
                Require(stageCounter(nullptr, L"n", &ignored) == E_POINTER, "reject a null stage name");
                Require(stageCounter(L"wakeup", nullptr, &ignored) == E_POINTER, "reject a null counter name");
                Require(stageCounter(L"wakeup", L"n", nullptr) == E_POINTER, "reject a null output");
            }
            Require(SUCCEEDED(api.destroy(core)), "shell core island teardown");
            DestroyWindow(core);
            Pump();
        }
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
    uint32_t requested = 99;
    // Since ABI v11 `requested` is the presentation-model revision this
    // snapshot produced, not a selection: selection travels on the request
    // path. `noSelectionRequest` is how "nothing was echoed back" is asserted
    // now -- on the transport selection actually uses.
    uint32_t modelRevision = 0;
    auto advancedRevision = [&] {
        bool advanced = requested > modelRevision;
        modelRevision = requested;
        return advanced;
    };
    auto noSelectionRequest = [&](HWND target) { return !NextRequest(api, target, 1, nullptr, 2).Size(); };
    // Close and selection share one queue now, and draining completes whatever
    // it takes -- so asking "was there a selection?" first would swallow the
    // close. One drain, both questions.
    auto closeWithoutSelection = [&](HWND target, uint32_t expected, int attempts = 40) {
        uint32_t closed = 0;
        bool selected = false;
        for (int attempt = 0; attempt < attempts && !closed; ++attempt) {
            auto batch = DrainRequests(api, target);
            if (batch.Size()) {
                for (auto value : batch.GetNamedArray(L"requests")) {
                    auto request = value.GetObject();
                    auto kind = uint32_t(request.GetNamedNumber(L"kind"));
                    if (kind == 3) closed = uint32_t(request.GetNamedNumber(L"tab"));
                    if (kind == 1) selected = true;
                }
                CompleteRequests(api, target, batch);
            }
            Pump();
            Sleep(5);
        }
        Require(closed == expected, "the clicked tab's close reaches the request queue");
        Require(!selected, "closing a background tab does not select its row");
    };
    WinUITab tabs[]{{11, L"First tab"}, {22, L"Unicode tab \u65e5\u672c\u8a9e"}};
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested)) &&
            advancedRevision() && noSelectionRequest(first),
        "initial tabs do not emit user selection, and the snapshot reports a model revision");
    tabs[0].title = L"Renamed tab";
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 22, &requested)) &&
            advancedRevision() && noSelectionRequest(first),
        "title and Firefox selection updates do not echo");
    WinUITab reordered[]{{22, L"Moved tab"}, {11, L"Renamed tab"}};
    Require(SUCCEEDED(api.syncTabs(first, reordered, 2, 22, &requested)),
            "reorder tabs");
    Require(SUCCEEDED(api.syncTabs(first, reordered, 1, 22, &requested)),
            "close tab");
    Require(api.syncTabs(first, tabs, 2, 99, &requested) ==
                E_INVALIDARG,
            "reject missing selected tab");
    Require(api.syncTabs(first, nullptr, 2, 0, &requested) ==
                E_INVALIDARG,
            "reject null snapshot");
    WinUITab duplicate[]{{11, L"One"}, {11, L"Two"}};
    Require(api.syncTabs(first, duplicate, 2, 11, &requested) ==
                E_INVALIDARG,
            "reject duplicate identities");
    Require(
        SUCCEEDED(api.syncTabs(first, nullptr, 0, 0, &requested)),
        "empty list");
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested)),
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
            CheckTabKeyboardNavigation(module, api, first);
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
    {
        auto request = NextRequest(api, first, 1);
        Require(request.Size() && request.GetNamedObject(L"action").GetNamedNumber(L"tab") == 22,
                "native keyboard selection reaches the Firefox bridge on the request path");
        Require(noSelectionRequest(first), "selection request is delivered exactly once");
    }
    key(VK_UP);
    {
        auto request = NextRequest(api, first, 1);
        Require(request.Size() && request.GetNamedObject(L"action").GetNamedNumber(L"tab") == 11,
                "the reverse keyboard selection is its own request");
    }
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested)),
        "restore after keyboard selection");
    WithTab(first, tabs[1].title, tabs[1].title, true);
    {
        auto request = NextRequest(api, first, 1);
        Require(request.Size() && request.GetNamedObject(L"action").GetNamedNumber(L"tab") == 22,
                "accessibility selection reaches the browser bridge on the request path");
    }
    Require(
        SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested)),
        "restore selection before close");
    WithTab(first, tabs[1].title, tabs[1].title, false, true);
    closeWithoutSelection(first, 22, 200);
    // Since ABI v14 a close is a request on the shared queue, not something
    // syncTabs hands back. It is answered by an outcome, and the pane holds it
    // as outstanding until then -- which is what stops a second click closing
    // the same tab twice while Firefox is still deciding.
    WithTab(first, tabs[1].title, tabs[1].title, false, true);
    closeWithoutSelection(first, 22);
    WithTab(first, tabs[1].title, tabs[1].title, false, true);
    closeWithoutSelection(first, 22);
    Require(SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested)), "restore close fixtures");
    // A close remains suppressed both while queued and while Firefox is
    // deciding. A rejected outcome releases it without needing a publication.
    WithTab(first, tabs[1].title, tabs[1].title, false, true);
    WithTab(first, tabs[1].title, tabs[1].title, false, true);
    auto heldClose = DrainRequests(api, first);
    Require(heldClose.Size() && heldClose.GetNamedArray(L"requests").Size() == 1 &&
                heldClose.GetNamedArray(L"requests").GetObjectAt(0).GetNamedNumber(L"kind") == 3,
            "duplicate queued close is suppressed by ShellCore");
    WithTab(first, tabs[1].title, tabs[1].title, false, true);
    CompleteRequests(api, first, heldClose, 2);
    Require(!NextRequest(api, first, 3, nullptr, 3).Size(), "duplicate in-flight close was not queued");
    WithTab(first, tabs[1].title, tabs[1].title, false, true);
    Require(NextRequest(api, first, 3).Size(), "rejected close can be retried without a snapshot");
    // Two different tabs, two requests, in the order they were clicked. The
    // queue never collapses them: each is a command the user gave.
    WithTab(first, tabs[0].title, tabs[0].title, false, true);
    WithTab(first, tabs[1].title, tabs[1].title, false, true);
    std::vector<uint32_t> closed;
    for (int attempt = 0; attempt < 40 && closed.size() < 2; ++attempt) {
        auto batch = DrainRequests(api, first);
        if (batch.Size()) {
            for (auto value : batch.GetNamedArray(L"requests")) {
                auto request = value.GetObject();
                if (uint32_t(request.GetNamedNumber(L"kind")) == 3)
                    closed.push_back(uint32_t(request.GetNamedNumber(L"tab")));
            }
            CompleteRequests(api, first, batch);
        }
        Pump();
        Sleep(5);
    }
    Require(closed == std::vector<uint32_t>({11, 22}), "both queued closes arrive, in order");
    for (int input : {2}) {
        WithTab(first, tabs[1].title, tabs[1].title, false, true, input);
        closeWithoutSelection(first, 22);
    }
    Require(api.syncTabs(first, tabs, 2, 11, nullptr) == E_POINTER, "reject null revision output");
    for (int input : {0, 2, 3}) {
        WithTab(first, L"FirefoxWinUINewTab", L"New tab", false, false, input);
        Require(NextRequest(api, first, 4, nullptr).Size(),
                "UIA/Space/Enter requests a new tab without inventing rows");
        Require(!NextRequest(api, first, 4, nullptr, 3).Size(),
                "new-tab request delivered exactly once");
        Require(noSelectionRequest(first), "requesting a new tab does not select a row");
    }
    WithTab(first, L"FirefoxWinUINewTab", L"New tab");
    WithTab(first, L"FirefoxWinUINewTab", L"New tab");
    // New tab is never superseded either: two clicks are two tabs.
    int newTabs = 0;
    for (int attempt = 0; attempt < 40 && newTabs < 2; ++attempt) {
        auto batch = DrainRequests(api, first);
        if (batch.Size()) {
            for (auto value : batch.GetNamedArray(L"requests"))
                if (uint32_t(value.GetObject().GetNamedNumber(L"kind")) == 4) ++newTabs;
            CompleteRequests(api, first, batch);
        }
        Pump();
        Sleep(5);
    }
    Require(newTabs == 2, "repeated new-tab requests are each delivered");
    if (argc == 3) {
        // Validate the stall classifier on a stall this test creates, rather
        // than trusting a counter that has only ever been printed. Queue a
        // request, then stop pumping for longer than the stall threshold: from
        // the pane's side that is exactly what a busy browser main thread looks
        // like, because Translate is what stops arriving.
        auto stageCounter = reinterpret_cast<HRESULT(WINAPI*)(const wchar_t*, const wchar_t*, uint64_t*)>(
            GetProcAddress(module, "WinUIPaneTestGetStageCounter"));
        Require(stageCounter != nullptr, "stage counters available");
        auto stalledWakeups = [&] {
            uint64_t value = 0;
            Require(SUCCEEDED(stageCounter(L"wakeup", L"stalled", &value)), "read stalled wakeup samples");
            return value;
        };
        auto stalledBefore = stalledWakeups();
        WithTab(first, L"FirefoxWinUINewTab", L"New tab");
        Sleep(300);  // no Pump: from the pane's side the message pump has stopped
        // The gap is observed by the next message to reach the pane's hook, so
        // post one rather than hoping something else arrives. Without this the
        // check passes only when an unrelated message happens to be pending.
        Require(PostMessageW(first, WM_NULL, 0, 0), "post a message to end the stall");
        Pump();
        Require(NextRequest(api, first, 4, nullptr).Size(), "the request queued before the stall still arrives");
        Require(stalledWakeups() == stalledBefore + 1,
                "a wait spent with the pump not reaching us is attributed to the stall, not to the wakeup path");
        // Presentation is measured by subscribing to the rendering callback,
        // which is the one thing section 6 forbids keeping. It must therefore
        // be armed by a change and dropped by the very next frame: one change
        // produces exactly one sample however long the pump runs afterwards.
        // If it stayed subscribed this count would climb with every frame.
        auto presentSamples = [&] {
            uint64_t value = 0;
            Require(SUCCEEDED(stageCounter(L"present", L"n", &value)), "read presentation samples");
            return value;
        };
        auto presentedBefore = presentSamples();
        Require(SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested)), "a change to present");
        for (int i = 0; i < 60; ++i) {
            Pump();
            Sleep(10);
        }
        Require(presentSamples() == presentedBefore + 1,
                "one change is one presentation sample, and the frame callback is not left subscribed");
    }
    WinUITab duplicates[]{{11, L"New tab"}, {22, L"New tab"}};
    Require(SUCCEEDED(api.syncTabs(first, duplicates, 2, 11, &requested)), "identical title snapshot");
    WithTab(first, L"New tab", L"New tab", false, true, 0, 2, 1);
    closeWithoutSelection(first, 22);
    Require(SUCCEEDED(api.syncTabs(first, tabs, 2, 11, &requested)),
            "restore titles after duplicate regression");
    if (argc == 3) {
        CheckContextMenu(module, api, first);
        Require(SUCCEEDED(
                    api.syncTabs(first, tabs, 2, 11, &requested)),
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
    if (argc == 3) {
        // Firefox calls update() and clip() from a requestAnimationFrame
        // whenever anything is dirty, so most calls repeat what the island
        // already has. Measured on a tab/urlbar/resize workload: 41% of rects
        // and 87% of exclusion sets were repeats, and re-issuing them cost
        // 45ms of the 106ms spent in these four operations. What must NOT be
        // skipped is putting the island back on top, because anything may have
        // restacked it without telling us -- so that is read from the window
        // rather than remembered.
        auto work = [&](uint64_t (&values)[6]) {
            auto counters = reinterpret_cast<HRESULT(WINAPI*)(uint64_t*, uint32_t)>(
                GetProcAddress(module, "WinUIPaneTestGetGeometryWork"));
            Require(counters != nullptr, "geometry-work counters available");
            Require(counters(values, 5) == E_INVALIDARG, "reject an undersized counter buffer");
            Require(counters(nullptr, 6) == E_POINTER, "reject a null counter buffer");
            Require(SUCCEEDED(counters(values, 6)), "read geometry-work counters");
        };
        uint64_t before[6]{}, after[6]{};
        Require(SUCCEEDED(api.update(first, &rect, TRUE)), "settle geometry before counting");
        work(before);
        Require(SUCCEEDED(api.update(first, &rect, TRUE)), "repeat the same geometry");
        work(after);
        Require(after[0] == before[0] + 1, "the repeat is still counted as an update");
        Require(after[1] == before[1], "a repeated rect is not handed to the island again");
        Require(after[3] == before[3], "a repeated visibility is not handed to the island again");
        RECT widened{rect.left, rect.top, rect.right + 8, rect.bottom};
        Require(SUCCEEDED(api.update(first, &widened, TRUE)), "change the geometry");
        work(before);
        Require(before[1] == after[1] + 1, "a changed rect is handed to the island");
        Require(SUCCEEDED(api.update(first, &rect, TRUE)), "restore geometry after counting");
        // A real change of visibility still has to reach the island; only a
        // repeat of the visibility it already has is dropped.
        work(before);
        Require(SUCCEEDED(api.update(first, &rect, FALSE)), "hide the island");
        work(after);
        Require(after[3] == before[3] + 1, "hiding is handed to the island");
        Require(SUCCEEDED(api.update(first, &rect, TRUE)), "show the island again");
        work(before);
        Require(before[3] == after[3] + 1, "showing again is handed to the island");
        // A restacked island is put back on top even though nothing else
        // changed -- the case the sibling compositor produces in Firefox.
        Require(SetWindowPos(compositor, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE),
                "restack the sibling above the island");
        work(before);
        Require(SUCCEEDED(api.update(first, &rect, TRUE)), "same geometry, restacked sibling");
        work(after);
        Require(after[2] == before[2] + 1, "a restacked island is put back on top with no other change");
        Require(GetWindow(first, GW_CHILD) == island, "island is above the compositor again");
        Require(after[1] == before[1], "restacking alone does not re-apply geometry");
        // The same exclusions against the same client rect are the same region.
        Require(SUCCEEDED(api.clip(first, cuts, 2)), "settle exclusions before counting");
        work(before);
        Require(SUCCEEDED(api.clip(first, cuts, 2)), "repeat the same exclusions");
        work(after);
        Require(after[4] == before[4] + 1, "the repeat is still counted as a clip");
        Require(after[5] == before[5], "a repeated region is not set on the window again");
        // Moving the island does not invalidate a region expressed in its own
        // coordinates, so the repeat still holds across a move.
        RECT movedRight{rect.left + 8, rect.top, rect.right + 8, rect.bottom};
        Require(SUCCEEDED(api.update(first, &movedRight, TRUE)), "move without resizing");
        work(before);
        Require(SUCCEEDED(api.clip(first, cuts, 2)), "same exclusions after a move");
        work(after);
        Require(after[5] == before[5], "moving the island leaves its region valid");
        Require(SUCCEEDED(api.update(first, &rect, TRUE)), "restore geometry after the move");
        work(before);
        Require(SUCCEEDED(api.clip(first, nullptr, 0)), "change the exclusions");
        work(after);
        Require(after[5] == before[5] + 1, "a changed region is set on the window");
        HRGN restored = CreateRectRgn(0, 0, 0, 0);
        Require(GetWindowRgn(island, restored) == SIMPLEREGION, "the changed region reached the window");
        DeleteObject(restored);
        // Every ABI entry point accounts for its own calls, which is what makes
        // native cost attributable at all. The count is what this pins; the
        // timing alongside it is only taken when the work flag is set, and a
        // test that asserted on it would be asserting on the clock.
        auto entryCalls = [&](const wchar_t* name) {
            auto counts = reinterpret_cast<HRESULT(WINAPI*)(const wchar_t*, uint64_t*)>(
                GetProcAddress(module, "WinUIPaneTestGetEntryCalls"));
            Require(counts != nullptr, "entry-call counters available");
            uint64_t value = 0;
            Require(SUCCEEDED(counts(name, &value)), "read an entry's call count");
            return value;
        };
        {
            auto counts = reinterpret_cast<HRESULT(WINAPI*)(const wchar_t*, uint64_t*)>(
                GetProcAddress(module, "WinUIPaneTestGetEntryCalls"));
            uint64_t ignored = 0;
            Require(counts(L"noSuchEntry", &ignored) == E_INVALIDARG, "reject an unknown entry name");
            Require(counts(nullptr, &ignored) == E_POINTER, "reject a null entry name");
            Require(counts(L"update", nullptr) == E_POINTER, "reject a null count output");
        }
        auto updatesBefore = entryCalls(L"update");
        auto clipsBefore = entryCalls(L"clip");
        Require(SUCCEEDED(api.update(first, &rect, TRUE)), "counted update");
        Require(SUCCEEDED(api.clip(first, nullptr, 0)), "counted clip");
        Require(entryCalls(L"update") == updatesBefore + 1, "update accounts for its own call");
        Require(entryCalls(L"clip") == clipsBefore + 1, "clip accounts for its own call");
        // A rejected call is still a call: it reached the entry point and spent
        // time there, so leaving it uncounted would understate the cost.
        auto rejectedBefore = entryCalls(L"update");
        Require(api.update(nullptr, &rect, TRUE) == E_INVALIDARG, "rejected update");
        Require(entryCalls(L"update") == rejectedBefore + 1, "a rejected call is still accounted for");
        Require(entryCalls(L"syncTabs") > 0, "tab publication is accounted for");
        Require(entryCalls(L"translate") > 0, "message translation is accounted for");
        // The request path's own latency, which is what milestones 3 to 6 were
        // for. Every request this suite has already driven should have passed
        // through all three stages: queued, drained, answered.
        auto wakeups = entryCalls(L"wakeup");
        auto executes = entryCalls(L"execute");
        auto totals = entryCalls(L"total");
        Require(wakeups > 0, "a queued request records how long the host took to drain it");
        Require(executes > 0, "a drained request records how long the host took to answer it");
        Require(totals > 0, "an answered request records its whole round trip");
        // A request cannot be answered before it was drained, nor drained
        // before it was queued, so these counts can only run in this order.
        Require(executes <= wakeups && totals <= wakeups,
                "no request is answered or completed without first being drained");
    }
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
        Require(SUCCEEDED(api.syncTabs(second, other, 1, 11, &requested)) &&
                    requested && noSelectionRequest(second),
                "independent window can reuse tab IDs and keeps its own model revision");
        if (cycle == 0) {
            ShowWindow(second, SW_SHOWNOACTIVATE);
            Pump();
            WithTab(second, L"FirefoxWinUINewTab", L"New tab");
            Require(!NextRequest(api, first, 4, nullptr, 3).Size(),
                    "second-window new-tab action cannot leak to first window");
            Require(NextRequest(api, second, 4, nullptr).Size(),
                    "new-tab request reaches its owning window's queue");
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
