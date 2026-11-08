#pragma once
// Shared Win32/TSF/UIA page implementation. No XAML or Windows App SDK dependency.
#include <windowsx.h>
#include <uiautomation.h>
#include <winrt/base.h>
#include <chrono>
#include <cstdio>
#include "PageInput.h"
#pragma comment(lib,"uiautomationcore.lib")
namespace NativeWindowHost {
inline constexpr wchar_t PageProperty[] = L"Fenn.NativeWindowHost.Page";
inline constexpr wchar_t PageWindowProperty[] = L"Fenn.PageWindow";
inline constexpr wchar_t StartupPageHoldProperty[] = L"Fenn.NativeStartupPageHold";
// Software rendering writes into this existing layered HWND. Keep its pixels
// updating while startup holds presentation; hiding/clipping it can stop paint.
// Called only by the page owner, also after the existing shape message arrives.
inline bool ApplyStartupPageVisibility(HWND page, HWND shell) noexcept {
    const BYTE desired=GetPropW(shell,StartupPageHoldProperty)?0:255;
    BYTE alpha{};DWORD flags{};
    if(GetLayeredWindowAttributes(page,nullptr,&alpha,&flags) && (flags&LWA_ALPHA) && alpha==desired)return true;
    return SetLayeredWindowAttributes(page,0,desired,LWA_ALPHA)!=FALSE;
}
struct PageState;
inline COLORREF StartupBackgroundColor() noexcept;
// Set by the input thread: it owns the states of the page children it created.
inline void (*PageDestroyed)(HWND page, PageState* state) noexcept = nullptr;
// Page input state, owned by the thread of the window it serves: the shell's thread for the
// top-level window until its page child exists, the input thread for the page child (M5c).
struct PageState {
    HWND endpoint{};
    // The shell window a page child belongs to; null for the top-level window's own state.
    HWND shell{};
    HBRUSH background{};
    winrt::com_ptr<IRawElementProviderSimple> accessibility;
    // This thread holds capture for a page drag: Gecko cannot capture a window it does not own.
    bool capture{};
    bool tracking{};
    HCURSOR cursor{};
    bool cursorOwned{};
    PageState()=default;
    PageState(const PageState&)=delete;
    PageState& operator=(const PageState&)=delete;
    ~PageState() {
        if (background) DeleteObject(background);
        if (cursorOwned && cursor) DestroyCursor(cursor);
    }
};
// Posted to the shell window by the page child's thread: wParam the page, lParam 1 when it was
// created, 0 when it was destroyed. A post, so it also crosses a process boundary later.
inline UINT PageMessage() { static UINT message=RegisterWindowMessageW(L"Fenn.NativeWindowHost.Page.v1"); return message; }
// Posted to the shell window by the page child's thread when the page is pressed (wParam 1),
// gains focus (2) or loses it (3); lParam is the page. The shell's own message filter saw these
// while the page child was its own.
inline UINT PageReportMessage() { static UINT message=RegisterWindowMessageW(L"Fenn.NativeWindowHost.PageReport.v1"); return message; }

inline bool Owner(HWND hwnd) { return GetWindowThreadProcessId(hwnd,nullptr)==GetCurrentThreadId(); }

// The native navigation allocation describes the page surround as well. Only
// the contiguous full strip + inset navigation layout has a frame; retained
// toolbars, hidden rows and fullscreen keep their original unrestricted region.
inline bool ContentFrame(const RECT& client, const std::vector<RECT>& chrome, UINT dpi, RECT& content) {
    if(chrome.size()!=2)return false;
    const auto& strip=chrome[0];const auto& nav=chrome[1];
    const LONG inset=nav.left-client.left;
    if(strip.left!=client.left || strip.top!=client.top || strip.right!=client.right ||
       strip.bottom!=nav.top || inset<=0 || inset>MulDiv(32,dpi,96) ||
       client.right-nav.right!=inset || nav.bottom>=client.bottom-inset)return false;
    content={nav.left,nav.bottom,nav.right,client.bottom-inset};
    return true;
}
inline HRGN RoundedPageRegion(const RECT& content, UINT dpi) {
    const LONG radius=MulDiv(8,dpi,96);
    HRGN rounded=CreateRoundRectRgn(content.left,content.top,content.right+1,content.bottom+1,2*radius,2*radius);
    if(rounded) {
        // The page joins the toolbar at a square top edge; only its bottom
        // corners expose the native material.
        HRGN top=CreateRectRgn(content.left,content.top,content.right,(std::max)(content.top,content.bottom-radius));
        if(top){CombineRgn(rounded,rounded,top,RGN_OR);DeleteObject(top);}
        HRGN bounds=CreateRectRgnIndirect(&content);
        if(bounds){CombineRgn(rounded,rounded,bounds,RGN_AND);DeleteObject(bounds);}
    }
    return rounded;
}
inline HRGN PageRegion(const RECT& client, const std::vector<RECT>& chrome, const std::vector<RECT>& exclusions, UINT dpi=96) {
    HRGN region=CreateRectRgnIndirect(&client);
    if (!region) return nullptr;
    for (auto const& rect : chrome) {
        if (HRGN cut=CreateRectRgnIndirect(&rect)) { CombineRgn(region,region,cut,RGN_DIFF); DeleteObject(cut); }
    }
    RECT content{};
    if(ContentFrame(client,chrome,dpi,content)) {
        if(HRGN page=RoundedPageRegion(content,dpi)) {
            CombineRgn(region,region,page,RGN_AND);DeleteObject(page);
        }
    }
    for (auto const& rect : exclusions) {
        if (HRGN add=CreateRectRgnIndirect(&rect)) { CombineRgn(region,region,add,RGN_OR); DeleteObject(add); }
    }
    return region;
}

inline WPARAM PageKeyPacket(WPARAM key) {
    uint32_t modifiers=0;
    for (size_t i=0;i<std::size(WinUIPaneModifierKeys);++i) {
        const auto state=GetKeyState(WinUIPaneModifierKeys[i]);
        if (state&0x8000) modifiers|=1u<<(i*2);
        if (state&1) modifiers|=2u<<(i*2);
    }
    return (static_cast<WPARAM>(modifiers)<<32)|(key&0xffffffffu);
}

inline bool ForwardPage(HWND window, PageState* host, UINT message, WPARAM w, LPARAM l, LRESULT& result) {
    result=0;
    // Says whether touch or pen made a mouse message. Read before anything here can make this
    // thread take a sent message, which replaces it: taking capture from a window of another
    // thread does (WM_CAPTURECHANGED), and a touch tap's promoted press then reached Gecko as a
    // mouse press.
    const auto extra=static_cast<uint32_t>(GetMessageExtraInfo());
    if (message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_MBUTTONDOWN ||
        message==WM_LBUTTONDBLCLK || message==WM_RBUTTONDBLCLK || message==WM_MBUTTONDBLCLK) {
        if (!GetCapture()) { SetCapture(window); host->capture=GetCapture()==window; }
    }
    // Leave tracking belongs to this thread, so nsWindow's own request has no effect. As in
    // nsWindow::CaptureMouse it pauses during a capture; asking again when capture ends
    // reports a leave at once if the drag ended outside the window.
    const auto trackLeave=[&]{
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};
        host->tracking=TrackMouseEvent(&track)!=FALSE;
    };
    if (message==WM_MOUSEMOVE && !host->tracking && !host->capture) trackLeave();
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
    if (message==WM_SETCURSOR && reinterpret_cast<HWND>(w)==window && LOWORD(l)==HTCLIENT && host->cursor) {
        SetCursor(host->cursor);
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
    if (message==WM_MOUSELEAVE) { host->tracking=false; PostMessageW(host->endpoint,message+WINUI_PANE_ENDPOINT_KEY_OFFSET,0,0); }
    if (message==WM_CAPTURECHANGED && host->capture) { host->capture=false; trackLeave(); }
    // Only pointer-free notifications cross this boundary. Synchronous
    // non-client, accessibility, IME and drag messages are not forwarded.
    if (message==WM_CONTEXTMENU) {
        // Coordinates (or -1 for keyboard invocation) and source HWND are
        // scalar values. Gecko owns context targeting and cancellation.
        PostMessageW(host->endpoint,message,w,l);
        return true;
    }
    switch (message) {
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            // DefWindowProc propagates wheel messages to the parent, which
            // also forwards page input. Consume our copy to avoid two Gecko
            // deliveries for one wheel input.
            PostMessageW(host->endpoint,message,w,l);
            return true;
        case WM_SETFOCUS: case WM_KILLFOCUS:
            PostMessageW(host->endpoint,message,w,l); break;
        // Extra info says whether touch or pen made this message, and it is per thread.
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK:
            PostMessageW(host->endpoint,message,
                (static_cast<WPARAM>(extra)<<32)|(w&0xffffffffu),l);
            break;
    }
    if ((message==WM_LBUTTONUP || message==WM_RBUTTONUP || message==WM_MBUTTONUP) && host->capture &&
        !(w&(MK_LBUTTON|MK_RBUTTON|MK_MBUTTON)) && GetCapture()==window) ReleaseCapture();
    if (message==WM_KEYDOWN || message==WM_KEYUP || message==WM_SYSKEYDOWN || message==WM_SYSKEYUP) {
        PostMessageW(host->endpoint,message+WINUI_PANE_ENDPOINT_KEY_OFFSET,PageKeyPacket(w),l);
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
    auto* state=static_cast<PageState*>(GetPropW(page,PageProperty));
    if (state) {
        if (message==NativePageInput::LockMessage()) { NativePageInput::Pump(page,w); return 0; }
        if (message==WM_SETFOCUS) NativePageInput::Focus(page);
        if (message==WM_KILLFOCUS) NativePageInput::Blur(page);
        // A page child on another thread tells its shell what the shell's message filter would see.
        if ((message==WM_SETFOCUS || message==WM_KILLFOCUS) && state->shell && !Owner(state->shell))
            PostMessageW(state->shell,PageReportMessage(),message==WM_SETFOCUS ? 2 : 3,reinterpret_cast<LPARAM>(page));
        if (message==WM_GETOBJECT && static_cast<LONG>(l)==UiaRootObjectId) {
            if (state->accessibility)return UiaReturnRawElementProvider(page,w,l,state->accessibility.get());
            // A real root may not exist yet when a second window binds with a11y
            // already active. Request it on Gecko; never wait in WM_GETOBJECT.
            static UINT wake=RegisterWindowMessageW(L"Fenn.NativeWindowHost.AccessibilityWake.v1");
            if(state->endpoint)PostMessageW(state->endpoint,wake,0,0);
        }
        // The startup fill is completed before this page is published. Once
        // published, Firefox owns its pixels (including the retained layered
        // software surface). A later native erase must not overwrite a frame.
        if (message==WM_ERASEBKGND) return 1;
        if (state->endpoint) {
            LRESULT result{};
            if (ForwardPage(page,state,message,w,l,result)) return result;
        }
        if (message==WM_NCDESTROY) {
            UiaReturnRawElementProvider(page,0,0,nullptr);
            NativePageInput::Remove(page);
            RemovePropW(page,PageProperty);
            RemovePropW(page,WINUI_PANE_ENDPOINT_PROPERTY);
            if (state->shell) PostMessageW(state->shell,PageMessage(),reinterpret_cast<WPARAM>(page),0);
            if (PageDestroyed) PageDestroyed(page,state);
            else delete state;
        }
    }
    return DefWindowProcW(page,message,w,l);
}
// Software rendering draws into this window (Gecko's SurfaceTarget) through GDI. It is layered at full
// opacity so it has its own surface: the WinUI Window extends its frame over the whole client area for
// the backdrop, where GDI's zero alpha would let the backdrop show through the page.
// Runs on the thread that will own the page child; the state is the child's from then on.
inline HWND CreatePageWindow(HWND shell, PageState* state) noexcept {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(PageProc),&module)) return nullptr;
    constexpr auto name=L"Fenn.NativePageInput.v1";
    WNDCLASSW wc{};
    if (!GetClassInfoW(module,name,&wc)) {
        wc.lpfnWndProc=PageProc; wc.hInstance=module; wc.lpszClassName=name;
        wc.style=CS_DBLCLKS; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if (!RegisterClassW(&wc)) return nullptr;
    } else if (wc.lpfnWndProc!=PageProc) return nullptr;
    RECT client{};
    GetClientRect(shell,&client);
    HWND page=CreateWindowExW(WS_EX_NOPARENTNOTIFY|WS_EX_LAYERED,name,L"",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
        0,0,client.right,client.bottom,shell,nullptr,module,nullptr);
    if (!page) return nullptr;
    if(!ApplyStartupPageVisibility(page,shell)){DestroyWindow(page);return nullptr;}
    state->shell=shell;
    if (!SetPropW(page,PageProperty,state) || !SetPropW(page,WINUI_PANE_ENDPOINT_PROPERTY,state->endpoint)) {
        RemovePropW(page,PageProperty); state->shell=nullptr; DestroyWindow(page); return nullptr;
    }
    // Finish native background drawing before the caller publishes this page
    // through Fenn.PageWindow. SurfaceTarget cannot select it before that
    // publication. Flush this thread's GDI batch before another thread can copy
    // Firefox's frame; there is no asynchronous first-frame ownership flag.
    HDC dc=GetDC(page);
    const bool initialized=dc && state->background && FillRect(dc,&client,state->background) && GdiFlush();
    if (dc) ReleaseDC(page,dc);
    if (!initialized) {
        RemovePropW(page,PageProperty); state->shell=nullptr; DestroyWindow(page); return nullptr;
    }
    return page;
}

inline void ApplyPageCursor(PageState* state, HWND window, HCURSOR cursor, BOOL owned) noexcept {
    const auto previous=state->cursorOwned ? state->cursor : nullptr;
    state->cursor=cursor; state->cursorOwned=owned!=FALSE;
    POINT point{};
    if (GetCapture()==window || (GetCursorPos(&point) && WindowFromPoint(point)==window)) SetCursor(cursor);
    if (previous && previous!=cursor) DestroyCursor(previous);
}

inline COLORREF StartupBackgroundColor() noexcept {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0) && (contrast.dwFlags&HCF_HIGHCONTRASTON))
        return GetSysColor(COLOR_WINDOW);
    DWORD light=1,size=sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size);
    return light?RGB(249,249,251):RGB(43,42,51);
}

}
