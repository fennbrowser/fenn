#pragma once
#include <windows.h>
#include <stdint.h>

// ABI v1: caller supplies a live HWND on its owning thread in this process.
// No ownership transfer, allocation, callback, retained HWND or XAML activation.
#define WINUI_BRIDGE_ABI_VERSION 1u
typedef struct WinUIBridgeInfo {
  uint32_t size;
  uint32_t abiVersion;
  uint32_t processId;
  uint32_t threadId;
  int32_t apartmentType;
  int32_t apartmentQualifier;
} WinUIBridgeInfo;

typedef HRESULT(WINAPI* WinUIBridgeProbeFn)(HWND, uint32_t, uint32_t,
                                         WinUIBridgeInfo*);

#ifdef __cplusplus
extern "C" {
#endif
HRESULT WINAPI WinUIBridgeProbe(HWND window, uint32_t requestedVersion,
                               uint32_t infoSize, WinUIBridgeInfo* info);
#ifdef __cplusplus
}
#endif
