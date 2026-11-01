#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
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
// The newest geometry for one window and call. Only the first call for a surface
// waits, because that one creates it and its caller acts on the result.
struct GeometrySlot { bool pending{}; RECT rect{}; std::vector<RECT> clip; };
struct Host {
    std::thread thread;
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<HWND> dispatcher{}, shell{};
    std::atomic<DWORD> owner{};
    std::atomic<uint32_t> posted{};
    std::atomic<size_t> postedChars{};
    std::mutex geometryLock;
    std::map<std::pair<HWND,int>,struct GeometrySlot> geometry;
    std::map<HWND,std::pair<WinUIPaneNotifyFn,void*>> sinks;
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
// For a call whose result no caller acts on: owns copies of its arguments, keeps the
// order of other routed calls, and does not wait. The bound keeps a stalled native
// thread from growing the queue; going over it is the caller's failure, not a wait.
template<class Apply>
HRESULT PostJob(size_t chars, Apply apply) noexcept {
    auto& host=State();
    if (!host.dispatcher) return RO_E_CLOSED;
    if (host.posted.fetch_add(1)>=MaxPosted || host.postedChars.fetch_add(chars)+chars>MaxPostedChars) {
        host.posted.fetch_sub(1); host.postedChars.fetch_sub(chars); return E_OUTOFMEMORY;
    }
    struct Job : Work { size_t chars; Apply apply; explicit Job(Apply&& value) : apply(std::move(value)) {} };
    auto* job=new (std::nothrow) Job(std::move(apply));
    if (!job) { host.posted.fetch_sub(1); host.postedChars.fetch_sub(chars); return E_OUTOFMEMORY; }
    job->complete=nullptr; job->chars=chars;
    job->run=[](Work* work) {
        std::unique_ptr<Job> job(static_cast<Job*>(work));
        const HRESULT result=job->apply();
        State().posted.fetch_sub(1); State().postedChars.fetch_sub(job->chars);
        if (FAILED(result) && DiagnosticsEnabled()) fprintf(stderr,"WinUIPane posted call failed hr=%08x\n",unsigned(result));
    };
    if (!PostMessageW(host.dispatcher,InvokeMessage,0,reinterpret_cast<LPARAM>(job))) {
        delete job; host.posted.fetch_sub(1); host.postedChars.fetch_sub(chars); return RO_E_CLOSED;
    }
    return S_OK;
}
template<auto Function>
HRESULT WINAPI Post(HWND hwnd, const wchar_t* text) noexcept {
    if (State().owner==GetCurrentThreadId()) return Function(hwnd,text);
    if (!text) return E_POINTER;
    try {
        std::wstring copy=text;
        const auto chars=copy.size();
        return PostJob(chars,[hwnd,copy=std::move(copy)]{return Function(hwnd,copy.c_str());});
    } catch (...) { return E_OUTOFMEMORY; }
}
// Focus is fire and forget: a failure comes back as a refused publication.
template<auto Function, auto Reported>
HRESULT WINAPI PostFocus(HWND hwnd, BOOL flag) noexcept {
    if (State().owner==GetCurrentThreadId()) return Function(hwnd,flag);
    return PostJob(0,[hwnd,flag]{return Reported(hwnd,flag);});
}
// Validated here as well, so a malformed publication still fails at its caller.
inline HRESULT WINAPI PostSyncTabs(HWND hwnd, const WinUITab* tabs, uint32_t count, uint32_t selected,
                                   uint32_t revision, uint32_t* requested) noexcept {
    if (State().owner==GetCurrentThreadId()) return SyncTabs(hwnd,tabs,count,selected,revision,requested);
    if (!requested) return E_POINTER;
    *requested=0;
    try {
        if (!hwnd || !ValidTabPublication(tabs,count,selected,revision)) return E_INVALIDARG;
        std::vector<std::wstring> titles;
        std::vector<WinUITab> rows(tabs,tabs+count);
        titles.reserve(count);
        size_t chars=0;
        for (uint32_t i=0;i<count;++i) { titles.emplace_back(tabs[i].title); chars+=titles.back().size(); }
        const auto result=PostJob(chars,[hwnd,titles=std::move(titles),rows=std::move(rows),selected,revision]() mutable {
            for (size_t i=0;i<rows.size();++i) rows[i].title=titles[i].c_str();
            return ReportedSyncTabs(hwnd,rows.data(),uint32_t(rows.size()),selected,revision);
        });
        if (SUCCEEDED(result)) *requested=revision;
        return result;
    } catch (...) { return E_OUTOFMEMORY; }
}
// Any nonzero revision tells the widget the delta was taken; the producer already
// holds its own stamp, and a refusal returns as a publication-rejected request.
inline HRESULT WINAPI PostTabDelta(HWND hwnd, const wchar_t* input, uint32_t* revision) noexcept {
    if (State().owner==GetCurrentThreadId()) return ApplyTabDelta(hwnd,input,revision);
    if (!revision) return E_POINTER;
    *revision=0;
    const auto result=Post<PostedApplyTabDelta>(hwnd,input);
    if (SUCCEEDED(result)) *revision=1;
    return result;
}
// Geometry is latest-wins, so a stalled native thread queues at most one job per
// surface and going over a bound can never turn into a permanent fallback.
template<auto Function, int Kind, class Prepare, class Apply>
HRESULT PostGeometry(HWND hwnd, Prepare prepare, Apply apply) noexcept {
    auto& host=State();
    if (!host.dispatcher) return RO_E_CLOSED;
    try {
        std::unique_lock lock(host.geometryLock);
        auto& slot=host.geometry[{hwnd,Kind}];
        prepare(slot);
        if (slot.pending) return S_OK;
        slot.pending=true;
        lock.unlock();
        struct Job : Work { HWND hwnd; Apply apply; };
        auto* job=new Job;
        job->complete=nullptr; job->hwnd=hwnd; job->apply=std::move(apply);
        job->run=[](Work* work) {
            std::unique_ptr<Job> job(static_cast<Job*>(work));
            GeometrySlot slot;
            {
                std::lock_guard guard(State().geometryLock);
                auto found=State().geometry.find({job->hwnd,Kind});
                if (found==State().geometry.end() || !found->second.pending) return;
                found->second.pending=false; slot=found->second;
            }
            job->apply(job->hwnd,slot);
        };
        if (!PostMessageW(host.dispatcher,InvokeMessage,0,reinterpret_cast<LPARAM>(job))) {
            delete job;
            std::lock_guard guard(host.geometryLock);
            host.geometry[{hwnd,Kind}].pending=false;
            return RO_E_CLOSED;
        }
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}
inline bool GeometryKnown(HWND hwnd, int kind) {
    auto& host=State();
    std::lock_guard guard(host.geometryLock);
    return host.geometry.count({hwnd,kind})!=0;
}
inline void GeometryForget(HWND hwnd, int kind) {
    auto& host=State();
    std::lock_guard guard(host.geometryLock);
    host.geometry.erase({hwnd,kind});
}
inline void GeometryRecord(HWND hwnd, int kind) {
    auto& host=State();
    std::lock_guard guard(host.geometryLock);
    host.geometry[{hwnd,kind}];
}
inline void GeometryForgetWindow(HWND hwnd) {
    auto& host=State();
    std::lock_guard guard(host.geometryLock);
    std::erase_if(host.geometry,[hwnd](auto const& entry){return entry.first.first==hwnd;});
    host.sinks.erase(hwnd);
}
template<auto Function, auto Reported, int Kind>
HRESULT WINAPI PostRect(HWND hwnd, const RECT* rect, BOOL visible) noexcept {
    auto& host=State();
    if (host.owner==GetCurrentThreadId()) return Function(hwnd,rect,visible);
    if (!rect || rect->right<rect->left || rect->bottom<rect->top) return E_INVALIDARG;
    if (!visible) { GeometryForget(hwnd,Kind); return Call<Function>(hwnd,rect,visible); }
    if (!GeometryKnown(hwnd,Kind)) {
        const auto result=Call<Function>(hwnd,rect,visible);
        if (SUCCEEDED(result)) GeometryRecord(hwnd,Kind);
        return result;
    }
    const RECT copy=*rect;
    return PostGeometry<Function,Kind>(hwnd,[copy](GeometrySlot& slot){slot.rect=copy;},
        [](HWND target, GeometrySlot const& slot){Reported(target,&slot.rect,TRUE);});
}
template<auto Function, auto Reported, int Kind>
HRESULT WINAPI PostClip(HWND hwnd, const RECT* rects, uint32_t count) noexcept {
    auto& host=State();
    if (host.owner==GetCurrentThreadId()) return Function(hwnd,rects,count);
    if (!GeometryKnown(hwnd,Kind)) {
        const auto result=Call<Function>(hwnd,rects,count);
        if (SUCCEEDED(result)) GeometryRecord(hwnd,Kind);
        return result;
    }
    try {
        std::vector<RECT> copy(rects,rects+count);
        return PostGeometry<Function,Kind>(hwnd,[copy=std::move(copy)](GeometrySlot& slot){slot.clip=copy;},
            [](HWND target, GeometrySlot const& slot){Reported(target,slot.clip.data(),uint32_t(slot.clip.size()));});
    } catch (...) { return E_OUTOFMEMORY; }
}
// Registered again on every geometry update; only a change reaches the native thread.
inline HRESULT WINAPI PostSink(HWND hwnd, WinUIPaneNotifyFn notify, void* context) noexcept {
    auto& host=State();
    if (host.owner==GetCurrentThreadId()) return SetNotifySink(hwnd,notify,context);
    {
        std::lock_guard guard(host.geometryLock);
        auto found=host.sinks.find(hwnd);
        if (notify && found!=host.sinks.end() && found->second==std::make_pair(notify,context)) return S_OK;
    }
    const auto result=Call<SetNotifySink>(hwnd,notify,context);
    std::lock_guard guard(host.geometryLock);
    if (SUCCEEDED(result) && notify) host.sinks[hwnd]={notify,context};
    else host.sinks.erase(hwnd);
    return result;
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
    api.focus=PostFocus<Focus,ReportedFocus>; api.destroy=[](HWND hwnd) noexcept { GeometryForgetWindow(hwnd); return Call<Destroy>(hwnd); }; api.translate=IgnoreTranslate; api.shutdown=Stop;
    api.syncTabs=PostSyncTabs; api.syncAddress=Post<PostedSyncAddress>; api.clip=PostClip<Clip,ReportedClip,1>;
    api.syncTabMenu=Post<PostedSyncTabMenu>; api.updateTabStrip=PostRect<UpdateTabStrip,ReportedUpdateTabStrip,0>;
    api.setNotifySink=PostSink; ROUTE(drainRequests,DrainRequests);
    api.completeRequests=Post<CompleteRequests>; api.applyTabDelta=PostTabDelta;
    api.updateAddress=PostRect<UpdateAddress,ReportedUpdateAddress,2>; api.focusAddress=PostFocus<FocusAddress,ReportedFocusAddress>;
    // Reads only window state the OS answers from any thread, then computes.
    api.measureChromeLayout=MeasureChromeLayout;
    ROUTE(createWindowHost,Create); ROUTE(detachWindowHost,NativeWindowHost::Detach);
    api.destroyWindowHost=[](HWND hwnd) noexcept { GeometryForgetWindow(hwnd); return Call<NativeWindowHost::Destroy>(hwnd); };
    ROUTE(showStartupSurface,ShowStartupSurface); ROUTE(dismissStartupSurface,DismissStartupSurface);
    ROUTE(bindIndependentEndpoint,Bind); ROUTE(cancelIndependentClose,Resolve);
    ROUTE(setIndependentAccessibility,NativeWindowHost::SetAccessibility);
    ROUTE(publishIndependentGeometry,NativePageInput::Geometry); ROUTE(publishIndependentInput,NativePageInput::Publish); ROUTE(drainIndependentInput,NativePageInput::Drain);
    ROUTE(commitIndependentInput,NativePageInput::Commit);
    ROUTE(startupTransfer,StartupTransfer);
#undef ROUTE
}
}
