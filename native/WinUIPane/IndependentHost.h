#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include <thread>
#include <tuple>
#include <set>
#include "StartupPlacement.h"
#include "shell_core.h"

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
    // Surfaces that exist on the shell. Only the first call for a surface waits, because that
    // one creates it and its caller acts on the result.
    std::mutex geometryLock;
    std::set<std::pair<HWND,int>> geometry;
    HRESULT result=E_PENDING;
    bool started{};
};
inline Host& State() { static auto host=new Host; return *host; }
}
#include "PaneLink.h"
namespace IndependentHost {
inline void ShellReceive(uint8_t* data, size_t length) noexcept;
inline LRESULT CALLBACK Dispatch(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message==PaneLink::FrameMessage) { ShellReceive(reinterpret_cast<uint8_t*>(l),static_cast<size_t>(w)); return 0; }
    if (message==InvokeMessage) {
        auto* work=reinterpret_cast<Work*>(l);
        const auto complete=work->complete;
        work->run(work); if (complete) SetEvent(complete); return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
// The OS and WinUI can send a message to a Gecko window from the native thread
// (WM_ACTIVATE forwarding). That sender cannot return until the Gecko thread takes the
// message, so a Gecko wait on the native thread has to deliver sent messages.
// Answers whether handle was the one signaled, so a caller can also stop on the other.
inline bool WaitDeliveringSent(HANDLE handle, HANDLE other=nullptr) noexcept {
    const HANDLE handles[2]{handle,other};
    const DWORD count=other?2:1;
    for (;;) {
        const auto result=MsgWaitForMultipleObjectsEx(count,handles,INFINITE,QS_SENDMESSAGE,0);
        if (result!=WAIT_OBJECT_0+count) return result==WAIT_OBJECT_0;
        MSG message;
        PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE|PM_QS_SENDMESSAGE);
    }
}
template<auto Function, class... Args>
auto Call(Args... args) noexcept -> decltype(Function(args...)) {
    using Result=decltype(Function(args...));
    auto& host=State();
    if (host.owner==GetCurrentThreadId()) return Function(args...);
    if (!host.dispatcher) return static_cast<Result>(RO_E_CLOSED);
    PaneLink::Flush(true);
    struct Job : Work { std::tuple<Args...> args; Result result{}; };
    Job job{}; job.args=std::make_tuple(args...);
    job.complete=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!job.complete) return static_cast<Result>(E_OUTOFMEMORY);
    job.run=[](Work* work) { auto& job=*static_cast<Job*>(work); job.result=std::apply(Function,job.args); };
    if (!PostMessageW(host.dispatcher,InvokeMessage,0,reinterpret_cast<LPARAM>(&job))) {
        CloseHandle(job.complete); return static_cast<Result>(RO_E_CLOSED);
    }
    // Borrowed ABI buffers remain valid until the native call completes. Only
    // the Gecko caller waits.
    // A thread that exits with this job queued never runs it.
    const bool ran=WaitDeliveringSent(job.complete,host.thread.native_handle());
    CloseHandle(job.complete); return ran ? job.result : static_cast<Result>(RO_E_CLOSED);
}
// For a call whose result no caller acts on: owns copies of its arguments, keeps the
// order of other routed calls, and does not wait. The bound keeps a stalled native
// thread from growing the queue; going over it is the caller's failure, not a wait.
template<class Apply>
HRESULT PostJob(size_t chars, Apply apply) noexcept {
    auto& host=State();
    if (!host.dispatcher) return RO_E_CLOSED;
    PaneLink::Flush(true);
    const auto count=host.posted.fetch_add(1);
    if (host.postedChars.fetch_add(chars)+chars>MaxPostedChars || count>=MaxPosted) {
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
template<auto Function, uint16_t Channel, uint16_t Kind=0>
HRESULT WINAPI PostText(HWND hwnd, const wchar_t* text) noexcept {
    if (State().owner==GetCurrentThreadId()) return Function(hwnd,text);
    return PaneLink::SendText(hwnd,Channel,Kind,text);
}
// Focus is fire and forget: a failure comes back as a refused publication.
template<auto Function, uint16_t Kind>
HRESULT WINAPI PostFocus(HWND hwnd, BOOL flag) noexcept {
    if (State().owner==GetCurrentThreadId()) return Function(hwnd,flag);
    const uint8_t value=flag ? 1 : 0;
    return PaneLink::Send(hwnd,SHELL_FRAME_FOCUS,Kind,0,&value,1);
}
// Validated here as well, so a malformed publication still fails at its caller.
inline HRESULT WINAPI PostSyncTabs(HWND hwnd, const WinUITab* tabs, uint32_t count, uint32_t selected,
                                   uint32_t revision, uint32_t* requested) noexcept {
    if (State().owner==GetCurrentThreadId()) return SyncTabs(hwnd,tabs,count,selected,revision,requested);
    if (!requested) return E_POINTER;
    *requested=0;
    try {
        if (!hwnd || !ValidTabPublication(tabs,count,selected,revision)) return E_INVALIDARG;
        std::vector<ShellFrameTabIn> rows(count);
        for (uint32_t i=0;i<count;++i)
            rows[i]={tabs[i].id,tabs[i].commands,reinterpret_cast<const uint16_t*>(tabs[i].title),wcslen(tabs[i].title)};
        uint8_t* data{}; size_t length{};
        if (shell_frame_tabs_encode(rows.data(),rows.size(),selected,revision,&data,&length)!=SHELL_STATUS_OK) return E_INVALIDARG;
        const auto result=PaneLink::Send(hwnd,SHELL_FRAME_TABS,0,revision,data,length);
        shell_link_free_frame(data,length);
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
    const auto result=PaneLink::SendText(hwnd,SHELL_FRAME_TABS,1,input);
    if (SUCCEEDED(result)) *revision=1;
    return result;
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
    host.geometry.insert({hwnd,kind});
}
inline void GeometryForgetWindow(HWND hwnd) {
    auto& host=State();
    std::lock_guard guard(host.geometryLock);
    std::erase_if(host.geometry,[hwnd](auto const& entry){return entry.first==hwnd;});
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
    // Latest-wins on the link: a stalled shell gets only the newest rectangle.
    uint8_t payload[sizeof(RECT)+1];
    memcpy(payload,rect,sizeof(RECT)); payload[sizeof(RECT)]=1;
    return PaneLink::Send(hwnd,SHELL_FRAME_GEOMETRY,Kind==0?0:1,0,payload,sizeof(payload));
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
    if (count && !rects) return E_POINTER;
    return PaneLink::Send(hwnd,SHELL_FRAME_GEOMETRY,2,0,rects,size_t(count)*sizeof(RECT));
}
// The pane's wakeup can fire inside its own drain bookkeeping, so the batch is prepared on a later
// turn and sent to Gecko as a frame. An empty frame only wakes Gecko, as the wakeup did before.
inline void PrepareAndNotify(HWND hwnd) noexcept {
    try {
        static thread_local std::vector<wchar_t> buffer(262144);
        buffer[0]=0;
        const auto result=PrepareDrain(hwnd,buffer.data(),uint32_t(buffer.size()));
        if (FAILED(result) && DiagnosticsEnabled()) fprintf(stderr,"WinUIPane drain failed hr=%08x\n",unsigned(result));
        const size_t units=result==S_OK ? wcsnlen(buffer.data(),buffer.size()) : 0;
        PaneLink::ShellSend(hwnd,SHELL_FRAME_REQUESTS,0,0,buffer.data(),units*sizeof(wchar_t));
    } catch (...) {}
}
inline BOOL WINAPI NotifyPrepared(void* context) noexcept {
    struct Job : Work { HWND hwnd; };
    auto* job=new (std::nothrow) Job;
    if (!job) return FALSE;
    job->complete=nullptr; job->hwnd=static_cast<HWND>(context);
    job->run=[](Work* work) {
        std::unique_ptr<Job> job(static_cast<Job*>(work));
        PrepareAndNotify(job->hwnd);
    };
    if (!PostMessageW(State().dispatcher,InvokeMessage,0,reinterpret_cast<LPARAM>(job))) { delete job; return FALSE; }
    return TRUE;
}
inline HRESULT WINAPI PostDrain(HWND hwnd, wchar_t* output, uint32_t capacity) noexcept {
    if (State().owner==GetCurrentThreadId()) return DrainRequests(hwnd,output,capacity);
    return PaneLink::Drain(hwnd,output,capacity);
}
inline HRESULT WINAPI PostSink(HWND hwnd, WinUIPaneNotifyFn notify, void* context) noexcept {
    if (State().owner==GetCurrentThreadId()) return SetNotifySink(hwnd,notify,context);
    return PaneLink::SetSink(hwnd,notify,context);
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
                PaneLink::StartShell();
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
                        const auto* binding=NativeWindowHost::ForInput(message.hwnd);
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
        WaitDeliveringSent(host.ready);
        if (FAILED(host.result)) { host.thread.join(); return host.result; }
        *output=host.shell; return S_OK;
    } catch (...) { return to_hresult(); }
}
// Gecko names the top-level window; its page input lives on the page child.
inline HRESULT WINAPI PageGeometry(HWND hwnd, const WinUIPageGeometry* input) noexcept { return NativePageInput::Geometry(NativeWindowHost::PageWindow(hwnd),input); }
inline HRESULT WINAPI PagePublish(HWND hwnd, const WinUIPageInput* input) noexcept { return NativePageInput::Publish(NativeWindowHost::PageWindow(hwnd),input); }
inline HRESULT WINAPI PageDrain(HWND hwnd, WinUIPageInputEvent* output, WCHAR* text, uint32_t capacity) noexcept { return NativePageInput::Drain(NativeWindowHost::PageWindow(hwnd),output,text,capacity); }
inline HRESULT WINAPI PageCommit(HWND hwnd, BOOL cancel) noexcept { return NativePageInput::Commit(NativeWindowHost::PageWindow(hwnd),cancel); }
inline HRESULT WINAPI Stop() noexcept {
    auto& host=State();
    if (!host.thread.joinable()) return S_FALSE;
    if (host.owner==GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    // The Gecko shutdown path has already destroyed its widgets and stopped
    // publications. The owner releases all WinRT objects before COM teardown.
    PostThreadMessageW(host.owner,WM_QUIT,0,0);
    WaitDeliveringSent(host.thread.native_handle());
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
// An overflowing link refuses this like any ordered frame; it no longer waits.
inline HRESULT WINAPI PostResolve(HWND hwnd, uint64_t generation, uint64_t request) noexcept {
    if (State().owner==GetCurrentThreadId()) return Resolve(hwnd,generation,request);
    const uint64_t payload[2]{generation,request};
    return PaneLink::Send(hwnd,SHELL_FRAME_CLOSE,0,0,payload,sizeof(payload));
}
// Setting a provider has no caller left to fail to: without it the window only lacks the
// root until teardown disconnects it. Clearing waits, so nothing can use a released provider.
inline HRESULT WINAPI PostAccessibility(HWND hwnd, IUnknown* provider) noexcept {
    if (State().owner==GetCurrentThreadId()) return NativeWindowHost::SetAccessibility(hwnd,provider);
    if (!provider) return Call<NativeWindowHost::SetAccessibility>(hwnd,provider);
    winrt::com_ptr<IUnknown> held;
    held.copy_from(provider);
    return PostJob(0,[hwnd,held=std::move(held)]{return NativeWindowHost::SetAccessibility(hwnd,held.get());});
}
// Removing the property here retires the surface for Gecko at once and answers S_OK only
// once. The close still runs: construction may not have set the property yet.
inline HRESULT WINAPI PostDismissSurface(HWND hwnd) noexcept {
    if (State().owner==GetCurrentThreadId()) return DismissStartupSurface(hwnd);
    const bool retired=hwnd && RemovePropW(hwnd,NativeStartupSurface::Property);
    auto result=PostJob(0,[hwnd]{return DismissStartupSurface(hwnd);});
    // The property is gone, so the close has to happen: nothing else would take the surface down.
    if (FAILED(result)) result=Call<DismissStartupSurface>(hwnd);
    return FAILED(result) ? result : retired ? S_OK : S_FALSE;
}
inline HRESULT WINAPI SetTitle(HWND hwnd, const wchar_t* title) noexcept {
    return SetWindowTextW(hwnd,title) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}
// Longer titles are cut to the frame's bound; the window caption shows far less.
inline HRESULT WINAPI PostTitle(HWND hwnd, const wchar_t* title) noexcept {
    if (State().owner==GetCurrentThreadId()) return SetTitle(hwnd,title);
    if (!title) return E_POINTER;
    return PaneLink::Send(hwnd,SHELL_FRAME_WINDOW,1,0,title,wcsnlen(title,32768)*sizeof(wchar_t));
}
// A refused cursor only leaves the previous one showing, until the next change.
inline HRESULT WINAPI PostPageCursor(HWND hwnd, HCURSOR cursor, BOOL owned) noexcept {
    const auto result=PostJob(0,[hwnd,cursor,owned]{return NativeWindowHost::SetPageCursor(hwnd,cursor,owned);});
    if (FAILED(result) && owned) DestroyCursor(cursor);
    return result;
}
inline HRESULT WINAPI PostPageTouch(HWND hwnd) noexcept {
    return PostJob(0,[hwnd]{return NativeWindowHost::RegisterPageTouch(hwnd);});
}
inline HRESULT ApplyWindowRequest(HWND hwnd, const WinUIPaneWindowRequest* request) noexcept {
    return NativeWindowHost::ApplyWindowRequest(hwnd,*request);
}
// The shell acknowledges each request once applied, which decrements Gecko's pending counter.
inline HRESULT WINAPI PostWindowRequest(HWND hwnd, const WinUIPaneWindowRequest* request) noexcept {
    if (!request) return E_POINTER;
    if (State().owner==GetCurrentThreadId()) return NativeWindowHost::ApplyWindowRequest(hwnd,*request);
    return PaneLink::SendRequest(hwnd,*request);
}
// Frames from Gecko, applied on the shell's thread through the same handlers the posted calls used.
inline void ShellApply(const ShellFrameView& v) {
    auto& shell=PaneLink::Native();
    if (v.channel==SHELL_FRAME_LIFETIME && v.kind==SHELL_FRAME_LIFETIME_INTRODUCE) {
        // Only a window of this thread can be introduced.
        const auto hwnd=reinterpret_cast<HWND>(static_cast<uintptr_t>(PaneLink::Read<uint64_t>(v.payload)));
        if (IsWindow(hwnd) && GetWindowThreadProcessId(hwnd,nullptr)==GetCurrentThreadId())
            shell_link_adopt(shell.link,v.window,reinterpret_cast<uintptr_t>(hwnd));
        return;
    }
    uintptr_t handle{};
    if (shell_link_handle(shell.link,v.window,&handle)!=SHELL_STATUS_OK) return;
    const auto hwnd=reinterpret_cast<HWND>(handle);
    const auto text=[&]{return PaneLink::Text(v.payload,v.length);};
    switch (v.channel) {
        case SHELL_FRAME_GEOMETRY:
            if (v.kind==2) {
                std::vector<RECT> rects(v.length/sizeof(RECT));
                if (!rects.empty()) memcpy(rects.data(),v.payload,rects.size()*sizeof(RECT));
                ReportedClip(hwnd,rects.data(),uint32_t(rects.size()));
            } else {
                const auto rect=PaneLink::Read<RECT>(v.payload);
                const BOOL visible=v.payload[sizeof(RECT)];
                if (v.kind==0) ReportedUpdateTabStrip(hwnd,&rect,visible); else ReportedUpdateAddress(hwnd,&rect,visible);
            }
            break;
        case SHELL_FRAME_TABS:
            if (v.kind==1) { PostedApplyTabDelta(hwnd,text().c_str()); break; }
            {
                std::vector<ShellFrameTab> rows(10000);
                size_t count{}; uint32_t selected{}, revision{};
                if (shell_frame_tabs_decode(v.payload,v.length,rows.data(),rows.size(),&count,&selected,&revision)!=SHELL_STATUS_OK) break;
                std::vector<std::wstring> titles; titles.reserve(count);
                std::vector<WinUITab> tabs(count);
                for (size_t i=0;i<count;++i) {
                    titles.emplace_back(rows[i].title_length,L'\0');
                    if (rows[i].title_length) memcpy(titles.back().data(),rows[i].title,rows[i].title_length*sizeof(wchar_t));
                    tabs[i]={rows[i].id,titles.back().c_str(),rows[i].commands};
                }
                ReportedSyncTabs(hwnd,tabs.data(),uint32_t(count),selected,revision);
            }
            break;
        case SHELL_FRAME_ADDRESS: PostedSyncAddress(hwnd,text().c_str()); break;
        case SHELL_FRAME_TAB_MENU: PostedSyncTabMenu(hwnd,text().c_str()); break;
        case SHELL_FRAME_FOCUS:
            if (v.kind==0) ReportedFocus(hwnd,v.payload[0]); else ReportedFocusAddress(hwnd,v.payload[0]);
            break;
        case SHELL_FRAME_WINDOW:
            if (v.kind==0) {
                const auto r=PaneLink::Read<ShellFrameWindowRequest>(v.payload);
                const WinUIPaneWindowRequest request{r.kind,r.flags,{r.rect[0],r.rect[1],r.rect[2],r.rect[3]},
                    static_cast<LONG_PTR>(r.value),nullptr};
                NativeWindowHost::ApplyWindowRequest(hwnd,request,false);
                auto* host=NativeWindowHost::Get(hwnd);
                const uint64_t endpoint=host ? reinterpret_cast<uintptr_t>(host->endpoint) : 0;
                PaneLink::ShellSend(hwnd,SHELL_FRAME_WINDOW,2,v.revision,&endpoint,sizeof(endpoint));
            } else if (v.kind==1) SetTitle(hwnd,text().c_str());
            break;
        case SHELL_FRAME_REQUESTS: CompleteRequests(hwnd,text().c_str()); break;
        case SHELL_FRAME_CLOSE:
            Resolve(hwnd,PaneLink::Read<uint64_t>(v.payload),PaneLink::Read<uint64_t>(v.payload+8));
            break;
        case SHELL_FRAME_LIFETIME:
            if (v.kind==SHELL_FRAME_LIFETIME_WAKEUP_ON) SetNotifySink(hwnd,NotifyPrepared,hwnd);
            else if (v.kind==SHELL_FRAME_LIFETIME_WAKEUP_OFF) SetNotifySink(hwnd,nullptr,nullptr);
            break;
    }
}
inline void ShellReceive(uint8_t* data, size_t length) noexcept {
    auto& shell=PaneLink::Native();
    ShellFrameView view{};
    if (shell.link && shell_link_receive(shell.link,data,length,&view)==SHELL_STATUS_OK && view.delivered)
        try { ShellApply(view); } catch (...) {}
    shell_link_free_frame(data,length);
    PaneLink::FlushShell();
}
inline HRESULT WINAPI DestroyHostRetired(HWND hwnd) noexcept {
    const auto result=NativeWindowHost::Destroy(hwnd);
    PaneLink::ShellRetire(hwnd);
    return result;
}
inline BOOL WINAPI IgnoreTranslate(MSG*) noexcept { return FALSE; }
inline void Route(WinUIPaneApi& api) {
    if (!State().started) return;
#define ROUTE(field, function) api.field=[](auto... args) noexcept { return Call<function>(args...); }
    api.focus=PostFocus<Focus,0>; api.destroy=[](HWND hwnd) noexcept { GeometryForgetWindow(hwnd); return Call<Destroy>(hwnd); }; api.translate=IgnoreTranslate; api.shutdown=Stop;
    api.syncTabs=PostSyncTabs; api.syncAddress=PostText<PostedSyncAddress,SHELL_FRAME_ADDRESS>; api.clip=PostClip<Clip,ReportedClip,1>;
    api.syncTabMenu=PostText<PostedSyncTabMenu,SHELL_FRAME_TAB_MENU>; api.updateTabStrip=PostRect<UpdateTabStrip,ReportedUpdateTabStrip,0>;
    api.setNotifySink=PostSink; api.drainRequests=PostDrain;
    api.completeRequests=PostText<CompleteRequests,SHELL_FRAME_REQUESTS>; api.applyTabDelta=PostTabDelta;
    api.updateAddress=PostRect<UpdateAddress,ReportedUpdateAddress,2>; api.focusAddress=PostFocus<FocusAddress,1>;
    // Reads only window state the OS answers from any thread, then computes.
    api.measureChromeLayout=MeasureChromeLayout;
    ROUTE(createWindowHost,Create); api.requestIndependentWindow=PostWindowRequest;
    api.destroyWindowHost=[](HWND hwnd) noexcept { GeometryForgetWindow(hwnd); PaneLink::Retire(hwnd); return Call<DestroyHostRetired>(hwnd); };
    ROUTE(showStartupSurface,ShowStartupSurface); api.dismissStartupSurface=PostDismissSurface;
    ROUTE(bindIndependentEndpoint,Bind); api.cancelIndependentClose=PostResolve;
    api.setIndependentAccessibility=PostAccessibility; api.setIndependentTitle=PostTitle;
    api.setIndependentCursor=PostPageCursor; api.registerIndependentTouch=PostPageTouch;
    ROUTE(publishIndependentGeometry,PageGeometry); ROUTE(publishIndependentInput,PagePublish); ROUTE(drainIndependentInput,PageDrain);
    ROUTE(commitIndependentInput,PageCommit);
    ROUTE(startupTransfer,StartupTransfer);
#undef ROUTE
}
}
