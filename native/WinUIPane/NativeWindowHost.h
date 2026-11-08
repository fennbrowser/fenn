#pragma once
#include <memory>
#include <functional>
#include <chrono>
#include <cstdio>
#include <uiautomation.h>
#pragma comment(lib,"uiautomationcore.lib")
#include "shell_core.h"
#include "NativePageWindow.h"

// The HWND is native-owned; nsWindow remains the logical browser adapter, on its own hidden
// window on Gecko's thread (the proxy, which is also the endpoint). Gecko keeps its compositor
// children and all window/focus/browser semantics. The binding's content identity is the
// attached nsWindow, not a disposable compositor HWND (which Gecko may replace after a
// graphics-device reset).
namespace NativeWindowHost {
// Set only by the separate shell after OS pipe authentication. Zero retains in-process ownership.
inline DWORD RemoteEnginePid{};
// Separate-process host publishes native placement after Windows applies it.
inline void (*PlacementChanged)(HWND,int activation) noexcept = nullptr;
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
// Set by the pane: what the shell does with a report (outside-press dismissal, focus observation).
inline void (*PageReported)(HWND shell, HWND page, WPARAM kind) noexcept = nullptr;
// Set by the input thread when it runs: creates the page child there instead of here.
inline bool (*RequestPage)(HWND shell, HWND endpoint, COLORREF background) noexcept = nullptr;
// Sends the shape of a page child the input thread owns to that thread, which applies it.
inline void (*SendPageShape)(HWND shell, const RECT& client, const std::vector<RECT>& chrome,
    const std::vector<RECT>& exclusions) noexcept = nullptr;
// The window is gone: its handle can come back for another window.
inline void (*RetirePageShape)(HWND shell) noexcept = nullptr;
struct Context {
    std::unique_ptr<ShellCore,decltype(&shell_core_destroy)> core{shell_core_create(nullptr),shell_core_destroy};
    // The browser window is a WinUI Window: its single XAML tree (root) and the XAML content
    // window that hosts it (bridge), below Gecko's compositor and the page child.
    winrt::Microsoft::UI::Xaml::Window window{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Canvas root{nullptr};
    HWND bridge{};
    winrt::Microsoft::UI::Xaml::Controls::Border startupPageBackground{nullptr};
    // The page colour, painted until XAML and Gecko have presented.
    HBRUSH background{};
    RECT startupFrame{}, startupFrameClient{};
    bool startupFrameValid{};
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
    DWORD endpointProcess{};
    // The page input child, created when the endpoint binds.
    HWND page{};
    // Whether the page held focus when the window last deactivated (true before the first).
    bool pageFocusOnActivate{true};
    // Native owner thread only. Returns keyboard focus to the local startup editor.
    std::function<void()> restoreEditorFocus;
    // The top-level window's own page input, used until the page child exists.
    PageState input;
    // Copies of Gecko's window icons, which Gecko destroys on its own schedule.
    HICON icons[2]{};
    ShellWindowBinding state{};
    ShellStatus Call(uint32_t operation, ShellWindowBinding input={}) {
        return shell_core_window_host(core.get(),operation,&input,&state);
    }
};
inline Context* Get(HWND hwnd) { return static_cast<Context*>(GetPropW(hwnd,Property)); }
// The Window's system backdrop is the persistent surround. This inner page
// backing is present before Gecko and follows the same native slot allocation.
// Gecko remains above it and owns the actual page pixels and browser controls.
#ifdef FENN_SHELL_PROCESS
inline LONG ContentBridgeTopInset(HWND hwnd, const Context& host) {
    POINT bridgeOrigin{}, clientOrigin{};
    if (!ClientToScreen(host.bridge, &bridgeOrigin) || !ClientToScreen(hwnd, &clientOrigin)) return 0;
    return (std::max)(0L, bridgeOrigin.y - clientOrigin.y);
}
#endif
inline void RefreshPageBackground(HWND hwnd) {
    auto* host=Get(hwnd);if(!host || !host->startupPageBackground)return;
    RECT client{},content{};GetClientRect(hwnd,&client);
    const UINT dpi=GetDpiForWindow(hwnd);
    bool framed=ContentFrame(client,host->chrome,dpi,content);
    const bool held=GetPropW(hwnd,StartupPageHoldProperty)!=nullptr;
    if(held && framed) {
        host->startupFrame=content;host->startupFrameClient=client;host->startupFrameValid=true;
    } else if(held && host->startupFrameValid && EqualRect(&client,&host->startupFrameClient)) {
        // Gecko publishes the strip and navigation bounds separately. An
        // intermediate nonmatching pair must not expand the opaque startup
        // backing across the material. Keep only the last complete allocation,
        // for this client size and only until the page hold ends.
        content=host->startupFrame;framed=true;
    } else if(!held)host->startupFrameValid=false;
    double left{},top{},width=host->root.ActualWidth(),height=host->root.ActualHeight();
    if(framed) {
        const double scale=96.0/dpi;
        left=content.left*scale;top=content.top*scale;
        width=(content.right-content.left)*scale;height=(content.bottom-content.top)*scale;
    } else {
        for(const auto& row:host->chrome)top=(std::max)(top,row.bottom*96.0/dpi);
        height=(std::max)(0.0,height-top);
    }
#ifdef FENN_SHELL_PROCESS
    // Match the page's owner-client top edge even when WinUI reserves a
    // physical pixel above its XAML content for the restored window frame.
    const double inset = (std::min)(top, ContentBridgeTopInset(hwnd, *host) * 96.0 / dpi);
    top -= inset;
    height += inset;
#endif
    auto& page=host->startupPageBackground;
    using namespace winrt::Microsoft::UI::Xaml;
    Controls::Canvas::SetLeft(page,left);Controls::Canvas::SetTop(page,top);
    page.Width((std::max)(0.0,width));page.Height((std::max)(0.0,height));
    page.CornerRadius(framed?CornerRadius{0,0,8,8}:CornerRadius{});
}
inline void LayoutStartupBackground(HWND hwnd) {
    auto* host=Get(hwnd);if(!host)return;
    using namespace winrt::Microsoft::UI::Xaml;
    if(!host->startupPageBackground) {
        host->startupPageBackground=Controls::Border();
        host->startupPageBackground.IsHitTestVisible(false);
        const auto color=StartupBackgroundColor();
        host->startupPageBackground.Background(Media::SolidColorBrush(winrt::Windows::UI::Color{
            255,GetRValue(color),GetGValue(color),GetBValue(color)}));
        host->root.Children().InsertAt(0,host->startupPageBackground);
        host->root.SizeChanged([hwnd](auto const& sender,auto const&) {
            auto* current=Get(hwnd);if(!current || sender!=current->root)return;
            RefreshPageBackground(hwnd);
        });
    }
    RefreshPageBackground(hwnd);
}
// The host whose page input this top-level window carries. Page children have their own
// state (PageState, PageProperty) on the thread that owns them.
inline Context* ForInput(HWND hwnd) { return Get(hwnd); }
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
// The page child's region: its client area less the chrome, plus the exclusions.
// The page child takes input everywhere except the visible native chrome, where Gecko's own
// controls (exclusions) still belong to the page.
// Firefox republishes geometry continually, mostly unchanged. Reshaping the page child under a
// resting pointer makes Windows hit-test it again, which WinUI reports to an open popup as the pointer
// leaving and re-entering, cancelling a press in it: only a changed region is applied.
// The page child fills the client area. A child the input thread owns is sized and shaped there:
// SetWindowRgn only works on a window of the calling process, and sizing it from here would wait
// for that thread.
inline void ApplyPageRegion(HWND shell, Context* host) {
    if (!host || !host->page) return;
    RECT client{};
    GetClientRect(shell,&client);
    const auto same=[](std::vector<RECT> const& a, std::vector<RECT> const& b) {
        return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](RECT const& x, RECT const& y){ return EqualRect(&x,&y)!=0; });
    };
    if (host->pageRegionApplied && EqualRect(&client,&host->pageRegionClient) &&
        same(host->chrome,host->pageRegionChrome) && same(host->exclusions,host->pageRegionExclusions)) return;
    host->pageRegionApplied=true; host->pageRegionClient=client;
    host->pageRegionChrome=host->chrome; host->pageRegionExclusions=host->exclusions;
    if (!Owner(host->page)) {
        if (SendPageShape) SendPageShape(shell,client,host->chrome,host->exclusions);
        return;
    }
    ApplyStartupPageVisibility(host->page,shell);
    HRGN region=PageRegion(client,host->chrome,host->exclusions,GetDpiForWindow(shell));
    if (region && !SetWindowRgn(host->page,region,TRUE)) DeleteObject(region);
}
inline void SetPageRegion(HWND hwnd, std::vector<RECT> chrome, std::vector<RECT> exclusions) {
    auto* host=Get(hwnd);
    if (!host) return;
    host->chrome=std::move(chrome); host->exclusions=std::move(exclusions);
    RefreshPageBackground(hwnd);
    ApplyPageRegion(hwnd,host);
}
// Page input for the browser behind `window`: the top-level window before the page child exists,
// the page child after. Returns true when the message is consumed, with its result.
// The shell takes a page child its window got: it keeps it in order and shaped below the chrome.
// Below the chrome's XAML and Gecko's compositor. The compositor is disabled, so pointer input
// passes it and reaches this window, as it reached the top-level window before.
inline void AdoptPage(HWND hwnd, HWND page) noexcept {
    auto* host=Get(hwnd);
    if (!host || !IsWindow(page) || GetParent(page)!=hwnd) return;
    if (host->page && host->page!=page) return;
    host->page=page;
    if (!SetPropW(hwnd,PageWindowProperty,page)) FocusTrace("page-property-failed",reinterpret_cast<uintptr_t>(page));
    host->pageRegionApplied=false;
    KeepOrder(hwnd);
    ApplyPageRegion(hwnd,host);
    if (GetFocus()==hwnd) SetFocus(page);
    // Gecko may have enabled OLE before the input child existed. Let its owner thread
    // attach the local drag target after this page becomes discoverable.
    if(host->endpoint)PostMessageW(host->endpoint,EndpointGeometryMessage(),0,0);
}
inline void ForgetPage(HWND hwnd, HWND page) noexcept {
    auto* host=Get(hwnd);
    if (!host || host->page!=page) return;
    host->page=nullptr;
    RemovePropW(hwnd,PageWindowProperty);
}
// Without the input thread the page child is this thread's, as before M5c.
inline HRESULT CreatePage(HWND hwnd, Context* host, HWND endpoint) noexcept {
    const auto background=StartupBackgroundColor();
    if (RequestPage && RequestPage(hwnd,endpoint,background)) return S_OK;
    auto* state=new (std::nothrow) PageState;
    if (!state) return E_OUTOFMEMORY;
    state->endpoint=endpoint;
    state->background=CreateSolidBrush(background);
    const HWND page=CreatePageWindow(hwnd,state);
    if (!page) { delete state; return E_FAIL; }
    AdoptPage(hwnd,page);
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
    if (message==PageReportMessage()) {
        if (PageReported && host->page==reinterpret_cast<HWND>(l)) PageReported(hwnd,host->page,w);
        return 0;
    }
    if (message==PageMessage()) {
        if (l && RemoteEnginePid) {
            DWORD pid{};wchar_t role[80]{};const auto page=reinterpret_cast<HWND>(w);
            GetWindowThreadProcessId(page,&pid);GetClassNameW(page,role,80);
            if(pid!=RemoteEnginePid || wcscmp(role,L"Fenn.NativePageInput.v1") || GetParent(page)!=hwnd)return 0;
        }
        if (l) AdoptPage(hwnd,reinterpret_cast<HWND>(w)); else ForgetPage(hwnd,reinterpret_cast<HWND>(w));
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
    // The page-focus path below returns early. Publish this transition before
    // entering it, using wParam rather than the not-yet-updated foreground HWND.
    if(message==WM_ACTIVATE && PlacementChanged && host->endpoint)PlacementChanged(hwnd,LOWORD(w)!=WA_INACTIVE);
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
        if (static_cast<LONG>(l)==UiaRootObjectId && host->input.accessibility)
            return UiaReturnRawElementProvider(hwnd,w,l,host->input.accessibility.get());
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
            if (Owner(host->page))
                SetWindowPos(host->page,nullptr,0,0,LOWORD(l),HIWORD(l),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
            ApplyPageRegion(hwnd,host);
        }
        if (message==WM_SIZE || message==WM_MOVE) PostMessageW(host->endpoint,EndpointGeometryMessage(),0,0);
        if (message==TaskbarButtonCreatedMessage()) PostMessageW(host->endpoint,message,w,l);
        if (message==WM_ACTIVATE) PostMessageW(host->endpoint,message,w,l);
        // With a page child, focus messages of the window itself are only a hop to or from the page.
        if (!GetPropW(hwnd,L"Fenn.RecoveryPending") &&
            !(host->page && (message==WM_SETFOCUS || message==WM_KILLFOCUS))) {
            LRESULT result{};
            if (ForwardPage(hwnd,&host->input,message,w,l,result)) return result;
        }
    }
    const auto result=DefSubclassProc(hwnd,message,w,l);
    if(PlacementChanged && host->endpoint && message==WM_WINDOWPOSCHANGED)PlacementChanged(hwnd,-1);
    if (message==WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd,BaseSubclass,1);
        RemovePropW(hwnd,PageWindowProperty);
        if (host->background) DeleteObject(host->background);
        // Released after this message: XAML is still unwinding its own window.
        if (host->window)
            if (auto queue=winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread())
                queue.TryEnqueue([window=std::move(host->window)]{});
        for (auto icon : host->icons) if (icon) DestroyIcon(icon);
        NativePageInput::Remove(hwnd);
        if (RetirePageShape) RetirePageShape(hwnd);
        if (host->state.phase==6) host->Call(SHELL_WINDOW_HOST_OPERATION_CANCEL_STARTUP,host->state);
        if (host->state.phase==4 || host->state.phase==7)
            host->Call(SHELL_WINDOW_HOST_OPERATION_SHELL_DESTROYED,host->state);
        RemovePropW(hwnd,Property);
        delete host;
    }
    return result;
}
// Takes an owned cursor over and destroys the one it replaces, after replacing it.
// The state page input of this window uses on the calling thread: its page child's when this
// thread owns that child, else the top-level window's own.
inline PageState* LocalPageState(Context* host) noexcept {
    if (host->page && Owner(host->page))
        if (auto* state=static_cast<PageState*>(GetPropW(host->page,PageProperty))) return state;
    return &host->input;
}
// Applies a cursor to one page state on its owning thread; the window is the one it serves.
inline HRESULT WINAPI SetPageCursor(HWND hwnd, HCURSOR cursor, BOOL owned) noexcept {
    auto* host=Get(hwnd);
    if (!host || !Owner(hwnd)) { if (owned) DestroyCursor(cursor); return E_INVALIDARG; }
    auto* state=LocalPageState(host);
    ApplyPageCursor(state,state==&host->input ? hwnd : host->page,cursor,owned);
    return S_OK;
}
inline HRESULT WINAPI RegisterPageTouch(HWND hwnd) noexcept {
    return ::RegisterTouchWindow(PageWindow(hwnd),TWF_WANTPALM) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}
inline HRESULT WINAPI SetAccessibility(HWND hwnd,IUnknown* provider) noexcept {
    auto* host=Get(hwnd);
    if (!host || !Owner(hwnd) || !host->endpoint) return E_INVALIDARG;
    try {
        auto* local=LocalPageState(host);
        if (!provider) {
            UiaReturnRawElementProvider(hwnd,0,0,nullptr);
            if (local!=&host->input) { UiaReturnRawElementProvider(host->page,0,0,nullptr); local->accessibility=nullptr; }
            host->input.accessibility=nullptr;
            return S_OK;
        }
        winrt::com_ptr<IRawElementProviderSimple> uia;
        const HRESULT uiaResult=provider->QueryInterface(__uuidof(IRawElementProviderSimple),uia.put_void());
        if (uiaResult!=E_NOINTERFACE) check_hresult(uiaResult);
        host->input.accessibility=uia;
        if (local!=&host->input) local->accessibility=std::move(uia);
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}
inline LRESULT CALLBACK BaseSubclass(HWND hwnd, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    return Base(hwnd,message,w,l);
}
// The system app mode, which XAML also follows: the window is this colour until
// XAML and Gecko's first paint cover it, so a dark theme never flashes white.
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
        (process!=GetCurrentProcessId() && process!=RemoteEnginePid) || !EndpointCloseMessage()) return E_INVALIDARG;
    if (host->state.phase!=6 || host->state.generation!=generation || host->endpoint)
        return E_NOT_VALID_STATE;
    ShellWindowBinding binding{};
    binding.shell=reinterpret_cast<uintptr_t>(hwnd); binding.content=identity;
    if (host->Call(SHELL_WINDOW_HOST_OPERATION_BIND,binding)!=SHELL_STATUS_OK) return E_FAIL;
    host->endpoint=endpoint;
    host->endpointProcess=process;
    host->input.endpoint=endpoint;
    // Without the child, page input stays on this window, as before it existed.
    if (const auto created=CreatePage(hwnd,host,endpoint); FAILED(created))
        FocusTrace("page-child-failed",static_cast<uintptr_t>(static_cast<uint32_t>(created)));
    // A shell shown before Firefox attached already has its taskbar button.
    if (IsWindowVisible(hwnd)) PostMessageW(endpoint,TaskbarButtonCreatedMessage(),0,0);
    return S_OK;
}
// A held, signaled peer handle is required: transport loss alone cannot revoke
// a live browser. Pane/popups must be closed by the caller before re-preparing.
inline winrt::Microsoft::UI::Xaml::Controls::ContentControl GateChrome(Context* host) {
    using namespace winrt::Microsoft::UI::Xaml;
    auto gate=host->window.Content().try_as<Controls::ContentControl>();
    if(!gate) {
        gate=Controls::ContentControl();
        gate.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        gate.VerticalContentAlignment(VerticalAlignment::Stretch);
        host->window.Content(nullptr);gate.Content(host->root);host->window.Content(gate);
    } else if(gate.Content()!=host->root)winrt::throw_hresult(E_NOT_VALID_STATE);
    gate.IsEnabled(false);
    return gate;
}
inline HRESULT RetireDeadEndpoint(HWND hwnd, HANDLE peer) noexcept {
    try {
    auto* host=Get(hwnd);
    if(!host || !Owner(hwnd) || !peer || !host->endpoint || !host->endpointProcess ||
       host->state.phase<1 || host->state.phase>3 ||
       host->endpointProcess==GetCurrentProcessId() || GetProcessId(peer)!=host->endpointProcess ||
       WaitForSingleObject(peer,0)!=WAIT_OBJECT_0)return E_INVALIDARG;
    GateChrome(host);
    if(host->Call(SHELL_WINDOW_HOST_OPERATION_CONTENT_DESTROYED,host->state)!=SHELL_STATUS_OK)return E_NOT_VALID_STATE;
    host->endpoint=nullptr;host->endpointProcess=0;
    host->input.endpoint=nullptr;host->page=nullptr;
    RemovePropW(hwnd,PageWindowProperty);
    UiaReturnRawElementProvider(hwnd,0,0,nullptr);
    host->input.accessibility=nullptr;
    NativePageInput::Remove(hwnd);
    if(GetCapture()==hwnd)ReleaseCapture();
    TRACKMOUSEEVENT tracking{sizeof(tracking),TME_CANCEL|TME_LEAVE,hwnd,0};TrackMouseEvent(&tracking);
    host->input.capture=false;host->input.tracking=false;
    if(host->input.cursorOwned && host->input.cursor)DestroyCursor(host->input.cursor);
    host->input.cursor=nullptr;host->input.cursorOwned=false;
    host->restoreEditorFocus={};host->pageFocusOnActivate=false;
    RemovePropW(hwnd,EditorOwnedProperty);RemovePropW(hwnd,EditorGraceProperty);
    host->pageRegionApplied=false;host->chrome.clear();host->exclusions.clear();
    host->pageRegionChrome.clear();host->pageRegionExclusions.clear();
    return S_OK;
    }catch(...){return winrt::to_hresult();}
}
inline HRESULT PrepareReplacementEndpoint(HWND hwnd) noexcept {
    try {
    auto* host=Get(hwnd);
    if(!host || !Owner(hwnd) || host->state.phase!=4 || host->endpoint || host->page ||
       host->input.accessibility || host->root.Children().Size())return E_NOT_VALID_STATE;
        std::unique_ptr<ShellCore,decltype(&shell_core_destroy)> next{shell_core_create(nullptr),shell_core_destroy};
        if(!next)return E_OUTOFMEMORY;
        ShellWindowBinding prepared{},input{};input.shell=reinterpret_cast<uintptr_t>(hwnd);
        if(shell_core_window_host(next.get(),SHELL_WINDOW_HOST_OPERATION_PREPARE_STARTUP,&input,&prepared)!=SHELL_STATUS_OK)return E_FAIL;
        host->core=std::move(next);host->state=prepared;
        for(const auto* name:{L"Fenn.RecoveryPrivacy",L"Fenn.RecoverySessionReady",L"Fenn.RecoveryReady",
            L"Fenn.RecoveryEvidence",L"Fenn.RecoveryDeferred"})RemovePropW(hwnd,name);
        return S_OK;
    }catch(...){return winrt::to_hresult();}
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
