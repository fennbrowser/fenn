#pragma once
#include <windows.h>
#include <stdint.h>

// Owning-thread HWND marker: while present, Windows App SDK owns caption
// button input and Gecko must not redirect non-client presses to DOM buttons.
#define WINUI_PANE_SYSTEM_CAPTION_PROPERTY L"FirefoxWinUIPane.SystemCaption.1"

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
#define WINUI_PANE_ABI_VERSION 14u

// All calls run on the browser's owning STA. Coordinates are client physical
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

struct WinUIPaneApi {
    uint32_t size;
    uint32_t version;
    HRESULT (WINAPI* update)(HWND, const RECT*, BOOL);
    HRESULT (WINAPI* focus)(HWND, BOOL);
    HRESULT (WINAPI* destroy)(HWND);
    BOOL (WINAPI* translate)(MSG*);
    HRESULT (WINAPI* shutdown)();
    // Copies a snapshot and drains one close or new-tab request.
    // Since v11 the first output is the pane's presentation-model revision
    // after this snapshot, which the next applyTabDelta must name as its base;
    // selection travels on the request path below instead, so no command
    // reaches Firefox through both executors. The final two outputs are
    // reserved and always zero.
    // Only one action is drained per snapshot; Firefox revalidates availability.
    // A close request does not remove a row or choose a successor. The next
    // snapshot acknowledges it: do not sync while Firefox is deciding closure.
    // Publication only since v14; the single out-parameter is the pane's
    // presentation-model revision after this snapshot.
    HRESULT (WINAPI* syncTabs)(HWND, const WinUITab*, uint32_t, uint32_t, uint32_t*);
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
    // Same lifetime/geometry contract as update; one presentation per HWND.
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
    // visibility report. Every native intent appears here and nowhere else, so
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
};
using WinUIPaneGetApiFn = HRESULT (WINAPI*)(uint32_t, uint32_t, WinUIPaneApi*);
