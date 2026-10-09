#include "../../bridge/WinUIPaneApi.h"
// Deliberately rejects every handshake to exercise Firefox's pane fallback:
// the module loads and exports the entry point, but offers no compatible ABI.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneGetApi(uint32_t, uint32_t, WinUIPaneApi*) {
    return E_NOINTERFACE;
}
