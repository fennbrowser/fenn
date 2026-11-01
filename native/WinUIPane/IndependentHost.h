#pragma once
#include <atomic>
#include <thread>
#include <tuple>
#include "StartupPlacement.h"

// Development-only owning-thread dispatcher. Gecko can wait for bounded pane
// calls; this thread never waits for Gecko or runs its window procedure.
namespace IndependentHost {
inline constexpr UINT InvokeMessage=WM_APP+901;
inline constexpr uint32_t MaxPosted=1024;
inline constexpr size_t MaxPostedChars=16*1024*1024;
struct Work { void (*run)(Work*); HANDLE complete; };
struct Host {
    std::thread thread;
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<HWND> dispatcher{}, shell{};
    std::atomic<DWORD> owner{};
    std::atomic<uint32_t> posted{};
    std::atomic<size_t> postedChars{};
    HRESULT result=E_PENDING;
    bool started{};
};
inline Host& State() { static auto host=new Host; return *host; }
inline LRESULT CALLBACK Dispatch(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message==InvokeMessage) {
        auto* work=reinterpret_cast<Work*>(l);
        const auto complete=work->complete;
        work->run(work); if (complete) SetEvent(complete); return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
template<auto Function, class... Args>
auto Call(Args... args) noexcept -> decltype(Function(args...)) {
    using Result=decltype(Function(args...));
    auto& host=State();
    if (host.owner==GetCurrentThreadId()) return Function(args...);
    if (!host.dispatcher) return static_cast<Result>(RO_E_CLOSED);
    struct Job : Work { std::tuple<Args...> args; Result result{}; };
    Job job{}; job.args=std::make_tuple(args...);
    job.complete=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!job.complete) return static_cast<Result>(E_OUTOFMEMORY);
    job.run=[](Work* work) { auto& job=*static_cast<Job*>(work); job.result=std::apply(Function,job.args); };
    if (!PostMessageW(host.dispatcher,InvokeMessage,0,reinterpret_cast<LPARAM>(&job))) {
        CloseHandle(job.complete); return static_cast<Result>(RO_E_CLOSED);
    }
    // Borrowed ABI buffers remain valid until the native call completes. Only
    // the Gecko caller waits; no Gecko callbacks occur on the native thread.
    WaitForSingleObject(job.complete,INFINITE);
    CloseHandle(job.complete); return job.result;
}
// For a call whose result no caller acts on: copies its text, keeps the order of
// other routed calls, and does not wait. The bound keeps a stalled native thread
// from growing the queue; going over it is the caller's failure, not a wait.
template<auto Function>
HRESULT WINAPI Post(HWND hwnd, const wchar_t* text) noexcept {
    auto& host=State();
    if (host.owner==GetCurrentThreadId()) return Function(hwnd,text);
    if (!text) return E_POINTER;
    if (!host.dispatcher) return RO_E_CLOSED;
    const auto chars=wcslen(text);
    if (host.posted.fetch_add(1)>=MaxPosted || host.postedChars.fetch_add(chars)+chars>MaxPostedChars) {
        host.posted.fetch_sub(1); host.postedChars.fetch_sub(chars); return E_OUTOFMEMORY;
    }
    struct Job : Work { HWND hwnd; std::wstring text; };
    auto* job=new (std::nothrow) Job;
    if (job) try { job->text=text; } catch (...) { delete job; job=nullptr; }
    if (!job) { host.posted.fetch_sub(1); host.postedChars.fetch_sub(chars); return E_OUTOFMEMORY; }
    job->complete=nullptr; job->hwnd=hwnd;
    job->run=[](Work* work) {
        std::unique_ptr<Job> job(static_cast<Job*>(work));
        const auto result=Function(job->hwnd,job->text.c_str());
        State().posted.fetch_sub(1); State().postedChars.fetch_sub(job->text.size());
        if (FAILED(result) && DiagnosticsEnabled()) fprintf(stderr,"WinUIPane posted call failed hr=%08x\n",unsigned(result));
    };
    if (!PostMessageW(host.dispatcher,InvokeMessage,0,reinterpret_cast<LPARAM>(job))) {
        delete job; host.posted.fetch_sub(1); host.postedChars.fetch_sub(chars); return RO_E_CLOSED;
    }
    return S_OK;
}
inline HRESULT WINAPI Start(const wchar_t* profilePath, const wchar_t* initialPresentation, HWND* output) noexcept {
    if (!output) return E_POINTER;
    *output=nullptr;
    if(initialPresentation && wcsnlen(initialPresentation,32769)>32768)return E_INVALIDARG;
    auto& host=State();
    if (host.started) return E_NOT_VALID_STATE;
    host.started=true;
    try {
        const std::wstring profile=profilePath ? profilePath : L"";
        const std::wstring presentation=initialPresentation ? initialPresentation : L"";
        // Early startup (FENN_NATIVE_EARLY_STARTUP=1): hand Gecko the HWND once the shell window exists and
        // build the controls on this thread while Gecko keeps starting. Gecko's later claim is a job on this
        // thread, so it waits only if construction has not finished by then.
        wchar_t earlyValue[2]{};
        const bool early=GetEnvironmentVariableW(L"FENN_NATIVE_EARLY_STARTUP",earlyValue,2)==1 && earlyValue[0]==L'1';
        host.thread=std::thread([&host,profile,presentation,early] {
            NativeWindowHost::FocusTrace("host-thread-start");
            HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            try {
                check_hresult(apartment); host.owner=GetCurrentThreadId();
                WNDCLASSW wc{}; wc.lpfnWndProc=Dispatch; wc.hInstance=GetModuleHandleW(nullptr);
                wc.lpszClassName=L"Fenn.NativeDispatcher.v1";
                check_bool(RegisterClassW(&wc));
                host.dispatcher=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,nullptr);
                check_bool(host.dispatcher!=nullptr);
                const auto placement=NativeStartupPlacement::Read(profile);
                HWND shell{};
                check_hresult(NativeWindowHost::CreateStartup(0,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,placement.bounds,&shell));
                host.shell=shell;
                NativeStartupPlacement::Apply(shell,placement);
                check_bool(SetPropW(shell,L"Fenn.NativeStartupOrigin",reinterpret_cast<HANDLE>(2)));
                NativeWindowHost::FocusTrace("host-shell-created");
                if (early) { host.result=S_OK; SetEvent(host.ready); }
#ifdef WINUI_PANE_TESTING
                if (early) {
                    wchar_t fault[32]{};
                    if (GetEnvironmentVariableW(L"FENN_TEST_NATIVE_FAULT",fault,32)>0 && !wcscmp(fault,L"after-ready"))check_hresult(E_FAIL);
                }
#endif
                check_hresult(ShowStartupSurfaceForProfile(shell,profile.c_str(),presentation.empty()?nullptr:presentation.c_str()));
                if (!early) { host.result=S_OK; SetEvent(host.ready); }
                NativeWindowHost::FocusTrace("host-pump-start");
                MSG message{};
                while (GetMessageW(&message,nullptr,0,0)>0) {
                    if (!NativePageInput::Key(&message) && !Translate(&message)) {
                        const auto* binding=NativeWindowHost::Get(message.hwnd);
                        const bool browserKey=binding && binding->endpoint &&
                            (message.message==WM_KEYDOWN || message.message==WM_KEYUP ||
                             message.message==WM_SYSKEYDOWN || message.message==WM_SYSKEYUP);
                        // Gecko translates these transported keys exactly once.
                        // Translating here as well mutates the dead-key state
                        // before Gecko consumes the same sequence. XAML child
                        // windows retain their ordinary native translation.
                        if (!browserKey) TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
            } catch (...) {
                host.result=to_hresult();
                // Tell Gecko this was a failure, so it starts a normal window instead of treating the
                // missing host as the user closing it.
                SetEnvironmentVariableW(L"FENN_NATIVE_HOST_FAILED",L"1");
                SetEvent(host.ready);
            }
            if (IsWindow(host.shell)) NativeWindowHost::Destroy(host.shell);
            Shutdown();
            if (host.dispatcher) DestroyWindow(host.dispatcher);
            host.dispatcher=nullptr;
            if (SUCCEEDED(apartment)) CoUninitialize();
        });
        WaitForSingleObject(host.ready,INFINITE);
        if (FAILED(host.result)) { host.thread.join(); return host.result; }
        *output=host.shell; return S_OK;
    } catch (...) { return to_hresult(); }
}
inline HRESULT WINAPI Stop() noexcept {
    auto& host=State();
    if (!host.thread.joinable()) return S_FALSE;
    if (host.owner==GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    // The Gecko shutdown path has already destroyed its widgets and stopped
    // publications. The owner releases all WinRT objects before COM teardown.
    PostThreadMessageW(host.owner,WM_QUIT,0,0);
    host.thread.join(); return S_OK;
}
inline HRESULT Create(DWORD ex, const wchar_t*, DWORD style, const RECT* bounds,
                      HWND parent, HINSTANCE, HWND* output) noexcept {
    if (!bounds || !output) return E_POINTER;
    if (parent) return E_INVALIDARG;
    // Later browser windows also begin in the native class, never a Firefox
    // window class whose default procedure could execute on this UI thread.
    return NativeWindowHost::CreateStartup(ex,style,*bounds,output);
}
inline HRESULT Bind(HWND hwnd, uintptr_t identity, HWND endpoint) noexcept {
    auto* host=NativeWindowHost::Get(hwnd);
    if (!host) return E_INVALIDARG;
    if (host->state.phase==0) {
        ShellWindowBinding binding{}; binding.shell=reinterpret_cast<uintptr_t>(hwnd);
        if (host->Call(SHELL_WINDOW_HOST_OPERATION_PREPARE_STARTUP,binding)!=SHELL_STATUS_OK) return E_FAIL;
    }
    return NativeWindowHost::BindEndpoint(hwnd,host->state.generation,identity,endpoint);
}
inline HRESULT Resolve(HWND hwnd, uint64_t generation, uint64_t request) noexcept {
    return NativeWindowHost::ResolveEndpointClose(hwnd,generation,request,false);
}
inline BOOL WINAPI IgnoreTranslate(MSG*) noexcept { return FALSE; }
inline void Route(WinUIPaneApi& api) {
    if (!State().started) return;
#define ROUTE(field, function) api.field=[](auto... args) noexcept { return Call<function>(args...); }
    ROUTE(focus,Focus); ROUTE(destroy,Destroy); api.translate=IgnoreTranslate; api.shutdown=Stop;
    ROUTE(syncTabs,SyncTabs); api.syncAddress=Post<PostedSyncAddress>; ROUTE(clip,Clip);
    api.syncTabMenu=Post<PostedSyncTabMenu>; ROUTE(updateTabStrip,UpdateTabStrip);
    ROUTE(setNotifySink,SetNotifySink); ROUTE(drainRequests,DrainRequests);
    api.completeRequests=Post<CompleteRequests>; ROUTE(applyTabDelta,ApplyTabDelta);
    ROUTE(updateAddress,UpdateAddress); ROUTE(focusAddress,FocusAddress);
    ROUTE(measureChromeLayout,MeasureChromeLayout);
    ROUTE(createWindowHost,Create); ROUTE(detachWindowHost,NativeWindowHost::Detach);
    ROUTE(destroyWindowHost,NativeWindowHost::Destroy);
    ROUTE(showStartupSurface,ShowStartupSurface); ROUTE(dismissStartupSurface,DismissStartupSurface);
    ROUTE(bindIndependentEndpoint,Bind); ROUTE(cancelIndependentClose,Resolve);
    ROUTE(setIndependentAccessibility,NativeWindowHost::SetAccessibility);
    ROUTE(publishIndependentGeometry,NativePageInput::Geometry); ROUTE(publishIndependentInput,NativePageInput::Publish); ROUTE(drainIndependentInput,NativePageInput::Drain);
    ROUTE(commitIndependentInput,NativePageInput::Commit);
    ROUTE(startupTransfer,StartupTransfer);
#undef ROUTE
}
}
