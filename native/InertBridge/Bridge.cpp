#include "../../bridge/WinUIBridgeApi.h"
#include <objbase.h>

extern "C" HRESULT WINAPI WinUIBridgeProbe(HWND window, uint32_t requestedVersion, uint32_t infoSize,
                                           WinUIBridgeInfo* info) {
    if (requestedVersion != WINUI_BRIDGE_ABI_VERSION) return E_NOINTERFACE;
    if (!info || infoSize != sizeof(WinUIBridgeInfo)) return E_INVALIDARG;
    DWORD processId = 0;
    DWORD threadId = GetWindowThreadProcessId(window, &processId);
    if (!threadId || !IsWindow(window)) return E_HANDLE;
    if (processId != GetCurrentProcessId() || threadId != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    APTTYPE apartment;
    APTTYPEQUALIFIER qualifier;
    HRESULT hr = CoGetApartmentType(&apartment, &qualifier);
    if (FAILED(hr)) return hr;
    *info = {sizeof(WinUIBridgeInfo),         WINUI_BRIDGE_ABI_VERSION,       processId, threadId,
             static_cast<int32_t>(apartment), static_cast<int32_t>(qualifier)};
    return S_OK;
}
