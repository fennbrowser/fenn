#pragma once
#include <windows.h>
#include "shell_core.h"

// The host owns HWND measurement; ShellCore owns row allocation. This is the
// allocation seam is shared by the opt-in Gecko integration and standalone
// host. No DOM geometry is read here.
inline HRESULT MeasureNativeChromeLayout(HWND host, uint32_t stripHeight,
                                         bool navigation, uint32_t contentInset,
                                         ShellChromeLayout* output) noexcept {
    if (!output) return E_POINTER;
    if (!host || !IsWindow(host)) return E_INVALIDARG;
    if (GetWindowThreadProcessId(host, nullptr) != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    RECT client{};
    if (!GetClientRect(host, &client)) return HRESULT_FROM_WIN32(GetLastError());
    ShellChromeLayoutInput input{client.right - client.left, client.bottom - client.top,
                                GetDpiForWindow(host), stripHeight, navigation, contentInset};
    const auto status = shell_core_chrome_layout(&input, output);
    return status == SHELL_STATUS_OK ? S_OK : status == SHELL_STATUS_INVALID_ARGUMENT ? E_INVALIDARG : E_FAIL;
}
