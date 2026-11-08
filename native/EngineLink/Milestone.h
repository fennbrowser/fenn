#pragma once
#include <windows.h>
#include <stdint.h>
// Explicit development fixture, not the product pane ABI or a default launch option.
// Invoked on Gecko's owning thread after Firefox has accepted an isolated test profile.
// State values: 0 idle, 1 pending, 2 bound, 3 retired; the same ShellCore binding contract.
inline constexpr uint32_t FENN_SPLIT_FIXTURE_VERSION=1;
inline UINT FennSplitStallMessage() { static UINT id=RegisterWindowMessageW(L"Fenn.SplitFixture.Stall.v1"); return id; }
struct FennSplitStatus {
    uint64_t phase, shell, shellPid, proxy, page, authenticated, error, dropped, ready, closed;
};
using FennSplitStartFn=HRESULT(WINAPI*)(HWND,const wchar_t*,uint32_t);
using FennSplitStatusFn=HRESULT(WINAPI*)(FennSplitStatus*);
