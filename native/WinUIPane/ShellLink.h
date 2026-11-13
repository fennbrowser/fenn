#pragma once
#include "ShellLinkPresentation.h"

// Owning-thread shell endpoint and frame sends, shared by both hosts.
// ShellCore owns protocol validation, credits, coalescing and serial identity.
// The host-specific transport implements FlushShell; this layer never chooses one.
namespace PaneLink {
inline HRESULT Result(ShellLinkSend outcome) noexcept {
    switch (outcome) {
        case SHELL_LINK_SEND_QUEUED:
        case SHELL_LINK_SEND_COALESCED:
            return S_OK;
        case SHELL_LINK_SEND_OVERFLOW:
            return E_OUTOFMEMORY;
        case SHELL_LINK_SEND_CLOSED:
            return RO_E_CLOSED;
        default:
            return E_INVALIDARG;
    }
}

struct Shell {
    ShellLinkEnd* link{};
};
inline Shell& Native() {
    static auto* shell = new Shell;
    return *shell;
}
HRESULT ShellSend(HWND hwnd, uint16_t channel, uint16_t kind, uint64_t revision, const void* payload,
                  size_t length) noexcept {
    auto& shell = Native();
    if (!shell.link) return RO_E_CLOSED;
    uint32_t serial{};
    if (shell_link_serial(shell.link, reinterpret_cast<uintptr_t>(hwnd), false, &serial) != SHELL_STATUS_OK)
        return E_INVALIDARG;
    ShellLinkSend outcome{};
    if (shell_link_send(shell.link, serial, channel, kind, revision, static_cast<const uint8_t*>(payload), length,
                        &outcome) != SHELL_STATUS_OK)
        return E_UNEXPECTED;
    FlushShell();
    return Result(outcome);
}
// For a window this side never adopted, so it has no handle here.
void ShellSendSerial(uint32_t serial, uint16_t channel, uint16_t kind, uint64_t revision, const void* payload,
                     size_t length) noexcept {
    auto& shell = Native();
    if (!shell.link) return;
    ShellLinkSend outcome{};
    shell_link_send(shell.link, serial, channel, kind, revision, static_cast<const uint8_t*>(payload), length,
                    &outcome);
    FlushShell();
}
inline void ShellRefuseSerial(uint32_t serial, uint16_t surface) noexcept {
    auto& shell = Native();
    if (!shell.link) return;
    ShellLinkSend outcome{};
    shell_link_send(shell.link, serial, SHELL_FRAME_WINDOW, 3, 0, reinterpret_cast<const uint8_t*>(&surface),
                    sizeof(surface), &outcome);
    FlushShell();
}
// A destroyed window: frames still on their way for it are dropped, and its handle can be reused.
inline void ShellRetire(HWND hwnd) noexcept {
    auto& shell = Native();
    uint32_t serial{};
    if (shell.link &&
        shell_link_serial(shell.link, reinterpret_cast<uintptr_t>(hwnd), false, &serial) == SHELL_STATUS_OK)
        shell_link_retire(shell.link, serial);
}
}  // namespace PaneLink
