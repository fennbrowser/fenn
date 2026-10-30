#pragma once
#include <memory>
#include <functional>
#include <chrono>
#include <cstdio>
#include <uiautomation.h>
#pragma comment(lib,"uiautomationcore.lib")
#include "shell_core.h"

// The HWND is native-owned; nsWindow remains the logical browser adapter.
// Gecko keeps its compositor children and all window/focus/browser semantics.
// The binding's content identity is the attached nsWindow, not a disposable
// compositor HWND (which Gecko may replace after a graphics-device reset).
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
struct Context {
    std::unique_ptr<ShellCore,decltype(&shell_core_destroy)> core{shell_core_create(nullptr),shell_core_destroy};
    WNDPROC base{}, fallback{}, gecko{};
    // Independent browser endpoint. Never call its window procedure on the
    // native UI thread or synchronously wait for its close decision.
    HWND endpoint{};
    // Native owner thread only. Returns keyboard focus to the local startup editor.
    std::function<void()> restoreEditorFocus;
    winrt::com_ptr<IRawElementProviderSimple> accessibility;
    ShellWindowBinding state{};
    ShellStatus Call(uint32_t operation, ShellWindowBinding input={}) {
        return shell_core_window_host(core.get(),operation,&input,&state);
    }
};
inline Context* Get(HWND hwnd) { return static_cast<Context*>(GetPropW(hwnd,Property)); }
inline bool Owner(HWND hwnd) { return GetWindowThreadProcessId(hwnd,nullptr)==GetCurrentThreadId(); }

inline LRESULT CALLBACK Base(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto* host=Get(hwnd);
    if (!host) return DefWindowProcW(hwnd,message,w,l);
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
            if (!keep) SetFocus(hwnd);
        }
        return 0;
    }
    if (message==RestoreEditorFocusMessage()) {
        if (EditorOwned(hwnd) && host->restoreEditorFocus) host->restoreEditorFocus();
        return 0;
    }
    // Focus can land on the host HWND itself, not the XAML island: for example when a Gecko
    // dialog that held activation closes. Return it to the editor on the next turn.
    if (message==WM_SETFOCUS) FocusTrace(EditorOwned(hwnd)?"host-setfocus-owned":"host-setfocus");
    if (message==WM_SETFOCUS && EditorOwned(hwnd) && host->restoreEditorFocus)
        PostMessageW(hwnd,RestoreEditorFocusMessage(),0,0);
    if(message==NativePageInput::LockMessage()) {NativePageInput::Pump(hwnd,w);return 0;}
    if(host->endpoint && message==WM_SETFOCUS) NativePageInput::Focus(hwnd);
    if(host->endpoint && message==WM_KILLFOCUS) NativePageInput::Blur(hwnd);
    if (message==WM_GETOBJECT && host->endpoint) {
        if (static_cast<LONG>(l)==UiaRootObjectId && host->accessibility)
            return UiaReturnRawElementProvider(hwnd,w,l,host->accessibility.get());
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
            DestroyWindow(hwnd);
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
            return 0;
        }
        // Only pointer-free notifications cross this boundary. Synchronous
        // non-client, accessibility, IME and drag messages are not forwarded.
        if (message==WM_CONTEXTMENU) {
            // Coordinates (or -1 for keyboard invocation) and source HWND are
            // scalar values. Gecko owns context targeting and cancellation.
            PostMessageW(host->endpoint,message,w,l);
            return 0;
        }
        switch (message) {
            case WM_SIZE: case WM_MOVE: case WM_ACTIVATE:
            case WM_SETFOCUS: case WM_KILLFOCUS:
            case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                PostMessageW(host->endpoint,message,w,l); break;
        }
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
            PAINTSTRUCT paint{}; BeginPaint(hwnd,&paint); EndPaint(hwnd,&paint);
            return 0;
        }
    }
    const auto base=host->base;
    const auto result=CallWindowProcW(base,hwnd,message,w,l);
    if (message==WM_NCDESTROY) {
        NativePageInput::Remove(hwnd);
        if (host->state.phase==6) host->Call(SHELL_WINDOW_HOST_OPERATION_CANCEL_STARTUP,host->state);
        if (host->state.phase==4 || host->state.phase==7)
            host->Call(SHELL_WINDOW_HOST_OPERATION_SHELL_DESTROYED,host->state);
        RemovePropW(hwnd,Property);
        delete host;
    }
    return result;
}
inline LRESULT CALLBACK Forward(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto* host=Get(hwnd);
    if (!host) return DefWindowProcW(hwnd,message,w,l);
    if (message==WM_CLOSE) {
        if (host->gecko && host->Call(SHELL_WINDOW_HOST_OPERATION_REQUEST_CLOSE)==SHELL_STATUS_OK) {
            if (!PostMessageW(hwnd,CloseMessage(),static_cast<WPARAM>(host->state.close_request),
                              static_cast<LPARAM>(host->state.generation)))
                host->Call(SHELL_WINDOW_HOST_OPERATION_CANCEL_CLOSE,host->state);
        }
        return 0;
    }
    if (message==CloseMessage()) {
        const auto request=host->state;
        if (!host->gecko || request.phase!=2 || request.close_request!=static_cast<uint64_t>(w) ||
            request.generation!=static_cast<uint64_t>(l)) return 0;
        // Gecko's existing RequestWindowClose dispatches the browser close
        // event, including confirmation/unload cancellation. Destroy() is the
        // only approval to destroy the native HWND. Never infer approval from
        // the window-procedure return value or retain host across that callback.
        const auto gecko=host->gecko;
        CallWindowProcW(gecko,hwnd,WM_CLOSE,0,0);
        auto* current=Get(hwnd);
        if (current && current->state.generation==request.generation && current->state.phase==2)
            current->Call(SHELL_WINDOW_HOST_OPERATION_CANCEL_CLOSE,request);
        return 0;
    }
    const auto procedure=host->gecko ? host->gecko : host->fallback;
    return CallWindowProcW(procedure,hwnd,message,w,l);
}
inline HRESULT WINAPI SetAccessibility(HWND hwnd,IUnknown* provider) noexcept {
    auto* host=Get(hwnd);
    if (!host || !Owner(hwnd) || !host->endpoint) return E_INVALIDARG;
    try {
        if (!provider) {
            UiaReturnRawElementProvider(hwnd,0,0,nullptr);
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
inline HRESULT WINAPI Create(DWORD ex, const wchar_t* name, DWORD style, const RECT* bounds,
                             HWND parent, HINSTANCE instance, HWND* output) noexcept {
    if (!output || !name || !bounds) return E_POINTER;
    *output=nullptr;
    if (parent || (style&WS_CHILD)) return E_INVALIDARG;
    try {
        auto host=std::make_unique<Context>();
        if (!host->core || !CloseMessage()) return E_FAIL;
        HWND hwnd=CreateWindowExW(ex,name,L"",style,bounds->left,bounds->top,
            bounds->right-bounds->left,bounds->bottom-bounds->top,parent,nullptr,instance,nullptr);
        if (!hwnd) return HRESULT_FROM_WIN32(GetLastError());
        host->base=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd,GWLP_WNDPROC));
        if (!SetPropW(hwnd,Property,host.get())) { auto error=HRESULT_FROM_WIN32(GetLastError()); DestroyWindow(hwnd); return error; }
        SetLastError(ERROR_SUCCESS);
        auto previous=SetWindowLongPtrW(hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Base));
        if (!previous && GetLastError()!=ERROR_SUCCESS) {
            auto error=HRESULT_FROM_WIN32(GetLastError()); RemovePropW(hwnd,Property); DestroyWindow(hwnd); return error;
        }
        host.release(); *output=hwnd; return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}
inline HRESULT WINAPI Attach(HWND hwnd, uintptr_t identity, WNDPROC gecko, WNDPROC* fallback) noexcept {
    auto* host=Get(hwnd);
    if (!host || !Owner(hwnd) || !identity || !gecko || !fallback) return E_INVALIDARG;
    if (host->state.phase!=0 && host->state.phase!=6) return E_NOT_VALID_STATE;
    // Windows App SDK has already associated the window. Preserve its chain
    // below Gecko's adapter, with Base at the bottom for final native cleanup.
    host->fallback=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd,GWLP_WNDPROC));
    ShellWindowBinding binding{}; binding.shell=reinterpret_cast<uintptr_t>(hwnd); binding.content=identity;
    if (host->Call(SHELL_WINDOW_HOST_OPERATION_BIND,binding)!=SHELL_STATUS_OK) return E_FAIL;
    host->gecko=gecko;
    SetLastError(ERROR_SUCCESS);
    auto previous=SetWindowLongPtrW(hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Forward));
    if (!previous && GetLastError()!=ERROR_SUCCESS) { host->gecko=nullptr; return HRESULT_FROM_WIN32(GetLastError()); }
    *fallback=host->fallback;
    return S_OK;
}
// Independent-thread foundation. The outer HWND stays on its original native
// thread; an opaque browser identity and message-only notification endpoint
// replace the same-thread Gecko WNDPROC attachment in the development lane.
inline HRESULT BindEndpoint(HWND hwnd, uint64_t generation, uintptr_t identity, HWND endpoint) noexcept {
    auto* host=Get(hwnd);
    DWORD process{};
    const auto thread=GetWindowThreadProcessId(endpoint,&process);
    if (!host || !Owner(hwnd) || !identity || !thread || thread==GetCurrentThreadId() ||
        process!=GetCurrentProcessId() || !EndpointCloseMessage()) return E_INVALIDARG;
    if (host->state.phase!=6 || host->state.generation!=generation || host->gecko || host->endpoint)
        return E_NOT_VALID_STATE;
    ShellWindowBinding binding{};
    binding.shell=reinterpret_cast<uintptr_t>(hwnd); binding.content=identity;
    if (host->Call(SHELL_WINDOW_HOST_OPERATION_BIND,binding)!=SHELL_STATUS_OK) return E_FAIL;
    host->endpoint=endpoint;
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
// Transfer ownership only from Fenn's pre-XUL native bootstrap, never from a
// Firefox/skeleton HWND. The bootstrap has already consumed its one-use slot.
inline HRESULT WINAPI AdoptStartup(HWND hwnd) noexcept {
    if (!hwnd || !Owner(hwnd) || Get(hwnd) ||
        GetPropW(hwnd,L"Fenn.NativeStartupOrigin")!=reinterpret_cast<HANDLE>(1)) return E_INVALIDARG;
    wchar_t name[64]{};
    if (!GetClassNameW(hwnd,name,64) || wcscmp(name,L"Fenn.NativeStartupBootstrap.v1")) return E_INVALIDARG;
    try {
        auto host=std::make_unique<Context>();
        if (!host->core || !CloseMessage()) return E_FAIL;
        ShellWindowBinding binding{}; binding.shell=reinterpret_cast<uintptr_t>(hwnd);
        if (host->Call(SHELL_WINDOW_HOST_OPERATION_PREPARE_STARTUP,binding)!=SHELL_STATUS_OK) return E_FAIL;
        host->base=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd,GWLP_WNDPROC));
        if (!SetPropW(hwnd,Property,host.get())) return HRESULT_FROM_WIN32(GetLastError());
        SetLastError(ERROR_SUCCESS);
        auto previous=SetWindowLongPtrW(hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Base));
        if (!previous && GetLastError()!=ERROR_SUCCESS) {
            auto error=HRESULT_FROM_WIN32(GetLastError()); RemovePropW(hwnd,Property); return error;
        }
        host.release();
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}
inline HRESULT WINAPI Detach(HWND hwnd) noexcept {
    auto* host=Get(hwnd);
    if (!host || !Owner(hwnd)) return E_INVALIDARG;
    if (!host->gecko) return S_OK;
    SetLastError(ERROR_SUCCESS);
    auto previous=SetWindowLongPtrW(hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(host->fallback));
    if (!previous && GetLastError()!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(GetLastError());
    host->gecko=nullptr;
    return S_OK;
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
    return DestroyWindow(hwnd) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

// Bootstrap foundation: no Firefox class registration, nsWindow, XAML object,
// or browser publication is needed to create this HWND. It returns hidden;
// presentation is explicit.
inline LRESULT CALLBACK StartupProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message==WM_NCCREATE) EnableNonClientDpiScaling(hwnd);
    return DefWindowProcW(hwnd,message,w,l);
}
inline HRESULT CreateStartup(DWORD ex, DWORD style, const RECT& bounds, HWND* output) noexcept {
    if (!output) return E_POINTER;
    *output=nullptr;
    if ((style&WS_CHILD) || bounds.right<=bounds.left || bounds.bottom<=bounds.top) return E_INVALIDARG;
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(StartupProc),&module)) return HRESULT_FROM_WIN32(GetLastError());
    constexpr auto name=L"Fenn.NativeStartupHost.v1";
    WNDCLASSW wc{};
    if (!GetClassInfoW(module,name,&wc)) {
        wc.lpfnWndProc=StartupProc; wc.hInstance=module; wc.lpszClassName=name;
        wc.style=CS_DBLCLKS; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if (!RegisterClassW(&wc)) return HRESULT_FROM_WIN32(GetLastError());
    } else if (wc.lpfnWndProc!=StartupProc) return E_UNEXPECTED;
    HWND hwnd{};
    HRESULT hr=Create(ex,name,style&~WS_VISIBLE,&bounds,nullptr,module,&hwnd);
    if (FAILED(hr)) return hr;
    auto* host=Get(hwnd);
    ShellWindowBinding binding{}; binding.shell=reinterpret_cast<uintptr_t>(hwnd);
    if (host->Call(SHELL_WINDOW_HOST_OPERATION_PREPARE_STARTUP,binding)!=SHELL_STATUS_OK) {
        DestroyWindow(hwnd); return E_FAIL;
    }
    *output=hwnd;
    return S_OK;
}
}
