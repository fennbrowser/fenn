#pragma once
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Rollback-only loopback: Gecko and the native UI thread own separate endpoints.
// Frames move via posted messages and the Gecko-thread inbox; no pipe selection
// or process recovery state belongs to this transport.
#include "ShellLink.h"
#ifdef FENN_SHELL_PROCESS
// The optional pane inspection build of the executable still exposes rollback
// test entry points, but process sends always use the explicit pipe transport.
#include "ProcessShellTransport.h"
#endif
namespace PaneLink {
inline constexpr UINT FrameMessage = WM_APP + 902;
inline constexpr UINT WakeMessage = WM_APP + 903;
inline constexpr uint32_t Generation = 1;

// Shell to engine: frames wait here until Gecko's thread takes them.
struct Inbox {
    std::mutex lock;
    std::deque<std::pair<uint8_t*, size_t>> frames;
    std::atomic<HWND> wake{};
    std::atomic<bool> posted{};
};
inline Inbox& ToEngine() {
    static auto* inbox = new Inbox;
    return *inbox;
}
inline void DeliverToEngine(uint8_t* data, size_t length) noexcept {
    auto& inbox = ToEngine();
    try {
        std::lock_guard guard(inbox.lock);
        inbox.frames.emplace_back(data, length);
    } catch (...) {
        shell_link_free_frame(data, length);
        return;
    }
    if (!inbox.posted.exchange(true)) {
        const HWND wake = inbox.wake;
        if (!wake || !PostMessageW(wake, WakeMessage, 0, 0)) inbox.posted = false;
    }
}

// --- Engine half: Gecko's thread ------------------------------------------------------------
struct Engine {
    ShellLinkEnd* link{};
    HWND wake{};
    bool pumping{};
    // One prepared request batch per window, taken by drainRequests.
    std::map<uint32_t, std::wstring> mailbox;
    std::map<uint32_t, std::pair<WinUIPaneNotifyFn, void*>> sinks;
    // Window requests the shell has not acknowledged yet, with Gecko's pending counter for each.
    std::map<uint64_t, std::pair<uint32_t, volatile LONG*>> pending;
    uint64_t nextRequest{};
    // Surfaces the shell could not create (one bit per SHELL_FRAME_REFUSED_* surface), and the
    // proxy each window bound, to close it if the bind is refused.
    std::map<uint32_t, uint32_t> refused;
    std::map<uint32_t, HWND> endpoints;
    // Startup transfer replies go to Gecko's sink, never during the call that asked.
    WinUIPaneStartupReplyFn startupSink{};
    void* startupContext{};
    uint64_t nextStartup{};
    std::vector<std::tuple<uint64_t, HRESULT, std::wstring>> localReplies;
};
inline Engine& Gecko() {
    static auto* engine = new Engine;
    return *engine;
}
inline void Pump() noexcept;
inline LRESULT CALLBACK EngineWake(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WakeMessage) {
        Pump();
        return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}
inline bool EnsureEngine() noexcept {
    auto& engine = Gecko();
    if (engine.link) return true;
    engine.link = shell_link_create(Generation, nullptr);
    if (!engine.link) return false;
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(EngineWake), &module);
    WNDCLASSW wc{};
    wc.lpfnWndProc = EngineWake;
    wc.hInstance = module;
    wc.lpszClassName = L"Fenn.EngineLink.v1";
    RegisterClassW(&wc);
    engine.wake = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, nullptr);
    ToEngine().wake = engine.wake;
    return true;
}
// Hands every frame that may go to the native thread. A caller about to wait for that thread
// forces them out, so nothing queued is overtaken by its wait.
inline void Flush(bool force) noexcept {
    auto& engine = Gecko();
    if (!engine.link) return;
    const HWND target = IndependentHost::State().dispatcher;
    for (;;) {
        uint8_t* data{};
        size_t length{};
        if (shell_link_take(engine.link, force, &data, &length) != SHELL_STATUS_OK || !data) return;
        if (!target || !PostMessageW(target, FrameMessage, length, reinterpret_cast<LPARAM>(data))) {
            shell_link_free_frame(data, length);
            shell_link_close(engine.link);
            return;
        }
    }
}
using Replies = std::vector<std::tuple<uint64_t, HRESULT, std::wstring>>;
inline void Deliver(const ShellFrameView& view, std::vector<std::pair<WinUIPaneNotifyFn, void*>>& wakes,
                    Replies& replies) {
    auto& engine = Gecko();
    if (view.channel == SHELL_FRAME_STARTUP) {
        if (view.length < sizeof(HRESULT)) return;
        replies.emplace_back(view.revision, Read<HRESULT>(view.payload),
                             Text(view.payload + sizeof(HRESULT), view.length - sizeof(HRESULT)));
        return;
    }
    if (view.channel == SHELL_FRAME_REQUESTS) {
        if (view.length) engine.mailbox[view.window] = Text(view.payload, view.length);
        if (auto found = engine.sinks.find(view.window); found != engine.sinks.end()) wakes.push_back(found->second);
    } else if (view.channel == SHELL_FRAME_WINDOW && view.kind == 2) {
        // Applied on the shell: count it applied, then tell the proxy to read the shell's state.
        auto found = engine.pending.find(view.revision);
        if (found == engine.pending.end()) return;
        InterlockedDecrement(found->second.second);
        engine.pending.erase(found);
        if (const auto endpoint = reinterpret_cast<HWND>(static_cast<uintptr_t>(Read<uint64_t>(view.payload))))
            PostMessageW(endpoint, NativeWindowHost::EndpointGeometryMessage(), 0, 0);
    } else if (view.channel == SHELL_FRAME_WINDOW && view.kind == 3) {
        const auto surface = Read<uint16_t>(view.payload);
        if (surface == SHELL_FRAME_REFUSED_BIND) {
            if (auto found = engine.endpoints.find(view.window); found != engine.endpoints.end())
                PostMessageW(found->second, WM_CLOSE, 0, 0);
        } else {
            engine.refused[view.window] |= 1u << surface;
        }
        if (auto found = engine.sinks.find(view.window); found != engine.sinks.end()) wakes.push_back(found->second);
    }
}
inline void Pump() noexcept {
    auto& engine = Gecko();
    if (!engine.link || engine.pumping) return;
    engine.pumping = true;
    std::deque<std::pair<uint8_t*, size_t>> frames;
    std::vector<std::pair<WinUIPaneNotifyFn, void*>> wakes;
    Replies replies;
    replies.swap(engine.localReplies);
    {
        auto& inbox = ToEngine();
        inbox.posted = false;
        std::lock_guard guard(inbox.lock);
        frames.swap(inbox.frames);
    }
    for (auto [data, length] : frames) {
        ShellFrameView view{};
        if (shell_link_receive(engine.link, data, length, &view) == SHELL_STATUS_OK && view.delivered) try {
                Deliver(view, wakes, replies);
            } catch (...) {}
        shell_link_free_frame(data, length);
    }
    engine.pumping = false;
    Flush(false);
    // Wakeups only schedule Gecko work; they run after the link is consistent again.
    for (auto& [notify, context] : wakes) notify(context);
    for (auto& [request, result, reply] : replies)
        if (engine.startupSink)
            engine.startupSink(engine.startupContext, request, result, SUCCEEDED(result) ? reply.c_str() : nullptr);
}
inline uint64_t NextStartup() noexcept {
    return ++Gecko().nextStartup;
}
// A reply produced on Gecko's thread itself, delivered on a later turn like a framed one.
inline void PostLocalReply(uint64_t request, HRESULT result, std::wstring reply) noexcept {
    auto& engine = Gecko();
    try {
        engine.localReplies.emplace_back(request, result, std::move(reply));
    } catch (...) {
        return;
    }
    if (engine.wake) PostMessageW(engine.wake, WakeMessage, 0, 0);
}
inline HRESULT WINAPI SetStartupSink(WinUIPaneStartupReplyFn sink, void* context) noexcept {
    if (sink && !EnsureEngine()) return E_OUTOFMEMORY;
    auto& engine = Gecko();
    engine.startupSink = sink;
    engine.startupContext = context;
    return S_OK;
}
// The window's serial, introducing a new window to the shell with its handle first.
inline uint32_t Serial(HWND hwnd) noexcept {
    auto& engine = Gecko();
    uint32_t serial{};
    if (shell_link_serial(engine.link, reinterpret_cast<uintptr_t>(hwnd), false, &serial) == SHELL_STATUS_OK)
        return serial;
    if (shell_link_serial(engine.link, reinterpret_cast<uintptr_t>(hwnd), true, &serial) != SHELL_STATUS_OK) return 0;
    const uint64_t handle = reinterpret_cast<uintptr_t>(hwnd);
    ShellLinkSend outcome{};
    shell_link_send(engine.link, serial, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_INTRODUCE, 0,
                    reinterpret_cast<const uint8_t*>(&handle), sizeof(handle), &outcome);
    return serial;
}
inline HRESULT Send(HWND hwnd, uint16_t channel, uint16_t kind, uint64_t revision, const void* payload,
                    size_t length) noexcept {
    if (!hwnd) return E_INVALIDARG;
    if (!EnsureEngine()) return E_OUTOFMEMORY;
    auto& engine = Gecko();
    Pump();
    const auto serial = Serial(hwnd);
    if (!serial) return E_INVALIDARG;
    // A window the shell could not give a pane has none: its publications fail here, so Gecko's
    // own failure handling runs as it did when the first, waited call failed.
    if (channel != SHELL_FRAME_LIFETIME) {
        auto refused = engine.refused.find(serial);
        if (refused != engine.refused.end() && (refused->second & (1u << SHELL_FRAME_REFUSED_STRIP))) return E_FAIL;
    }
    ShellLinkSend outcome{};
    if (shell_link_send(engine.link, serial, channel, kind, revision, static_cast<const uint8_t*>(payload), length,
                        &outcome) != SHELL_STATUS_OK)
        return E_UNEXPECTED;
    Flush(false);
    return Result(outcome);
}
inline HRESULT SendText(HWND hwnd, uint16_t channel, uint16_t kind, const wchar_t* text) noexcept {
    if (!text) return E_POINTER;
    const size_t units = wcsnlen(text, 1048577);
    if (units > 1048576) return E_INVALIDARG;
    return Send(hwnd, channel, kind, 0, text, units * sizeof(wchar_t));
}
inline HRESULT SendRequest(HWND hwnd, const WinUIPaneWindowRequest& request) noexcept {
    if (!EnsureEngine()) return E_OUTOFMEMORY;
    auto& engine = Gecko();
    const ShellFrameWindowRequest frame{request.kind,
                                        request.flags,
                                        {request.rect.left, request.rect.top, request.rect.right, request.rect.bottom},
                                        static_cast<int64_t>(request.value)};
    const auto id = ++engine.nextRequest;
    Pump();
    const auto serial = Serial(hwnd);
    if (!serial) return E_INVALIDARG;
    if (request.pending) engine.pending[id] = {serial, request.pending};
    const auto result = Send(hwnd, SHELL_FRAME_WINDOW, 0, id, &frame, sizeof(frame));
    if (FAILED(result)) engine.pending.erase(id);
    return result;
}
// Registered here; the shell only learns whether to send batches at all.
inline HRESULT SetSink(HWND hwnd, WinUIPaneNotifyFn notify, void* context) noexcept {
    if (!EnsureEngine()) return E_OUTOFMEMORY;
    auto& engine = Gecko();
    Pump();
    const auto serial = Serial(hwnd);
    if (!serial) return E_INVALIDARG;
    auto found = engine.sinks.find(serial);
    if (notify) {
        if (found != engine.sinks.end() && found->second == std::make_pair(notify, context)) return S_OK;
        const bool first = found == engine.sinks.end();
        engine.sinks[serial] = {notify, context};
        return first ? Send(hwnd, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_WAKEUP_ON, 0, nullptr, 0) : S_OK;
    }
    if (found == engine.sinks.end()) return S_OK;
    engine.sinks.erase(found);
    return Send(hwnd, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_WAKEUP_OFF, 0, nullptr, 0);
}
inline HRESULT Drain(HWND hwnd, wchar_t* output, uint32_t capacity) noexcept {
    if (!output || capacity < 1) return E_INVALIDARG;
    output[0] = 0;
    if (!EnsureEngine()) return E_OUTOFMEMORY;
    auto& engine = Gecko();
    Pump();
    uint32_t serial{};
    if (shell_link_serial(engine.link, reinterpret_cast<uintptr_t>(hwnd), false, &serial) != SHELL_STATUS_OK)
        return S_FALSE;
    auto found = engine.mailbox.find(serial);
    if (found == engine.mailbox.end()) return S_FALSE;
    if (found->second.size() >= capacity) return E_NOT_SUFFICIENT_BUFFER;
    wcscpy_s(output, capacity, found->second.c_str());
    engine.mailbox.erase(found);
    return S_OK;
}
inline uint32_t KnownSerial(HWND hwnd) noexcept {
    auto& engine = Gecko();
    uint32_t serial{};
    if (!engine.link ||
        shell_link_serial(engine.link, reinterpret_cast<uintptr_t>(hwnd), false, &serial) != SHELL_STATUS_OK)
        return 0;
    return serial;
}
inline bool Refused(HWND hwnd, uint16_t surface) noexcept {
    Pump();
    const auto serial = KnownSerial(hwnd);
    auto& refused = Gecko().refused;
    auto found = serial ? refused.find(serial) : refused.end();
    return found != refused.end() && (found->second & (1u << surface));
}
inline void ClearRefused(HWND hwnd) noexcept {
    if (const auto serial = KnownSerial(hwnd)) Gecko().refused.erase(serial);
}
inline void RememberEndpoint(HWND hwnd, HWND endpoint) noexcept {
    if (!EnsureEngine()) return;
    if (const auto serial = Serial(hwnd)) Gecko().endpoints[serial] = endpoint;
}
// The window is gone on Gecko's side: nothing more is sent for it or delivered to it.
inline void Retire(HWND hwnd) noexcept {
    auto& engine = Gecko();
    if (!engine.link) return;
    uint32_t serial{};
    if (shell_link_serial(engine.link, reinterpret_cast<uintptr_t>(hwnd), false, &serial) != SHELL_STATUS_OK) return;
    engine.sinks.erase(serial);
    engine.mailbox.erase(serial);
    engine.refused.erase(serial);
    engine.endpoints.erase(serial);
    std::erase_if(engine.pending, [serial](auto const& entry) { return entry.second.first == serial; });
    shell_link_retire(engine.link, serial);
}

inline void StartShell() noexcept {
    Native().link = shell_link_create(Generation, nullptr);
}
#ifndef FENN_SHELL_PROCESS
void FlushShell() noexcept {
    auto& shell = Native();
    if (!shell.link) return;
    for (;;) {
        uint8_t* data{};
        size_t length{};
        if (shell_link_take(shell.link, false, &data, &length) != SHELL_STATUS_OK || !data) return;
        // Ownership transfers to the inbox, which releases each received frame.
        DeliverToEngine(data, length);
    }
}
#endif
}  // namespace PaneLink
