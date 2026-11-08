#include "../WinUIPane/pch.h"
#include "../WinUIPane/Pane.cpp"
#include "../../bridge/WinUIPaneApi.h"
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <appmodel.h>
#include <commctrl.h>
#include <shellapi.h>
#include <memory>
#include <vector>
#include <functional>
#include "../EngineLink/Pipe.h"
#include "../EngineLink/ProcessLaunch.h"
#include "../EngineLink/IconPixels.h"
#include "../EngineLink/WindowPlacement.h"
#include "../EngineLink/Milestone.h"
#include "../WinUIPane/ShellCore.h"
#include "../WinUIPane/PageInput.h"
using namespace winrt;
#ifdef FENN_SHELL_TESTING
// Faults exist only in the separate shell-test output, never in the staged product binary.
static bool ProcessFault(const wchar_t* expected) {
    wchar_t value[64]{};
    return GetEnvironmentVariableW(L"FENN_TEST_PROCESS_FAULT",value,64)<64 && !wcscmp(value,expected);
}
#endif
#include "../WinUIPane/NativeWindowHost.h"
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"runtimeobject.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace {
std::unique_ptr<EnginePipe::Connection> connection;
ShellLinkEnd* links[2]{};
HWND dispatcher{}, shell{}, proxy{};
DWORD enginePid{};
uint32_t generation{}, flags{};
uint64_t pendingReply{};
bool retired{}, bound{}, shown{};
bool lateSent{};
bool product{},recoveryLaunch{};
Application app{nullptr};

namespace Product {
bool RecoverEngine();
bool EngineRecoveryFrame(const ShellFrameView&);
void EngineRecoveryTick();
void RestoreEngineDraft(HWND);
bool EngineDraftEditorReady(HWND);
void RememberEnginePresentation();
void ImportEngineRecoveryHistory();
inline HWND recoveredTarget{};
inline bool engineRestoring{},applyingRecovered{};
inline constexpr UINT EngineCleanupDone=WM_APP+930;
inline std::wstring profile,presentation;
inline bool confirmed{};
inline HWND startup{};
inline std::map<HWND,ShellRecoveryReadiness> readiness;
inline std::map<HWND,uint8_t> deferredAddressFocus;
inline bool watchingRender{};
inline void PlacementChanged(HWND window,int activation=-1) noexcept {
    EnginePlacement::State state{};
    if(EnginePlacement::Read(window,state)) {
        // During WM_ACTIVATE, GetForegroundWindow can still name the old owner.
        if(activation>=0)state.foreground=uint32_t(activation);
        // The foreground shell may authorize its authenticated engine to hand
        // focus to a replacement. Windows revokes this grant on intervening
        // input/another grant; never bypass that decision with attached queues.
        if(state.foreground && GetForegroundWindow()==window && enginePid)AllowSetForegroundWindow(enginePid);
        PaneLink::ShellSend(window,SHELL_FRAME_WINDOW,6,0,&state,sizeof(state));
    }
}
inline std::map<HWND,bool> softwareSurfaces;
inline void Observe(HWND window,uint32_t evidence) {
    auto it=readiness.find(window);
    if(it!=readiness.end() && it->second.phase==0 &&
       shell_recovery_readiness_observe(&it->second,generation,it->second.window,evidence,GetTickCount64())==1) {
        EnginePipe::Trace("recovery-presented",it->second.window);
        PaneLink::ShellSend(window,SHELL_FRAME_LIFETIME,SHELL_FRAME_LIFETIME_PRESENTED,0,nullptr,0);
    }
    if(it!=readiness.end())SetPropW(window,L"Fenn.RecoveryEvidence",reinterpret_cast<HANDLE>(uintptr_t(it->second.observed+1)));
}
inline void SetDeferred(HWND window,ShellRecoveryReadiness& status,bool deferred,uint32_t prerequisites=3) {
    if(bool(status.paused)==deferred)return;
    const auto result=deferred ? shell_recovery_readiness_pause(&status,generation,status.window,prerequisites,GetTickCount64()) :
        shell_recovery_readiness_resume(&status,generation,status.window,GetTickCount64());
    if(result!=0)return;
    if(deferred)SetPropW(window,L"Fenn.RecoveryDeferred",reinterpret_cast<HANDLE>(1));
    else RemovePropW(window,L"Fenn.RecoveryDeferred");
    PaneLink::ShellSend(window,SHELL_FRAME_LIFETIME,deferred?SHELL_FRAME_LIFETIME_RECOVERY_DEFERRED:SHELL_FRAME_LIFETIME_RECOVERY_RESUMED,0,nullptr,0);
}
inline void CheckReadiness() {
    for(auto it=readiness.begin();it!=readiness.end();) {
        const HWND window=it->first;auto& status=it->second;
        if(!IsWindow(window)){deferredAddressFocus.erase(window);softwareSurfaces.erase(window);it=readiness.erase(it);continue;}
        if(status.phase==2){connection->Break();return;}
        if(status.phase==0) {
            const auto* host=NativeWindowHost::Get(window);
            // Minimized WinUI publication can wait for exposure. An authenticated
            // binding with its actual page child is enough to pause; requiring
            // toolbar/address publication first would make that wait circular.
            if(IsIconic(window) && host && host->page && GetParent(host->page)==window)
                SetDeferred(window,status,true,0);
            else if(status.paused || (status.observed&3)==3)
                SetDeferred(window,status,(status.observed&3)==3 && GetForegroundWindow()!=window && !(status.observed&8));
#ifdef FENN_SHELL_TESTING
            if(ProcessFault(L"hold-readiness") && !GetPropW(window,L"Fenn.TestReleaseReadiness")) {
                Observe(window,0);
                if(status.phase==2){connection->Break();return;}
                ++it;continue;
            }
#endif
            const HWND compositor=FindWindowExW(window,nullptr,L"MozillaCompositorWindowClass",nullptr);
            RECT bounds{};
            const auto surface=softwareSurfaces.find(window);
            const HWND target=surface!=softwareSurfaces.end() && surface->second && host ? host->page : compositor;
            if(!status.paused && (status.observed&15)==15 && host && host->page && GetParent(host->page)==window && target &&
               GetClientRect(target,&bounds) && bounds.right>1 && bounds.bottom>1)
                Observe(window,16);
            else Observe(window,0);
            if(status.phase==2) {connection->Break();return;}
        }
        ++it;
    }
    if(readiness.empty())KillTimer(dispatcher,3);
    PaneLink::FlushShell();
}
inline void BeginReadiness(HWND window,uint32_t serial) {
    auto* host=NativeWindowHost::Get(window);
    NativeWindowHost::GateChrome(host);
    SetPropW(window,L"Fenn.RecoveryPending",reinterpret_cast<HANDLE>(1));
    readiness.emplace(window,shell_recovery_readiness_begin(generation,serial,31,GetTickCount64()));
    check_bool(SetTimer(dispatcher,3,50,nullptr));
    if(!watchingRender) {
        watchingRender=true;
        Media::CompositionTarget::Rendering([](auto const&,auto const&) {
            for(auto& [window,status]:readiness)if(status.phase==0 && (status.observed&3)==3)Observe(window,4);
        });
    }
}

inline bool PipeSend(const uint8_t* data,size_t length) noexcept {return connection && connection->Send(0,data,length);}
inline void Shape(HWND window,const RECT& client,const std::vector<RECT>& chrome,const std::vector<RECT>& exclusions) noexcept {
    try {
        uint32_t serial{};ShellLinkSend outcome{};const uint64_t handle=reinterpret_cast<uintptr_t>(window);
        if(shell_link_serial(links[1],handle,false,&serial)!=SHELL_STATUS_OK) {
            if(shell_link_serial(links[1],handle,true,&serial)!=SHELL_STATUS_OK)return;
            shell_link_send(links[1],serial,SHELL_FRAME_LIFETIME,SHELL_FRAME_LIFETIME_INTRODUCE,0,
                reinterpret_cast<const uint8_t*>(&handle),8,&outcome);
        }
        const uint32_t count=uint32_t((std::min)(chrome.size(),size_t(2)));
        const size_t clips=(std::min)(exclusions.size(),size_t(64));
        std::vector<uint8_t> data(4+16*(1+count+clips));memcpy(data.data(),&count,4);memcpy(data.data()+4,&client,16);
        if(count)memcpy(data.data()+20,chrome.data(),count*16);
        if(clips)memcpy(data.data()+20+count*16,exclusions.data(),clips*16);
        EnginePipe::Send(links[1],serial,SHELL_FRAME_PAGE,0,data.data(),data.size());
        EnginePipe::Flush(*connection,links[1],1);
    }catch(...){connection->Break();}
}
inline void RetireShape(HWND window) noexcept {
    uint32_t serial{};
    if(shell_link_serial(links[1],reinterpret_cast<uintptr_t>(window),false,&serial)==SHELL_STATUS_OK)
        shell_link_retire(links[1],serial);
    if(engineRestoring)PostMessageW(dispatcher,EnginePipe::Wake,0,0);
}
inline void InstallKeyRouting(HWND window) {
    auto* host=NativeWindowHost::Get(window);
    if(!host || GetPropW(window,L"Fenn.ProcessKeyRouting"))return;
    host->root.PreviewKeyDown([window](Windows::Foundation::IInspectable const& sender,Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) noexcept {
        try {
            auto* current=NativeWindowHost::Get(window);
            auto pane=state().panes.find(window);
            if(!current || !SameObject(sender,current->root) || pane==state().panes.end() || args.Handled())return;
            const auto status=args.KeyStatus();
            const uint32_t bits=status.RepeatCount | (status.ScanCode<<16) | (uint32_t(status.IsExtendedKey)<<24) |
                (uint32_t(status.IsMenuKeyDown)<<29) | (uint32_t(status.WasKeyDown)<<30) | (uint32_t(status.IsKeyReleased)<<31);
            const MSG message{current->bridge,UINT(status.IsMenuKeyDown?WM_SYSKEYDOWN:WM_KEYDOWN),WPARAM(args.Key()),LPARAM(bits),DWORD(GetMessageTime()),{}};
            // Application owns this message loop, so the in-process Translate
            // hook cannot dismiss customization during a native toolbar drag.
            if(args.Key()==Windows::System::VirtualKey::Escape && pane->second->address &&
               pane->second->address->navigation &&
               pane->second->address->navigation->customization.DismissOnEscape()) {
                args.Handled(true);return;
            }
            if((pane->second->startupKey && pane->second->startupKey(message)) || RouteBrowserShortcut(*pane->second,message))args.Handled(true);
        }catch(...){}
    });
    check_bool(SetPropW(window,L"Fenn.ProcessKeyRouting",reinterpret_cast<HANDLE>(1)));
}
inline HWND Startup() {
    if(startup && IsWindow(startup))return startup;
    const auto placement=NativeStartupPlacement::Read(profile);
    check_hresult(NativeWindowHost::CreateStartup(0,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,placement.bounds,&startup));
    NativeStartupPlacement::Apply(startup,placement);
    SetPropW(startup,L"Fenn.NativeStartupOrigin",reinterpret_cast<HANDLE>(2));
    // Called only after Firefox confirms routing, never on speculative process creation.
    check_hresult(ShowInteractiveStartup(startup,profile.c_str(),presentation.empty()?nullptr:presentation.c_str()));
    InstallKeyRouting(startup);
    EnginePipe::Trace("show-startup");
    return startup;
}
inline void Apply(const ShellFrameView& view) {
    if(!applyingRecovered && EngineRecoveryFrame(view))return;
    if(view.channel==SHELL_FRAME_LIFETIME && view.kind==SHELL_FRAME_LIFETIME_PROFILE && !view.window) {
        const auto value=PaneLink::Text(view.payload,view.length);const auto split=value.find(L'\0');
        profile=value.substr(0,split);presentation=value.substr(split+1);RememberEnginePresentation();return;
    }
    if(view.channel==SHELL_FRAME_LIFETIME && view.kind==SHELL_FRAME_LIFETIME_CONFIRM && !view.window) {
        confirmed=true;if(!recoveryLaunch)Startup();return;
    }
    if(view.channel==SHELL_FRAME_LIFETIME && view.kind==SHELL_FRAME_LIFETIME_CREATE) {
#ifdef FENN_SHELL_TESTING
        static uint32_t creates=0;++creates;
        if(ProcessFault(L"refuse-bind") || (ProcessFault(L"refuse-second-bind") && creates==2)) {
            const uint16_t refused=SHELL_FRAME_REFUSED_BIND;
            PaneLink::ShellSendSerial(view.window,SHELL_FRAME_WINDOW,3,0,&refused,sizeof(refused));return;
        }
#endif
        const auto data=PaneLink::Read<EnginePlacement::Create>(view.payload);
        const HWND endpoint=reinterpret_cast<HWND>(uintptr_t(data.proxy));
        wchar_t role[80]{};GetClassNameW(endpoint,role,80);
        if(!confirmed){connection->Break();return;}
        if(!EnginePipe::Owns(endpoint,enginePid) || wcscmp(role,L"MozillaWindowClass")) {
            // A legitimate create can outlive its local proxy when Firefox closes before bind.
            // Refuse this serial; other windows on the authenticated connection stay alive.
            const uint16_t refused=SHELL_FRAME_REFUSED_BIND;
            PaneLink::ShellSendSerial(view.window,SHELL_FRAME_WINDOW,3,0,&refused,sizeof(refused));return;
        }
        HWND window{};
        const bool reused=recoveredTarget!=nullptr;
        if(reused){window=recoveredTarget;recoveredTarget=nullptr;}
        else if(startup && IsWindow(startup) && !NativeWindowHost::Get(startup)->endpoint) {window=startup;startup=nullptr;}
        else {
            check_hresult(NativeWindowHost::CreateStartup(0,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,data.bounds,&window));
            // Later normal/private windows need the same complete initial chrome as
            // the first window. Prepare while hidden; Firefox still owns show,
            // placement and activation. Recovery retains its separate presenter.
            if(!(data.flags&2)) {
                const auto prepared=PrepareInteractiveStartup(window,profile.c_str(),presentation.empty()?nullptr:presentation.c_str());
                check_hresult(prepared);
                if(prepared==S_FALSE)check_bool(SetPropW(window,L"Fenn.PendingStartupPreparation",reinterpret_cast<HANDLE>(1)));
            }
        }
        check_bool(shell_link_adopt(links[0],view.window,reinterpret_cast<uintptr_t>(window))==SHELL_STATUS_OK);
#ifdef FENN_SHELL_TESTING
        if((data.flags&2) && (ProcessFault(L"hold-recovery-bind") || (ProcessFault(L"hold-second-recovery-bind") && creates==2)) && !GetPropW(window,L"Fenn.TestReleaseBind")) {
            check_bool(SetPropW(window,L"Fenn.TestPendingBind",endpoint));
            return;
        }
#endif
        check_hresult(IndependentHost::Bind(window,data.identity,endpoint));
        InstallKeyRouting(window);
        if(data.flags&2){
            BeginReadiness(window,view.window);
            if(!reused && !EnginePlacement::Restore(window,data.placement))ShowWindow(window,SW_SHOWNOACTIVATE);
        }
        EnginePipe::Trace("bound",view.window);
#ifdef FENN_SHELL_TESTING
        if(ProcessFault(L"delay-bind"))Sleep(12000);
#endif
        uint64_t handle=reinterpret_cast<uintptr_t>(window);
#ifdef FENN_SHELL_TESTING
        if((data.flags&2) && ProcessFault(L"foreign-recovery-window"))handle=reinterpret_cast<uintptr_t>(GetDesktopWindow());
#endif
        PaneLink::ShellSendSerial(view.window,SHELL_FRAME_LIFETIME,SHELL_FRAME_LIFETIME_BOUND,0,&handle,8);
        PlacementChanged(window);
        return;
    }
    uintptr_t handle{};if(shell_link_handle(links[0],view.window,&handle)!=SHELL_STATUS_OK)return;
    const HWND window=reinterpret_cast<HWND>(handle);
    // Firefox can create a later proxy with provisional, too-small bounds.
    // Retry after its placement request, before processing show or publication.
    if(GetPropW(window,L"Fenn.PendingStartupPreparation")) {
        const auto prepared=PrepareInteractiveStartup(window,profile.c_str(),presentation.empty()?nullptr:presentation.c_str());
        check_hresult(prepared);
        if(prepared==S_OK)RemovePropW(window,L"Fenn.PendingStartupPreparation");
    }
    if(view.channel==SHELL_FRAME_LIFETIME && view.kind==SHELL_FRAME_LIFETIME_RECOVERY_COMMIT) {
        auto found=readiness.find(window);
        if(found==readiness.end() || shell_recovery_readiness_observe(&found->second,generation,view.window,0,GetTickCount64())!=1){connection->Break();return;}
        NativeWindowHost::Get(window)->window.Content().as<Controls::ContentControl>().IsEnabled(true);
        RemovePropW(window,L"Fenn.RecoveryPending");
        RemovePropW(window,L"Fenn.RecoveryDeferred");
        SetPropW(window,L"Fenn.RecoveryReady",reinterpret_cast<HANDLE>(1));
        if(auto focus=deferredAddressFocus.find(window);focus!=deferredAddressFocus.end()) {
            if(GetForegroundWindow()==window)ReportedFocusAddress(window,focus->second!=0);
            deferredAddressFocus.erase(focus);
        }
        RestoreEngineDraft(window);
        readiness.erase(found);softwareSurfaces.erase(window);
        PaneLink::ShellSend(window,SHELL_FRAME_LIFETIME,SHELL_FRAME_LIFETIME_RECOVERY_COMMIT,0,nullptr,0);return;
    }
    if(view.channel==SHELL_FRAME_LIFETIME && view.kind==SHELL_FRAME_LIFETIME_DESTROY_HOST){readiness.erase(window);deferredAddressFocus.erase(window);softwareSurfaces.erase(window);}
    if(view.channel==SHELL_FRAME_TAB_MENU) {
        const auto value=NativeTabMenu::Object::Parse(PaneLink::Text(view.payload,view.length));
        if(value.HasKey(L"recoveryWindow")) {
            check_bool(view.length<=512 && value.Size()==3 && value.GetNamedNumber(L"version")==2 && value.GetNamedBoolean(L"publishOnly"));
            const auto identity=value.GetNamedObject(L"recoveryWindow");
            const auto revision=identity.GetNamedNumber(L"revision"),privacy=identity.GetNamedNumber(L"privacy");
            const auto key=identity.GetNamedString(L"key");
            check_bool((identity.Size()==3 || (engineRestoring && identity.Size()==4)) && revision>=1 && revision<=9007199254740991.0 && floor(revision)==revision &&
                (privacy==1 || privacy==2));
            auto* host=NativeWindowHost::Get(window);check_bool(host!=nullptr);
            ShellRecoveryWindowIdentity accepted{};
            check_bool(shell_core_recovery_identity(host->core.get(),host->state.generation,uint64_t(revision),uint32_t(privacy),
                reinterpret_cast<const uint16_t*>(key.c_str()),key.size(),&accepted)==SHELL_STATUS_OK);
            SetPropW(window,L"Fenn.RecoveryPrivacy",reinterpret_cast<HANDLE>(uintptr_t(accepted.privacy)));
            const bool keyed=std::any_of(std::begin(accepted.key),std::end(accepted.key),[](uint8_t byte){return byte!=0;});
            if(keyed)SetPropW(window,L"Fenn.RecoverySessionReady",reinterpret_cast<HANDLE>(1));
            else RemovePropW(window,L"Fenn.RecoverySessionReady");
            return;
        }
        if(recoveryLaunch && value.HasKey(L"recoveryPainted")) {
            check_bool(value.Size()==4 && value.GetNamedNumber(L"version")==2 &&
                value.GetNamedBoolean(L"publishOnly") && value.GetNamedBoolean(L"recoveryPainted"));
            if(auto it=readiness.find(window);it!=readiness.end()) {
                SetDeferred(window,it->second,false);
                softwareSurfaces[window]=value.GetNamedBoolean(L"software");Observe(window,8);
            }
            return;
        }
    }
    if(readiness.contains(window) && view.channel==SHELL_FRAME_TABS && view.kind==0) {
        std::vector<ShellFrameTab> rows(10000);
        size_t count{};uint32_t selected{},revision{};
        if(shell_frame_tabs_decode(view.payload,view.length,rows.data(),rows.size(),&count,&selected,&revision)!=SHELL_STATUS_OK){connection->Break();return;}
        std::vector<std::wstring> titles;titles.reserve(count);
        std::vector<WinUITab> tabs(count);
        for(size_t i=0;i<count;++i) {
            titles.emplace_back(rows[i].title_length,L'\0');
            if(rows[i].title_length)memcpy(titles.back().data(),rows[i].title,rows[i].title_length*sizeof(wchar_t));
            tabs[i]={rows[i].id,titles.back().c_str(),rows[i].commands};
        }
        if(SUCCEEDED(ReportedSyncTabs(window,tabs.data(),uint32_t(count),selected,revision)))Observe(window,1);
        return;
    }
    if(readiness.contains(window) && view.channel==SHELL_FRAME_FOCUS && view.kind==1) {
        deferredAddressFocus[window]=view.payload[0];return;
    }
    if(readiness.contains(window) && view.channel==SHELL_FRAME_ADDRESS) {
        if(SUCCEEDED(PostedSyncAddress(window,PaneLink::Text(view.payload,view.length).c_str())) && EngineDraftEditorReady(window))Observe(window,2);
        return;
    }
    if(view.channel==SHELL_FRAME_LIFETIME && view.kind==SHELL_FRAME_LIFETIME_PAGE_READY) {
        const auto page=reinterpret_cast<HWND>(uintptr_t(PaneLink::Read<uint64_t>(view.payload)));
        if(!EnginePipe::Owns(page,enginePid) || GetParent(page)!=window){connection->Break();return;}
        NativeWindowHost::AdoptPage(window,page);
        RECT client{};GetClientRect(window,&client);const auto* host=NativeWindowHost::Get(window);
        Shape(window,client,host->chrome,host->exclusions);
        return;
    }
    if(view.channel==SHELL_FRAME_WINDOW && view.kind==7) {
        // An OS gesture is never deferred into a recovered window or replayed.
        auto report=[&](HRESULT hr,uint32_t phase=0) {
            uint8_t result[24]{};memcpy(result,&hr,4);memcpy(result+4,&phase,4);
            auto pane=state().panes.find(window);
            if(pane!=state().panes.end() && pane->second->address) {
                const uint64_t session=_wcstoui64(pane->second->address->session.c_str(),nullptr,10);
                const uint64_t document=_wcstoui64(pane->second->address->shareDocument.c_str(),nullptr,10);
                memcpy(result+8,&session,8);memcpy(result+16,&document,8);
            }
            PaneLink::ShellSend(window,SHELL_FRAME_WINDOW,8,view.revision,result,sizeof(result));
        };
        if(readiness.contains(window)){report(E_PENDING);return;}
        if(GetForegroundWindow()!=window){report(E_ACCESSDENIED);return;}
        auto found=state().panes.find(window);if(found==state().panes.end() || !found->second->address){report(RO_E_CLOSED);return;}
        auto bytes=std::make_shared<std::vector<uint8_t>>(view.payload,view.payload+view.length);
        auto current=[window,bytes,epoch=generation]() {
            if(epoch!=generation || !connection || connection->Closed() || !connection->Ready() || readiness.contains(window))return false;
            auto pane=state().panes.find(window);
            if(pane==state().panes.end() || !pane->second->address)return false;
            auto& address=*pane->second->address;
            if(address.root.Visibility()!=Visibility::Visible || (!address.navigation || !address.navigation->visible))return false;
            return shell_frame_share_current(bytes->data(),bytes->size(),_wcstoui64(address.session.c_str(),nullptr,10),
                _wcstoui64(address.shareDocument.c_str(),nullptr,10),GetTickCount64());
        };
        if(!current()){report(E_NOT_VALID_STATE);return;}
        const size_t title=PaneLink::Read<uint32_t>(view.payload+24),url=PaneLink::Read<uint32_t>(view.payload+28);
        const auto text=reinterpret_cast<const wchar_t*>(view.payload+32);
        report(found->second->windowsShare.Open(window,hstring(text,uint32_t(title)),hstring(text+title,uint32_t(url)),current),1);
        return;
    }
    if(view.channel==SHELL_FRAME_WINDOW && (view.kind==4 || view.kind==5)) {
        const HICON icon=EngineIcon::Decode(view.payload);
        const WinUIPaneWindowRequest request{WinUIPaneWindowIcon,view.kind==4?ICON_BIG:ICON_SMALL,{},reinterpret_cast<LONG_PTR>(icon),nullptr};
        NativeWindowHost::ApplyWindowRequest(window,request,false);
        const uint64_t endpoint=reinterpret_cast<uintptr_t>(NativeWindowHost::Get(window)->endpoint);
        PaneLink::ShellSend(window,SHELL_FRAME_WINDOW,2,view.revision,&endpoint,sizeof(endpoint));return;
    }
    IndependentHost::ShellApply(view);
}
#ifdef FENN_SHELL_TESTING
// Hold the authenticated recovery stream in its existing bounded transport
// queue. Release through the ordinary dispatcher; do not synthesize a bind or
// bypass reconciliation/retirement when a window closes while delivery waits.
inline bool TestHoldBeforeBind() {
    static bool released=false;
    static winrt::handle release;
    static ULONGLONG deadline{};
    wchar_t phase[64]{},name[200]{};
    if(released || (!recoveryLaunch && !engineRestoring) || !connection->Ready() || connection->Closed() ||
       !GetEnvironmentVariableW(L"FENN_TEST_LAUNCH_PHASE",phase,64) || wcscmp(phase,L"before-bind"))return false;
    if(!release) {
        const auto length=GetEnvironmentVariableW(L"FENN_TEST_LAUNCH_GATE",name,200);
        check_bool(length && length<200);
        release.attach(OpenEventW(SYNCHRONIZE,FALSE,name));
        winrt::handle reached{OpenEventW(EVENT_MODIFY_STATE,FALSE,(std::wstring(name)+L".reached").c_str())};
        check_bool(release && reached);check_bool(SetEvent(reached.get()));
        deadline=GetTickCount64()+10000;SetTimer(dispatcher,6,10,nullptr);
    }
    if(WaitForSingleObject(release.get(),0)==WAIT_OBJECT_0) {
        released=true;KillTimer(dispatcher,6);release.close();return false;
    }
    if(GetTickCount64()>=deadline) {KillTimer(dispatcher,6);connection->Break();return false;}
    return true;
}
#endif
inline void Pump() {
    if(!connection)return;
#ifdef FENN_SHELL_TESTING
    if(TestHoldBeforeBind())return;
#endif
    ImportEngineRecoveryHistory();
    if(engineRestoring && connection->Ready()) {
        enginePid=connection->PeerPid();NativeWindowHost::RemoteEnginePid=enginePid;
        NativePackageUpdates::restartProcess=connection->PeerHandle();
    }
    uint32_t route{};uint8_t* bytes{};size_t length{};
    while(connection->Take(route,bytes,length)) {
        ShellFrameView view{};const auto status=shell_link_receive(links[route],bytes,length,&view);
        try {if(status==SHELL_STATUS_OK && view.delivered && route==0)Apply(view);
            else if(status!=SHELL_STATUS_OK && status!=SHELL_STATUS_REJECTED)connection->Break();}
        catch(...){connection->Break();}
        shell_link_free_frame(bytes,length);
    }
    if(connection->Closed()) {
        EnginePipe::Trace("peer-closed");
        if(RecoverEngine())return;
        std::vector<HWND> windows;for(const auto& item:state().panes)windows.push_back(item.first);
        if(startup)windows.push_back(startup);
        for(HWND window:windows)if(IsWindow(window))NativeWindowHost::Destroy(window);
        app.Exit();return;
    }
    PaneLink::FlushShell();EnginePipe::Flush(*connection,links[1],1);
}
}
#include "EngineRecovery.h"
#include "EngineRecoverySurface.h"

void Flush() { for (uint32_t route=0;route<2;++route) EnginePipe::Flush(*connection,links[route],route); }
void Exit() {
    if (shell) { NativeWindowHost::CloseHostWindow(shell); shell=nullptr; }
    if (app) app.Exit();
}
void Shape(HWND window,const RECT& client,const std::vector<RECT>& chrome,const std::vector<RECT>& exclusions) noexcept {
    if (!bound || retired || window!=shell) return;
    try {
        const uint32_t count=uint32_t((std::min)(chrome.size(),size_t(2)));
        const size_t excluded=(std::min)(exclusions.size(),size_t(64));
        std::vector<uint8_t> bytes(4+sizeof(RECT)*(1+count+excluded));
        memcpy(bytes.data(),&count,4); memcpy(bytes.data()+4,&client,sizeof(RECT));
        if (count) memcpy(bytes.data()+4+sizeof(RECT),chrome.data(),sizeof(RECT)*count);
        if (excluded) memcpy(bytes.data()+4+sizeof(RECT)*(1+count),exclusions.data(),sizeof(RECT)*excluded);
        EnginePipe::Send(links[1],1,SHELL_FRAME_PAGE,0,bytes.data(),bytes.size()); Flush();
    } catch (...) { connection->Break(); }
}
void Reply() {
    if (pendingReply) EnginePipe::Send(links[0],1,SHELL_FRAME_WINDOW,2,&pendingReply,8);
    pendingReply=0; Flush();
    if (retired) lateSent=true;
}
LRESULT CALLBACK WindowProcedure(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if (message==FennSplitStallMessage() && w<=2000) {
        SetPropW(window,L"Fenn.SplitFixtureStalled",reinterpret_cast<HANDLE>(1));
        Sleep(DWORD(w)); RemovePropW(window,L"Fenn.SplitFixtureStalled"); return 0;
    }
    if (message==WM_SIZE && shown && !retired) {
        RECT client{}; GetClientRect(window,&client); Shape(window,client,{},{});
    }
    if (message==WM_CLOSE) {
        if (!bound) { retired=true; Exit(); }
        else { const uint64_t close[]{generation,1}; EnginePipe::Send(links[0],1,SHELL_FRAME_CLOSE,0,close,sizeof(close)); Flush(); }
        return 0;
    }
    return DefSubclassProc(window,message,w,l);
}
void Bind(const ShellFrameView& view) {
    if (retired || bound || view.window!=1 || shell) { connection->Break(); return; }
    uint64_t payload[2]{}; memcpy(payload,view.payload,sizeof(payload));
    proxy=reinterpret_cast<HWND>(static_cast<uintptr_t>(payload[1]));
    wchar_t role[80]{}; GetClassNameW(proxy,role,80);
    if (payload[0]!=1 || !EnginePipe::Owns(proxy,enginePid) || wcscmp(role,L"MozillaWindowClass")) {
        connection->Break(); return;
    }
    Window window;
    Controls::Canvas root;
    window.Content(root);
    // Live system backdrop and stock system caption controls; no cloak or replacement opening effect.
    window.SystemBackdrop(Media::MicaBackdrop());
    shell=Microsoft::UI::GetWindowFromWindowId(window.AppWindow().Id());
    const HWND bridge=FindWindowExW(shell,nullptr,L"Microsoft.UI.Content.DesktopChildSiteBridge",nullptr);
    check_hresult(NativeWindowHost::Attach(shell,window,root,bridge));
    RECT bounds{}; GetWindowRect(proxy,&bounds);
    check_bool(SetWindowPos(shell,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
        SWP_NOZORDER|SWP_NOACTIVATE));
    check_bool(SetWindowSubclass(shell,WindowProcedure,2,0));
    check_bool(shell_link_adopt(links[0],1,reinterpret_cast<uintptr_t>(shell))==SHELL_STATUS_OK);
    bound=true;
    pendingReply=reinterpret_cast<uintptr_t>(shell);
    if (flags&1) SetTimer(dispatcher,2,1200,nullptr); else Reply();
}
void Pump() {
    uint32_t route{}; uint8_t* bytes{}; size_t length{};
    while (connection->Take(route,bytes,length)) {
        ShellFrameView view{};
        const auto status=shell_link_receive(links[route],bytes,length,&view);
        if (status==SHELL_STATUS_OK && view.delivered && route==0) {
            if (view.channel==SHELL_FRAME_LIFETIME && view.kind==SHELL_FRAME_LIFETIME_BIND) Bind(view);
            else if (view.channel==SHELL_FRAME_LIFETIME && view.kind==SHELL_FRAME_LIFETIME_DESTROY_HOST) {
                retired=true;
                if (shell) { NativeWindowHost::CloseHostWindow(shell); shell=nullptr; }
                // A fault fixture deliberately sends the reply after the engine has retired this serial.
                if (!(flags&1) || !pendingReply) { shell_link_free_frame(bytes,length); Exit(); return; }
            } else if (view.channel==SHELL_FRAME_LIFETIME && view.kind==SHELL_FRAME_LIFETIME_WAKEUP_ON && !retired && !shown) {
                shown=true;
                const uint64_t handle=reinterpret_cast<uintptr_t>(shell);
                shell_link_adopt(links[1],1,handle);
                EnginePipe::Send(links[1],1,SHELL_FRAME_LIFETIME,SHELL_FRAME_LIFETIME_INTRODUCE,&handle,8);
                // Preserve the current window's page shape contract, applied only by the engine owner.
                NativeWindowHost::SendPageShape=Shape;
                RECT client{}; GetClientRect(shell,&client);
                Shape(shell,client,{},{});
                ShowWindow(shell,SW_SHOWNORMAL);
            }
        } else if (status!=SHELL_STATUS_OK && status!=SHELL_STATUS_REJECTED) connection->Break();
        shell_link_free_frame(bytes,length);
    }
    if (connection->Closed()) { retired=true; Exit(); return; }
    Flush();
    if (retired && lateSent) {
        ShellLinkCounts counts{}; shell_link_counts(links[0],&counts);
        if (!counts.in_flight && !counts.outbox) Exit();
    }
}
LRESULT CALLBACK Dispatch(HWND window,UINT message,WPARAM w,LPARAM l) {
    try {
        if(product) {
#ifdef FENN_SHELL_TESTING
            if(message==WM_TIMER && w==6){Product::Pump();return 0;}
#endif
            if(message==Product::EngineCleanupDone || message==EnginePipe::ProcessLaunch::Completed || (message==WM_TIMER && w==4)) {Product::EngineRecoveryTick();return 0;}
            if(message==EnginePipe::Wake){Product::Pump();return 0;}
            if(message==WM_TIMER && w==3){Product::CheckReadiness();return 0;}
            if(message==WM_TIMER && w==5){KillTimer(window,5);Product::RefreshEngineTerminal();return 0;}
            if(message==IndependentHost::InvokeMessage)return IndependentHost::Dispatch(window,message,w,l);
            return DefWindowProcW(window,message,w,l);
        }
        if (message==EnginePipe::Wake) { Pump(); return 0; }
        if (message==WM_TIMER && w==2) { KillTimer(window,2); Reply(); return 0; }
        if (message==WM_TIMER && w==1 && !shown) { connection->Break(); Exit(); return 0; }
    } catch (...) { if(connection)connection->Break(); Exit(); }
    return DefWindowProcW(window,message,w,l);
}
struct ShellApp : ApplicationT<ShellApp,Markup::IXamlMetadataProvider> {
    XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata;
    Markup::IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const& name) { return metadata.GetXamlType(name); }
    Markup::IXamlType GetXamlType(hstring const& name) { return metadata.GetXamlType(name); }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return metadata.GetXmlnsDefinitions(); }
    void OnLaunched(LaunchActivatedEventArgs const&) {
        Resources().MergedDictionaries().Append(Controls::XamlControlsResources());
    }
};
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        int count{}; auto** arguments=CommandLineToArgvW(GetCommandLineW(),&count);
        std::wstring name;
        for (int i=1;i+1<count;i+=2) {
            if (!wcscmp(arguments[i],L"--pipe")) name=arguments[i+1];
            else if (!wcscmp(arguments[i],L"--engine")) enginePid=wcstoul(arguments[i+1],nullptr,10);
            else if (!wcscmp(arguments[i],L"--generation")) generation=wcstoul(arguments[i+1],nullptr,10);
            else if (!wcscmp(arguments[i],L"--flags")) flags=wcstoul(arguments[i+1],nullptr,10);
            else if (!wcscmp(arguments[i],L"--recovery")) recoveryLaunch=wcstoul(arguments[i+1],nullptr,10)==1;
            else if (!wcscmp(arguments[i],L"--product")) product=wcstoul(arguments[i+1],nullptr,10)==1;
            else if (wcscmp(arguments[i],L"--profile")) return 2;
        }
        LocalFree(arguments);
#ifndef FENN_SHELL_TESTING
        if(!product || flags)return 2;
#endif
        if (!enginePid || !generation || name.rfind(L"\\\\.\\pipe\\Fenn.",0)!=0) return 2;
        HANDLE parent=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ,FALSE,enginePid);
        if (!parent) return 3;
        HANDLE pipe=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if (pipe==INVALID_HANDLE_VALUE) { CloseHandle(parent); return 4; }
        UINT32 size{};
        if (GetCurrentPackageFullName(&size,nullptr)==APPMODEL_ERROR_NO_PACKAGE) {
            auto bootstrapPath=EnginePipe::Sibling(WINUI_PANE_RUNTIME_BOOTSTRAP_DLL);
#ifdef FENN_SHELL_TESTING
            if(recoveryLaunch && ProcessFault(L"missing-replacement-dependency"))
                bootstrapPath=EnginePipe::Sibling(L"fenn-missing-bootstrap.dll");
#endif
            HMODULE bootstrap=LoadLibraryExW(bootstrapPath.c_str(),nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!bootstrap) return 5;
            using Init=HRESULT(WINAPI*)(UINT32,PCWSTR,PACKAGE_VERSION,UINT32);
            auto initialize=reinterpret_cast<Init>(GetProcAddress(bootstrap,"MddBootstrapInitialize2"));
            PACKAGE_VERSION minimum{}; minimum.Version=WINUI_PANE_RUNTIME_MIN_VERSION;
            if (!initialize || FAILED(initialize(WINUI_PANE_RUNTIME_MAJOR_MINOR,L"",minimum,0x10))) return 6;
        }
        init_apartment(apartment_type::single_threaded);
        Application::Start([&](auto&&) {
            app=make<ShellApp>();
            app.DispatcherShutdownMode(DispatcherShutdownMode::OnExplicitShutdown);
            WNDCLASSW wc{}; wc.lpfnWndProc=Dispatch; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"Fenn.SplitShellDispatcher.v1";
            RegisterClassW(&wc);
            dispatcher=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,nullptr);
            links[0]=EnginePipe::Link(generation,!product); links[1]=EnginePipe::Link(generation,!product);
            if(product) {
                NativePackageUpdates::restartProcess=parent;
                PaneLink::Generation=generation;PaneLink::Native().link=links[0];PaneLink::ShellTransport=Product::PipeSend;
                NativeWindowHost::PlacementChanged=Product::PlacementChanged;
                IndependentHost::State().dispatcher=dispatcher;IndependentHost::State().owner=GetCurrentThreadId();
                NativeWindowHost::RemoteEnginePid=enginePid;
                NativeWindowHost::RequestPage=[](HWND,HWND,COLORREF)noexcept{return true;};
                NativeWindowHost::SendPageShape=Product::Shape;NativeWindowHost::RetirePageShape=Product::RetireShape;
            }
            connection=std::make_unique<EnginePipe::Connection>();
            uint32_t protocol=EnginePipe::Protocol+((flags&2)?1:0);
            DWORD expectedEngine=enginePid;
#ifdef FENN_SHELL_TESTING
            if(product && ProcessFault(L"wrong-version"))protocol=EnginePipe::Protocol+1;
            if(product && ProcessFault(L"wrong-peer"))++expectedEngine;
#endif
            connection->Start(pipe,parent,expectedEngine,generation,dispatcher,false,protocol);
            if(!product)SetTimer(dispatcher,1,10000,nullptr);
        });
        NativePackageUpdates::restartProcess=nullptr;
        if(product)Product::StopEngineRecovery();
        if(connection)connection->Stop();
        for (auto* link:links) shell_link_destroy(link);
        if (dispatcher) DestroyWindow(dispatcher);
        return 0;
    } catch (...) { return 7; }
}
