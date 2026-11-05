#pragma once
#include <memory>
#include <functional>
#include <chrono>
#include <cstdio>
#include <uiautomation.h>
#pragma comment(lib,"uiautomationcore.lib")
#include "shell_core.h"

// The HWND is native-owned; nsWindow remains the logical browser adapter, on its own hidden
// window on Gecko's thread (the proxy, which is also the endpoint). Gecko keeps its compositor
// children and all window/focus/browser semantics. The binding's content identity is the
// attached nsWindow, not a disposable compositor HWND (which Gecko may replace after a
// graphics-device reset).
namespace NativeWindowHost {
inline constexpr wchar_t Property[] = L"Fenn.NativeWindowHost";
inline UINT CloseMessage() { static UINT message=RegisterWindowMessageW(L"Fenn.NativeWindowHost.Close.v1"); return message; }
// Set on the host HWND while the local startup editor owns keyboard focus (before Firefox has
// adopted it). Gecko's own startup focus changes must not take focus from a field the user is
// typing into; see the native interactive startup plan (input loss during adoption).
inline constexpr wchar_t EditorOwnedProperty[] = L"Fenn.NativeStartupEditorOwned";
inline UINT RestoreEditorFocusMessage() { static UINT message=RegisterWindowMessageW(L"Fenn.NativeWindowHost.RestoreEditorFocus.v1"); return message; }
inline void FocusTrace(const char* event, uintptr_t detail=0) {
    static const bool enabled=GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS",nullptr,0)!=0;
    if (enabled) fprintf(stderr,"FennFocus ms=%lld event=%s detail=%llu\n",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()),
        event,static_cast<unsigned long long>(detail));
}
// After Firefox adopts the editor its own startup focus calls can still arrive (one landed about a
// second later), so ownership continues for a bounded grace period (a GetTickCount64 deadline).
inline constexpr wchar_t EditorGraceProperty[] = L"Fenn.NativeStartupEditorGraceUntil";
inline constexpr ULONGLONG EditorGraceMilliseconds = 5000;
inline bool EditorOwned(HWND hwnd) {
    if (GetPropW(hwnd,EditorOwnedProperty)) return true;
    const auto until=reinterpret_cast<ULONGLONG>(GetPropW(hwnd,EditorGraceProperty));
    return until && GetTickCount64()<until;
}
inline UINT EndpointCloseMessage() { static UINT message=RegisterWindowMessageW(L"Fenn.NativeWindowHost.EndpointClose.v1"); return message; }
inline UINT EndpointDpiMessage() { static UINT message=RegisterWindowMessageW(L"Fenn.NativeWindowHost.EndpointDpi.v1"); return message; }
inline UINT EndpointLayoutMessage() { static UINT message=RegisterWindowMessageW(L"Fenn.NativeWindowHost.EndpointLayout.v1"); return message; }
// The shell moved, resized or changed size mode; the proxy reads the new state itself.
// Explorer sends this to the window with the taskbar button, the shell. Firefox applies taskbar
// progress, overlay icons and thumbnail buttons only once its own window has seen it.
inline UINT TaskbarButtonCreatedMessage() { static UINT message=RegisterWindowMessageW(L"TaskbarButtonCreated"); return message; }
inline UINT EndpointGeometryMessage() { static UINT message=RegisterWindowMessageW(L"Fenn.NativeWindowHost.EndpointGeometry.v1"); return message; }
// Page input arrives on a child window that covers the client area below the chrome's XAML, so
// the top-level window's own content can change without moving page input. Client coordinates
// are the top-level's. Gecko's accessibility root names this window (PageWindowProperty).
inline constexpr wchar_t PageProperty[] = L"Fenn.NativeWindowHost.Page";
inline constexpr wchar_t PageWindowProperty[] = L"Fenn.PageWindow";
struct Context {
    std::unique_ptr<ShellCore,decltype(&shell_core_destroy)> core{shell_core_create(nullptr),shell_core_destroy};
    // The browser window is a WinUI Window: its single XAML tree (root) and the XAML content
    // window that hosts it (bridge), below Gecko's compositor and the page child.
    winrt::Microsoft::UI::Xaml::Window window{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Canvas root{nullptr};
    HWND bridge{};
    // The page colour, painted until XAML and Gecko have presented.
    HBRUSH background{};
    // Native chrome rectangles, and the parts of them where Gecko's own controls show, both in
    // client pixels: the page child covers the rest and these exclusions.
    std::vector<RECT> chrome, exclusions;
    // What the page child's region was last built from (ApplyPageRegion).
    bool pageRegionApplied{};
    RECT pageRegionClient{};
    std::vector<RECT> pageRegionChrome, pageRegionExclusions;
    // Independent browser endpoint. Never call its window procedure on the
    // native UI thread or synchronously wait for its close decision.
    HWND endpoint{};
    // The page input child, created when the endpoint binds.
    HWND page{};
    // Whether the page held focus when the window last deactivated (true before the first).
    bool pageFocusOnActivate{true};
    // Native owner thread only. Returns keyboard focus to the local startup editor.
    std::function<void()> restoreEditorFocus;
    winrt::com_ptr<IRawElementProviderSimple> accessibility;
    // This thread holds capture for a page drag: Gecko cannot capture a window it does not own.
    bool pageCapture{};
    bool pageTracking{};
    HCURSOR pageCursor{};
    bool pageCursorOwned{};
    // Copies of Gecko's window icons, which Gecko destroys on its own schedule.
    HICON icons[2]{};
    ShellWindowBinding state{};
    ShellStatus Call(uint32_t operation, ShellWindowBinding input={}) {
        return shell_core_window_host(core.get(),operation,&input,&state);
    }
};
inline Context* Get(HWND hwnd) { return static_cast<Context*>(GetPropW(hwnd,Property)); }
inline bool Owner(HWND hwnd) { return GetWindowThreadProcessId(hwnd,nullptr)==GetCurrentThreadId(); }
// The host whose page input this window carries: the top-level window or its page child.
inline Context* ForInput(HWND hwnd) {
    if (auto* host=Get(hwnd)) return host;
    return hwnd ? static_cast<Context*>(GetPropW(hwnd,PageProperty)) : nullptr;
}
// Where page focus and page input belong: the page child once it exists, else the window itself.
inline HWND PageWindow(HWND hwnd) {
    auto* host=Get(hwnd);
    return host && host->page ? host->page : hwnd;
}
// Bottom up: XAML's content window, the page child, then Gecko's compositor and XAML's caption
// windows. Gecko's rows are transparent, so the chrome shows through the compositor; the page
// child is below the compositor because software rendering draws into it through GDI.
inline void KeepOrder(HWND hwnd) {
    auto* host=Get(hwnd);
    if (!host) return;
    constexpr UINT flags=SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER;
    if (host->bridge && GetWindow(host->bridge,GW_HWNDNEXT)) SetWindowPos(host->bridge,HWND_BOTTOM,0,0,0,0,flags);
    if (host->page && host->bridge && GetWindow(host->page,GW_HWNDNEXT)!=host->bridge) SetWindowPos(host->page,host->bridge,0,0,0,0,flags);
    if (host->page && host->bridge && GetWindow(host->page,GW_HWNDNEXT)!=host->bridge) {
        SetWindowPos(host->page,HWND_BOTTOM,0,0,0,0,flags);
        SetWindowPos(host->bridge,HWND_BOTTOM,0,0,0,0,flags);
    }
}
// The page child takes input everywhere except the visible native chrome, where Gecko's own
// controls (exclusions) still belong to the page.
// Firefox republishes geometry continually, mostly unchanged. Reshaping the page child under a
// resting pointer makes Windows hit-test it again, which WinUI reports to an open popup as the pointer
// leaving and re-entering, cancelling a press in it: only a changed region is applied.
inline void ApplyPageRegion(Context* host) {
    if (!host || !host->page) return;
    RECT client{};
    GetClientRect(host->page,&client);
    const auto same=[](std::vector<RECT> const& a, std::vector<RECT> const& b) {
        return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](RECT const& x, RECT const& y){ return EqualRect(&x,&y)!=0; });
    };
    if (host->pageRegionApplied && EqualRect(&client,&host->pageRegionClient) &&
        same(host->chrome,host->pageRegionChrome) && same(host->exclusions,host->pageRegionExclusions)) return;
    host->pageRegionApplied=true; host->pageRegionClient=client;
    host->pageRegionChrome=host->chrome; host->pageRegionExclusions=host->exclusions;
    HRGN region=CreateRectRgnIndirect(&client);
    if (!region) return;
    for (auto const& rect : host->chrome) {
        if (HRGN cut=CreateRectRgnIndirect(&rect)) { CombineRgn(region,region,cut,RGN_DIFF); DeleteObject(cut); }
    }
    for (auto const& rect : host->exclusions) {
        if (HRGN add=CreateRectRgnIndirect(&rect)) { CombineRgn(region,region,add,RGN_OR); DeleteObject(add); }
    }
    if (!SetWindowRgn(host->page,region,TRUE)) DeleteObject(region);
}
inline void SetPageRegion(HWND hwnd, std::vector<RECT> chrome, std::vector<RECT> exclusions) {
    auto* host=Get(hwnd);
    if (!host) return;
    host->chrome=std::move(chrome); host->exclusions=std::move(exclusions);
    ApplyPageRegion(host);
}
// Page input for the browser behind `window`: the top-level window before the page child exists,
// the page child after. Returns true when the message is consumed, with its result.
inline bool ForwardPage(HWND window, Context* host, UINT message, WPARAM w, LPARAM l, LRESULT& result) {
    result=0;
    if (message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_MBUTTONDOWN ||
        message==WM_LBUTTONDBLCLK || message==WM_RBUTTONDBLCLK || message==WM_MBUTTONDBLCLK) {
        if (!GetCapture()) { SetCapture(window); host->pageCapture=GetCapture()==window; }
    }
    // Leave tracking belongs to this thread, so nsWindow's own request has no effect. As in
    // nsWindow::CaptureMouse it pauses during a capture; asking again when capture ends
    // reports a leave at once if the drag ended outside the window.
    const auto trackLeave=[&]{
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};
        host->pageTracking=TrackMouseEvent(&track)!=FALSE;
    };
    if (message==WM_MOUSEMOVE && !host->pageTracking && !host->pageCapture) trackLeave();
    // Stock consumes pen messages, so nothing is promoted to mouse messages here either.
    if (message==WM_POINTERDOWN || message==WM_POINTERUP || message==WM_POINTERUPDATE || message==WM_POINTERLEAVE) {
        POINTER_INPUT_TYPE type{};
        if (GetPointerType(GET_POINTERID_WPARAM(w),&type) && type==PT_PEN) {
            // Moves can be dropped behind a stalled Gecko; presses and releases cannot.
            static volatile LONG pending=0;
            constexpr LONG MaxPendingPen=1024;
            if (message==WM_POINTERUPDATE && pending>=MaxPendingPen) return true;
            auto* copy=static_cast<WinUIPanePenMessage*>(HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(WinUIPanePenMessage)));
            if (!copy) return true;
            if (message!=WM_POINTERLEAVE && !GetPointerPenInfo(GET_POINTERID_WPARAM(w),&copy->pen)) { HeapFree(GetProcessHeap(),0,copy); return true; }
            copy->wParam=w; copy->pending=&pending;
            InterlockedIncrement(&pending);
            if (!PostMessageW(host->endpoint,message+WINUI_PANE_ENDPOINT_KEY_OFFSET,reinterpret_cast<WPARAM>(copy),l)) {
                InterlockedDecrement(&pending); HeapFree(GetProcessHeap(),0,copy);
            }
            return true;
        }
    }
    // Only the owner's SetCursor counts. An island's own message names its window, not this one.
    if (message==WM_SETCURSOR && reinterpret_cast<HWND>(w)==window && LOWORD(l)==HTCLIENT && host->pageCursor) {
        SetCursor(host->pageCursor);
        result=TRUE;
        return true;
    }
    // Gecko reads and closes the handle on its thread, which the OS allows.
    if (message==WM_TOUCH) {
        if (!PostMessageW(host->endpoint,message,w,l)) CloseTouchInputHandle(reinterpret_cast<HTOUCHINPUT>(l));
        return true;
    }
    // The commands nsWindow consumes. Others, such as volume, stay with the system.
    if (message==WM_APPCOMMAND) {
        switch (GET_APPCOMMAND_LPARAM(l)) {
            case APPCOMMAND_BROWSER_BACKWARD: case APPCOMMAND_BROWSER_FORWARD: case APPCOMMAND_BROWSER_REFRESH:
            case APPCOMMAND_BROWSER_STOP: case APPCOMMAND_BROWSER_SEARCH: case APPCOMMAND_BROWSER_FAVORITES:
            case APPCOMMAND_BROWSER_HOME: case APPCOMMAND_CLOSE: case APPCOMMAND_FIND: case APPCOMMAND_HELP:
            case APPCOMMAND_NEW: case APPCOMMAND_OPEN: case APPCOMMAND_PRINT: case APPCOMMAND_SAVE:
            case APPCOMMAND_FORWARD_MAIL: case APPCOMMAND_REPLY_TO_MAIL: case APPCOMMAND_SEND_MAIL:
            case APPCOMMAND_MEDIA_NEXTTRACK: case APPCOMMAND_MEDIA_PREVIOUSTRACK: case APPCOMMAND_MEDIA_STOP:
            case APPCOMMAND_MEDIA_PLAY_PAUSE: case APPCOMMAND_COPY: case APPCOMMAND_CUT: case APPCOMMAND_PASTE:
            case APPCOMMAND_REDO: case APPCOMMAND_UNDO:
                PostMessageW(host->endpoint,message,w,l);
                result=TRUE;
                return true;
        }
    }
    // Offset, so Gecko can tell it from the leave its own tracking of the hidden proxy produces.
    if (message==WM_MOUSELEAVE) { host->pageTracking=false; PostMessageW(host->endpoint,message+WINUI_PANE_ENDPOINT_KEY_OFFSET,0,0); }
    if (message==WM_CAPTURECHANGED && host->pageCapture) { host->pageCapture=false; trackLeave(); }
    // Only pointer-free notifications cross this boundary. Synchronous
    // non-client, accessibility, IME and drag messages are not forwarded.
    if (message==WM_CONTEXTMENU) {
        // Coordinates (or -1 for keyboard invocation) and source HWND are
        // scalar values. Gecko owns context targeting and cancellation.
        PostMessageW(host->endpoint,message,w,l);
        return true;
    }
    switch (message) {
        case WM_SETFOCUS: case WM_KILLFOCUS:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            PostMessageW(host->endpoint,message,w,l); break;
        // Extra info says whether touch or pen made this message, and it is per thread.
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK:
            PostMessageW(host->endpoint,message,
                (static_cast<WPARAM>(static_cast<uint32_t>(GetMessageExtraInfo()))<<32)|(w&0xffffffffu),l);
            break;
    }
    if ((message==WM_LBUTTONUP || message==WM_RBUTTONUP || message==WM_MBUTTONUP) && host->pageCapture &&
        !(w&(MK_LBUTTON|MK_RBUTTON|MK_MBUTTON)) && GetCapture()==window) ReleaseCapture();
    if (message==WM_KEYDOWN || message==WM_KEYUP || message==WM_SYSKEYDOWN || message==WM_SYSKEYUP) {
        uint32_t modifiers=0;
        for (size_t i=0;i<std::size(WinUIPaneModifierKeys);++i) {
            const auto state=GetKeyState(WinUIPaneModifierKeys[i]);
            if (state&0x8000) modifiers|=1u<<(i*2);
            if (state&1) modifiers|=2u<<(i*2);
        }
        PostMessageW(host->endpoint,message+WINUI_PANE_ENDPOINT_KEY_OFFSET,
            (static_cast<WPARAM>(modifiers)<<32)|(w&0xffffffffu),l);
    }
    // Native TranslateMessage chars are deliberately not forwarded. Gecko
    // translates the transported key with its captured modifier state.
    if (message==WM_PAINT) {
        PAINTSTRUCT paint{}; BeginPaint(window,&paint); EndPaint(window,&paint);
        return true;
    }
    return false;
}

inline LRESULT CALLBACK PageProc(HWND page, UINT message, WPARAM w, LPARAM l) {
    auto* host=static_cast<Context*>(GetPropW(page,PageProperty));
    if (host) {
        if (message==NativePageInput::LockMessage()) { NativePageInput::Pump(page,w); return 0; }
        if (message==WM_SETFOCUS) NativePageInput::Focus(page);
        if (message==WM_KILLFOCUS) NativePageInput::Blur(page);
        if (message==WM_GETOBJECT && static_cast<LONG>(l)==UiaRootObjectId && host->accessibility)
            return UiaReturnRawElementProvider(page,w,l,host->accessibility.get());
        // Paint what the window itself would show here, its background, until Gecko presents.
        if (message==WM_ERASEBKGND) {
            RECT client{};
            GetClientRect(page,&client);
            if (host->background) FillRect(reinterpret_cast<HDC>(w),&client,host->background);
            return 1;
        }
        if (host->endpoint) {
            LRESULT result{};
            if (ForwardPage(page,host,message,w,l,result)) return result;
        }
        if (message==WM_NCDESTROY) {
            UiaReturnRawElementProvider(page,0,0,nullptr);
            NativePageInput::Remove(page);
            if (host->page==page) host->page=nullptr;
            RemovePropW(page,PageProperty);
            RemovePropW(page,WINUI_PANE_ENDPOINT_PROPERTY);
        }
    }
    return DefWindowProcW(page,message,w,l);
}
// Software rendering draws into this window (Gecko's SurfaceTarget) through GDI. It is layered at full
// opacity so it has its own surface: the WinUI Window extends its frame over the whole client area for
// the backdrop, where GDI's zero alpha would let the backdrop show through the page.
inline HRESULT CreatePage(HWND hwnd, Context* host, HWND endpoint) noexcept {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(PageProc),&module)) return HRESULT_FROM_WIN32(GetLastError());
    constexpr auto name=L"Fenn.NativePageInput.v1";
    WNDCLASSW wc{};
    if (!GetClassInfoW(module,name,&wc)) {
        wc.lpfnWndProc=PageProc; wc.hInstance=module; wc.lpszClassName=name;
        wc.style=CS_DBLCLKS; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if (!RegisterClassW(&wc)) return HRESULT_FROM_WIN32(GetLastError());
    } else if (wc.lpfnWndProc!=PageProc) return E_UNEXPECTED;
    RECT client{};
    GetClientRect(hwnd,&client);
    HWND page=CreateWindowExW(WS_EX_NOPARENTNOTIFY|WS_EX_LAYERED,name,L"",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
        0,0,client.right,client.bottom,hwnd,nullptr,module,nullptr);
    if (!page) return HRESULT_FROM_WIN32(GetLastError());
    SetLayeredWindowAttributes(page,0,255,LWA_ALPHA);
    if (!SetPropW(page,PageProperty,host) || !SetPropW(page,WINUI_PANE_ENDPOINT_PROPERTY,endpoint) ||
        !SetPropW(hwnd,PageWindowProperty,page)) {
        const auto error=HRESULT_FROM_WIN32(GetLastError());
        RemovePropW(page,PageProperty); DestroyWindow(page); return error;
    }
    // Below the chrome's XAML and Gecko's compositor. The compositor is disabled, so pointer
    // input passes it and reaches this window, as it reached the top-level window before.
    // Software rendering presents into this window from now on.
    InvalidateRect(page,nullptr,TRUE);
    host->page=page;
    host->pageRegionApplied=false;
    KeepOrder(hwnd);
    ApplyPageRegion(host);
    if (GetFocus()==hwnd) SetFocus(page);
    return S_OK;
}

inline void CloseHostWindow(HWND hwnd) {
    auto* host=Get(hwnd);
    if (host && host->window) host->window.Close();
    else DestroyWindow(hwnd);
}
inline LRESULT CALLBACK BaseSubclass(HWND hwnd, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR);
inline LRESULT CALLBACK Base(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto* host=Get(hwnd);
    if (!host) return DefSubclassProc(hwnd,message,w,l);
    static const UINT focusMessage=RegisterWindowMessageW(L"Fenn.NativeWindowHost.Focus.v1");
    if (focusMessage && message==focusMessage) {
        // An old endpoint must not focus a detached/rebound native window.
        // This runs on the native owner; Gecko never borrows its input queue.
        if (host->endpoint && reinterpret_cast<HWND>(w)==host->endpoint && IsWindowEnabled(hwnd)) {
            // Gecko's startup focus requests (window focus, then urlbar and content) are meant
            // for a window nobody has typed into. While the native editor owns focus, keep it.
            const HWND focused=GetFocus();
            const bool keep=EditorOwned(hwnd) && focused && (focused==hwnd || IsChild(hwnd,focused));
            FocusTrace(keep?"gecko-focus-ignored":"gecko-focus-applied",reinterpret_cast<uintptr_t>(focused));
            if (!keep) SetFocus(host->page ? host->page : hwnd);
        }
        return 0;
    }
    if (message==RestoreEditorFocusMessage()) {
        if (EditorOwned(hwnd) && host->restoreEditorFocus) host->restoreEditorFocus();
        return 0;
    }
    // Focus can land on the host HWND itself, not the XAML island: for example when a Gecko
    // dialog that held activation closes. Return it to the editor on the next turn.
    if (message==WM_SETFOCUS) FocusTrace(EditorOwned(hwnd)?"host-setfocus-owned":"host-setfocus",reinterpret_cast<uintptr_t>(hwnd));
    if (message==WM_ACTIVATE) FocusTrace(LOWORD(w)==WA_INACTIVE?"host-deactivate":"host-activate",reinterpret_cast<uintptr_t>(GetFocus()));
    // WinUI's Window handles activation itself and restores focus to its XAML content, so the
    // window never receives WM_SETFOCUS to pass on to the page. Activation returns focus to the
    // page when the page had it (or nothing had yet); XAML keeps a focus it owned.
    if (message==WM_ACTIVATE && host->page && host->window) {
        // Gecko learns of the shell window's activation only from these posts: without them a later
        // window stays inactive to Firefox (Services.focus.activeWindow), and its prompts never show.
        // nsWindow only records WM_ACTIVATE and dispatches on the focus message that follows, which
        // the window itself no longer gets once its page child owns page focus, so the pair is sent.
        if (host->endpoint) {
            PostMessageW(host->endpoint,message,w,l);
            PostMessageW(host->endpoint,LOWORD(w)==WA_INACTIVE ? WM_KILLFOCUS : WM_SETFOCUS,0,0);
        }
        if (LOWORD(w)==WA_INACTIVE) {
            const HWND focused=GetFocus();
            host->pageFocusOnActivate=!focused || focused==host->page || focused==hwnd;
            return DefSubclassProc(hwnd,message,w,l);
        }
        const LRESULT result=DefSubclassProc(hwnd,message,w,l);
        if (!HIWORD(w) && host->pageFocusOnActivate && !EditorOwned(hwnd) && IsWindow(host->page)) {
            FocusTrace("host-activate-page",reinterpret_cast<uintptr_t>(GetFocus()));
            SetFocus(host->page);
        }
        return result;
    }
    if (message==WM_KILLFOCUS) FocusTrace("host-killfocus",static_cast<uintptr_t>(w));
    if (message==WM_SETFOCUS && EditorOwned(hwnd) && host->restoreEditorFocus)
        PostMessageW(hwnd,RestoreEditorFocusMessage(),0,0);
    // Focus that lands on the window itself, for example on activation, belongs to the page.
    else if (message==WM_SETFOCUS && host->page) { SetFocus(host->page); return 0; }
    if(message==NativePageInput::LockMessage()) {NativePageInput::Pump(hwnd,w);return 0;}
    if(host->endpoint && !host->page && message==WM_SETFOCUS) NativePageInput::Focus(hwnd);
    if(host->endpoint && !host->page && message==WM_KILLFOCUS) NativePageInput::Blur(hwnd);
    if (message==WM_GETOBJECT && host->endpoint && !host->page) {
        if (static_cast<LONG>(l)==UiaRootObjectId && host->accessibility)
            return UiaReturnRawElementProvider(hwnd,w,l,host->accessibility.get());
    }
    if (message==WM_ERASEBKGND && host->background) {
        RECT client{};
        GetClientRect(hwnd,&client);
        FillRect(reinterpret_cast<HDC>(w),&client,host->background);
        return 1;
    }
    if (message==WM_CLOSE && host->endpoint) {
        if (host->Call(SHELL_WINDOW_HOST_OPERATION_REQUEST_CLOSE)==SHELL_STATUS_OK &&
            !PostMessageW(host->endpoint,EndpointCloseMessage(),
                          static_cast<WPARAM>(host->state.close_request),
                          static_cast<LPARAM>(host->state.generation)))
            host->Call(SHELL_WINDOW_HOST_OPERATION_CANCEL_CLOSE,host->state);
        return 0;
    }
    if (message==WM_CLOSE && host->state.phase==6) {
        // No browser exists yet. Cancel the startup lease before destroying its
        // HWND, so a late attachment cannot revive this endpoint.
        if (host->Call(SHELL_WINDOW_HOST_OPERATION_CANCEL_STARTUP,host->state)==SHELL_STATUS_OK)
            CloseHostWindow(hwnd);
        return 0;
    }
    if (host->endpoint) {
        if (message==WM_INPUTLANGCHANGE) {
            // HKL is a process-wide scalar handle, not a transient pointer.
            // Gecko translates transported keys on its own input queue.
            PostMessageW(host->endpoint,EndpointLayoutMessage(),w,l);
        }
        if (message==WM_DPICHANGED) {
            // Consume the transient suggested RECT on its owning thread. Send
            // only a notification; Gecko reconstructs current physical bounds.
            const auto bounds=*reinterpret_cast<const RECT*>(l);
            SetWindowPos(hwnd,nullptr,bounds.left,bounds.top,
                bounds.right-bounds.left,bounds.bottom-bounds.top,
                SWP_NOZORDER|SWP_NOACTIVATE);
            PostMessageW(host->endpoint,EndpointDpiMessage(),w,0);
        }
        if (message==WM_SIZE && host->page) {
            SetWindowPos(host->page,nullptr,0,0,LOWORD(l),HIWORD(l),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
            ApplyPageRegion(host);
        }
        if (message==WM_SIZE || message==WM_MOVE) PostMessageW(host->endpoint,EndpointGeometryMessage(),0,0);
        if (message==TaskbarButtonCreatedMessage()) PostMessageW(host->endpoint,message,w,l);
        if (message==WM_ACTIVATE) PostMessageW(host->endpoint,message,w,l);
        // With a page child, focus messages of the window itself are only a hop to or from the page.
        if (!(host->page && (message==WM_SETFOCUS || message==WM_KILLFOCUS))) {
            LRESULT result{};
            if (ForwardPage(hwnd,host,message,w,l,result)) return result;
        }
    }
    const auto result=DefSubclassProc(hwnd,message,w,l);
    if (message==WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd,BaseSubclass,1);
        RemovePropW(hwnd,PageWindowProperty);
        if (host->background) DeleteObject(host->background);
        // Released after this message: XAML is still unwinding its own window.
        if (host->window)
            if (auto queue=winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread())
                queue.TryEnqueue([window=std::move(host->window)]{});
        if (host->pageCursorOwned) DestroyCursor(host->pageCursor);
        for (auto icon : host->icons) if (icon) DestroyIcon(icon);
        NativePageInput::Remove(hwnd);
        if (host->state.phase==6) host->Call(SHELL_WINDOW_HOST_OPERATION_CANCEL_STARTUP,host->state);
        if (host->state.phase==4 || host->state.phase==7)
            host->Call(SHELL_WINDOW_HOST_OPERATION_SHELL_DESTROYED,host->state);
        RemovePropW(hwnd,Property);
        delete host;
    }
    return result;
}
// Takes an owned cursor over and destroys the one it replaces, after replacing it.
inline HRESULT WINAPI SetPageCursor(HWND hwnd, HCURSOR cursor, BOOL owned) noexcept {
    auto* host=Get(hwnd);
    if (!host || !Owner(hwnd)) { if (owned) DestroyCursor(cursor); return E_INVALIDARG; }
    const auto previous=host->pageCursorOwned ? host->pageCursor : nullptr;
    host->pageCursor=cursor; host->pageCursorOwned=owned!=FALSE;
    POINT point{};
    const HWND page=PageWindow(hwnd);
    if (GetCapture()==page || (GetCursorPos(&point) && WindowFromPoint(point)==page)) SetCursor(cursor);
    if (previous) DestroyCursor(previous);
    return S_OK;
}
inline HRESULT WINAPI RegisterPageTouch(HWND hwnd) noexcept {
    return ::RegisterTouchWindow(PageWindow(hwnd),TWF_WANTPALM) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}
inline HRESULT WINAPI SetAccessibility(HWND hwnd,IUnknown* provider) noexcept {
    auto* host=Get(hwnd);
    if (!host || !Owner(hwnd) || !host->endpoint) return E_INVALIDARG;
    try {
        if (!provider) {
            UiaReturnRawElementProvider(hwnd,0,0,nullptr);
            if (host->page) UiaReturnRawElementProvider(host->page,0,0,nullptr);
            host->accessibility=nullptr;
            return S_OK;
        }
        winrt::com_ptr<IRawElementProviderSimple> uia;
        const HRESULT uiaResult=provider->QueryInterface(__uuidof(IRawElementProviderSimple),uia.put_void());
        if (uiaResult!=E_NOINTERFACE) check_hresult(uiaResult);
        host->accessibility=std::move(uia);
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}
inline LRESULT CALLBACK BaseSubclass(HWND hwnd, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    return Base(hwnd,message,w,l);
}
// The system app mode, which XAML also follows: the window is this colour until
// XAML and Gecko's first paint cover it, so a dark theme never flashes white.
inline COLORREF StartupBackgroundColor() noexcept {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0) && (contrast.dwFlags&HCF_HIGHCONTRASTON))
        return GetSysColor(COLOR_WINDOW);
    DWORD light=1,size=sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size);
    return light?RGB(249,249,251):RGB(43,42,51);
}
// Makes a WinUI Window the browser window: binding state, window procedure duties and page colour.
inline HRESULT Attach(HWND hwnd, winrt::Microsoft::UI::Xaml::Window const& window,
                      winrt::Microsoft::UI::Xaml::Controls::Canvas const& root, HWND bridge) noexcept {
    try {
        auto host=std::make_unique<Context>();
        if (!host->core || !CloseMessage()) return E_FAIL;
        host->window=window; host->root=root; host->bridge=bridge;
        host->background=CreateSolidBrush(StartupBackgroundColor());
        if (!SetPropW(hwnd,Property,host.get())) return HRESULT_FROM_WIN32(GetLastError());
        if (!SetWindowSubclass(hwnd,BaseSubclass,1,0)) { RemovePropW(hwnd,Property); return E_FAIL; }
        host.release();
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}
// The outer HWND stays on its native thread; an opaque browser identity and Gecko's hidden
// proxy window, which receives the posted input, are all it knows of the browser.
inline HRESULT BindEndpoint(HWND hwnd, uint64_t generation, uintptr_t identity, HWND endpoint) noexcept {
    auto* host=Get(hwnd);
    DWORD process{};
    const auto thread=GetWindowThreadProcessId(endpoint,&process);
    if (!host || !Owner(hwnd) || !identity || !thread || thread==GetCurrentThreadId() ||
        process!=GetCurrentProcessId() || !EndpointCloseMessage()) return E_INVALIDARG;
    if (host->state.phase!=6 || host->state.generation!=generation || host->endpoint)
        return E_NOT_VALID_STATE;
    ShellWindowBinding binding{};
    binding.shell=reinterpret_cast<uintptr_t>(hwnd); binding.content=identity;
    if (host->Call(SHELL_WINDOW_HOST_OPERATION_BIND,binding)!=SHELL_STATUS_OK) return E_FAIL;
    host->endpoint=endpoint;
    // Without the child, page input stays on this window, as before it existed.
    if (const auto created=CreatePage(hwnd,host,endpoint); FAILED(created))
        FocusTrace("page-child-failed",static_cast<uintptr_t>(static_cast<uint32_t>(created)));
    // A shell shown before Firefox attached already has its taskbar button.
    if (IsWindowVisible(hwnd)) PostMessageW(endpoint,TaskbarButtonCreatedMessage(),0,0);
    return S_OK;
}
inline HRESULT ResolveEndpointClose(HWND hwnd, uint64_t generation, uint64_t request, bool allow) noexcept {
    auto* host=Get(hwnd);
    if (!host || !Owner(hwnd) || !host->endpoint) return E_INVALIDARG;
    auto decision=host->state;
    decision.generation=generation; decision.close_request=request;
    return host->Call(allow ? SHELL_WINDOW_HOST_OPERATION_APPROVE_CLOSE :
        SHELL_WINDOW_HOST_OPERATION_CANCEL_CLOSE,decision)==SHELL_STATUS_OK ? S_OK : E_NOT_VALID_STATE;
}
// Gecko asked for this on its proxy window. Applied here, where the shell lives, so Gecko never
// waits for this thread; the proxy follows once the shell reports its new state.
// Without report, the caller tells the endpoint itself once the request counts as applied there:
// a framed request is acknowledged on Gecko's side, which then reports (PaneLink).
inline HRESULT ApplyWindowRequest(HWND hwnd, const WinUIPaneWindowRequest& request, bool report=true) noexcept {
    auto* host=Get(hwnd);
    HRESULT result=S_OK;
    if (!host || !Owner(hwnd)) result=E_INVALIDARG;
    else switch (request.kind) {
        case WinUIPaneWindowPosition: {
            constexpr UINT kept=SWP_NOMOVE|SWP_NOSIZE|SWP_FRAMECHANGED|SWP_NOZORDER;
            // Only always-on-top crosses: the shell keeps its own place among other windows.
            const auto after=reinterpret_cast<HWND>(request.value);
            const bool topmost=!(request.flags&SWP_NOZORDER) && (after==HWND_TOPMOST || after==HWND_NOTOPMOST);
            if (!SetWindowPos(hwnd,topmost?after:nullptr,request.rect.left,request.rect.top,request.rect.right-request.rect.left,
                    request.rect.bottom-request.rect.top,(request.flags&kept)|(topmost?0:SWP_NOZORDER)|SWP_NOOWNERZORDER|SWP_NOACTIVATE))
                result=HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        case WinUIPaneWindowShow: ShowWindow(hwnd,static_cast<int>(request.value)); break;
        case WinUIPaneWindowStyle: {
            const LONG_PTR current=GetWindowLongPtrW(hwnd,GWL_STYLE);
            const LONG_PTR next=(current&~WinUIPaneShellStyleMask)|(request.value&WinUIPaneShellStyleMask);
            if (next!=current) {
                SetWindowLongPtrW(hwnd,GWL_STYLE,next);
                SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
            }
            break;
        }
        case WinUIPaneWindowEnable: EnableWindow(hwnd,request.value!=0); break;
        case WinUIPaneWindowIcon: {
            const auto icon=reinterpret_cast<HICON>(request.value);
            auto& held=host->icons[request.flags==ICON_BIG?0:1];
            SendMessageW(hwnd,WM_SETICON,request.flags==ICON_BIG?ICON_BIG:ICON_SMALL,reinterpret_cast<LPARAM>(icon));
            if (held) DestroyIcon(held);
            held=icon;
            break;
        }
        default: result=E_INVALIDARG;
    }
    if (FAILED(result) && request.kind==WinUIPaneWindowIcon && request.value) DestroyIcon(reinterpret_cast<HICON>(request.value));
    if (request.pending) InterlockedDecrement(request.pending);
    if (report && host && host->endpoint) PostMessageW(host->endpoint,EndpointGeometryMessage(),0,0);
    return result;
}
inline HRESULT WINAPI Destroy(HWND hwnd) noexcept {
    auto* host=Get(hwnd);
    if (!host || !Owner(hwnd)) return E_INVALIDARG;
    if (host->state.phase==6 &&
        host->Call(SHELL_WINDOW_HOST_OPERATION_CANCEL_STARTUP,host->state)!=SHELL_STATUS_OK) return E_FAIL;
    // Only nsWindow::Destroy calls this, after Firefox's close/unload decision
    // and compositor/layer teardown. Ordinary WM_CLOSE can never reach it.
    if (host->state.phase==2) host->Call(SHELL_WINDOW_HOST_OPERATION_APPROVE_CLOSE,host->state);
    if (host->state.phase>=1 && host->state.phase<=3 &&
        host->Call(SHELL_WINDOW_HOST_OPERATION_CONTENT_DESTROYED,host->state)!=SHELL_STATUS_OK) return E_FAIL;
    CloseHostWindow(hwnd);
    return S_OK;
}

// Defined with the pane: initializes XAML on this thread and creates the Window. Returns hidden;
// presentation is explicit.
inline HRESULT (*CreateShellWindow)(const RECT& bounds, HWND* output) noexcept = nullptr;
inline HRESULT CreateStartup(DWORD, DWORD style, const RECT& bounds, HWND* output) noexcept {
    if (!output) return E_POINTER;
    *output=nullptr;
    if ((style&WS_CHILD) || bounds.right<=bounds.left || bounds.bottom<=bounds.top) return E_INVALIDARG;
    HWND hwnd{};
    if (!CreateShellWindow) return E_NOT_VALID_STATE;
    HRESULT hr=CreateShellWindow(bounds,&hwnd);
    if (FAILED(hr)) return hr;
    auto* host=Get(hwnd);
    ShellWindowBinding binding{}; binding.shell=reinterpret_cast<uintptr_t>(hwnd);
    if (host->Call(SHELL_WINDOW_HOST_OPERATION_PREPARE_STARTUP,binding)!=SHELL_STATUS_OK) {
        CloseHostWindow(hwnd); return E_FAIL;
    }
    *output=hwnd;
    return S_OK;
}
}
