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
// The input link (shell to input thread): frames to this thread's dispatcher, credit returns to
// the shell's (CreditMessage, declared with the shell's dispatcher). M5d carries both over the
// engine connection instead.
inline constexpr UINT FrameMessage=WM_APP+906;
#ifdef FENN_ENGINE_LINK
inline constexpr UINT CreditMessage=WM_APP+907;
#endif
// Installed only by the split engine; a failed handoff closes that generation.
inline bool (*ReturnCredit)(const uint8_t*,size_t) noexcept = nullptr;
inline constexpr UINT TransportMessage=WM_APP+909;
inline void (*DrainTransport)() noexcept = nullptr;
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
    ShellLinkEnd* link{};
};
inline Host& State() { static auto* host=new Host; return *host; }
inline bool Running() noexcept { return State().dispatcher.load()!=nullptr; }
inline bool OnThread() noexcept { return State().owner==GetCurrentThreadId(); }

inline void Receive(uint8_t* data, size_t length) noexcept;
inline LRESULT CALLBACK Dispatch(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message==TransportMessage && DrainTransport) { DrainTransport(); return 0; }
    if (message==FrameMessage) { Receive(reinterpret_cast<uint8_t*>(l),static_cast<size_t>(w)); return 0; }
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
// Only for test exports, which read the input thread's state. The input thread never waits for
// Gecko, so this wait is bounded by its own work.
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
    const HANDLE waits[]{job.complete,State().thread.native_handle()};
    DWORD waitResult{};
    do {
        waitResult=MsgWaitForMultipleObjectsEx(2,waits,INFINITE,QS_SENDMESSAGE,0);
        if (waitResult==WAIT_OBJECT_0+2) { MSG message{}; PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE|PM_QS_SENDMESSAGE); }
    } while (waitResult==WAIT_OBJECT_0+2);
    const bool ran=waitResult==WAIT_OBJECT_0;
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
#ifndef FENN_ENGINE_LINK
        NativeWindowHost::FocusTrace("input-page-failed",reinterpret_cast<uintptr_t>(shell));
#endif
        delete state;
        return;
    }
    window.page=page; window.state=state;
    if (window.touch) RegisterTouchWindow(page,TWF_WANTPALM);
    PostMessageW(shell,NativeWindowHost::PageMessage(),reinterpret_cast<WPARAM>(page),1);
}
inline void Forget(HWND shell) noexcept {
    auto& host=State();
    uint32_t serial{};
    if (host.link && shell_link_serial(host.link,reinterpret_cast<uintptr_t>(shell),false,&serial)==SHELL_STATUS_OK)
        shell_link_retire(host.link,serial);
    host.windows.erase(shell);
}
inline void Destroyed(HWND page, NativeWindowHost::PageState* state) noexcept {
    // A page child another thread created for itself is that thread's alone.
    if (!OnThread()) { delete state; return; }
    auto& windows=State().windows;
    for (auto it=windows.begin();it!=windows.end();++it)
        if (it->second.page==page) { Forget(it->first); break; }
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
        else Forget(shell);
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
// Both post. This thread holds its own reference until it drops the provider, so nothing here
// uses a released one; Gecko disconnects the provider before clearing it, which makes any call
// that reaches it in between inert.
inline HRESULT WINAPI SetAccessibility(HWND shell, IUnknown* provider) noexcept {
    try {
        winrt::com_ptr<IRawElementProviderSimple> uia;
        if (provider) {
            const HRESULT result=provider->QueryInterface(__uuidof(IRawElementProviderSimple),uia.put_void());
            if (result!=E_NOINTERFACE && FAILED(result)) return result;
        }
        return Post([shell,uia]{
            if (!uia) {
                auto* window=Find(shell);
                if (!window) return;
                window->accessibility=nullptr;
                if (window->state) {
                    UiaReturnRawElementProvider(window->page,0,0,nullptr);
                    window->state->accessibility=nullptr;
                }
                return;
            }
            auto& window=State().windows[shell];
            window.accessibility=uia;
            if (window.state) {
                window.state->accessibility=uia;
                NotifyWinEvent(EVENT_OBJECT_REORDER,window.page,OBJID_CLIENT,CHILDID_SELF);
            }
        }) ? S_OK : RO_E_CLOSED;
    } catch (...) { return winrt::to_hresult(); }
}

// --- Page IME, from Gecko's thread; none of these waits -------------------------------------
// Posts a call on the page child of a shell window.
template<class F> HRESULT PostPage(HWND shell, F run) noexcept {
    return Post([shell,run=std::move(run)]() mutable {
        auto* window=Find(shell);
        if (window && window->page) run(window->page);
    }) ? S_OK : RO_E_CLOSED;
}
// S_OK means queued: Gecko learns that the store took a publication from its acceptance event.
inline HRESULT WINAPI PublishInput(HWND shell, const WinUIPageInput* input) noexcept {
    if (!input || input->scopeCount>8 || input->length>PAGE_INPUT_MAX_TEXT || (input->length && !input->text)) return E_INVALIDARG;
    try {
        WinUIPageInput copy=*input;
        std::vector<WCHAR> text(input->text,input->text+input->length);
        return PostPage(shell,[copy,text=std::move(text)](HWND page) mutable {
            copy.text=text.empty() ? nullptr : text.data();
            NativePageInput::Publish(page,&copy);
        });
    } catch (...) { return E_OUTOFMEMORY; }
}
inline HRESULT WINAPI PublishGeometry(HWND shell, const WinUIPageGeometry* input) noexcept {
    if (!input) return E_INVALIDARG;
    const WinUIPageGeometry copy=*input;
    return PostPage(shell,[copy](HWND page){ NativePageInput::Geometry(page,&copy); });
}
// Gecko's TextComposition expects an IME that commits later (as iBus does): its own
// commit stands, and it ignores the composition events that arrive before the IME's.
inline HRESULT WINAPI CommitInput(HWND shell, BOOL cancel) noexcept {
    return PostPage(shell,[cancel](HWND page){ NativePageInput::Commit(page,cancel); });
}
inline HRESULT WINAPI DrainInput(HWND shell, WinUIPageInputEvent* output, wchar_t* text, uint32_t capacity) noexcept {
    if (!output || !text) return E_INVALIDARG;
    auto box=NativePageInput::FindMailbox(shell);
    if (!box) return S_FALSE;
    // As the store's own drain did: any drain lets the next event wake Gecko again. The store
    // moves what queued meanwhile and wakes Gecko only if it moved something.
    if (!box->refill.exchange(true)) {
        if (!Post([box]{
                box->refill=false;
                if (auto* store=NativePageInput::Get(box->page); store && store->mailbox==box) store->Refill();
            }))
            box->refill=false;
    }
    std::lock_guard guard(box->lock);
    if (box->items.empty()) return S_FALSE;
    auto& item=box->items.front();
    if (item.text.size()>capacity) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    *output=item.event;
    if (!item.text.empty()) memcpy(text,item.text.data(),item.text.size()*sizeof(WCHAR));
    box->items.pop_front();
    return S_OK;
}

// --- The input link ---------------------------------------------------------------------------
#ifndef FENN_ENGINE_LINK
// The shell's half, on the shell's thread.
struct ShellEnd { ShellLinkEnd* link{}; uint64_t revision{}; };
inline ShellEnd& ShellSide() { static auto* side=new ShellEnd; return *side; }
inline void FlushToInput() noexcept {
    auto& side=ShellSide();
    for (;;) {
        uint8_t* data{}; size_t length{};
        if (shell_link_take(side.link,false,&data,&length)!=SHELL_STATUS_OK || !data) return;
        const HWND target=State().dispatcher;
        if (!target || !PostMessageW(target,FrameMessage,length,reinterpret_cast<LPARAM>(data))) {
            shell_link_free_frame(data,length);
            shell_link_close(side.link);
            return;
        }
    }
}
inline void SendShape(HWND shell, const RECT& client, const std::vector<RECT>& chrome, const std::vector<RECT>& exclusions) noexcept {
    auto& side=ShellSide();
    if (!Running()) return;
    if (!side.link) side.link=shell_link_create(1,nullptr);
    if (!side.link) return;
    try {
        uint32_t serial{};
        ShellLinkSend outcome{};
        const auto handle=reinterpret_cast<uintptr_t>(shell);
        if (shell_link_serial(side.link,handle,false,&serial)!=SHELL_STATUS_OK) {
            if (shell_link_serial(side.link,handle,true,&serial)!=SHELL_STATUS_OK) return;
            const uint64_t introduced=handle;
            shell_link_send(side.link,serial,SHELL_FRAME_LIFETIME,SHELL_FRAME_LIFETIME_INTRODUCE,0,
                reinterpret_cast<const uint8_t*>(&introduced),sizeof(introduced),&outcome);
        }
        // Clip lists that came in frames are already within the bound; more would only cut less.
        static_assert(sizeof(RECT)==16);
        const size_t chromeCount=(std::min)(chrome.size(),size_t(2)), exclusionCount=(std::min)(exclusions.size(),size_t(64));
        std::vector<uint8_t> payload(4+sizeof(RECT)*(1+chromeCount+exclusionCount));
        const uint32_t count=static_cast<uint32_t>(chromeCount);
        memcpy(payload.data(),&count,4);
        uint8_t* cursor=payload.data()+4;
        memcpy(cursor,&client,sizeof(RECT)); cursor+=sizeof(RECT);
        if (chromeCount) { memcpy(cursor,chrome.data(),chromeCount*sizeof(RECT)); cursor+=chromeCount*sizeof(RECT); }
        if (exclusionCount) memcpy(cursor,exclusions.data(),exclusionCount*sizeof(RECT));
        shell_link_send(side.link,serial,SHELL_FRAME_PAGE,0,++side.revision,payload.data(),payload.size(),&outcome);
        FlushToInput();
    } catch (...) {}
}
inline void ShellReceive(uint8_t* data, size_t length) noexcept {
    auto& side=ShellSide();
    ShellFrameView view{};
    if (side.link) shell_link_receive(side.link,data,length,&view);
    shell_link_free_frame(data,length);
    if (side.link) FlushToInput();
}
inline void RetireShape(HWND shell) noexcept {
    auto& side=ShellSide();
    uint32_t serial{};
    if (side.link && shell_link_serial(side.link,reinterpret_cast<uintptr_t>(shell),false,&serial)==SHELL_STATUS_OK)
        shell_link_retire(side.link,serial);
}
#endif
// The input thread's half. ShellCore validated the payload's layout and rectangles.
inline void ApplyShape(HWND shell, const uint8_t* payload, size_t length) noexcept {
    auto* window=Find(shell);
    if (!window || !window->page || length<4+sizeof(RECT)) return;
    try {
        uint32_t chromeCount{};
        memcpy(&chromeCount,payload,4);
        const size_t total=(length-4)/sizeof(RECT);
        if (chromeCount+1>total) return;
        std::vector<RECT> all(total);
        memcpy(all.data(),payload+4,total*sizeof(RECT));
        const RECT client=all[0];
        const std::vector<RECT> chrome(all.begin()+1,all.begin()+1+chromeCount), exclusions(all.begin()+1+chromeCount,all.end());
        RECT current{};
        GetClientRect(window->page,&current);
        if (!EqualRect(&current,&client))
            SetWindowPos(window->page,nullptr,0,0,client.right-client.left,client.bottom-client.top,
                SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        NativeWindowHost::ApplyStartupPageVisibility(window->page,shell);
        HRGN region=NativeWindowHost::PageRegion(client,chrome,exclusions,GetDpiForWindow(shell));
        if (region && !SetWindowRgn(window->page,region,TRUE)) DeleteObject(region);
    } catch (...) {}
}
inline void Receive(uint8_t* data, size_t length) noexcept {
    auto& host=State();
    ShellFrameView view{};
    if (host.link && shell_link_receive(host.link,data,length,&view)==SHELL_STATUS_OK && view.delivered) {
        if (view.channel==SHELL_FRAME_LIFETIME && view.kind==SHELL_FRAME_LIFETIME_INTRODUCE) {
            uint64_t handle{};
            memcpy(&handle,view.payload,sizeof(handle));
            // Only a window this thread made a page child for.
            if (host.windows.count(reinterpret_cast<HWND>(static_cast<uintptr_t>(handle))))
                shell_link_adopt(host.link,view.window,static_cast<uintptr_t>(handle));
        } else if (view.channel==SHELL_FRAME_PAGE && view.kind==0) {
            uintptr_t handle{};
            if (shell_link_handle(host.link,view.window,&handle)==SHELL_STATUS_OK)
                ApplyShape(reinterpret_cast<HWND>(handle),view.payload,view.length);
        }
    }
    shell_link_free_frame(data,length);
    if (!host.link) return;
    // Credit returns go back to the shell.
    for (;;) {
        uint8_t* out{}; size_t size{};
        if (shell_link_take(host.link,false,&out,&size)!=SHELL_STATUS_OK || !out) return;
        if (ReturnCredit) {
            const bool accepted=ReturnCredit(out,size);
            shell_link_free_frame(out,size);
            if (!accepted) shell_link_close(host.link);
            if (!accepted) return;
            continue;
        }
#ifndef FENN_ENGINE_LINK
        const HWND target=IndependentHost::State().dispatcher;
        if (!target || !PostMessageW(target,CreditMessage,size,reinterpret_cast<LPARAM>(out))) {
            shell_link_free_frame(out,size);
            return;
        }
#else
        shell_link_free_frame(out,size);
        shell_link_close(host.link);
        return;
#endif
    }
}

// --- The thread -------------------------------------------------------------------------------
inline bool PageKey(const MSG& message) noexcept {
    if (message.message!=WM_KEYDOWN && message.message!=WM_KEYUP &&
        message.message!=WM_SYSKEYDOWN && message.message!=WM_SYSKEYUP) return false;
    auto* state=static_cast<NativeWindowHost::PageState*>(GetPropW(message.hwnd,NativeWindowHost::PageProperty));
    return state && state->endpoint;
}
inline HRESULT Start(uint32_t generation=1) noexcept {
    auto& host=State();
    if (host.thread.joinable()) return S_FALSE;
    const HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!ready) return E_OUTOFMEMORY;
    try {
        host.thread=std::thread([&host,ready,generation] {
            const HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            WNDCLASSW wc{}; wc.lpfnWndProc=Dispatch; wc.hInstance=GetModuleHandleW(nullptr);
            wc.lpszClassName=L"Fenn.InputDispatcher.v1";
            RegisterClassW(&wc);
            const HWND dispatcher=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,nullptr);
            host.owner=GetCurrentThreadId();
            host.link=shell_link_create(generation,nullptr);
            NativePageInput::Mailboxed()=true;
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
            // Destroying a page child erases its entry (Destroyed), so not while iterating.
            std::vector<HWND> pages;
            for (auto& [shell,window] : host.windows) if (window.page) pages.push_back(window.page);
            for (HWND page : pages) if (IsWindow(page)) DestroyWindow(page);
            host.windows.clear();
            host.dispatcher=nullptr;
            if (dispatcher) DestroyWindow(dispatcher);
            // Frames still posted to the dispatcher go with it, and so does the link.
            shell_link_destroy(host.link); host.link=nullptr;
            if (SUCCEEDED(apartment)) CoUninitialize();
        });
    } catch (...) { CloseHandle(ready); return E_OUTOFMEMORY; }
    WaitForSingleObject(ready,10000);
    CloseHandle(ready);
    if (!host.dispatcher) return E_FAIL;
    NativeWindowHost::PageDestroyed=Destroyed;
#ifndef FENN_ENGINE_LINK
    NativeWindowHost::RequestPage=RequestPage;
    NativeWindowHost::SendPageShape=SendShape;
    NativeWindowHost::RetirePageShape=RetireShape;
#endif
    return S_OK;
}
// After the shell's thread: its windows' page children are destroyed with them, which needs
// this thread pumping.
inline void Stop() noexcept {
    auto& host=State();
    if (!host.thread.joinable()) return;
#ifndef FENN_ENGINE_LINK
    NativeWindowHost::RequestPage=nullptr;
    NativeWindowHost::SendPageShape=nullptr;
    NativeWindowHost::RetirePageShape=nullptr;
#endif
    PostThreadMessageW(host.owner,WM_QUIT,0,0);
    host.thread.join();
}
}
