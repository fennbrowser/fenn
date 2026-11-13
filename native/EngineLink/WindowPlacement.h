#pragma once

// Fixed process-wire layout; HWND ownership and the actual placement calls stay
// in the Win32 adapter. Rust validates the bytes before either peer reads them.
namespace EnginePlacement {
struct State {
    RECT bounds{}, normal{};
    POINT minimum{}, maximum{};
    uint32_t flags{}, show{}, foreground{}, reserved{};
};
static_assert(sizeof(State) == 64);
struct Create {
    uint64_t proxy, identity;
    RECT bounds;
    uint32_t flags, reserved;
    State placement;
};
static_assert(sizeof(Create) == 104);
inline bool Read(HWND window, State& state) {
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(window, &placement) || !GetWindowRect(window, &state.bounds)) return false;
    state.normal = placement.rcNormalPosition;
    state.minimum = placement.ptMinPosition;
    state.maximum = placement.ptMaxPosition;
    state.flags = placement.flags;
    state.show = placement.showCmd;
    state.foreground = GetForegroundWindow() == window;
    return true;
}
inline bool Restore(HWND window, const State& state) {
    if (!state.show) return false;
    WINDOWPLACEMENT placement{sizeof(placement)};
    placement.flags = state.flags;
    placement.showCmd = state.show == SW_SHOWNORMAL      ? SW_SHOWNOACTIVATE
                        : state.show == SW_SHOWMINIMIZED ? SW_SHOWMINNOACTIVE
                                                         : state.show;
    placement.rcNormalPosition = state.normal;
    placement.ptMinPosition = state.minimum;
    placement.ptMaxPosition = state.maximum;
    // SetWindowPlacement otherwise activates maximized windows too. Rebuild all
    // siblings without activation, then request focus only for the former owner.
    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_NOACTIVATE);
    const bool restored = SetWindowPlacement(window, &placement) != FALSE;
    SetWindowLongPtrW(window, GWL_EXSTYLE, style);
    if (restored && state.foreground) SetForegroundWindow(window);
    return restored;
}
}  // namespace EnginePlacement
