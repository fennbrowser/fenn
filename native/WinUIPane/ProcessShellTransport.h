#pragma once
#include "ShellLink.h"

// The process host borrows its endpoint from ShellMain/EngineRecovery and sends
// copied bytes through the authenticated pipe. It never falls back to loopback.
namespace PaneLink {
inline bool (*ProcessSend)(const uint8_t*,size_t) noexcept = nullptr;
void SetProcessShellTransport(ShellLinkEnd* link, bool (*send)(const uint8_t*,size_t) noexcept) noexcept {
    Native().link=link;
    ProcessSend=send;
}
void FlushShell() noexcept {
    auto& shell=Native();
    if (!shell.link) return;
    for (;;) {
        uint8_t* data{}; size_t length{};
        if (shell_link_take(shell.link,false,&data,&length)!=SHELL_STATUS_OK || !data) return;
        const bool sent=ProcessSend && ProcessSend(data,length);
        shell_link_free_frame(data,length);
        if (!sent) { shell_link_close(shell.link); return; }
    }
}
}
