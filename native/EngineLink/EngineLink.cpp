#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define FENN_ENGINE_LINK
#include <windows.h>
#include <objbase.h>
#include <uiautomation.h>
#include <commctrl.h>
#include <winrt/base.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <functional>
#include <vector>
#include "../../bridge/WinUIPaneApi.h"
#include "Pipe.h"
#include "Milestone.h"
#include "../WinUIPane/NativePageWindow.h"
#include "../WinUIPane/InputThread.h"
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "runtimeobject.lib")

#ifdef FENN_ENGINE_LINK_TESTING
namespace MilestoneFixture {
EnginePipe::Connection* connection{};
ShellLinkEnd* mainLink{};
ShellPipeBinding binding{};
HWND dispatcher{}, proxy{}, shell{};
DWORD owner{}, shellPid{};
std::atomic<HWND> page{};
uint64_t dropped{};
bool ready{}, retired{}, failed{};
UINT_PTR timer{};
constexpr UINT PageReady = WM_APP + 921;

void Flush() {
    if (mainLink && connection) EnginePipe::Flush(*connection, mainLink, 0);
}
bool Credit(const uint8_t* data, size_t length) noexcept {
    return connection && connection->Send(1, data, length);
}
void DrainInput() noexcept {
    uint8_t* bytes{};
    size_t length{};
    while (connection && connection->TakeRoute(1, bytes, length)) InputThread::Receive(bytes, length);
}
void Retire(bool normal) {
    if (retired) return;
    retired = true;
    if (binding.phase) shell_pipe_binding(&binding, 2, binding.serial, 0);
    if (mainLink) {
        if (normal) EnginePipe::Send(mainLink, 1, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_DESTROY_HOST);
        Flush();
        shell_link_retire(mainLink, 1);
    }
    if (shell) InputThread::DestroyPage(shell);
    page = nullptr;
    ready = false;
    // Before a successful attachment Firefox retains its ordinary visible window and toolbar.
    // Post-bind disconnect only tears down in M5d; M5e owns recovery.
    if (!shell && IsWindow(proxy)) ShowWindow(proxy, SW_SHOWNOACTIVATE);
}
LRESULT CALLBACK ProxySubclass(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    // nsWindow::OnDestroy restores its previous WNDPROC during WM_DESTROY,
    // so a comctl subclass cannot rely on seeing WM_NCDESTROY afterwards.
    if (message == WM_DESTROY || message == WM_NCDESTROY) {
        Retire(true);
        RemoveWindowSubclass(window, ProxySubclass, 1);
        proxy = nullptr;
    }
    return DefSubclassProc(window, message, w, l);
}
void Pump() {
    if (!connection) return;
    uint32_t route{};
    uint8_t* bytes{};
    size_t length{};
    while (connection->TakeRoute(0, bytes, length)) {
        if (route == 0 && mainLink) {
            ShellFrameView view{};
            const auto status = shell_link_receive(mainLink, bytes, length, &view);
            if (status == SHELL_STATUS_OK && view.delivered && !retired) {
                if (view.channel == SHELL_FRAME_WINDOW && view.kind == 2 && view.window == 1) {
                    uint64_t handle{};
                    memcpy(&handle, view.payload, 8);
                    const auto candidate = reinterpret_cast<HWND>(static_cast<uintptr_t>(handle));
                    wchar_t role[80]{};
                    GetClassNameW(candidate, role, 80);
                    if (!EnginePipe::Owns(candidate, shellPid) || wcscmp(role, L"WinUIDesktopWin32WindowClass") ||
                        !shell_pipe_binding(&binding, 1, view.window, handle)) {
                        connection->Break();
                    } else {
                        shell = candidate;
                        InputThread::RequestPage(shell, proxy, NativeWindowHost::StartupBackgroundColor());
                        InputThread::Post([candidate, wake = dispatcher] {
                            auto* window = InputThread::Find(candidate);
                            PostMessageW(wake, PageReady, reinterpret_cast<WPARAM>(window ? window->page : nullptr),
                                         reinterpret_cast<LPARAM>(candidate));
                        });
                    }
                } else if (view.channel == SHELL_FRAME_CLOSE) {
                    // Firefox owns beforeunload and close confirmation, including veto.
                    PostMessageW(proxy, WM_CLOSE, 0, 0);
                }
            } else if (status != SHELL_STATUS_OK && status != SHELL_STATUS_REJECTED)
                connection->Break();
        }
        shell_link_free_frame(bytes, length);
    }
    if (mainLink) {
        ShellLinkCounts counts{};
        shell_link_counts(mainLink, &counts);
        dropped = counts.dropped;
    }
    if (connection->Closed()) {
        failed = true;
        Retire(false);
        // Never wait for the input thread while its parent in another process is being destroyed.
    } else
        Flush();
}
LRESULT CALLBACK Dispatch(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == EnginePipe::Wake) {
        Pump();
        return 0;
    }
    if (message == PageReady) {
        const auto candidate = reinterpret_cast<HWND>(w);
        if (!retired && reinterpret_cast<HWND>(l) == shell && candidate &&
            EnginePipe::Owns(candidate, GetCurrentProcessId()) && GetParent(candidate) == shell) {
            page = candidate;
            ready = true;
            ShowWindow(proxy, SW_HIDE);
            EnginePipe::Send(mainLink, 1, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_WAKEUP_ON);
            Flush();
        } else if (!retired) {
            connection->Break();
        } else if (shell)
            InputThread::DestroyPage(shell);
        return 0;
    }
    if (message == WM_TIMER && w == timer) {
        if (!retired && !ready) {
            failed = true;
            Retire(true);
            connection->Break();
        }
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
extern "C" __declspec(dllexport) HRESULT WINAPI FennSplitStart(HWND window, const wchar_t* profile,
                                                               uint32_t flags) noexcept {
    if (connection || !profile || !EnginePipe::Owns(window, GetCurrentProcessId()) ||
        GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())
        return E_INVALIDARG;
    try {
        owner = GetCurrentThreadId();
        proxy = window;
        WNDCLASSW wc{};
        wc.lpfnWndProc = Dispatch;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Fenn.SplitEngineDispatcher.v1";
        RegisterClassW(&wc);
        dispatcher =
            CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!dispatcher || !SetWindowSubclass(proxy, ProxySubclass, 1, 0)) return E_FAIL;
        std::wstring name;
        uint32_t generation{};
        HANDLE pipe = EnginePipe::Server(name, generation);
        if (pipe == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        const auto executable = EnginePipe::Sibling(L"fenn-shell.exe");
        std::wstring command = L"\"" + executable + L"\" --pipe " + name + L" --engine " +
                               std::to_wstring(GetCurrentProcessId()) + L" --generation " +
                               std::to_wstring(generation) + L" --flags " + std::to_wstring(flags) + L" --profile \"" +
                               profile + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
            CloseHandle(pipe);
            return HRESULT_FROM_WIN32(GetLastError());
        }
        shellPid = child.dwProcessId;
        mainLink = EnginePipe::Link(generation, true);
        shell_pipe_binding(&binding, 0, 1, 0);
        shell_link_adopt(mainLink, 1, reinterpret_cast<uintptr_t>(proxy));
        InputThread::ReturnCredit = Credit;
        InputThread::DrainTransport = DrainInput;
        const HRESULT input = InputThread::Start(generation);
        connection = new EnginePipe::Connection;
        connection->Start(pipe, child.hProcess, shellPid + ((flags & 4) ? 1 : 0), generation, dispatcher, true,
                          EnginePipe::Protocol, InputThread::State().dispatcher, InputThread::TransportMessage);
        ResumeThread(child.hThread);
        CloseHandle(child.hThread);
        if (FAILED(input)) {
            Retire(true);
            connection->Break();
            return input;
        }
        // Firefox has accepted this local fixture window. Process creation itself never shows the shell.
        const uint64_t bind[]{1, reinterpret_cast<uintptr_t>(proxy)};
        EnginePipe::Send(mainLink, 1, SHELL_FRAME_LIFETIME, SHELL_FRAME_LIFETIME_BIND, bind, sizeof(bind));
        Flush();
        timer = SetTimer(dispatcher, 1, 10000, nullptr);
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
}
extern "C" __declspec(dllexport) HRESULT WINAPI FennSplitStatus(FennSplitStatus* output) noexcept {
    if (!output || owner != GetCurrentThreadId()) return E_INVALIDARG;
    Pump();
    *output = {binding.phase,
               reinterpret_cast<uintptr_t>(shell),
               shellPid,
               reinterpret_cast<uintptr_t>(proxy),
               reinterpret_cast<uintptr_t>(page.load()),
               connection && connection->Ready(),
               connection ? connection->Error() : 0,
               dropped,
               ready,
               connection && connection->Closed()};
    return S_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI FennSplitClose() noexcept {
    if (owner != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    Retire(true);
    return S_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI FennSplitBreak() noexcept {
    if (owner != GetCurrentThreadId() || !connection) return E_INVALIDARG;
    connection->Break();
    Pump();
    return S_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI FennSplitStall(uint32_t milliseconds) noexcept {
    if (owner != GetCurrentThreadId() || milliseconds > 2000 || !proxy) return E_INVALIDARG;
    SetPropW(proxy, L"Fenn.SplitFixtureStalled", reinterpret_cast<HANDLE>(1));
    Sleep(milliseconds);
    RemovePropW(proxy, L"Fenn.SplitFixtureStalled");
    return S_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI FennSplitStop() noexcept {
    if (owner != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    Retire(true);
    if (timer) KillTimer(dispatcher, timer);
    if (connection) connection->Stop();
    InputThread::Stop();
    if (proxy && IsWindow(proxy)) RemoveWindowSubclass(proxy, ProxySubclass, 1);
    if (dispatcher) DestroyWindow(dispatcher);
    if (mainLink) {
        shell_link_destroy(mainLink);
        mainLink = nullptr;
    }
    delete connection;
    connection = nullptr;
    return S_OK;
}
}  // namespace MilestoneFixture
#endif
#include "ProductLink.h"
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneGetApi(uint32_t version, uint32_t size,
                                                                WinUIPaneApi* output) noexcept {
    return ProductLink::GetApi(version, size, output);
}
