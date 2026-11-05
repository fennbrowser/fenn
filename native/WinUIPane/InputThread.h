#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <thread>

// The engine's page input thread (M5c; the native shell process design).
//
// It owns each browser window's page input child: mouse, keys, touch, pen, capture, cursor, the
// page's TSF store and its UIA root. The child is still a child of the shell window, below the
// compositor; only the owning thread changes. This thread never waits for Gecko or the shell. Its
// posts to Gecko's proxy are the ones the shell made before. M5d moves it, unchanged, into the
// engine process.
//
// Gecko reaches it through the pane's API (cursor, touch, accessibility, page IME) and the shell
// learns of its page children by PageMessage. Neither holds a pointer into its state.
namespace InputThread {
inline constexpr UINT InvokeMessage=WM_APP+905;
struct Job { std::function<void()> run; HANDLE complete{}; };
// What the input thread keeps for one shell window. Gecko can set cursor, touch and its
// accessibility provider before the page child exists; they are applied when it is created.
struct Window {
    HWND page{};
    NativeWindowHost::PageState* state{};
    bool touch{};
    HCURSOR cursor{};
    bool cursorOwned{};
    winrt::com_ptr<IRawElementProviderSimple> accessibility;
};
struct Host {
    std::thread thread;
    std::atomic<HWND> dispatcher{};
    std::atomic<DWORD> owner{};
    // The input thread's own: no other thread reads it.
    std::map<HWND,Window> windows;
};
inline Host& State() { static auto* host=new Host; return *host; }
inline bool Running() noexcept { return State().dispatcher.load()!=nullptr; }
inline bool OnThread() noexcept { return State().owner==GetCurrentThreadId(); }

inline LRESULT CALLBACK Dispatch(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message==InvokeMessage) {
        auto* job=reinterpret_cast<Job*>(l);
        const HANDLE complete=job->complete;
        try { job->run(); } catch (...) {}
        if (complete) SetEvent(complete); else delete job;
        return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
// Owns its work and never waits.
inline bool Post(std::function<void()> run) noexcept {
    const HWND dispatcher=State().dispatcher;
    if (!dispatcher) return false;
    try {
        auto* job=new Job{std::move(run)};
        if (!PostMessageW(dispatcher,InvokeMessage,0,reinterpret_cast<LPARAM>(job))) { delete job; return false; }
        return true;
    } catch (...) { return false; }
}
// For a call that still answers Gecko with a result (removed later in M5c). The input thread
// never waits for Gecko, so this wait is bounded by its own work.
template<class F> HRESULT Call(F run) noexcept {
    if (OnThread()) return run();
    const HWND dispatcher=State().dispatcher;
    if (!dispatcher) return RO_E_CLOSED;
    HRESULT result=RO_E_CLOSED;
    Job job{[&]{ result=run(); },CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    if (!job.complete) return E_OUTOFMEMORY;
    if (!PostMessageW(dispatcher,InvokeMessage,0,reinterpret_cast<LPARAM>(&job))) {
        CloseHandle(job.complete); return RO_E_CLOSED;
    }
    const bool ran=IndependentHost::WaitDeliveringSent(job.complete,State().thread.native_handle());
    CloseHandle(job.complete);
    return ran ? result : RO_E_CLOSED;
}

// --- On the input thread ----------------------------------------------------------------------
inline void Create(HWND shell, HWND endpoint, COLORREF background) noexcept {
    auto& window=State().windows[shell];
    if (window.page && IsWindow(window.page)) return;
    auto* state=new (std::nothrow) NativeWindowHost::PageState;
    if (!state) return;
    state->endpoint=endpoint;
    state->background=CreateSolidBrush(background);
    state->accessibility=window.accessibility;
    state->cursor=window.cursor; state->cursorOwned=window.cursorOwned;
    window.cursor=nullptr; window.cursorOwned=false;
    const HWND page=NativeWindowHost::CreatePageWindow(shell,state);
    if (!page) {
        NativeWindowHost::FocusTrace("input-page-failed",reinterpret_cast<uintptr_t>(shell));
        delete state;
        return;
    }
    window.page=page; window.state=state;
    if (window.touch) RegisterTouchWindow(page,TWF_WANTPALM);
    PostMessageW(shell,NativeWindowHost::PageMessage(),reinterpret_cast<WPARAM>(page),1);
}
inline void Destroyed(HWND page, NativeWindowHost::PageState* state) noexcept {
    // A page child another thread created for itself is that thread's alone.
    if (!OnThread()) { delete state; return; }
    auto& windows=State().windows;
    for (auto it=windows.begin();it!=windows.end();++it)
        if (it->second.page==page) { windows.erase(it); break; }
    delete state;
}
inline Window* Find(HWND shell) {
    auto& windows=State().windows;
    auto found=windows.find(shell);
    return found==windows.end() ? nullptr : &found->second;
}

// --- From Gecko's thread or the shell's --------------------------------------------------------
inline bool RequestPage(HWND shell, HWND endpoint, COLORREF background) noexcept {
    return Post([=]{ Create(shell,endpoint,background); });
}
inline void DestroyPage(HWND shell) noexcept {
    Post([shell]{
        auto* window=Find(shell);
        if (window && window->page && IsWindow(window->page)) DestroyWindow(window->page);
        else State().windows.erase(shell);
    });
}
// Calls a page-input function on the page child of a shell window.
template<class F> HRESULT OnPage(HWND shell, F run) noexcept {
    return Call([shell,&run]() -> HRESULT {
        auto* window=Find(shell);
        return window && window->page ? run(window->page) : E_INVALIDARG;
    });
}
inline HRESULT WINAPI SetCursor(HWND shell, HCURSOR cursor, BOOL owned) noexcept {
    const bool posted=Post([=]{
        auto& window=State().windows[shell];
        if (window.state) { NativeWindowHost::ApplyPageCursor(window.state,window.page,cursor,owned); return; }
        if (window.cursorOwned && window.cursor && window.cursor!=cursor) DestroyCursor(window.cursor);
        window.cursor=cursor; window.cursorOwned=owned!=FALSE;
    });
    if (!posted && owned) DestroyCursor(cursor);
    return posted ? S_OK : RO_E_CLOSED;
}
inline HRESULT WINAPI RegisterTouch(HWND shell) noexcept {
    return Post([shell]{
        auto& window=State().windows[shell];
        window.touch=true;
        if (window.page) RegisterTouchWindow(window.page,TWF_WANTPALM);
    }) ? S_OK : RO_E_CLOSED;
}
// Setting posts. Clearing waits, so nothing on this thread can use a released provider.
inline HRESULT WINAPI SetAccessibility(HWND shell, IUnknown* provider) noexcept {
    try {
        winrt::com_ptr<IRawElementProviderSimple> uia;
        if (provider) {
            const HRESULT result=provider->QueryInterface(__uuidof(IRawElementProviderSimple),uia.put_void());
            if (result!=E_NOINTERFACE && FAILED(result)) return result;
            return Post([shell,uia]{
                auto& window=State().windows[shell];
                window.accessibility=uia;
                if (window.state) window.state->accessibility=uia;
            }) ? S_OK : RO_E_CLOSED;
        }
        return Call([shell]() -> HRESULT {
            auto* window=Find(shell);
            if (!window) return S_OK;
            window->accessibility=nullptr;
            if (window->state) {
                UiaReturnRawElementProvider(window->page,0,0,nullptr);
                window->state->accessibility=nullptr;
            }
            return S_OK;
        });
    } catch (...) { return winrt::to_hresult(); }
}

// --- The thread -------------------------------------------------------------------------------
inline bool PageKey(const MSG& message) noexcept {
    if (message.message!=WM_KEYDOWN && message.message!=WM_KEYUP &&
        message.message!=WM_SYSKEYDOWN && message.message!=WM_SYSKEYUP) return false;
    auto* state=static_cast<NativeWindowHost::PageState*>(GetPropW(message.hwnd,NativeWindowHost::PageProperty));
    return state && state->endpoint;
}
inline HRESULT Start() noexcept {
    auto& host=State();
    if (host.thread.joinable()) return S_FALSE;
    const HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!ready) return E_OUTOFMEMORY;
    try {
        host.thread=std::thread([&host,ready] {
            const HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            WNDCLASSW wc{}; wc.lpfnWndProc=Dispatch; wc.hInstance=GetModuleHandleW(nullptr);
            wc.lpszClassName=L"Fenn.InputDispatcher.v1";
            RegisterClassW(&wc);
            const HWND dispatcher=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,nullptr);
            host.owner=GetCurrentThreadId();
            host.dispatcher=dispatcher;
            SetEvent(ready);
            if (dispatcher) {
                MSG message{};
                while (GetMessageW(&message,nullptr,0,0)>0) {
                    // A press in the page closes the shell's popups, as its own filter did.
                    switch (message.message) {
                        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
                        case WM_POINTERDOWN:
                            if (auto* state=static_cast<NativeWindowHost::PageState*>(GetPropW(message.hwnd,NativeWindowHost::PageProperty)))
                                if (state->shell) PostMessageW(state->shell,NativeWindowHost::PageReportMessage(),1,reinterpret_cast<LPARAM>(message.hwnd));
                    }
                    if (NativePageInput::Key(&message)) continue;
                    // Gecko translates page keys exactly once; translating here as well would
                    // change dead-key state before it does.
                    if (!PageKey(message)) TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            for (auto& [shell,window] : host.windows)
                if (window.page && IsWindow(window.page)) DestroyWindow(window.page);
            host.windows.clear();
            host.dispatcher=nullptr;
            if (dispatcher) DestroyWindow(dispatcher);
            if (SUCCEEDED(apartment)) CoUninitialize();
        });
    } catch (...) { CloseHandle(ready); return E_OUTOFMEMORY; }
    WaitForSingleObject(ready,10000);
    CloseHandle(ready);
    if (!host.dispatcher) return E_FAIL;
    NativeWindowHost::RequestPage=RequestPage;
    NativeWindowHost::PageDestroyed=Destroyed;
    return S_OK;
}
// After the shell's thread: its windows' page children are destroyed with them, which needs
// this thread pumping.
inline void Stop() noexcept {
    auto& host=State();
    if (!host.thread.joinable()) return;
    NativeWindowHost::RequestPage=nullptr;
    PostThreadMessageW(host.owner,WM_QUIT,0,0);
    host.thread.join();
}
}
