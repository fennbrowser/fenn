#pragma once
#include "../WinUIPane/ChromeLayout.h"
#include "../WinUIPane/WindowTitleFrame.h"
#include "IconPixels.h"
#include "ProcessLaunch.h"
#include "TestWorkGate.h"
#include "WindowPlacement.h"

// Engine-local Win32 callbacks/handles. Frame identity, bounds and scheduling remain in ShellCore.
namespace ProductLink {
inline constexpr UINT PageReadyMessage = WM_APP + 925;
inline constexpr UINT RecoverMessage = WM_APP + 927;
inline constexpr UINT InputReadyMessage = WM_APP + 928;
inline constexpr UINT StockFallbackMessage = WM_APP + 929;
inline constexpr UINT PeerRetiredMessage = WM_APP + 931;
struct Window {
    HWND proxy{}, shell{};
    uintptr_t identity{};
    ShellPipeBinding binding{};
    ULONGLONG deadline{};
    bool pageReady{}, recoverable{}, usable{}, needsReadiness{};
    ShellRecoveryReadiness readiness{};
    RECT recoveryRect{};
    uint32_t flags{};
    EnginePlacement::State placement{};
    std::wstring mailbox;
    WinUIPaneNotifyFn notify{};
    void* notifyContext{};
};
struct Pending {
    uint32_t serial;
    volatile LONG* counter;
};
inline std::map<uint32_t, Window> windows;
inline std::map<uint64_t, Pending> pending;
inline std::map<uint64_t, HWND> startup;
inline std::unique_ptr<EnginePipe::Connection> pipe;
inline std::unique_ptr<EnginePipe::ProcessLaunch> launch;
inline ShellLinkEnd* link{};
inline HWND wake{};
inline DWORD thread{}, peer{};
inline uint32_t generation{};
inline uint64_t request{};
#ifdef FENN_ENGINE_LINK_TESTING
inline std::vector<uint8_t> heldWorkFrame;
#endif
inline ShellRecovery* recovery{};
inline ShellRecoveryHistory recoveryHistory{};
inline void ImportRecoveryHistory() {
    if (pipe && pipe->TakeRecoveryHistory(recoveryHistory)) {
        if (!recovery) recovery = shell_recovery_create();
        if (!shell_recovery_import(recovery, recoveryHistory.shell, GetTickCount64())) pipe->Break();
    }
}
inline std::thread recoveryCleanup;
inline bool recovering{}, rebuildingWindows{}, engineReconnected{};
inline EnginePlacement::State rebuildingPlacement{};
inline bool peerRetirementAttempted{};
inline std::wstring recoveryProfile, recoveryPresentation;
// Keep the HWND inventory while Start may finish synchronously, before Create
// has rebuilt the serial map. A failed launch still owes these proxies fallback.
inline std::vector<HWND> replacementProxies;
inline HRESULT WINAPI Start(const wchar_t*, const wchar_t*, uint32_t) noexcept;
inline HRESULT WINAPI Create(HWND, uintptr_t, const RECT*, uint32_t) noexcept;
inline WinUIPaneStartupReplyFn startupSink{};
inline void* startupContext{};
inline WinUIPaneProcessNotifyFn sink{};
inline void* sinkContext{};
inline bool confirmed{}, failed{}, stopped{}, pumping{}, everBound{};
inline void Pump() noexcept;
// Before completion, publications remain in ShellCore's bounded/coalescing outbox.
inline void Flush() {
    if (pipe && link && !launch) EnginePipe::Flush(*pipe, link, 0);
}
inline void CancelLaunch() {
    if (launch) {
        launch->CancelAndJoin();
        launch.reset();
    }
}
inline Window* Find(HWND hwnd) {
    for (auto& [serial, window] : windows)
        if (window.proxy == hwnd || window.shell == hwnd) return &window;
    return nullptr;
}
inline uint32_t Serial(const Window& window) {
    return window.binding.serial;
}
inline HRESULT Send(Window& window, uint16_t channel, uint16_t kind, const void* data = nullptr, size_t length = 0,
                    uint64_t revision = 0) {
    if (failed || stopped || window.binding.phase == 3) return RO_E_CLOSED;
    const bool sent = EnginePipe::Send(link, Serial(window), channel, kind, data, length, revision);
    Flush();
    return sent ? S_OK : E_OUTOFMEMORY;
}
inline HRESULT Text(HWND hwnd, uint16_t channel, uint16_t kind, const wchar_t* value) noexcept {
    auto* window = Find(hwnd);
    if (!window || !value) return E_INVALIDARG;
    const size_t length = wcsnlen(value, 1048577);
    if (length > 1048576) return E_INVALIDARG;
    return Send(*window, channel, kind, value, length * sizeof(wchar_t));
}
inline HRESULT WINAPI SetTitle(HWND hwnd, const wchar_t* title) noexcept {
    auto* window = Find(hwnd);
    if (!window || !title) return E_INVALIDARG;
    const auto payload = WindowTitleFrame::Encode(title, 1048577);
    if (payload.bytes > 1048576 * sizeof(wchar_t)) return E_INVALIDARG;
    return Send(*window, SHELL_FRAME_WINDOW, WindowTitleFrame::Kind, payload.data, payload.bytes);
}
inline void Settle(uint32_t serial) {
    for (auto it = pending.begin(); it != pending.end();) {
        if (it->second.serial != serial) {
            ++it;
            continue;
        }
        if (it->second.counter) InterlockedDecrement(it->second.counter);
#ifdef FENN_ENGINE_LINK_TESTING
        EnginePipe::Trace("test-pending-settled", serial, uint32_t(it->first));
#endif
        it = pending.erase(it);
    }
    for (auto it = startup.begin(); it != startup.end();) {
        auto* window = Find(it->second);
        if (!window || Serial(*window) != serial) {
            ++it;
            continue;
        }
        const auto id = it->first;
        it = startup.erase(it);
#ifdef FENN_ENGINE_LINK_TESTING
        EnginePipe::Trace("test-startup-settled", serial, uint32_t(id));
#endif
        if (startupSink) startupSink(startupContext, id, RO_E_CLOSED, nullptr);
    }
}
inline void Retire(Window& window, bool destroy) {
    if (window.binding.phase == 3) return;
    EnginePipe::Trace("retire", Serial(window), destroy ? 1 : 0);
    if (window.pageReady && sink) sink(sinkContext, window.proxy, window.shell, 4, S_OK);
    if (destroy) Send(window, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_DESTROY_HOST);
    window.notify = nullptr;
    window.notifyContext = nullptr;
    window.mailbox.clear();
    window.pageReady = false;
    Settle(Serial(window));
    if (window.shell) InputThread::DestroyPage(window.shell);
    shell_pipe_binding(&window.binding, 2, Serial(window), 0);
    shell_link_retire(link, Serial(window));
}
inline void StockFallback(const char* reason) {
    failed = true;
    recovering = true;
    shell_recovery_event(recovery, 2, 0);
    EnginePipe::Trace(reason, 0, generation);
    for (auto& [serial, window] : windows) {
        window.recoverable = window.binding.phase != 3;
        if (window.recoverable) Retire(window, false);
    }
    const auto oldGeneration = generation;
    recoveryCleanup = std::thread([oldGeneration] {
        pipe->Stop();
        InputThread::Stop();
        PostMessageW(wake, StockFallbackMessage, oldGeneration, 0);
    });
}
inline void Fail() {
    ImportRecoveryHistory();
    if (failed || recovering) return;
    wchar_t recoveryFlag[2]{};
    const bool enabled = everBound && !stopped && pipe && recovery &&
                         GetEnvironmentVariableW(L"FENN_NATIVE_SHELL_RECOVERY", recoveryFlag, 2) == 1 &&
                         recoveryFlag[0] == L'1';
    // A failed CreateProcess has no owned peer and cannot satisfy an exit wait.
    // Its admitted attempt stays charged; stop supervision and use the existing
    // stock presentation route rather than stranding hidden browser proxies.
    if (enabled && !pipe->PeerHandle()) {
        StockFallback("recovery-launch-unavailable");
        return;
    }
    if (enabled && !pipe->PeerExited() && shell_recovery_event(recovery, 3, GetTickCount64())) {
        EnginePipe::Trace("recovery-wait-exit");
        return;
    }
    if (enabled && !pipe->PeerExited() && !peerRetirementAttempted) {
        peerRetirementAttempted = true;
        // EOF is not death. After the existing orderly-exit grace, retire only
        // the connection's owned held peer, never a process found by PID.
        // The bounded process wait runs off the engine's interactive thread.
        HANDLE held{};
        if (!recoveryCleanup.joinable() && DuplicateHandle(GetCurrentProcess(), pipe->PeerHandle(), GetCurrentProcess(),
                                                           &held, SYNCHRONIZE | PROCESS_TERMINATE, FALSE, 0)) {
            winrt::handle owned{held};
            const auto target = wake;
            const auto oldGeneration = generation;
            recoveryCleanup = std::thread([owned = std::move(owned), target, oldGeneration]() mutable {
                const auto before = WaitForSingleObject(owned.get(), 0);
                if (before == WAIT_TIMEOUT) TerminateProcess(owned.get(), ERROR_BROKEN_PIPE);
                const bool exited = WaitForSingleObject(owned.get(), 2000) == WAIT_OBJECT_0;
                PostMessageW(target, PeerRetiredMessage, oldGeneration, exited);
            });
            failed = true;
            recovering = true;
            EnginePipe::Trace("recovery-retire-owned-peer", 0, peer);
            return;
        }
        EnginePipe::Trace("recovery-retire-handle-failed", 0, GetLastError());
    }
    const bool exited = enabled && pipe->PeerExited() && shell_recovery_event(recovery, 5, GetTickCount64());
    const bool canRecover = exited && shell_recovery_event(recovery, 0, GetTickCount64());
    if (canRecover) {
        recovering = true;
        failed = true;
        EnginePipe::Trace("recovery-revoke", 0, generation);
        for (auto& [serial, window] : windows) {
            if (window.binding.phase == 3) continue;
            window.recoverable = true;
            if (window.placement.show)
                window.recoveryRect = window.placement.bounds;
            else
                GetWindowRect(window.proxy, &window.recoveryRect);
            Retire(window, false);
            if (sink) sink(sinkContext, window.proxy, window.shell, 5, RO_E_CLOSED);
        }
        const auto oldGeneration = generation;
        recoveryCleanup = std::thread([oldGeneration] {
            pipe->Stop();
            InputThread::Stop();
            PostMessageW(wake, RecoverMessage, oldGeneration, 0);
        });
        return;
    }
    if (exited && shell_recovery_event(recovery, 4, GetTickCount64())) {
        StockFallback("recovery-budget-exhausted");
        return;
    }
    failed = true;
    CancelLaunch();
    EnginePipe::Trace("failed", 0, pipe ? pipe->Error() : 0);
    for (auto& [serial, window] : windows) {
        if (window.binding.phase == 3) continue;
        const bool bound = window.pageReady;
        Retire(window, false);
        if (sink) sink(sinkContext, window.proxy, window.shell, bound ? 3 : 2, RO_E_CLOSED);
    }
    if (pipe) pipe->Break();
}
inline bool CompleteLaunch() {
    if (!launch) return true;
    if (!launch->Done()) return false;
    const auto result = launch->Result();
    peer = launch->Pid();
    launch.reset();  // Joins before the owner can stop/break the connection.
    if (FAILED(result)) {
        EnginePipe::Trace("start-failed", 0, uint32_t(result));
        Fail();
        return false;
    }
    // Pass only this child's permission, when Windows still permits the parent
    // to do so. This grants eligibility; it neither shows nor activates a window.
    const bool foregroundAllowed = AllowSetForegroundWindow(peer) != FALSE;
    EnginePipe::Trace("foreground-handoff", 0, foregroundAllowed);
    EnginePipe::Trace("spawn-complete", 0, peer);
    return true;
}
inline void FinishReadiness() {
    if (recovering) return;
    for (const auto& [serial, window] : windows)
        if (window.binding.phase != 3 && !window.usable && !window.readiness.paused) return;
    if (recovery) shell_recovery_event(recovery, 1, 0);
    replacementProxies.clear();
    if (engineReconnected) rebuildingWindows = false;
}
inline bool Credit(const uint8_t* data, size_t length) noexcept {
    return pipe && pipe->Send(1, data, length);
}
inline void DrainInput() noexcept {
    uint8_t* data{};
    size_t length{};
    while (pipe && pipe->TakeRoute(1, data, length)) InputThread::Receive(data, length);
}
inline LRESULT CALLBACK Dispatch(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
#ifdef FENN_ENGINE_LINK_TESTING
    if (message == RegisterWindowMessageW(L"Fenn.Test.InterruptedWork.v1")) {
        if (w == 0) return pending.size();
        if (w == 1) return startup.size();
        if (w == 2) return heldWorkFrame.size();
        if (w == 3 && link && !heldWorkFrame.empty()) {
            ShellFrameView view{};
            const auto status = shell_link_receive(link, heldWorkFrame.data(), heldWorkFrame.size(), &view);
            EnginePipe::Trace("test-old-work-replay", uint32_t(status), view.delivered);
            return 1 + uint32_t(status) + (view.delivered ? 65536 : 0);
        }
        return 0;
    }
#endif
    if (message == PeerRetiredMessage) {
        if (!recovering || uint32_t(w) != generation || stopped) return 0;
        if (recoveryCleanup.joinable()) recoveryCleanup.join();
        recovering = false;
        failed = false;
        EnginePipe::Trace("recovery-peer-retirement-complete", 0, uint32_t(l));
        // Fail checks the original held process handle again before charging a
        // restart. Failed retirement falls back; it cannot launch beside a peer.
        Fail();
        return 0;
    }
#ifdef FENN_ENGINE_LINK_TESTING
    if (message == RegisterWindowMessageW(L"Fenn.Test.LivePipeBreak.v1")) {
        wchar_t fault[64]{};
        if (GetEnvironmentVariableW(L"FENN_TEST_PROCESS_FAULT", fault, 64) >= 64 || wcscmp(fault, L"live-pipe-break") ||
            !pipe || !pipe->Ready() || pipe->PeerExited())
            return 0;
        // The server's normal Break disconnects the real pipe, not either process.
        pipe->Break();
        return 1;
    }
#endif
    if (message == StockFallbackMessage) {
        if (!recovering || uint32_t(w) != generation || stopped) return 0;
        if (recoveryCleanup.joinable()) recoveryCleanup.join();
        recovering = false;
        KillTimer(wake, 1);
        std::vector<std::pair<HWND, HWND>> survivors;
        for (const auto& [serial, window] : windows)
            if (window.recoverable && IsWindow(window.proxy)) survivors.emplace_back(window.proxy, window.shell);
        for (HWND proxy : replacementProxies)
            if (IsWindow(proxy) && std::none_of(survivors.begin(), survivors.end(),
                                                [proxy](const auto& item) { return item.first == proxy; }))
                survivors.emplace_back(proxy, nullptr);
        replacementProxies.clear();
        for (const auto& [proxy, shell] : survivors)
            if (sink) sink(sinkContext, proxy, shell, 7, S_OK);
        EnginePipe::Trace("recovery-stock-fallback", 0, uint32_t(survivors.size()));
        return 0;
    }
    if (message == RecoverMessage) {
        if (!recovering || uint32_t(w) != generation || stopped) return 0;
        if (recoveryCleanup.joinable()) recoveryCleanup.join();
        struct Saved {
            HWND proxy;
            uintptr_t identity;
            RECT rect;
            uint32_t flags;
            EnginePlacement::State placement;
        };
        std::vector<Saved> saved;
        for (const auto& [serial, window] : windows)
            if (window.recoverable && IsWindow(window.proxy))
                saved.push_back({window.proxy, window.identity, window.recoveryRect, window.flags, window.placement});
        replacementProxies.clear();
        for (const auto& item : saved) replacementProxies.push_back(item.proxy);
        windows.clear();
        pipe.reset();
        if (link) {
            shell_link_destroy(link);
            link = nullptr;
        }
        KillTimer(wake, 1);
        DestroyWindow(wake);
        wake = nullptr;
        confirmed = false;
        failed = false;
        recovering = false;
        if (saved.empty()) {
            shell_recovery_event(recovery, 2, 0);
            return 0;
        }
        const auto profile = recoveryProfile, presentation = recoveryPresentation;
        EnginePipe::Trace("recovery-launch", 0, uint32_t(saved.size()));
        const auto result = Start(profile.c_str(), presentation.c_str(), 1);
        rebuildingWindows = true;
        if (SUCCEEDED(result))
            for (const auto& item : saved) {
                rebuildingPlacement = item.placement;
                EnginePipe::Trace("recovery-placement", 0, item.placement.show | (item.placement.foreground << 8));
                Create(item.proxy, item.identity, &item.rect, item.flags);
            }
        rebuildingPlacement = {};
        rebuildingWindows = false;
        return 0;
    }
    if (message == EnginePipe::ProcessLaunch::Completed) {
        if (uint32_t(w) == generation) Pump();
        return 0;
    }
    if (message == EnginePipe::Wake) {
        Pump();
        return 0;
    }
    if (message == PageReadyMessage) {
        if (recovering || uint32_t(uint64_t(w) >> 32) != generation) return 0;
        auto found = windows.find(uint32_t(w));
        if (found == windows.end() || found->second.binding.phase != 2) return 0;
        auto& window = found->second;
        const HWND page = reinterpret_cast<HWND>(l);
        if (!EnginePipe::Owns(page, GetCurrentProcessId()) || GetParent(page) != window.shell) {
            Fail();
            return 0;
        }
        window.pageReady = true;
        everBound = true;
        if (!window.needsReadiness) window.usable = true;
        FinishReadiness();
        EnginePipe::Trace("page-ready", uint32_t(w));
        const uint64_t handle = reinterpret_cast<uintptr_t>(page);
        Send(window, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_PAGE_READY, &handle, 8);
        if (sink) sink(sinkContext, window.proxy, window.shell, 1, S_OK);
        return 0;
    }
    if (message == InputReadyMessage) {
        if (recovering || failed || stopped || uint32_t(uint64_t(w) >> 32) != generation) return 0;
        auto it = windows.find(uint32_t(w));
        if (it == windows.end() || it->second.binding.phase != 2 || it->second.usable) return 0;
        auto& window = it->second;
        if (!l ||
            shell_recovery_readiness_observe(&window.readiness, generation, uint32_t(w), 2, GetTickCount64()) != 1) {
            Fail();
            return 0;
        }
        if (FAILED(Send(window, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_RECOVERY_COMMIT))) {
            Fail();
            return 0;
        }
        return 0;
    }
    if (message == WM_TIMER) {
        if (recovering || stopped) return 0;
        if (CompleteLaunch()) Flush();
        if (pipe && pipe->Closed()) {
            Fail();
            return 0;
        }
        for (auto& [serial, window] : windows)
            if (window.binding.phase != 3 && !window.usable &&
                (window.needsReadiness && window.pageReady
                     ? shell_recovery_readiness_observe(&window.readiness, generation, serial, 0, GetTickCount64()) == 2
                     : GetTickCount64() > window.deadline)) {
                if (window.needsReadiness) {
                    EnginePipe::Trace("recovery-timeout-engine-evidence", serial, window.readiness.observed);
                    EnginePipe::Trace("recovery-timeout-shell-evidence", serial,
                                      uint32_t(uintptr_t(GetPropW(window.shell, L"Fenn.RecoveryEvidence"))));
                    const uint32_t state =
                        (IsWindow(window.shell) ? 1u : 0u) | (IsWindowVisible(window.shell) ? 2u : 0u) |
                        (IsIconic(window.shell) ? 4u : 0u) | (GetForegroundWindow() == window.shell ? 8u : 0u) |
                        (GetPropW(window.shell, L"Fenn.RecoveryPending") ? 16u : 0u) |
                        (GetPropW(window.shell, L"Fenn.RecoveryReady") ? 32u : 0u) |
                        (GetPropW(window.shell, L"Fenn.RecoveryDeferred") ? 64u : 0u);
                    EnginePipe::Trace("recovery-timeout-shell-state", serial, state);
                }
                const HWND proxy = window.proxy, shell = window.shell;
                Retire(window, true);
                if (sink)
                    sink(sinkContext, proxy, shell, window.needsReadiness ? 3 : 2, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
                if (!everBound) {
                    Fail();
                    break;
                }
            }
        return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}
inline void Pump() noexcept {
    if (!pipe || pumping || !CompleteLaunch() || failed || stopped) return;
    pumping = true;
    try {
        ImportRecoveryHistory();
        uint8_t* data{};
        size_t length{};
        while (pipe->TakeRoute(0, data, length)) {
            ShellFrameView view{};
            const auto status = shell_link_receive(link, data, length, &view);
#ifdef FENN_ENGINE_LINK_TESTING
            if (status == SHELL_STATUS_OK && view.delivered && heldWorkFrame.empty()) {
                const bool input =
                    view.channel == SHELL_FRAME_REQUESTS &&
                    std::wstring(reinterpret_cast<const wchar_t*>(view.payload), view.length / sizeof(wchar_t))
                            .find(L"\"kind\":\"activate\"") != std::wstring::npos;
                if ((input && TestWork::Armed(L"input")) ||
                    (view.channel == SHELL_FRAME_STARTUP && TestWork::Armed(L"startup"))) {
                    heldWorkFrame.assign(data, data + length);
                    SetPropW(wake, L"Fenn.TestWorkToken", reinterpret_cast<HANDLE>(uintptr_t(view.revision)));
                    EnginePipe::Trace("test-work-held", view.channel, uint32_t(view.revision));
                    shell_link_free_frame(data, length);
                    TestWork::Reached();
                    continue;
                }
            }
#endif
            if (status != SHELL_STATUS_OK && status != SHELL_STATUS_REJECTED) {
                shell_link_free_frame(data, length);
                Fail();
                break;
            }
            auto found = windows.find(view.window);
            if (view.delivered && found != windows.end() && found->second.binding.phase != 3) {
                auto& window = found->second;
                if (view.channel == SHELL_FRAME_LIFETIME && view.kind == SHELL_FRAME_LIFETIME_BOUND) {
                    uint64_t handle{};
                    memcpy(&handle, view.payload, 8);
                    const auto shell = reinterpret_cast<HWND>(uintptr_t(handle));
                    wchar_t role[80]{};
                    GetClassNameW(shell, role, 80);
                    if (!EnginePipe::Owns(shell, peer) || wcscmp(role, L"WinUIDesktopWin32WindowClass") ||
                        !shell_pipe_binding(&window.binding, 1, view.window, handle)) {
                        // Refusal must also end this connection. Otherwise Fail
                        // waits for an exit while an authenticated peer stays live.
                        pipe->Break();
                        Fail();
                    } else {
                        window.shell = shell;
                        InputThread::RequestPage(shell, window.proxy, NativeWindowHost::StartupBackgroundColor());
                        InputThread::Post([shell, serial = view.window, target = wake, epoch = generation,
                                           blocked = window.needsReadiness] {
                            auto* page = InputThread::Find(shell);
                            if (blocked && page && page->page) EnableWindow(page->page, FALSE);
                            PostMessageW(target, PageReadyMessage, WPARAM((uint64_t(epoch) << 32) | serial),
                                         reinterpret_cast<LPARAM>(page ? page->page : nullptr));
                        });
                    }
                } else if (view.channel == SHELL_FRAME_LIFETIME &&
                           (view.kind == SHELL_FRAME_LIFETIME_RECOVERY_DEFERRED ||
                            view.kind == SHELL_FRAME_LIFETIME_RECOVERY_RESUMED)) {
                    if (window.needsReadiness && !window.usable) {
                        const auto outcome = view.kind == SHELL_FRAME_LIFETIME_RECOVERY_DEFERRED
                                                 ? shell_recovery_readiness_pause(&window.readiness, generation,
                                                                                  view.window, 0, GetTickCount64())
                                                 : shell_recovery_readiness_resume(&window.readiness, generation,
                                                                                   view.window, GetTickCount64());
                        if (outcome == 0) {
                            FinishReadiness();
                            EnginePipe::Trace(window.readiness.paused ? "recovery-deferred" : "recovery-resumed",
                                              view.window);
                            // Minimized attachment may have accepted geometry without
                            // creating controls. Reannounce the existing bound page on
                            // exposure so Firefox republishes its current presentation.
                            if (!window.readiness.paused && window.pageReady && sink)
                                sink(sinkContext, window.proxy, window.shell, 1, S_OK);
                        }
                    }
                } else if (view.channel == SHELL_FRAME_LIFETIME && view.kind == SHELL_FRAME_LIFETIME_PRESENTED) {
                    if (window.needsReadiness && window.pageReady && !window.usable && !window.readiness.observed) {
                        if (shell_recovery_readiness_observe(&window.readiness, generation, view.window, 1,
                                                             GetTickCount64()) != 0) {
                            Fail();
                            break;
                        }
                        if (!InputThread::Post(
                                [shell = window.shell, serial = view.window, target = wake, epoch = generation] {
                                    const auto* page = InputThread::Find(shell);
                                    RECT bounds{};
                                    const bool valid = page && page->page && GetParent(page->page) == shell &&
                                                       GetClientRect(page->page, &bounds) && bounds.right > 1 &&
                                                       bounds.bottom > 1;
                                    if (valid) EnableWindow(page->page, TRUE);
                                    PostMessageW(target, InputReadyMessage, WPARAM((uint64_t(epoch) << 32) | serial),
                                                 valid);
                                }))
                            Fail();
                    }
                } else if (view.channel == SHELL_FRAME_LIFETIME && view.kind == SHELL_FRAME_LIFETIME_RECOVERY_COMMIT) {
                    if (window.needsReadiness && window.pageReady && !window.usable &&
                        shell_recovery_readiness_observe(&window.readiness, generation, view.window, 0,
                                                         GetTickCount64()) == 1) {
                        window.usable = true;
                        FinishReadiness();
                        EnginePipe::Trace("recovery-ready", view.window);
                        if (sink) sink(sinkContext, window.proxy, window.shell, 6, S_OK);
                    }
                } else if (view.channel == SHELL_FRAME_WINDOW && view.kind == 8) {
                    uint32_t hr{}, phase{};
                    uint64_t session{}, document{};
                    memcpy(&hr, view.payload, 4);
                    memcpy(&phase, view.payload + 4, 4);
                    memcpy(&session, view.payload + 8, 8);
                    memcpy(&document, view.payload + 16, 8);
                    if (GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0))
                        fprintf(stderr, "FennShare request=%llu result=0x%08X phase=%u session=%llu document=%llu\n",
                                static_cast<unsigned long long>(view.revision), hr, phase,
                                static_cast<unsigned long long>(session), static_cast<unsigned long long>(document));
                } else if (view.channel == SHELL_FRAME_WINDOW && view.kind == 6) {
                    memcpy(&window.placement, view.payload, sizeof(window.placement));
                    if (window.placement.foreground)
                        for (auto& [serial, other] : windows)
                            if (serial != view.window) other.placement.foreground = 0;
                } else if (view.channel == SHELL_FRAME_WINDOW && view.kind == 2) {
                    auto p = pending.find(view.revision);
                    if (p != pending.end() && p->second.serial == view.window) {
#ifdef FENN_ENGINE_LINK_TESTING
                        EnginePipe::Trace("test-pending-applied", view.window, uint32_t(view.revision));
#endif
                        if (p->second.counter) InterlockedDecrement(p->second.counter);
                        pending.erase(p);
                        PostMessageW(window.proxy, RegisterWindowMessageW(L"Fenn.NativeWindowHost.EndpointGeometry.v1"),
                                     0, 0);
                    }
                } else if (view.channel == SHELL_FRAME_LIFETIME &&
                           view.kind == SHELL_FRAME_LIFETIME_RECOVERY_SUPPRESS) {
                    if (!engineReconnected || window.shell) {
                        Fail();
                        break;
                    }
                    if (sink) sink(sinkContext, window.proxy, nullptr, 9, S_OK);
                } else if (view.channel == SHELL_FRAME_LIFETIME && view.kind == SHELL_FRAME_LIFETIME_RECOVERY_EMPTY) {
                    if (!engineReconnected || window.shell || !window.mailbox.empty()) {
                        Fail();
                        break;
                    }
                    std::wstring batch = L"[";
                    for (size_t at = 0; at < view.length; at += 16) {
                        if (at) batch += L",";
                        batch += L"\"";
                        for (size_t n = 0; n < 16; ++n) {
                            const auto b = view.payload[at + n];
                            batch += L"0123456789abcdef"[b >> 4];
                            batch += L"0123456789abcdef"[b & 15];
                        }
                        batch += L"\"";
                    }
                    window.mailbox = batch + L"]";
                    if (sink) sink(sinkContext, window.proxy, nullptr, 8, S_OK);
                } else if (view.channel == SHELL_FRAME_REQUESTS) {
                    if (view.length)
                        window.mailbox.assign(reinterpret_cast<const wchar_t*>(view.payload), view.length / 2);
                    if (window.notify) window.notify(window.notifyContext);
                } else if (view.channel == SHELL_FRAME_STARTUP) {
                    auto token = startup.find(view.revision);
                    if (token != startup.end() && token->second == window.proxy && view.length >= 4) {
                        startup.erase(token);
                        HRESULT result{};
                        memcpy(&result, view.payload, 4);
                        std::wstring reply(reinterpret_cast<const wchar_t*>(view.payload + 4), (view.length - 4) / 2);
                        if (startupSink)
                            startupSink(startupContext, view.revision, result,
                                        SUCCEEDED(result) ? reply.c_str() : nullptr);
                    }
                } else if (view.channel == SHELL_FRAME_WINDOW && view.kind == 3) {
                    const HWND proxy = window.proxy, shell = window.shell;
                    Retire(window, true);
                    if (sink) sink(sinkContext, proxy, shell, 2, E_FAIL);
                    if (!everBound) Fail();
                }
            }
            shell_link_free_frame(data, length);
        }
        if (pipe->Closed())
            Fail();
        else
            Flush();
    } catch (...) {
        Fail();
    }
    pumping = false;
}
inline HRESULT StartFailure(HRESULT result) {
    EnginePipe::Trace("start-failed", 0, uint32_t(result));
    CancelLaunch();
    if (pipe) {
        Fail();
        pipe->Stop();
    }
    failed = true;
    InputThread::Stop();
    pipe.reset();  // Input callbacks may still read the adapter until their thread has stopped.
    if (link) {
        shell_link_destroy(link);
        link = nullptr;
    }
    if (wake) {
        KillTimer(wake, 1);
        DestroyWindow(wake);
        wake = nullptr;
    }
    return result;
}
inline HRESULT WINAPI Start(const wchar_t* profile, const wchar_t* presentation, uint32_t flags) noexcept {
    EnginePipe::Trace("start-request", 0, flags);
    if (stopped || flags > 1 || (profile && wcsnlen(profile, 32769) > 32768) ||
        (presentation && wcsnlen(presentation, 32769) > 32768))
        return E_INVALIDARG;
    if (failed) return RO_E_CLOSED;
    try {
        recoveryProfile = profile ? profile : L"";
        recoveryPresentation = presentation ? presentation : L"";
        if (!recovery) recovery = shell_recovery_create();
        if (!pipe) {
            peerRetirementAttempted = false;
            thread = GetCurrentThreadId();
            WNDCLASSW wc{};
            wc.lpfnWndProc = Dispatch;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"Fenn.ProductEngineLink.v1";
            RegisterClassW(&wc);
            wake =
                CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
            if (!wake) return StartFailure(E_FAIL);
            std::wstring name;
            winrt::handle remote;
            wchar_t descriptor[65]{};
            const auto descriptorLength = GetEnvironmentVariableW(L"FENN_ENGINE_RECONNECT", descriptor, 65);
            HANDLE handle = INVALID_HANDLE_VALUE;
            if (descriptorLength) {
                ShellReconnectDescriptor parsed{};
                if (descriptorLength >= 65 ||
                    !shell_reconnect_parse(reinterpret_cast<const uint16_t*>(descriptor), descriptorLength, &parsed) ||
                    parsed.peer == GetCurrentProcessId())
                    return StartFailure(E_INVALIDARG);
                SetEnvironmentVariableW(L"FENN_ENGINE_RECONNECT", nullptr);
                remote.attach(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE,
                                          parsed.peer));
                wchar_t image[32768]{};
                DWORD imageLength = 32768;
                if (!remote || !QueryFullProcessImageNameW(remote.get(), 0, image, &imageLength) ||
                    _wcsicmp(image, EnginePipe::Sibling(L"fenn-shell.exe").c_str()))
                    return StartFailure(E_ACCESSDENIED);
                generation = parsed.generation;
                peer = parsed.peer;
                engineReconnected = true;
                rebuildingWindows = true;
                name = L"\\\\.\\pipe\\Fenn." + std::wstring(reinterpret_cast<const wchar_t*>(parsed.nonce), 32);
                handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED, nullptr);
            } else
                handle = EnginePipe::Server(name, generation);
            if (handle == INVALID_HANDLE_VALUE) return StartFailure(E_FAIL);
            winrt::handle server{handle};
            auto executable = EnginePipe::Sibling(L"fenn-shell.exe");
#ifdef FENN_ENGINE_LINK_TESTING
            wchar_t replacementFault[64]{};
            if (everBound && GetEnvironmentVariableW(L"FENN_TEST_PROCESS_FAULT", replacementFault, 64) < 64 &&
                !wcscmp(replacementFault, L"missing-replacement-shell"))
                executable = EnginePipe::Sibling(L"fenn-missing-replacement-shell.exe");
#endif
            std::wstring command = L"\"" + executable + L"\" --pipe " + name + L" --engine " +
                                   std::to_wstring(GetCurrentProcessId()) + L" --generation " +
                                   std::to_wstring(generation) + L" --flags 0 --product 1";
            if (everBound) command += L" --recovery 1";
            link = EnginePipe::Link(generation);
            if (!link) return StartFailure(E_OUTOFMEMORY);
            InputThread::ReturnCredit = Credit;
            InputThread::DrainTransport = DrainInput;
            const HRESULT input = InputThread::Start(generation);
            if (FAILED(input)) return StartFailure(input);
            winrt::check_bool(shell_recovery_export(recovery, GetTickCount64(), &recoveryHistory.shell));
            pipe = std::make_unique<EnginePipe::Connection>();
            if (!SetTimer(wake, 1, 250, nullptr)) return StartFailure(E_FAIL);
            if (engineReconnected && remote) {
                pipe->Start(server.detach(), remote.detach(), peer, generation, wake, false, EnginePipe::Protocol,
                            InputThread::State().dispatcher, InputThread::TransportMessage, false, recoveryHistory);
                EnginePipe::Trace("engine-reconnected", 0, generation);
            } else
                launch = std::make_unique<EnginePipe::ProcessLaunch>(
                    *pipe, std::move(server), executable, std::move(command), generation, wake,
                    InputThread::State().dispatcher, InputThread::TransportMessage, std::vector<wchar_t>{}, false,
                    recoveryHistory);
        }
        if (thread != GetCurrentThreadId() || failed) return RO_E_CLOSED;
        CompleteLaunch();
        if (failed) return RO_E_CLOSED;
        std::wstring value = profile ? profile : L"";
        value.push_back(0);
        value += presentation ? presentation : L"";
        if (!EnginePipe::Send(link, 0, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_PROFILE, value.data(),
                              value.size() * 2))
            return E_OUTOFMEMORY;
        if ((flags & 1) && !confirmed) {
            EnginePipe::Trace("confirm", 0, peer);
            confirmed = true;
            EnginePipe::Send(link, 0, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_CONFIRM);
        }
        Flush();
        return S_OK;
    } catch (...) {
        return StartFailure(winrt::to_hresult());
    }
}
inline HRESULT WINAPI Create(HWND proxy, uintptr_t identity, const RECT* rect, uint32_t flags) noexcept {
    if (!pipe || failed || stopped || windows.size() >= 128 || !rect || flags > 1 || !identity ||
        !EnginePipe::Owns(proxy, GetCurrentProcessId()) || GetWindowThreadProcessId(proxy, nullptr) != thread ||
        Find(proxy))
        return E_INVALIDARG;
    try {
        uint32_t serial{};
        if (shell_link_serial(link, reinterpret_cast<uintptr_t>(proxy), true, &serial) != SHELL_STATUS_OK)
            return E_FAIL;
        Window window{};
        window.proxy = proxy;
        window.identity = identity;
        window.recoveryRect = *rect;
        window.flags = flags;
        window.needsReadiness = rebuildingWindows;
        window.deadline = GetTickCount64() + (window.needsReadiness ? 30000 : 10000);
        if (window.needsReadiness)
            window.readiness = shell_recovery_readiness_begin(generation, serial, 3, GetTickCount64());
        if (!shell_pipe_binding(&window.binding, 0, serial, 0)) return E_FAIL;
        auto& stored = windows.emplace(serial, std::move(window)).first->second;
        EnginePipe::Trace("create", serial);
        stored.placement = rebuildingWindows ? rebuildingPlacement : EnginePlacement::State{};
        const EnginePlacement::Create data{reinterpret_cast<uintptr_t>(proxy),        serial, *rect,
                                           flags | (stored.needsReadiness ? 2u : 0u), 0,      stored.placement};
        return Send(stored, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_CREATE, &data, sizeof(data));
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}
inline HRESULT WINAPI Stop() noexcept {
    EnginePipe::Trace("stop", 0, peer);
    if (recovering) stopped = true;
    if (recovery) shell_recovery_event(recovery, 2, 0);
    if (recoveryCleanup.joinable()) recoveryCleanup.join();
    recovering = false;
    CancelLaunch();
    for (auto& [serial, window] : windows) Retire(window, true);
    sink = nullptr;
    startupSink = nullptr;
    stopped = true;
    if (pipe) pipe->Stop();
    InputThread::Stop();
    if (wake) {
        KillTimer(wake, 1);
        DestroyWindow(wake);
        wake = nullptr;
    }
    if (link) {
        shell_link_destroy(link);
        link = nullptr;
    }
    pipe.reset();
    shell_recovery_destroy(recovery);
    recovery = nullptr;
    return S_OK;
}
inline HRESULT WINAPI SetSink(WinUIPaneProcessNotifyFn notify, void* context) noexcept {
    sink = notify;
    sinkContext = context;
    return S_OK;
}
inline HRESULT WINAPI SetStartup(WinUIPaneStartupReplyFn notify, void* context) noexcept {
    startupSink = notify;
    startupContext = context;
    return S_OK;
}
inline HRESULT WINAPI DestroyHost(HWND hwnd) noexcept {
    auto* w = Find(hwnd);
    if (!w) return S_FALSE;
    const auto serial = Serial(*w);
    Retire(*w, true);
    windows.erase(serial);
    FinishReadiness();
    return S_OK;
}
inline HRESULT WINAPI DestroyPane(HWND hwnd) noexcept {
    auto* w = Find(hwnd);
    return w ? Send(*w, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_DESTROY) : S_FALSE;
}
inline HRESULT WINAPI SetNotify(HWND hwnd, WinUIPaneNotifyFn notify, void* context) noexcept {
    auto* w = Find(hwnd);
    if (!w) return E_INVALIDARG;
    const bool first = !w->notify && notify;
    w->notify = notify;
    w->notifyContext = context;
    if (first && w->pageReady && sink) sink(sinkContext, w->proxy, w->shell, 1, S_OK);
    return Send(*w, SHELL_FRAME_LIFETIME, notify ? SHELL_FRAME_LIFETIME_WAKEUP_ON : SHELL_FRAME_LIFETIME_WAKEUP_OFF);
}
inline HRESULT WINAPI Drain(HWND hwnd, wchar_t* out, uint32_t capacity) noexcept {
    auto* w = Find(hwnd);
    if (!w || !out || !capacity) return E_INVALIDARG;
    out[0] = 0;
    if (w->mailbox.empty()) return S_FALSE;
    if (w->mailbox.size() >= capacity) return E_NOT_SUFFICIENT_BUFFER;
    wcscpy_s(out, capacity, w->mailbox.c_str());
    w->mailbox.clear();
    return S_OK;
}
inline HRESULT WINAPI Rect(HWND hwnd, const RECT* rect, BOOL visible, uint16_t kind) noexcept {
    auto* w = Find(hwnd);
    if (!w || !rect || !w->pageReady) return E_PENDING;
    uint8_t value[17]{};
    memcpy(value, rect, 16);
    value[16] = visible ? 1 : 0;
    return Send(*w, SHELL_FRAME_GEOMETRY, kind, value, 17);
}
inline HRESULT WINAPI Tabs(HWND hwnd, const WinUITab* rows, uint32_t count, uint32_t selected, uint32_t revision,
                           uint32_t* ack) noexcept {
    if (!ack) return E_POINTER;
    *ack = 0;
    auto* w = Find(hwnd);
    if (!w || count > 10000 || (count && !rows)) return E_INVALIDARG;
    try {
        std::vector<ShellFrameTabIn> tabs(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (!rows[i].title) return E_INVALIDARG;
            tabs[i] = {rows[i].id, rows[i].commands, reinterpret_cast<const uint16_t*>(rows[i].title),
                       wcsnlen(rows[i].title, 32769)};
        }
        uint8_t* data{};
        size_t length{};
        const auto encoded = shell_frame_tabs_encode(tabs.data(), count, selected, revision, &data, &length);
        if (encoded != SHELL_STATUS_OK) {
            EnginePipe::Trace("tabs-encode-failed", Serial(*w), uint32_t(encoded));
            return E_INVALIDARG;
        }
        const auto result = Send(*w, SHELL_FRAME_TABS, 0, data, length, revision);
        shell_link_free_frame(data, length);
        if (FAILED(result)) EnginePipe::Trace("tabs-send-failed", Serial(*w), uint32_t(result));
        if (SUCCEEDED(result)) *ack = revision;
        return result;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}
inline HRESULT WINAPI WindowRequest(HWND hwnd, const WinUIPaneWindowRequest* input) noexcept {
    auto* w = Find(hwnd);
    if (!w || !input) return E_INVALIDARG;
    // During engine reconciliation the shell has no adopted HWND for this
    // serial yet. It cannot apply/acknowledge requests, and provisional Firefox
    // geometry must not overwrite the surviving shell's placement. Refuse
    // explicitly so RequestShell unwinds its pending counter. Bind reads the
    // surviving geometry and republishes icons once the real HWND is known.
    if (engineReconnected && w->needsReadiness && !w->shell) {
        if (input->kind == WinUIPaneWindowIcon && input->value) DestroyIcon(reinterpret_cast<HICON>(input->value));
        return E_PENDING;
    }
    if (input->kind == WinUIPaneWindowIcon) {
        auto icon = reinterpret_cast<HICON>(input->value);
        try {
            const auto pixels = EngineIcon::Encode(icon);
            if (icon) DestroyIcon(icon);
            icon = nullptr;
            if (pixels.empty()) return E_INVALIDARG;
            if (pending.size() >= 1024) return E_OUTOFMEMORY;
            const auto id = ++request;
            pending[id] = {Serial(*w), input->pending};
#ifdef FENN_ENGINE_LINK_TESTING
            EnginePipe::Trace("test-pending-queued", Serial(*w), uint32_t(id));
#endif
            const auto result =
                Send(*w, SHELL_FRAME_WINDOW, input->flags == ICON_BIG ? 4 : 5, pixels.data(), pixels.size(), id);
            if (FAILED(result)) pending.erase(id);
            return result;
        } catch (...) {
            if (icon) DestroyIcon(icon);
            return E_OUTOFMEMORY;
        }
    }
    if (pending.size() >= 1024) return E_OUTOFMEMORY;
    const auto id = ++request;
    const ShellFrameWindowRequest data{input->kind,
                                       input->flags,
                                       {input->rect.left, input->rect.top, input->rect.right, input->rect.bottom},
                                       int64_t(input->value)};
    pending[id] = {Serial(*w), input->pending};
#ifdef FENN_ENGINE_LINK_TESTING
    EnginePipe::Trace("test-pending-queued", Serial(*w), uint32_t(id));
#endif
    const auto result = Send(*w, SHELL_FRAME_WINDOW, 0, &data, sizeof(data), id);
    if (FAILED(result)) pending.erase(id);
    return result;
}
inline HRESULT WINAPI Startup(HWND hwnd, const wchar_t* input, wchar_t* out, uint32_t capacity) noexcept {
    auto* w = Find(hwnd);
    if (!w || !input || !out || capacity < 21 || startup.size() >= 128) return E_INVALIDARG;
    out[0] = 0;
    const auto id = ++request;
    startup[id] = w->proxy;
    const size_t length = wcsnlen(input, 131073);
    if (length > 131072) {
        startup.erase(id);
        return E_INVALIDARG;
    }
    const auto result = Send(*w, SHELL_FRAME_STARTUP, 0, input, length * 2, id);
    if (FAILED(result)) {
        startup.erase(id);
        return result;
    }
    swprintf_s(out, capacity, L"%llu", static_cast<unsigned long long>(id));
    return S_OK;
}
inline HRESULT GetApi(uint32_t version, uint32_t size, WinUIPaneApi* out) noexcept {
    if (!out) return E_POINTER;
    if (version != WINUI_PANE_ABI_VERSION || size != sizeof(*out)) return E_NOINTERFACE;
    *out = {};
    out->size = sizeof(*out);
    out->version = WINUI_PANE_ABI_VERSION;
    out->focus = [](HWND h, BOOL v) noexcept {
        auto* w = Find(h);
        const uint8_t b = v ? 1 : 0;
        return w ? Send(*w, SHELL_FRAME_FOCUS, 0, &b, 1) : E_INVALIDARG;
    };
    out->focusAddress = [](HWND h, BOOL v) noexcept {
        auto* w = Find(h);
        const uint8_t b = v ? 1 : 0;
        return w ? Send(*w, SHELL_FRAME_FOCUS, 1, &b, 1) : E_INVALIDARG;
    };
    out->translate = [](MSG*) noexcept -> BOOL { return FALSE; };
    out->shutdown = Stop;
    out->destroy = DestroyPane;
    out->requestShare = [](HWND hwnd, uint64_t session, uint64_t document, const wchar_t* title,
                           const wchar_t* url) noexcept -> HRESULT {
        auto* w = Find(hwnd);
        if (!w || !session || !document || !title || !url) return E_INVALIDARG;
        const size_t t = wcsnlen(title, 4097), u = wcsnlen(url, 32769);
        if (!t || t > 4096 || !u || u > 32768) return E_INVALIDARG;
        try {
            std::vector<uint8_t> bytes(32 + 2 * (t + u));
            const uint64_t deadline = GetTickCount64() + 5000;
            const uint32_t nt = uint32_t(t), nu = uint32_t(u);
            memcpy(bytes.data(), &session, 8);
            memcpy(bytes.data() + 8, &document, 8);
            memcpy(bytes.data() + 16, &deadline, 8);
            memcpy(bytes.data() + 24, &nt, 4);
            memcpy(bytes.data() + 28, &nu, 4);
            memcpy(bytes.data() + 32, title, t * 2);
            memcpy(bytes.data() + 32 + t * 2, url, u * 2);
            return Send(*w, SHELL_FRAME_WINDOW, 7, bytes.data(), bytes.size(), ++request);
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    };
    out->syncTabs = Tabs;
    out->syncAddress = [](HWND h, const wchar_t* s) noexcept { return Text(h, SHELL_FRAME_ADDRESS, 0, s); };
    out->syncTabMenu = [](HWND h, const wchar_t* s) noexcept { return Text(h, SHELL_FRAME_TAB_MENU, 0, s); };
    out->completeRequests = [](HWND h, const wchar_t* s) noexcept { return Text(h, SHELL_FRAME_REQUESTS, 0, s); };
    out->applyTabDelta = [](HWND h, const wchar_t* s, uint32_t* revision) noexcept {
        if (!revision) return E_POINTER;
        *revision = 0;
        auto r = Text(h, SHELL_FRAME_TABS, 1, s);
        if (SUCCEEDED(r)) *revision = 1;
        return r;
    };
    out->updateTabStrip = [](HWND h, const RECT* r, BOOL v) noexcept { return Rect(h, r, v, 0); };
    out->updateAddress = [](HWND h, const RECT* r, BOOL v) noexcept { return Rect(h, r, v, 1); };
    out->clip = [](HWND h, const RECT* r, uint32_t n) noexcept {
        auto* w = Find(h);
        return w ? Send(*w, SHELL_FRAME_GEOMETRY, 2, r, size_t(n) * 16) : E_INVALIDARG;
    };
    out->setNotifySink = SetNotify;
    out->drainRequests = Drain;
    out->measureChromeLayout = [](HWND h, uint32_t s, BOOL n, uint32_t i, WinUIChromeLayout* out) noexcept {
        if (!out) return E_POINTER;
        ShellChromeLayout plan{};
        const auto result = MeasureNativeChromeLayout(h, s, n != FALSE, i, &plan);
        if (FAILED(result)) return result;
        auto r = [](ShellChromeRect v) -> RECT { return {v.x, v.y, v.x + v.width, v.y + v.height}; };
        *out = {plan.fits, r(plan.strip), r(plan.navigation), r(plan.content)};
        return S_OK;
    };
    out->createWindowHost = [](DWORD, const wchar_t*, DWORD, const RECT*, HWND, HINSTANCE, HWND* out) noexcept {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    };
    out->startIndependentHost = [](const wchar_t*, const wchar_t*, HWND* out) noexcept {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    };
    out->destroyWindowHost = DestroyHost;
    out->showStartupSurface = [](HWND) noexcept { return S_FALSE; };
    out->dismissStartupSurface = [](HWND) noexcept { return S_FALSE; };
    out->bindIndependentEndpoint = [](HWND h, uintptr_t, HWND p) noexcept {
        auto* w = Find(h);
        return w && w->proxy == p && w->binding.phase == 2 && w->pageReady && EnginePipe::Owns(h, peer) ? S_OK
                                                                                                        : E_PENDING;
    };
    out->cancelIndependentClose = [](HWND h, uint64_t g, uint64_t r) noexcept {
        auto* w = Find(h);
        const uint64_t p[]{g, r};
        return w ? Send(*w, SHELL_FRAME_CLOSE, 0, p, 16) : E_INVALIDARG;
    };
    out->setIndependentTitle = SetTitle;
    out->requestIndependentWindow = WindowRequest;
    out->startupTransfer = Startup;
    out->setStartupSink = SetStartup;
    out->setIndependentAccessibility = InputThread::SetAccessibility;
    out->setIndependentCursor = InputThread::SetCursor;
    out->registerIndependentTouch = InputThread::RegisterTouch;
    out->publishIndependentInput = InputThread::PublishInput;
    out->drainIndependentInput = InputThread::DrainInput;
    out->commitIndependentInput = InputThread::CommitInput;
    out->publishIndependentGeometry = InputThread::PublishGeometry;
    out->startProcessHost = Start;
    out->setProcessSink = SetSink;
    out->createProcessWindow = Create;
    return S_OK;
}
}  // namespace ProductLink
