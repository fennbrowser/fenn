#pragma once
#include <cerrno>
#include <cwchar>

// Read Firefox's own persisted browser geometry through the WinRT JSON
// adapter. No second placement store or native session policy is introduced.
namespace NativeStartupPlacement {
struct Placement { RECT bounds{100,100,1380,1000}; bool saved{}, maximized{}; };
inline Placement Read(const std::wstring& profile) noexcept {
    Placement result;
    try {
        if (profile.empty()) return result;
        const auto path=profile+L"\\xulstore.json";
        HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file==INVALID_HANDLE_VALUE) return result;
        struct FileCloser { HANDLE value; ~FileCloser() { CloseHandle(value); } } closer{file};
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file,&length) || length.QuadPart<=0 || length.QuadPart>1024*1024) {
            return result;
        }
        std::string bytes(static_cast<size_t>(length.QuadPart),'\0'); DWORD read{};
        const bool ok=ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)!=FALSE;
        if (!ok || read!=bytes.size()) return result;
        auto window=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(bytes))
            .GetNamedObject(L"chrome://browser/content/browser.xhtml").GetNamedObject(L"main-window");
        auto number=[&](const wchar_t* key,LONG minimum,LONG maximum) {
            const auto text=window.GetNamedString(key); wchar_t* end{}; errno=0;
            const long value=wcstol(text.c_str(),&end,10);
            if (errno || end==text.c_str() || *end || value<minimum || value>maximum)
                throw winrt::hresult_invalid_argument();
            return value;
        };
        const LONG x=number(L"screenX",-100000,100000), y=number(L"screenY",-100000,100000);
        const LONG width=number(L"width",100,100000), height=number(L"height",100,100000);
        result.bounds={x,y,x+width,y+height};
        result.maximized=window.GetNamedString(L"sizemode",L"normal")==L"maximized";
        result.saved=true;
    } catch (...) { return {}; } // Missing/corrupt state keeps the bootstrap fallback.
    return result;
}
inline void Apply(HWND hwnd, const Placement& placement) {
    if (!placement.saved) return;
    // Firefox persists sizes in CSS pixels and Windows positions in desktop
    // pixels. Resolve the target monitor's current DPI on this hidden HWND.
    const UINT dpi=GetDpiForWindow(hwnd);
    const int width=MulDiv(placement.bounds.right-placement.bounds.left,dpi,96);
    const int height=MulDiv(placement.bounds.bottom-placement.bounds.top,dpi,96);
    MONITORINFO monitor{sizeof(monitor)};
    check_bool(GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor));
    const auto& work=monitor.rcWork;
    const int w=std::min<int>(width,work.right-work.left), h=std::min<int>(height,work.bottom-work.top);
    const int x=std::clamp<int>(placement.bounds.left,work.left,work.right-w);
    const int y=std::clamp<int>(placement.bounds.top,work.top,work.bottom-h);
    check_bool(SetWindowPos(hwnd,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE));
    if (placement.maximized)
        check_bool(SetPropW(hwnd,L"Fenn.NativeStartupMaximized",reinterpret_cast<HANDLE>(1)));
}
}
