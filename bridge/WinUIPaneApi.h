#pragma once
#include <windows.h>
#include <stdint.h>

// v10 adds an explicit strip-only host, retaining the sidebar entry point.
#define WINUI_PANE_ABI_VERSION 10u

// All calls run on the browser's owning STA. Coordinates are client physical
// pixels. The module remains loaded until process exit, including after shutdown.
// Legacy availability bits for tab snapshots and invalidation. Menu action IDs
// are opaque strings on syncTabMenu; native code assigns no command semantics.
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

struct WinUIPaneApi {
    uint32_t size;
    uint32_t version;
    HRESULT (WINAPI* update)(HWND, const RECT*, BOOL);
    HRESULT (WINAPI* focus)(HWND, BOOL);
    HRESULT (WINAPI* destroy)(HWND);
    BOOL (WINAPI* translate)(MSG*);
    HRESULT (WINAPI* shutdown)();
    // Copies a snapshot and drains one close or new-tab request, then selection.
    // The final two outputs are reserved and always zero since ABI v8.
    // Only one action is drained per snapshot; Firefox revalidates availability.
    // A close request does not remove a row or choose a successor. The next
    // snapshot acknowledges it: do not sync while Firefox is deciding closure.
    HRESULT (WINAPI* syncTabs)(HWND, const WinUITab*, uint32_t, uint32_t, uint32_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*);
    // Versioned JSON messages, copied during the call; output capacity is in wchar_t.
    HRESULT (WINAPI* syncAddress)(HWND, const wchar_t*, wchar_t*, uint32_t);
    // Exclusions are pane-local physical pixels; an empty list restores the pane.
    HRESULT (WINAPI* clip)(HWND, const RECT*, uint32_t);
    // Menu protocol v2: published state and bounded queued input, copied as JSON.
    HRESULT (WINAPI* syncTabMenu)(HWND, const wchar_t*, wchar_t*, uint32_t);
    // Same lifetime/geometry contract as update; one presentation per HWND.
    HRESULT (WINAPI* updateTabStrip)(HWND, const RECT*, BOOL);
};
using WinUIPaneGetApiFn = HRESULT (WINAPI*)(uint32_t, uint32_t, WinUIPaneApi*);
