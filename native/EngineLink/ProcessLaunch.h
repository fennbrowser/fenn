#pragma once

// Owns one Win32 CreateProcess/transport setup operation, not browser lifecycle
// state. ProductLink and both ShellCore link ends remain on their owner threads.
namespace EnginePipe {
class ProcessLaunch {
    std::atomic<bool> cancelled{}, done{};
    std::thread worker;
    HRESULT result = E_PENDING;
    DWORD pid{};
#if defined(FENN_ENGINE_LINK_TESTING) || defined(FENN_SHELL_TESTING)
    bool TestGate(const wchar_t* phase) {
        wchar_t selected[64]{}, name[200]{};
        if (!GetEnvironmentVariableW(L"FENN_TEST_LAUNCH_PHASE", selected, 64) || wcscmp(selected, phase)) return true;
        const auto length = GetEnvironmentVariableW(L"FENN_TEST_LAUNCH_GATE", name, 200);
        if (!length || length >= 200) return false;
        winrt::handle reached{OpenEventW(EVENT_MODIFY_STATE, FALSE, (std::wstring(name) + L".reached").c_str())};
        winrt::handle release{OpenEventW(SYNCHRONIZE, FALSE, name)};
        if (!reached || !release) return false;
        SetEvent(reached.get());
        const auto deadline = GetTickCount64() + 10000;
        while (!cancelled && GetTickCount64() < deadline)
            if (WaitForSingleObject(release.get(), 10) == WAIT_OBJECT_0) return true;
        return false;
    }
#endif
    struct Child {
        PROCESS_INFORMATION info{};
        bool released{};
        ~Child() {
            if (info.hProcess && !released) TerminateProcess(info.hProcess, ERROR_PROCESS_ABORTED);
            if (info.hThread) CloseHandle(info.hThread);
            if (info.hProcess) CloseHandle(info.hProcess);
        }
    };

   public:
    static constexpr UINT Completed = WM_APP + 926;
    ProcessLaunch(Connection& connection, winrt::handle server, std::wstring executable, std::wstring command,
                  uint32_t generation, HWND target, HWND input, UINT inputMessage,
                  std::vector<wchar_t> environment = {}, bool launcherChild = false, ShellRecoveryHistory budget = {}) {
        worker = std::thread([this, &connection, server = std::move(server), executable = std::move(executable),
                              command = std::move(command), environment = std::move(environment), generation, target,
                              input, inputMessage, launcherChild, budget]() mutable {
            result = [&]() -> HRESULT {
                try {
                    STARTUPINFOW info{sizeof(info)};
                    Child child;
                    if (cancelled) return E_ABORT;
#if defined(FENN_ENGINE_LINK_TESTING) || defined(FENN_SHELL_TESTING)
                    if (!TestGate(L"before-create")) return E_ABORT;
#endif
                    Trace("spawn-attempt");
                    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                        environment.empty() ? nullptr : environment.data(), nullptr, &info,
                                        &child.info))
                        return HRESULT_FROM_WIN32(GetLastError());
                    pid = child.info.dwProcessId;
                    Trace("spawn-hidden", 0, pid);
                    if (cancelled) return E_ABORT;
#if defined(FENN_ENGINE_LINK_TESTING) || defined(FENN_SHELL_TESTING)
                    if (!TestGate(L"before-resume")) return E_ABORT;
#endif
                    winrt::handle peer;
                    if (!DuplicateHandle(GetCurrentProcess(), child.info.hProcess, GetCurrentProcess(), peer.put(), 0,
                                         FALSE, DUPLICATE_SAME_ACCESS))
                        return HRESULT_FROM_WIN32(GetLastError());
                    connection.Start(server.detach(), peer.detach(), pid, generation, target, true, Protocol, input,
                                     inputMessage, launcherChild, budget);
#ifdef FENN_ENGINE_LINK_TESTING
                    wchar_t fault[64]{};
                    if (GetEnvironmentVariableW(L"FENN_TEST_PROCESS_FAULT", fault, 64) < 64 &&
                        !wcscmp(fault, L"before-resume"))
                        return E_ABORT;
#endif
                    if (cancelled) return E_ABORT;
                    if (ResumeThread(child.info.hThread) == DWORD(-1)) return HRESULT_FROM_WIN32(GetLastError());
                    // Stop joins this worker before closing the transport. If cancellation
                    // races resume, retain child ownership and terminate it here.
                    if (cancelled) return E_ABORT;
                    child.released = true;
                    return S_OK;
                } catch (...) {
                    return winrt::to_hresult();
                }
            }();
            done.store(true, std::memory_order_release);
            PostMessageW(target, Completed, generation, 0);
        });
    }
    ProcessLaunch(const ProcessLaunch&) = delete;
    ~ProcessLaunch() { CancelAndJoin(); }
    void CancelAndJoin() {
        cancelled = true;
        if (worker.joinable()) worker.join();
    }
    bool Done() const { return done.load(std::memory_order_acquire); }
    HRESULT Result() const { return result; }  // Only after Done or Join.
    DWORD Pid() const { return pid; }
};
}  // namespace EnginePipe
