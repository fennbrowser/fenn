#pragma once
struct IUnknown;
#include <windows.h>
#include <stdint.h>

// Owning-thread HWND marker: while present, Windows App SDK owns caption
// button input and Gecko must not redirect non-client presses to DOM buttons.
#define WINUI_PANE_SYSTEM_CAPTION_PROPERTY L"FennPane.SystemCaption.1"

// v16 retires the 240-px sidebar presentation and its `update` entry point:
// every pane is the TabView strip, created and positioned by updateTabStrip.
// v14 puts the last two commands -- close and new tab -- on the v11 request
// path. syncTabs therefore drains nothing: it loses its close/new-tab
// out-parameters and the reserved context/command pair with them, keeping only
// the presentation-model revision. No call in this ABI drains anything, and
// nothing in the shell polls.
// v13 put the address surface's own intents on the v11 request path as well,
// including the visibility of its rows -- the one thing about it that no
// browser event reports. syncAddress therefore returns nothing and loses its
// output buffer, and no entry point in this ABI is asked whether it has work.
// v12 put the TabView strip's own gestures (group toggle, reorder, detach,
// adopt) on the v11 request path, so syncTabMenu no longer returns anything and
// loses its output buffer: it is now a publication only, and nothing in the
// pane's protocol has to be asked whether it has work.
// v11 added the event-driven request path (setNotifySink / drainRequests /
// completeRequests) and incremental tab deltas (applyTabDelta), and retired
// syncTabs' selection out-parameter. v10 added an explicit strip-only host,
// retaining the sidebar entry point.
// v18 adds stateless native client-area allocation for browser layout.
// v38 adds the independent host's window title.
// v39 adds its page cursor and touch registration, and pen messages to the endpoint.
// v40 retires the native host on Gecko's thread (attach, detach, adopt) and adds window-state
// requests from Gecko's hidden proxy window to the shell.
#define WINUI_PANE_ABI_VERSION 40u

// Scalar key transport, translated exactly once by the Gecko endpoint. The endpoint is nsWindow's
// own hidden window (the proxy), which stands for the shell on Gecko's thread.
#define WINUI_PANE_ENDPOINT_KEY_OFFSET WM_APP
#define WINUI_PANE_ENDPOINT_PROPERTY L"Fenn.GeckoEndpoint"
inline constexpr int WinUIPaneModifierKeys[] = {
    VK_SHIFT, VK_CONTROL, VK_MENU, VK_LSHIFT, VK_RSHIFT, VK_LCONTROL,
    VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN, VK_CAPITAL, VK_NUMLOCK, VK_SCROLL};

// The pane is built framework-dependent on the shared Windows App SDK runtime
// (Microsoft.WindowsAppRuntime.2, at least 2.5.1.0). A packaged host declares it
// as a package dependency; an unpackaged host joins it with the bootstrapper
// (MddBootstrapInitialize2, which ignores the minor version from 2.0) before
// loading the pane or any other Windows App SDK DLL. Keep the minimum in step
// with native/WinUIPane/packages.config.
#define WINUI_PANE_RUNTIME_MAJOR_MINOR 0x00020000u
#define WINUI_PANE_RUNTIME_MIN_VERSION 0x0002000500010000ull
#define WINUI_PANE_RUNTIME_BOOTSTRAP_DLL L"Microsoft.WindowsAppRuntime.Bootstrap.dll"

// Calls originate on the browser STA. The experimental independent host
// dispatches UI work to its native STA; notify sinks must enqueue browser work.
// Coordinates are client physical
// pixels. The module remains loaded until process exit, including after shutdown.
// Legacy availability bits for tab snapshots and invalidation. Menu action IDs
// are opaque strings in the published state; native code assigns no command
// semantics.
enum WinUITabCommand : uint32_t {
    WinUITabReload = 1u, WinUITabDuplicate = 2u,
    WinUITabPin = 4u, WinUITabUnpin = 8u,
};
constexpr uint32_t WinUITabPinned = 16u; // State, not an executable command.
struct WinUITab {
    uint32_t id; // Nonzero, window-local identity; never reused during a pane lifetime.
    const wchar_t* title; // Borrowed only for the duration of syncTabs.
    uint32_t commands{}; // Availability plus Pinned state; never both Pin and Unpin.
};

// Wakeup callback for the v11 request path. The pane calls this when, and only
// when, its queue transitions from "nothing outstanding" to "work waiting": at
// most one call is outstanding per endpoint until the host drains.
//
// It is called on the owning STA, frequently from inside a XAML callback. The
// implementation may only *schedule* work -- post a task to the browser's own
// event loop -- and must never run browser code on that stack. `context` is the
// value passed to setNotifySink and is never interpreted by the pane.
//
// Return FALSE if the wakeup could not be scheduled (shutdown, a failed task
// post). The pane then puts its queue back into a state where the next request
// schedules a fresh wakeup, rather than waiting forever for one that will never
// arrive. Returning FALSE is not an error the host must otherwise handle.
using WinUIPaneNotifyFn = BOOL (WINAPI*)(void* context);

struct WinUIChromeLayout {
    BOOL fits;
    RECT strip, navigation, content;
};

// v27: bounded text-window base and asynchronous range geometry (event kind 6).
// Borrowed input buffers are copied during publication; events are copied
// into caller-owned storage. Only the native owner accesses the TSF objects.
struct WinUIPageInput {
    uint64_t session, revision, acknowledgment;
    uint32_t enabled, start, end, length, reversed, textBase;
    const wchar_t* text;
    RECT caret, editor;
    uint32_t scopeCount;
    int32_t scopes[8];
};
// v28: bounded composition clauses and display attributes.
struct WinUIPageClause {
    uint32_t start,end,kind,flags,line,bold,foreground,background,underline;
};
struct WinUIPageInputEvent {
    uint64_t session, edit, revision, request;
    uint32_t kind, start, end, caret, length;
    uint32_t clauseCount;
    WinUIPageClause clauses[64];
};
struct WinUIPageGeometry {
    uint64_t session, revision, edit, request;
    uint32_t start, end, valid, clipped;
    RECT bounds;
};
enum WinUIPaneWindowRequestKind : uint32_t {
    // rect and the SWP_NOMOVE, SWP_NOSIZE, SWP_FRAMECHANGED and SWP_NOZORDER bits of flags; without
    // SWP_NOZORDER, value is HWND_TOPMOST or HWND_NOTOPMOST
    WinUIPaneWindowPosition = 1,
    WinUIPaneWindowShow = 2,     // value is a ShowWindow command
    WinUIPaneWindowStyle = 3,    // value replaces the bits of WinUIPaneShellStyleMask
    WinUIPaneWindowEnable = 4,   // value is TRUE or FALSE
    WinUIPaneWindowIcon = 5,     // value is an HICON the shell takes over; flags is ICON_BIG or ICON_SMALL
};
inline constexpr LONG_PTR WinUIPaneShellStyleMask =
    WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
struct WinUIPaneWindowRequest {
    uint32_t kind;
    uint32_t flags;
    RECT rect;
    LONG_PTR value;
    volatile LONG* pending;
};
struct WinUIPaneApi {
    uint32_t size;
    uint32_t version;
    HRESULT (WINAPI* focus)(HWND, BOOL);
    HRESULT (WINAPI* destroy)(HWND);
    BOOL (WINAPI* translate)(MSG*);
    HRESULT (WINAPI* shutdown)();
    // Publication only since v14. v17 adds Firefox's nonzero revision input
    // after selected; all earlier fields remain. The output acknowledges that
    // exact stamp, which the next applyTabDelta names as its base.
    // Complete replay is a no-op; a conflicting or stale stamp is refused.
    // Native actions travel through the request path. A close never removes
    // its row or chooses a successor before Firefox publishes the result.
    HRESULT (WINAPI* syncTabs)(HWND, const WinUITab*, uint32_t, uint32_t, uint32_t, uint32_t*);
    // Versioned JSON messages, copied during the call; output capacity is in wchar_t.
    // Publication only since v13; intents arrive through drainRequests.
    HRESULT (WINAPI* syncAddress)(HWND, const wchar_t*);
    // Exclusions are pane-local physical pixels; an empty list restores the pane.
    HRESULT (WINAPI* clip)(HWND, const RECT*, uint32_t);
    // Menu protocol v2: published state, copied as JSON. The published state
    // carries tab identities and the selected set only; a tab's command
    // description is published on demand under the "menu" key, in answer to a
    // drained menu-open request. Since v12 this returns nothing: every native
    // intent, including the strip's own gestures, arrives through
    // drainRequests.
    HRESULT (WINAPI* syncTabMenu)(HWND, const wchar_t*);
    // Creates, positions, shows or hides the HWND's strip island. Client
    // physical pixels; hiding a window that has no island creates nothing.
    HRESULT (WINAPI* updateTabStrip)(HWND, const RECT*, BOOL);

    // --- v11: event-driven requests -------------------------------------
    // Register (or, with a null callback, clear) the wakeup for one endpoint.
    // Clearing is part of teardown: the pane makes no further calls afterwards.
    HRESULT (WINAPI* setNotifySink)(HWND, WinUIPaneNotifyFn, void*);
    // Take one bounded batch of native requests as JSON:
    //   {"version":1,"generation":N,"legacy":bool,
    //    "requests":[{"id":..,"kind":..,"tab":..,"revision":..,"action":{...}}]}
    // "kind" is 1 select-tab, 2 tab-menu transition, 3 close tab, 4 new tab,
    // 5 strip gesture, 6 address intent, 7 search-mode intent, 8 address
    // visibility report, 9 tab hover-card request. Every native intent appears here and nowhere else, so
    // none of them has two executors.
    // Returns S_FALSE with an empty output when there was nothing to take, or
    // when a drain is already open. Every batch must be closed by
    // completeRequests, including when execution is refused; otherwise the
    // endpoint stays busy.
    HRESULT (WINAPI* drainRequests)(HWND, wchar_t*, uint32_t);
    // Close the drain with explicit per-request outcomes:
    //   {"version":1,"generation":N,"outcomes":[{"id":..,"outcome":1..4}],
    //    "blocked":bool}
    // "blocked" tells the pane the host cannot execute right now (a nested
    // modal loop, a command already running); the pane then waits for an
    // unblocking completeRequests rather than re-posting notifications.
    HRESULT (WINAPI* completeRequests)(HWND, const wchar_t*);
    // Apply an incremental presentation update:
    //   {"version":1,"base":R,"selected":id?,"titles":[{"id":..,"title":".."}]}
    // "base" must equal the pane's current model revision; the out-parameter
    // receives the revision after applying, for the next delta's base.
    // E_NOT_VALID_STATE means the producer and the pane have diverged: answer
    // with one syncTabs snapshot, never with a retry loop. S_FALSE means the
    // delta applied but the pane wants a snapshot for an unrelated reason.
    HRESULT (WINAPI* applyTabDelta)(HWND, const wchar_t*, uint32_t*);
    // v15: independent toolbar address island; the request endpoint is shared.
    HRESULT (WINAPI* updateAddress)(HWND, const RECT*, BOOL);
    HRESULT (WINAPI* focusAddress)(HWND, BOOL);
    // No control mutation. Invalid requests leave output untouched; fits=false
    // requires the browser to keep/restore its existing layout.
    HRESULT (WINAPI* measureChromeLayout)(HWND, uint32_t, BOOL, uint32_t, WinUIChromeLayout*);
    // v19: opt-in native outer HWND; Gecko remains its logical widget adapter.
    HRESULT (WINAPI* createWindowHost)(DWORD, const wchar_t*, DWORD, const RECT*, HWND, HINSTANCE, HWND*);
    HRESULT (WINAPI* destroyWindowHost)(HWND);
    // v21: early loading island on the original native HWND, owning thread only.
    HRESULT (WINAPI* showStartupSurface)(HWND);
    HRESULT (WINAPI* dismissStartupSurface)(HWND);
    // v23: selected profile path for saved placement; borrowed during Start.
    // Development independent-thread host. Start before Gecko/other pane calls.
    // Profile placement/cache and optional Firefox-resolved presentation JSON.
    // Both strings are borrowed for the call; presentation is bounded to 32768
    // UTF-16 units and copied before dispatch to the native owner thread.
    HRESULT (WINAPI* startIndependentHost)(const wchar_t*, const wchar_t*, HWND*);
    HRESULT (WINAPI* bindIndependentEndpoint)(HWND, uintptr_t, HWND);
    HRESULT (WINAPI* cancelIndependentClose)(HWND, uint64_t, uint64_t);
    // v24: agile COM identity only. The provider must never wait for Gecko on
    // the HWND thread. Native retains a reference until cleared or destroyed.
    HRESULT (WINAPI* setIndependentAccessibility)(HWND, IUnknown*);
    HRESULT (WINAPI* publishIndependentInput)(HWND, const WinUIPageInput*);
    HRESULT (WINAPI* drainIndependentInput)(HWND, WinUIPageInputEvent*, wchar_t*, uint32_t);
    HRESULT (WINAPI* commitIndependentInput)(HWND, BOOL);
    HRESULT (WINAPI* publishIndependentGeometry)(HWND, const WinUIPageGeometry*);
    // v33 adds acknowledged restored/local editor selection leases (kinds 4/5).
    // Operation 13 releases the editor after terminal draft cleanup.
    // Operation 15 probes early ownership; 16 acknowledges connected presentation.
    // Operation 14 retires an untouched initial placeholder to the selected browser.
    // Operation 12 explicitly attaches the retained startup presenter.
    // Operation 11 publishes bounded browser-row identity snapshots.
    // Decimal u64 identities in JSON; borrowed buffers only for this call.
    // Operations 17/18 enable exact-state draft adoption and finish the retained presenter.
    HRESULT (WINAPI* startupTransfer)(HWND, const wchar_t*, wchar_t*, uint32_t);
    // v38: copied before return; never waits unless the bounded queue refuses it.
    HRESULT (WINAPI* setIndependentTitle)(HWND, const wchar_t*);
    // v39: the native owner destroys an owned cursor, including when this call fails.
    HRESULT (WINAPI* setIndependentCursor)(HWND, HCURSOR, BOOL);
    HRESULT (WINAPI* registerIndependentTouch)(HWND);
    // v40: copied and applied to the shell in order with other posted calls, never waited on unless
    // the bounded queue refuses it. The shell decrements pending once applied, then reports its state.
    HRESULT (WINAPI* requestIndependentWindow)(HWND, const WinUIPaneWindowRequest*);
};
// Pen data can only be read on the thread that owns the window, so the endpoint gets a copy.
// Sent as WM_POINTER*+WINUI_PANE_ENDPOINT_KEY_OFFSET with this as wParam and the original lParam.
// From GetProcessHeap; the receiver frees it and decrements pending.
struct WinUIPanePenMessage {
    WPARAM wParam;
    POINTER_PEN_INFO pen;
    volatile LONG* pending;
};
using WinUIPaneGetApiFn = HRESULT (WINAPI*)(uint32_t, uint32_t, WinUIPaneApi*);
