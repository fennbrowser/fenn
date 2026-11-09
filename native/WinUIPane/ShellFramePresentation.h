#pragma once
#include "WindowTitleFrame.h"

// Shared owning-thread frame application. Each host supplies its wakeup callback.
namespace ShellFramePresentation {
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
inline HRESULT WINAPI DestroyHostRetired(HWND hwnd) noexcept {
    const auto result=NativeWindowHost::Destroy(hwnd);
    PaneLink::ShellRetire(hwnd);
    return result;
}
// Frames from Gecko, applied on the shell's thread through the same handlers the posted calls used.
inline void Apply(const ShellFrameView& v, WinUIPaneNotifyFn notify) {
    auto& shell=PaneLink::Native();
    if (v.channel==SHELL_FRAME_LIFETIME && v.kind==SHELL_FRAME_LIFETIME_INTRODUCE) {
        // Only a window of this thread can be introduced. Any other, such as a Gecko window opened
        // with the native host turned off, can have no pane here, and Gecko is told so.
        const auto hwnd=reinterpret_cast<HWND>(static_cast<uintptr_t>(PaneLink::Read<uint64_t>(v.payload)));
        if (!IsWindow(hwnd) || GetWindowThreadProcessId(hwnd,nullptr)!=GetCurrentThreadId() ||
            shell_link_adopt(shell.link,v.window,reinterpret_cast<uintptr_t>(hwnd))!=SHELL_STATUS_OK)
            PaneLink::ShellRefuseSerial(v.window,SHELL_FRAME_REFUSED_STRIP);
        return;
    }
    uintptr_t handle{};
    if (shell_link_handle(shell.link,v.window,&handle)!=SHELL_STATUS_OK) {
        // A startup transfer is always answered, or Gecko's startup would wait for it forever.
        if (v.channel==SHELL_FRAME_STARTUP) {
            const HRESULT refused=E_INVALIDARG;
            PaneLink::ShellSendSerial(v.window,SHELL_FRAME_STARTUP,0,v.revision,&refused,sizeof(refused));
        }
        return;
    }
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
                const auto result=v.kind==0 ? ReportedUpdateTabStrip(hwnd,&rect,visible) : ReportedUpdateAddress(hwnd,&rect,visible);
                // Without a pane there is no request queue to report through, so the link says it.
                if (FAILED(result) && !state().panes.count(hwnd)) {
                    const uint16_t surface=v.kind==0 ? SHELL_FRAME_REFUSED_STRIP : SHELL_FRAME_REFUSED_ADDRESS;
                    PaneLink::ShellSend(hwnd,SHELL_FRAME_WINDOW,3,0,&surface,sizeof(surface));
                }
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
            } else if (v.kind==WindowTitleFrame::Kind) NativeWindowHost::SetTitle(hwnd,text().c_str());
            break;
        case SHELL_FRAME_REQUESTS: CompleteRequests(hwnd,text().c_str()); break;
        case SHELL_FRAME_STARTUP: {
            static thread_local std::vector<wchar_t> reply(262144);
            reply[0]=0;
            const HRESULT result=StartupTransfer(hwnd,text().c_str(),reply.data(),uint32_t(reply.size()));
            const size_t units=SUCCEEDED(result) ? wcsnlen(reply.data(),reply.size()) : 0;
            std::vector<uint8_t> payload(sizeof(HRESULT)+units*sizeof(wchar_t));
            memcpy(payload.data(),&result,sizeof(HRESULT));
            if (units) memcpy(payload.data()+sizeof(HRESULT),reply.data(),units*sizeof(wchar_t));
            PaneLink::ShellSend(hwnd,SHELL_FRAME_STARTUP,0,v.revision,payload.data(),payload.size());
            break;
        }
        case SHELL_FRAME_CLOSE:
            NativeWindowHost::CancelEndpointClose(hwnd,PaneLink::Read<uint64_t>(v.payload),PaneLink::Read<uint64_t>(v.payload+8));
            break;
        case SHELL_FRAME_LIFETIME:
            if (v.kind==SHELL_FRAME_LIFETIME_WAKEUP_ON) SetNotifySink(hwnd,notify,hwnd);
            else if (v.kind==SHELL_FRAME_LIFETIME_WAKEUP_OFF) SetNotifySink(hwnd,nullptr,nullptr);
            else if (v.kind==SHELL_FRAME_LIFETIME_DESTROY) Destroy(hwnd);
            else if (v.kind==SHELL_FRAME_LIFETIME_DESTROY_HOST) DestroyHostRetired(hwnd);
            else if (v.kind==SHELL_FRAME_LIFETIME_BIND) {
                const auto identity=static_cast<uintptr_t>(PaneLink::Read<uint64_t>(v.payload));
                const auto endpoint=reinterpret_cast<HWND>(static_cast<uintptr_t>(PaneLink::Read<uint64_t>(v.payload+8)));
                if (FAILED(NativeWindowHost::BindBrowserEndpoint(hwnd,identity,endpoint))) {
                    const uint16_t refused=SHELL_FRAME_REFUSED_BIND;
                    PaneLink::ShellSend(hwnd,SHELL_FRAME_WINDOW,3,0,&refused,sizeof(refused));
                }
            }
            break;
    }
}
}
