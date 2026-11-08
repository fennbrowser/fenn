#pragma once
#include <windows.h>
#include <sddl.h>
#include <bcrypt.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <array>
#include <cstdio>
#include <tlhelp32.h>
#include "shell_core.h"
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"bcrypt.lib")

// Win32 byte adapter only. ShellCore owns record validation, bounds, fairness and link credits.
// The worker never calls a link end or a browser/XAML callback. Wakes contain no pointers.
namespace EnginePipe {
inline void Trace(const char* event,uint32_t serial=0,uint32_t detail=0) {
    if(!GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS",nullptr,0))return;
    fprintf(stderr,"FennProcess pid=%lu event=%s serial=%u detail=%u\n",GetCurrentProcessId(),event,serial,detail);
}
inline constexpr UINT Wake=WM_APP+920;
inline constexpr uint32_t Protocol=2;
struct Hello { uint32_t magic=0x31504e46, version=Protocol, core=SHELL_CORE_ABI_VERSION,
    generation{}, pid=GetCurrentProcessId(), pane=WINUI_PANE_ABI_VERSION, reserved{}; };
inline bool Owns(HWND window, DWORD pid) {
    DWORD actual{}; return window && GetWindowThreadProcessId(window,&actual) && actual==pid;
}
inline std::wstring Sibling(const wchar_t* name, HMODULE module=nullptr) {
    wchar_t path[32768]{};
    const auto n=GetModuleFileNameW(module,path,32768);
    if (!n || n>=32768) return {};
    auto* separator=wcsrchr(path,L'\\');
    if (!separator) return {};
    return std::wstring(path,separator+1)+name;
}
inline HANDLE Server(std::wstring& name, uint32_t& generation) {
    std::array<ULONG,5> random{};
    if (BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(random.data()),sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
        return INVALID_HANDLE_VALUE;
    generation=random[4] ? random[4] : 1;
    wchar_t suffix[80]{};
    swprintf_s(suffix,L"\\\\.\\pipe\\Fenn.%08x%08x%08x%08x",random[0],random[1],random[2],random[3]);
    name=suffix;
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return INVALID_HANDLE_VALUE;
    DWORD size{}; GetTokenInformation(token,TokenGroups,nullptr,0,&size);
    std::vector<BYTE> bytes(size);
    const bool got=GetTokenInformation(token,TokenGroups,bytes.data(),size,&size)!=FALSE;
    CloseHandle(token);
    if (!got) return INVALID_HANDLE_VALUE;
    auto* groups=reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
    LPWSTR sid{};
    for (DWORD i=0;i<groups->GroupCount;++i)
        if ((groups->Groups[i].Attributes&SE_GROUP_LOGON_ID)==SE_GROUP_LOGON_ID)
            ConvertSidToStringSidW(groups->Groups[i].Sid,&sid);
    if (!sid) return INVALID_HANDLE_VALUE;
    const std::wstring dacl=L"D:P(A;;GA;;;"+std::wstring(sid)+L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))
        return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
    HANDLE pipe=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&attributes);
    LocalFree(descriptor);
    return pipe;
}

class Connection {
    HANDLE pipe=INVALID_HANDLE_VALUE, stop=CreateEventW(nullptr,TRUE,FALSE,nullptr),
        send=CreateEventW(nullptr,FALSE,FALSE,nullptr), peer{};
    HWND target{};
    HWND inputTarget{};
    UINT inputWake{};
    bool serverSide{};
    DWORD peerPid{};
    uint32_t generation{};
    std::thread reader;
    std::mutex lock;
    ShellPipeQueue* incoming=shell_pipe_queue_create();
    ShellPipeQueue* outgoing=shell_pipe_queue_create();
    std::atomic<bool> closing{}, authenticated{}, historyReady{};
    ShellRecoveryHistory history{};
    bool historyTaken{};
    std::atomic<DWORD> error{};
    bool Io(bool write, void* buffer, DWORD length, DWORD timeout=INFINITE) {
        const auto deadline=timeout==INFINITE ? 0 : GetTickCount64()+timeout;
        OVERLAPPED operation{}; operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!operation.hEvent) return false;
        auto* cursor=static_cast<BYTE*>(buffer);
        bool ok=true;
        while (length && !closing) {
            if (deadline && GetTickCount64()>=deadline) { ok=false; break; }
            ResetEvent(operation.hEvent);
            DWORD done{};
            BOOL started=write ? WriteFile(pipe,cursor,length,&done,&operation) : ReadFile(pipe,cursor,length,&done,&operation);
            ok=started!=FALSE;
            if (!ok && GetLastError()==ERROR_IO_PENDING) {
                const HANDLE waits[]{stop,peer,operation.hEvent};
                const DWORD remaining=deadline ? DWORD(deadline-GetTickCount64()) : INFINITE;
                const DWORD result=WaitForMultipleObjects(3,waits,FALSE,remaining);
                if (result!=WAIT_OBJECT_0+2) CancelIoEx(pipe,&operation);
                ok=GetOverlappedResult(pipe,&operation,&done,TRUE)!=FALSE && result==WAIT_OBJECT_0+2;
            }
            if (!ok || !done) { ok=false; break; }
            cursor+=done; length-=done;
        }
        CloseHandle(operation.hEvent);
        return ok && !length;
    }
    bool AdoptLauncherChild(DWORD actual) {
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
        if(snapshot==INVALID_HANDLE_VALUE)return false;
        PROCESSENTRY32W entry{sizeof(entry)};bool descendant=false;
        for(BOOL more=Process32FirstW(snapshot,&entry);more;more=Process32NextW(snapshot,&entry))
            if(entry.th32ProcessID==actual){descendant=entry.th32ParentProcessID==peerPid;break;}
        CloseHandle(snapshot);if(!descendant)return false;
        HANDLE child=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE|PROCESS_VM_READ,FALSE,actual);
        if(!child)return false;
        wchar_t parentImage[32768]{},childImage[32768]{};DWORD parentLength=32768,childLength=32768;
        FILETIME parentCreated{},childCreated{},exit{},kernel{},user{};
        const bool valid=QueryFullProcessImageNameW(peer,0,parentImage,&parentLength) &&
            QueryFullProcessImageNameW(child,0,childImage,&childLength) && !_wcsicmp(parentImage,childImage) &&
            GetProcessTimes(peer,&parentCreated,&exit,&kernel,&user) && GetProcessTimes(child,&childCreated,&exit,&kernel,&user) &&
            CompareFileTime(&childCreated,&parentCreated)>=0 && WaitForSingleObject(child,0)==WAIT_TIMEOUT;
        if(!valid){CloseHandle(child);return false;}
        CloseHandle(peer);peer=child;peerPid=actual;return true;
    }
    bool Handshake(bool server, uint32_t version, bool launcherChild) {
        if (server) {
            OVERLAPPED operation{}; operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if (!operation.hEvent) return false;
            bool ok=ConnectNamedPipe(pipe,&operation)!=FALSE;
            if (!ok) {
                const auto e=GetLastError();
                if (e==ERROR_PIPE_CONNECTED) ok=true;
                else if (e==ERROR_IO_PENDING) {
                    const HANDLE waits[]{stop,peer,operation.hEvent};
                    const auto result=WaitForMultipleObjects(3,waits,FALSE,10000);
                    if (result!=WAIT_OBJECT_0+2) CancelIoEx(pipe,&operation);
                    DWORD ignored{};
                    ok=GetOverlappedResult(pipe,&operation,&ignored,TRUE)!=FALSE && result==WAIT_OBJECT_0+2;
                }
            }
            CloseHandle(operation.hEvent);
            if (!ok) return false;
        }
        ULONG actual{};
        const bool got=server ? GetNamedPipeClientProcessId(pipe,&actual)!=FALSE : GetNamedPipeServerProcessId(pipe,&actual)!=FALSE;
        if (!got || (actual!=peerPid && !(server && launcherChild && AdoptLauncherChild(actual)))) { error=ERROR_ACCESS_DENIED; return false; }
        Hello ours{}; ours.generation=generation; ours.version=version;
#ifdef FENN_SHELL_TESTING
        wchar_t helloFault[64]{};
        if(GetEnvironmentVariableW(L"FENN_TEST_PROCESS_FAULT",helloFault,64)<64 && !wcscmp(helloFault,L"wrong-core"))++ours.core;
#endif
        Hello theirs{};
        if (!Io(true,&ours,sizeof(ours),10000) || !Io(false,&theirs,sizeof(theirs),10000)) return false;
        if (theirs.magic!=ours.magic || theirs.version!=Protocol || theirs.core!=ours.core ||
            theirs.generation!=generation || !generation || theirs.pid!=peerPid || theirs.pane!=ours.pane || theirs.reserved) {
            error=ERROR_REVISION_MISMATCH; return false;
        }
        // Exchange charges only after exact peer/ABI/generation authentication.
        ShellRecoveryHistory theirsHistory{},merged{};
        if(!Io(true,&history,sizeof(history),10000) || !Io(false,&theirsHistory,sizeof(theirsHistory),10000))return false;
        if(!shell_recovery_history_merge(history,theirsHistory,GetTickCount64(),&merged)) {
            error=ERROR_INVALID_DATA;return false;
        }
        history=merged;historyReady.store(true,std::memory_order_release);
        authenticated=true; PostMessageW(target,Wake,0,0); return true;
    }
    void Fail() {
        if (!closing.exchange(true)) {
            if (!error) error=ERROR_BROKEN_PIPE;
            authenticated=false;
            // Cancelling this process's I/O alone leaves the peer connected to an idle handle.
            // A server failure must disconnect the generation so the peer observes EOF now.
            if (serverSide) DisconnectNamedPipe(pipe);
            SetEvent(stop); CancelIoEx(pipe,nullptr);
            PostMessageW(target,Wake,0,0);
        }
    }
public:
    Connection()=default;
    Connection(const Connection&)=delete;
    ~Connection() { Stop(); shell_pipe_queue_destroy(incoming); shell_pipe_queue_destroy(outgoing);
        CloseHandle(stop); CloseHandle(send); }
    void Start(HANDLE handle, HANDLE process, DWORD pid, uint32_t gen, HWND wake, bool server,
               uint32_t version=Protocol, HWND input=nullptr, UINT inputMessage=0,bool launcherChild=false,ShellRecoveryHistory budget={}) {
        history=budget;
        pipe=handle; peer=process; peerPid=pid; generation=gen; target=wake;
        serverSide=server;
        inputTarget=input; inputWake=inputMessage;
        reader=std::thread([this,server,version,launcherChild] {
            std::thread writer;
            try {
            if (!Handshake(server,version,launcherChild)) { Fail(); return; }
            writer=std::thread([this] {
                const HANDLE waits[]{stop,peer,send};
                while (!closing) {
                    uint32_t route{}; uint8_t* data{}; size_t size{};
                    { std::lock_guard guard(lock); shell_pipe_queue_pop(outgoing,&route,&data,&size); }
                    if (!data) {
                        if (WaitForMultipleObjects(3,waits,FALSE,INFINITE)!=WAIT_OBJECT_0+2) break;
                        continue;
                    }
                    uint8_t header[12]{};
                    const bool ok=shell_pipe_header(route,size,header) && Io(true,header,12) && Io(true,data,DWORD(size));
                    shell_link_free_frame(data,size);
                    PostMessageW(target,Wake,0,0);
                    if (!ok) break;
                }
                Fail();
            });
            while (!closing) {
                uint8_t header[12]{}; uint32_t route{}; size_t size{};
                if (!Io(false,header,12) || !shell_pipe_decode_header(header,&route,&size)) break;
                std::vector<uint8_t> data(size);
                if (!Io(false,data.data(),DWORD(size))) break;
                bool accepted{};
                { std::lock_guard guard(lock); accepted=shell_pipe_queue_push(incoming,route,data.data(),size); }
                if (!accepted) { error=ERROR_BUFFER_OVERFLOW; break; }
                if (route==1 && inputTarget) PostMessageW(inputTarget,inputWake,0,0);
                else PostMessageW(target,Wake,0,0);
            }
            } catch (...) { error=ERROR_NOT_ENOUGH_MEMORY; }
            Fail();
            if (writer.joinable()) writer.join();
        });
    }
    bool Send(uint32_t route, const uint8_t* data, size_t length) {
        if (closing) return false;
        bool accepted{};
        { std::lock_guard guard(lock); accepted=shell_pipe_queue_push(outgoing,route,data,length); }
        if (accepted) SetEvent(send);
        return accepted;
    }
    bool Take(uint32_t& route, uint8_t*& data, size_t& length) {
        std::lock_guard guard(lock); return shell_pipe_queue_pop(incoming,&route,&data,&length);
    }
    bool TakeRoute(uint32_t route, uint8_t*& data, size_t& length) {
        std::lock_guard guard(lock); return shell_pipe_queue_pop_route(incoming,route,&data,&length);
    }
    bool TakeRecoveryHistory(ShellRecoveryHistory& value) {
        if(historyTaken || !historyReady.load(std::memory_order_acquire))return false;
        value=history;historyTaken=true;return true;
    }
    bool Ready() const { return authenticated; }
    bool PeerExited() const { return peer && WaitForSingleObject(peer,0)==WAIT_OBJECT_0; }
    HANDLE PeerHandle() const { return peer; } // Borrowed on the connection owner thread.
    DWORD PeerPid() const { return peerPid; } // Read after Ready on the owner thread.
    bool Closed() const { return closing; }
    DWORD Error() const { return error; }
    void Break() { Fail(); }
    void Stop() {
        closing=true; SetEvent(stop);
        if (pipe!=INVALID_HANDLE_VALUE) CancelIoEx(pipe,nullptr);
        if (reader.joinable()) reader.join();
        if (pipe!=INVALID_HANDLE_VALUE) { CloseHandle(pipe); pipe=INVALID_HANDLE_VALUE; }
        if (peer) { CloseHandle(peer); peer=nullptr; }
    }
};
// Small in-flight limits ensure the per-route byte adapter cannot accumulate an unbounded inbox.
inline ShellLinkEnd* Link(uint32_t generation, bool fixture=false) {
    // Keep the credited flight window below the bounded OS adapter's 16-record
    // queue. Product bursts use ShellCore's normal outbox/snapshot bounds;
    // fixture-only small outboxes deliberately exercise overflow.
    ShellLinkLimits limits{SHELL_PIPE_FLIGHT_FRAMES,SHELL_PIPE_FLIGHT_BYTES,fixture?32u:0u,fixture?32u*1024u*1024u:0u,fixture?32u:0u};
    return shell_link_create(generation,&limits);
}
inline bool Flush(Connection& pipe, ShellLinkEnd* link, uint32_t route) {
    for (;;) {
        uint8_t* data{}; size_t length{};
        if (shell_link_take(link,false,&data,&length)!=SHELL_STATUS_OK) return false;
        if (!data) return true;
        const bool accepted=pipe.Send(route,data,length);
        shell_link_free_frame(data,length);
        if (!accepted) { pipe.Break(); return false; }
    }
}
inline bool Send(ShellLinkEnd* link, uint32_t serial, uint16_t channel, uint16_t kind,
                 const void* payload=nullptr, size_t length=0, uint64_t revision=0) {
    ShellLinkSend outcome{};
    const auto status=shell_link_send(link,serial,channel,kind,revision,static_cast<const uint8_t*>(payload),length,&outcome);
    const bool sent=status==SHELL_STATUS_OK && (outcome==SHELL_LINK_SEND_QUEUED || outcome==SHELL_LINK_SEND_COALESCED);
    if(!sent){Trace("link-rejected",serial,uint32_t(status));Trace("link-outcome",serial,uint32_t(outcome));}
    return sent;
}
}
