#pragma once
#include <windows.h>
#include <string>
#include "shell_core.h"

namespace PaneLink {
inline std::wstring Text(const uint8_t* payload, size_t length) {
    std::wstring text(length / 2, L'\0');
    if (length) memcpy(text.data(), payload, length & ~size_t(1));
    return text;
}
template <class T>
T Read(const uint8_t* payload) {
    T value{};
    memcpy(&value, payload, sizeof(T));
    return value;
}

// Process host only; null endpoint/callback detaches before recovery destroys it.
void SetProcessShellTransport(ShellLinkEnd* link, bool (*send)(const uint8_t*, size_t) noexcept) noexcept;
void FlushShell() noexcept;
HRESULT ShellSend(HWND hwnd, uint16_t channel, uint16_t kind, uint64_t revision, const void* payload,
                  size_t length) noexcept;
void ShellSendSerial(uint32_t serial, uint16_t channel, uint16_t kind, uint64_t revision, const void* payload,
                     size_t length) noexcept;
}  // namespace PaneLink
