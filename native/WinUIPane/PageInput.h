#pragma once
#include <msctf.h>
#include <olectl.h>
#include <inputscope.h>
#include <textstor.h>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "shell_core.h"
#pragma comment(lib,"uuid.lib")

// TSF objects stay on the native STA. The Rust projection never calls Gecko;
// browser composition events are copied through the owning endpoint's queue.
namespace NativePageInput {
// GUID_PROP_INPUTSCOPE from the Windows SDK InputScope.h.
inline constexpr GUID InputScopeAttribute{0x1713dd5a,0x68e7,0x4a5b,{0x9a,0xf6,0x59,0x2a,0x59,0x5c,0x77,0x8d}};
inline bool Diagnostics(){static const bool enabled=GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS",nullptr,0)>0;return enabled;}
inline UINT WakeMessage() { static UINT id=RegisterWindowMessageW(L"Fenn.NativeWindowHost.PageInput.v1");return id; }
inline UINT LockMessage() { static UINT id=RegisterWindowMessageW(L"Fenn.NativeWindowHost.PageInputLock.v1");return id; }
// On the engine's input thread (M5c) Gecko never calls into the store. The store copies its
// events into this mailbox, which Gecko drains under its lock, and Gecko's other calls are posted
// jobs. Events that arrive while Gecko still has a batch to drain stay in the Rust queue, where
// composition updates coalesce as before.
struct Mailbox {
    struct Item { WinUIPageInputEvent event; std::vector<WCHAR> text; };
    std::mutex lock;
    std::deque<Item> items;
    std::atomic<bool> refill{};
    HWND page{};
};
// Keyed by the shell window, which is what Gecko names.
inline std::mutex& MailboxesLock() { static auto* lock=new std::mutex; return *lock; }
inline std::map<HWND,std::shared_ptr<Mailbox>>& Mailboxes() { static auto* map=new std::map<HWND,std::shared_ptr<Mailbox>>; return *map; }
inline std::shared_ptr<Mailbox> FindMailbox(HWND shell) {
    std::lock_guard guard(MailboxesLock());
    auto it=Mailboxes().find(shell);return it==Mailboxes().end() ? nullptr : it->second;
}
// Set on the input thread: its stores deliver through mailboxes.
inline bool& Mailboxed() { static thread_local bool value=false; return value; }
inline bool Fill(const ShellPageInputEvent& event,WinUIPageInputEvent& output) {
    if(event.clause_count>64)return false;
    output={event.session,event.edit,event.revision,event.request,event.kind,event.start,event.end,event.caret,static_cast<uint32_t>(event.length)};
    static_assert(sizeof(WinUIPageClause)==sizeof(ShellPageClause));
    output.clauseCount=static_cast<uint32_t>(event.clause_count);
    if(event.clause_count)memcpy(output.clauses,event.clauses,event.clause_count*sizeof(ShellPageClause));
    return true;
}
struct Store final : ITextStoreACP, ITfContextOwnerCompositionSink, ITfTextEditSink, ITfInputScope {
    std::atomic<ULONG> refs{1};
    HWND window{}, endpoint{};
    std::unique_ptr<ShellCore,decltype(&shell_core_destroy)> core{shell_core_create(nullptr),shell_core_destroy};
    winrt::com_ptr<ITfThreadMgr> manager;
    winrt::com_ptr<ITfDocumentMgr> document;
    winrt::com_ptr<ITfContext> context;
    winrt::com_ptr<ITextStoreACPSink> sink;
    winrt::com_ptr<ITfCompositionView> composition;
    TfClientId client=TF_CLIENTID_NULL;
    TfEditCookie editCookie{};
    DWORD editSink=TF_INVALID_COOKIE, mask{}, lock{}, pendingLock{};
    bool enabled{}, wake{}, dirty{}, ending{}, discarding{}, associated{};
    std::shared_ptr<Mailbox> mailbox;bool armed{};
    bool publicationPending{};WinUIPageInput pendingPublication{};std::vector<WCHAR> pendingText;
    LONG compositionStart{}, compositionLength{};
    TsActiveSelEnd selectionEnd=TS_AE_END;
    bool interimSelection{};
    std::vector<ShellPageClause> clauses;
    RECT caret{}, editor{};
    uint32_t textBase{};
    std::vector<WinUIPageGeometry> pendingGeometry;
    InputScope scopes[8]{};UINT scopeCount{};bool requestedScope{}, requestedValues{};
    explicit Store(HWND hwnd,HWND target):window(hwnd),endpoint(target) {}
    STDMETHODIMP QueryInterface(REFIID iid,void** output) override {
        if (!output) return E_POINTER;*output=nullptr;
        if (iid==IID_IUnknown || iid==IID_ITextStoreACP) *output=static_cast<ITextStoreACP*>(this);
        else if (iid==IID_ITfContextOwnerCompositionSink) *output=static_cast<ITfContextOwnerCompositionSink*>(this);
        else if (iid==IID_ITfInputScope) *output=static_cast<ITfInputScope*>(this);
        else if (iid==IID_ITfTextEditSink) *output=static_cast<ITfTextEditSink*>(this);
        if (!*output) return E_NOINTERFACE;AddRef();return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override {return ++refs;}
    STDMETHODIMP_(ULONG) Release() override {auto value=--refs;if (!value) delete this;return value;}
    ShellPageInputState Read(const uint16_t** text=nullptr) {
        ShellPageInputState state{};const uint16_t* ignored{};
        shell_core_page_input_read(core.get(),&state,text ? text : &ignored);return state;
    }
    bool Apply(uint32_t operation,LONG start=0,LONG end=0,const WCHAR* text=nullptr,ULONG count=0,ULONG reversed=0) {
        if(start<0 || end<start) return false;
        ShellPageInputCommand command{};command.operation=operation;command.start=start;command.end=end;command.caret=reversed;
        command.text=reinterpret_cast<const uint16_t*>(text);command.length=count;
        return shell_core_page_input_apply(core.get(),&command)==SHELL_STATUS_OK;
    }
    HRESULT Open() {
        HRESULT hr=CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(manager.put()));
        if(FAILED(hr)) return hr;
        if(FAILED(hr=manager->Activate(&client))) return hr;
        if(FAILED(hr=manager->CreateDocumentMgr(document.put()))) return hr;
        if(FAILED(hr=document->CreateContext(client,0,static_cast<ITextStoreACP*>(this),context.put(),&editCookie))) return hr;
        if(FAILED(hr=document->Push(context.get()))) return hr;
        winrt::com_ptr<ITfSource> source;
        if(SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(source.put()))))
            hr=source->AdviseSink(IID_ITfTextEditSink,static_cast<ITfTextEditSink*>(this),&editSink);
        return hr;
    }
    void Close() {
        enabled=false;
        if(manager && document) {winrt::com_ptr<ITfDocumentMgr> previous;manager->AssociateFocus(window,nullptr,previous.put());}
        if(context && editSink!=TF_INVALID_COOKIE) {auto source=context.try_as<ITfSource>();if(source) source->UnadviseSink(editSink);editSink=TF_INVALID_COOKIE;}
        composition=nullptr;
        if(document) document->Pop(TF_POPF_ALL);
        context=nullptr;document=nullptr;sink=nullptr;
        if(manager && client!=TF_CLIENTID_NULL) manager->Deactivate();
        client=TF_CLIENTID_NULL;manager=nullptr;endpoint=nullptr;
    }
    // Moves every queued event into the mailbox and wakes Gecko once. Until Gecko empties the
    // mailbox, later events stay queued.
    void Transfer() {
        bool moved=false;
        {
            std::lock_guard guard(mailbox->lock);
            ShellPageInputEvent event{};
            while(shell_core_page_input_event(core.get(),&event)==SHELL_STATUS_OK) {
                Mailbox::Item item{};
                if(event.length<=PAGE_INPUT_MAX_TEXT && Fill(event,item.event)) {
                    if(event.length)item.text.assign(reinterpret_cast<const WCHAR*>(event.text),reinterpret_cast<const WCHAR*>(event.text)+event.length);
                    mailbox->items.push_back(std::move(item));moved=true;
                }
                Apply(5);
            }
        }
        if(moved) {armed=true;if(!PostMessageW(endpoint,WakeMessage(),reinterpret_cast<WPARAM>(window),0))enabled=false;}
    }
    // Gecko emptied the mailbox.
    void Refill() {armed=false;Transfer();}
    bool Wake() {
        if(mailbox) {if(!armed)Transfer();return enabled;}
        if(!wake) {wake=PostMessageW(endpoint,WakeMessage(),reinterpret_cast<WPARAM>(window),0)!=FALSE;if(!wake) enabled=false;}
        return enabled;
    }
    bool Emit(uint32_t kind,LONG start,LONG end,const WCHAR* text,ULONG count,ULONG cursor) {
        ShellPageInputCommand command{};command.operation=4;command.kind=kind;command.start=start;command.end=end;
        command.caret=cursor;command.text=reinterpret_cast<const uint16_t*>(text);command.length=count;
        if(shell_core_page_input_apply(core.get(),&command)!=SHELL_STATUS_OK) {enabled=false;return false;}
        if(Diagnostics()) fprintf(stderr,"FennPageIME emit=%u length=%lu\n",kind,count);
        return Wake();
    }
    void Extent(ITfCompositionView* view) {
        winrt::com_ptr<ITfRange> range;
        if(view && SUCCEEDED(view->GetRange(range.put()))) {
            auto acp=range.try_as<ITfRangeACP>();if(acp) acp->GetExtent(&compositionStart,&compositionLength);
        }
    }
    void CaptureClauses(TfEditCookie cookie) {
        clauses.clear();if(!context || !composition || compositionLength<=0)return;
        winrt::com_ptr<ITfProperty> property;
        winrt::com_ptr<ITfRange> composing;
        winrt::com_ptr<IEnumTfRanges> ranges;
        winrt::com_ptr<ITfCategoryMgr> categories;
        winrt::com_ptr<ITfDisplayAttributeMgr> attributes;
        if(FAILED(context->GetProperty(GUID_PROP_ATTRIBUTE,property.put())) ||
           FAILED(composition->GetRange(composing.put())) ||
           FAILED(property->EnumRanges(cookie,ranges.put(),composing.get())) ||
           FAILED(CoCreateInstance(CLSID_TF_CategoryMgr,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(categories.put()))) ||
           FAILED(CoCreateInstance(CLSID_TF_DisplayAttributeMgr,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(attributes.put()))))return;
        uint32_t position=0;unsigned visited=0;
        for(;;) {
            winrt::com_ptr<ITfRange> range;ULONG fetched{};
            if(ranges->Next(1,range.put(),&fetched)!=S_OK)break;
            if(++visited>128) {clauses.clear();return;}
            auto acp=range.try_as<ITfRangeACP>();LONG start{},length{};
            if(!acp || FAILED(acp->GetExtent(&start,&length)) || length<0)continue;
            auto first=static_cast<uint32_t>(std::clamp<int64_t>(int64_t(start)-compositionStart,0,compositionLength));
            auto last=static_cast<uint32_t>(std::clamp<int64_t>(int64_t(start)+length-compositionStart,0,compositionLength));
            if(last<=first)continue;
            if(first<position || clauses.size()>61) {clauses.clear();return;}
            if(first>position)clauses.push_back({position,first,1});
            ShellPageClause clause{first,last,1};
            VARIANT value;VariantInit(&value);
            if(SUCCEEDED(property->GetValue(cookie,range.get(),&value)) && value.vt==VT_I4) {
                GUID guid{};winrt::com_ptr<ITfDisplayAttributeInfo> info;TF_DISPLAYATTRIBUTE attr{};
                if(SUCCEEDED(categories->GetGUID(value.lVal,&guid)) &&
                   SUCCEEDED(attributes->GetDisplayAttributeInfo(guid,info.put(),nullptr)) &&
                   SUCCEEDED(info->GetAttributeInfo(&attr))) {
                    clause.kind=attr.bAttr==TF_ATTR_TARGET_CONVERTED ? 4 : attr.bAttr==TF_ATTR_CONVERTED ? 3 : attr.bAttr==TF_ATTR_TARGET_NOTCONVERTED ? 2 : 1;
                    auto color=[&](const TF_DA_COLOR& c,uint32_t flag,uint32_t& output) {
                        if(c.type==TF_CT_COLORREF || c.type==TF_CT_SYSCOLOR) {
                            output=c.type==TF_CT_COLORREF ? c.cr : GetSysColor(c.nIndex);clause.flags|=flag;
                        }
                    };
                    color(attr.crText,1,clause.foreground);color(attr.crBk,2,clause.background);color(attr.crLine,4,clause.underline);
                    if(attr.lsStyle>=TF_LS_NONE && attr.lsStyle<=TF_LS_SQUIGGLE) {clause.flags|=8;clause.line=attr.lsStyle;clause.bold=attr.fBoldLine!=FALSE;}
                }
            }
            VariantClear(&value);clauses.push_back(clause);position=last;
        }
        if(position<static_cast<uint32_t>(compositionLength))clauses.push_back({position,static_cast<uint32_t>(compositionLength),1});
    }
    void Flush() {
        if(!dirty && !ending) return;
        const uint16_t* text{};auto state=Read(&text);
        if(compositionStart<0 || compositionLength<0 || uint64_t(compositionStart)+compositionLength>state.length) {enabled=false;return;}
        if(composition || ending) {
            auto count=discarding ? 0u : static_cast<ULONG>(compositionLength);
            auto cursor=static_cast<ULONG>(std::clamp<LONG>(static_cast<LONG>(state.end)-compositionStart,0,count));
            if(ending)Emit(3,compositionStart,compositionStart,reinterpret_cast<const WCHAR*>(text+compositionStart),count,cursor);
            else {
                ShellPageInputCommand command{};command.operation=8;command.start=compositionStart;command.end=compositionStart;
                command.text=text+compositionStart;command.length=count;command.caret=cursor;
                command.clauses=clauses.data();command.clause_count=clauses.size();
                if(shell_core_page_input_apply(core.get(),&command)!=SHELL_STATUS_OK)enabled=false;
                else Wake();
            }
        }
        dirty=false;ending=false;discarding=false;
    }
    void FinishComposition() {
        // A TIP may end one syllable and start another inside the same lock.
        // Snapshot the commit before its next SetText mutates the projection.
        composition=nullptr;ending=true;Flush();
    }
    HRESULT Commit(bool cancel) {
        if(Diagnostics()) fprintf(stderr,"FennPageIME commit cancel=%d composing=%d\n",int(cancel),int(bool(composition)));
        if(!composition) return S_FALSE;
        discarding=cancel;
        auto services=context.try_as<ITfContextOwnerCompositionServices>();
        return services ? services->TerminateComposition(nullptr) : E_NOINTERFACE;
    }
    HRESULT Publish(const WinUIPageInput& input) {
        if(input.reversed>1 || input.scopeCount>8 || input.length>PAGE_INPUT_MAX_TEXT || (input.length && !input.text))return E_INVALIDARG;
        auto old=Read();
        // A deferred publication must not acknowledge a base shift that can be
        // rejected after a reentrant TSF edit. Gecko retains the old base.
        if(input.enabled && old.session==input.session && input.textBase!=textBase &&
            (lock || composition || old.edit!=input.acknowledgment))return S_FALSE;
        if(lock) {
            // A COM callback may reenter publication. Own the latest bounded
            // snapshot until the native TSF lock is released.
            pendingText.clear();if(input.length)pendingText.assign(input.text,input.text+input.length);
            pendingPublication=input;pendingPublication.text=nullptr;publicationPending=true;return S_OK;
        }
        if(!input.enabled) {
            Commit(false);enabled=false;
            if(manager && associated) {winrt::com_ptr<ITfDocumentMgr> previous;manager->AssociateFocus(window,nullptr,previous.put());associated=false;}
            return S_OK;
        }
        if(old.session && old.session!=input.session) Commit(false);
        ShellPageInputCommand command{};command.operation=1;command.session=input.session;command.revision=input.revision;
        command.ack=input.acknowledgment;command.start=input.start;command.end=input.end;
        command.text=reinterpret_cast<const uint16_t*>(input.text);command.length=input.length;
        auto status=shell_core_page_input_apply(core.get(),&command);
        if(status!=SHELL_STATUS_OK && status!=SHELL_STATUS_BUSY) return E_INVALIDARG;
        if(status==SHELL_STATUS_OK) {if(old.session!=input.session || old.start!=input.start || old.end!=input.end)interimSelection=false;selectionEnd=input.reversed ? TS_AE_START : TS_AE_END;textBase=input.textBase;}
        auto layout=shell_core_page_geometry_context(core.get(),input.session,input.revision);
        // Read before waking: a mailbox takes the refreshed requests with it.
        const bool refreshing=shell_core_page_geometry_pending(core.get())==SHELL_STATUS_OK;
        // Gecko adopts this publication's text base when it reads the acceptance.
        Wake();
        if(layout==SHELL_STATUS_BUSY)return S_OK;
        if(layout!=SHELL_STATUS_OK)return E_INVALIDARG;
        caret=input.caret;editor=input.editor;enabled=true;
        scopeCount=input.scopeCount;for(UINT i=0;i<scopeCount;++i)scopes[i]=static_cast<InputScope>(input.scopes[i]);
        if(Diagnostics()) fprintf(stderr,"FennPageIME publish session=%llu revision=%llu ack=%llu status=%u composing=%d\n",input.session,input.revision,input.acknowledgment,unsigned(status),int(bool(composition)));
        if(!manager) {
            auto hr=Open();if(FAILED(hr)) {enabled=false;return hr;}
        }
        if(!associated) {
            winrt::com_ptr<ITfDocumentMgr> previous;
            if(SUCCEEDED(manager->AssociateFocus(window,document.get(),previous.put())))associated=true;
        }
        if(GetFocus()==window) {
            winrt::com_ptr<ITfDocumentMgr> focused;manager->GetFocus(focused.put());
            if(focused.get()!=document.get())manager->SetFocus(document.get());
        }
        if(sink) {
            if(status==SHELL_STATUS_OK) {
                TS_TEXTCHANGE change{0,static_cast<LONG>(old.length),static_cast<LONG>(input.length)};
                if(mask&TS_AS_TEXT_CHANGE) sink->OnTextChange(0,&change);
                if(mask&TS_AS_SEL_CHANGE) sink->OnSelectionChange();
            }
            // Refreshed requests notify layout when Gecko answers them (Geometry).
            if(!refreshing && (mask&TS_AS_LAYOUT_CHANGE)) sink->OnLayoutChange(TS_LC_CHANGE,0);
        }
        return S_OK;
    }
    bool Key(MSG* message) {
        if(!enabled || !manager || GetFocus()!=window || message->hwnd!=window) return false;
        const bool down=message->message==WM_KEYDOWN || message->message==WM_SYSKEYDOWN;
        const bool up=message->message==WM_KEYUP || message->message==WM_SYSKEYUP;
        if(!down && !up) return false;
        auto keys=manager.try_as<ITfKeystrokeMgr>();if(!keys) return false;
        BOOL eaten=FALSE;HRESULT hr=down ? keys->TestKeyDown(message->wParam,message->lParam,&eaten) : keys->TestKeyUp(message->wParam,message->lParam,&eaten);
        if(FAILED(hr) || !eaten) return false;
        hr=down ? keys->KeyDown(message->wParam,message->lParam,&eaten) : keys->KeyUp(message->wParam,message->lParam,&eaten);
        return SUCCEEDED(hr) && eaten;
    }
    STDMETHODIMP AdviseSink(REFIID iid,IUnknown* object,DWORD flags) override {
        if(iid!=IID_ITextStoreACPSink || !object) return E_INVALIDARG;
        winrt::com_ptr<ITextStoreACPSink> next;auto hr=object->QueryInterface(IID_PPV_ARGS(next.put()));if(FAILED(hr)) return hr;
        if(sink && sink.get()!=next.get()) return CONNECT_E_ADVISELIMIT;sink=next;mask=flags;return S_OK;
    }
    STDMETHODIMP UnadviseSink(IUnknown* object) override {
        if(!object || !sink) return CONNECT_E_NOCONNECTION;
        auto identity=sink.as<IUnknown>();winrt::com_ptr<IUnknown> other;
        if(FAILED(object->QueryInterface(IID_PPV_ARGS(other.put()))) || identity.get()!=other.get()) return CONNECT_E_NOCONNECTION;
        sink=nullptr;mask=0;return S_OK;
    }
    STDMETHODIMP RequestLock(DWORD flags,HRESULT* session) override {
        if(!session) return E_POINTER;if(!sink) return E_UNEXPECTED;
        if(lock) {if(flags&TS_LF_SYNC) {*session=TS_E_SYNCHRONOUS;return S_OK;} pendingLock=flags;*session=TS_S_ASYNC;return S_OK;}
        AddRef();unsigned passes=0;
        do {lock=flags;pendingLock=0;auto retained=sink;
            *session=retained->OnLockGranted(flags);lock=0;Flush();flags=pendingLock;
        } while(flags && ++passes<8);
        if(pendingLock) PostMessageW(window,LockMessage(),reinterpret_cast<WPARAM>(endpoint),0);
        if(publicationPending) {
            auto input=pendingPublication;auto text=std::move(pendingText);publicationPending=false;
            input.text=text.data();Publish(input);
        }
        FlushGeometry();
        Release();return S_OK;
    }
    STDMETHODIMP GetStatus(TS_STATUS* status) override {if(!status)return E_POINTER;status->dwDynamicFlags=enabled ? 0 : TS_SD_READONLY;status->dwStaticFlags=TS_SS_NOHIDDENTEXT;return S_OK;}
    STDMETHODIMP QueryInsert(LONG start,LONG end,ULONG count,LONG* first,LONG* last) override {
        if(!first || !last)return E_POINTER;auto state=Read();if(start<0 || end<start || static_cast<ULONG>(end)>state.length || state.length-(end-start)+count>PAGE_INPUT_MAX_TEXT)return E_INVALIDARG;
        *first=start;*last=end;return S_OK;
    }
    STDMETHODIMP GetSelection(ULONG index,ULONG count,TS_SELECTION_ACP* selection,ULONG* fetched) override {
        if(!selection || !fetched)return E_POINTER;*fetched=0;if(!lock)return TS_E_NOLOCK;
        if(!count || (index!=0 && index!=TS_DEFAULT_SELECTION))return E_INVALIDARG;
        auto state=Read();selection->acpStart=state.start;selection->acpEnd=state.end;selection->style={interimSelection ? TS_AE_NONE : selectionEnd,interimSelection};*fetched=1;return S_OK;
    }
    STDMETHODIMP SetSelection(ULONG count,const TS_SELECTION_ACP* selection) override {
        if((lock&TS_LF_READWRITE)!=TS_LF_READWRITE)return TS_E_NOLOCK;
        if(!enabled)return E_FAIL;
        if(count!=1 || !selection)return E_INVALIDARG;
        if(!selection->style.fInterimChar && selection->style.ase!=TS_AE_START && selection->style.ase!=TS_AE_END)return E_INVALIDARG;
        const bool direct=!composition && !ending;
        if(!Apply(direct ? 7u : 2u,selection->acpStart,selection->acpEnd,nullptr,0,selection->style.ase==TS_AE_START))return E_INVALIDARG;
        selectionEnd=selection->style.ase==TS_AE_START ? TS_AE_START : TS_AE_END;interimSelection=selection->style.fInterimChar!=FALSE;
        if(!direct)dirty=true;
        if(direct && !Wake())return E_FAIL;
        return S_OK;
    }
    STDMETHODIMP GetText(LONG start,LONG end,WCHAR* output,ULONG capacity,ULONG* copied,TS_RUNINFO* runs,ULONG runCapacity,ULONG* runCount,LONG* next) override {
        if(!copied || !runCount || !next || (capacity && !output) || (runCapacity && !runs))return E_POINTER;
        *copied=0;*runCount=0;if(!lock)return TS_E_NOLOCK;
        const uint16_t* text{};auto state=Read(&text);if(end==-1)end=state.length;
        if(start<0 || end<start || static_cast<ULONG>(end)>state.length)return TS_E_INVALIDPOS;
        ULONG count=capacity ? std::min<ULONG>(end-start,capacity) : (runCapacity ? end-start : 0);if(capacity && count)memcpy(output,text+start,count*sizeof(WCHAR));
        *copied=capacity ? count : 0;*next=start+count;if(runCapacity) {runs[0]={count,TS_RT_PLAIN};*runCount=1;}return S_OK;
    }
    STDMETHODIMP SetText(DWORD,LONG start,LONG end,const WCHAR* text,ULONG count,TS_TEXTCHANGE* change) override {
        if(!change || (count && !text))return E_POINTER;if((lock&TS_LF_READWRITE)!=TS_LF_READWRITE)return TS_E_NOLOCK;
        if(Diagnostics())fprintf(stderr,"FennPageIME set-text start=%ld end=%ld count=%lu composing=%d ending=%d extent=%ld,%ld\n",start,end,count,int(bool(composition)),int(ending),compositionStart,compositionLength);
        const bool direct=!composition && !ending;
        if(!enabled || !Apply(direct ? 6u : 3u,start,end,text,count))return E_INVALIDARG;
        selectionEnd=TS_AE_END;interimSelection=false;clauses.clear();
        *change={start,end,start+static_cast<LONG>(count)};
        if(direct)return Wake() ? S_OK : E_FAIL;
        dirty=true;return S_OK;
    }
    STDMETHODIMP GetFormattedText(LONG,LONG,IDataObject**) override {return E_NOTIMPL;}
    STDMETHODIMP GetEmbedded(LONG,REFGUID,REFIID,IUnknown**) override {return E_NOTIMPL;}
    STDMETHODIMP QueryInsertEmbedded(const GUID*,const FORMATETC*,BOOL* allowed) override {if(!allowed)return E_POINTER;*allowed=FALSE;return S_OK;}
    STDMETHODIMP InsertEmbedded(DWORD,LONG,LONG,IDataObject*,TS_TEXTCHANGE*) override {return E_NOTIMPL;}
    STDMETHODIMP InsertTextAtSelection(DWORD flags,const WCHAR* text,ULONG count,LONG* start,LONG* end,TS_TEXTCHANGE* change) override {
        auto state=Read();if(!lock)return TS_E_NOLOCK;
        if(start)*start=state.start;if(end)*end=state.end;
        if(flags&TS_IAS_QUERYONLY)return S_OK;
        auto hr=SetText(0,state.start,state.end,text,count,change);if(SUCCEEDED(hr) && end)*end=state.start+count;return hr;
    }
    STDMETHODIMP InsertEmbeddedAtSelection(DWORD,IDataObject*,LONG*,LONG*,TS_TEXTCHANGE*) override {return E_NOTIMPL;}
    STDMETHODIMP RequestSupportedAttrs(DWORD flags,ULONG count,const TS_ATTRID* attrs) override {
        if(count && !attrs)return E_INVALIDARG;requestedScope=false;requestedValues=(flags&TS_ATTR_FIND_WANT_VALUE)!=0;
        for(ULONG i=0;i<count;++i)if(attrs[i]==InputScopeAttribute)requestedScope=true;return S_OK;
    }
    STDMETHODIMP RequestAttrsAtPosition(LONG,ULONG count,const TS_ATTRID* attrs,DWORD flags) override {return RequestSupportedAttrs(flags|TS_ATTR_FIND_WANT_VALUE,count,attrs);}
    STDMETHODIMP RequestAttrsTransitioningAtPosition(LONG,ULONG,const TS_ATTRID*,DWORD) override {return S_OK;}
    STDMETHODIMP FindNextAttrTransition(LONG start,LONG,ULONG,const TS_ATTRID*,DWORD,LONG* next,BOOL* found,LONG* offset) override {if(!next || !found || !offset)return E_POINTER;*next=start;*found=FALSE;*offset=0;return S_OK;}
    STDMETHODIMP RetrieveRequestedAttrs(ULONG capacity,TS_ATTRVAL* values,ULONG* count) override {
        if(!count || (capacity && !values))return E_POINTER;*count=0;
        if(requestedScope && capacity) {
            values[0].idAttr=InputScopeAttribute;values[0].dwOverlapId=0;VariantInit(&values[0].varValue);
            if(requestedValues){values[0].varValue.vt=VT_UNKNOWN;values[0].varValue.punkVal=static_cast<ITfInputScope*>(this);AddRef();}
            *count=1;requestedScope=false;
        }return S_OK;
    }
    STDMETHODIMP GetInputScopes(InputScope** output,UINT* count) override {
        if(!output || !count)return E_POINTER;*count=std::max(1u,scopeCount);
        *output=static_cast<InputScope*>(CoTaskMemAlloc(*count*sizeof(InputScope)));if(!*output)return E_OUTOFMEMORY;
        if(scopeCount)memcpy(*output,scopes,scopeCount*sizeof(InputScope));else **output=IS_DEFAULT;return S_OK;
    }
    STDMETHODIMP GetPhrase(BSTR**,UINT*) override {return E_NOTIMPL;}
    STDMETHODIMP GetRegularExpression(BSTR*) override {return E_NOTIMPL;}
    STDMETHODIMP GetSRGS(BSTR*) override {return E_NOTIMPL;}
    STDMETHODIMP GetXML(BSTR*) override {return E_NOTIMPL;}
    STDMETHODIMP GetEndACP(LONG* end) override {if(!end)return E_POINTER;if(!lock)return TS_E_NOLOCK;*end=Read().length;return S_OK;}
    STDMETHODIMP GetActiveView(TsViewCookie* view) override {if(!view)return E_POINTER;*view=0;return S_OK;}
    STDMETHODIMP GetACPFromPoint(TsViewCookie,const POINT*,DWORD,LONG*) override {return E_NOTIMPL;}
    STDMETHODIMP GetTextExt(TsViewCookie view,LONG start,LONG end,RECT* bounds,BOOL* clipped) override {
        if(!bounds || !clipped)return E_POINTER;if(!lock)return TS_E_NOLOCK;
        if(view || start<0 || end<start || static_cast<ULONG>(end)>Read().length)return E_INVALIDARG;
        if(!enabled)return TS_E_NOLAYOUT;
        ShellPageGeometry result{};
        auto status=shell_core_page_geometry_read(core.get(),start,end,&result);
        if(status==SHELL_STATUS_BUSY) {Wake();return TS_E_NOLAYOUT;}
        if(status!=SHELL_STATUS_OK)return E_INVALIDARG;
        if(Diagnostics())fprintf(stderr,"FennPageIME extent=%ld,%ld rect=%d,%d,%d,%d\n",start,end,result.left,result.top,result.right,result.bottom);
        *bounds={result.left,result.top,result.right,result.bottom};*clipped=result.clipped!=0;return S_OK;
    }
    void FlushGeometry() {
        if(lock)return;
        auto replies=std::move(pendingGeometry);pendingGeometry.clear();
        for(const auto& reply:replies)Geometry(reply);
    }
    HRESULT Geometry(const WinUIPageGeometry& input) {
        if(!enabled)return S_FALSE;
        // COM can reenter while a TIP holds the text-store lock. Retain only
        // bounded replies and notify layout after that lock is released.
        if(lock) {if(pendingGeometry.size()>=16)return S_FALSE;pendingGeometry.push_back(input);return S_OK;}
        ShellPageGeometry reply{input.session,input.revision,input.edit,input.request,input.start,input.end,input.valid,input.clipped,
            input.bounds.left,input.bounds.top,input.bounds.right,input.bounds.bottom};
        if(shell_core_page_geometry_reply(core.get(),&reply)!=SHELL_STATUS_OK)return S_FALSE;
        if(input.valid && sink && (mask&TS_AS_LAYOUT_CHANGE))sink->OnLayoutChange(TS_LC_CHANGE,0);
        return S_OK;
    }
    STDMETHODIMP GetScreenExt(TsViewCookie view,RECT* bounds) override {if(!bounds)return E_POINTER;if(view)return E_INVALIDARG;*bounds=editor;return S_OK;}
    STDMETHODIMP GetWnd(TsViewCookie view,HWND* hwnd) override {if(!hwnd)return E_POINTER;if(view)return E_INVALIDARG;*hwnd=window;return S_OK;}
    STDMETHODIMP OnStartComposition(ITfCompositionView* view,BOOL* accept) override {
        if(!accept || !view)return E_POINTER;*accept=FALSE;if(!enabled || composition)return S_OK;
        auto selected=Read();
        if(Diagnostics())fprintf(stderr,"FennPageIME start selected=%u,%u ending=%d\n",selected.start,selected.end,int(ending));
        if(!Emit(1,selected.start,selected.end,nullptr,0,0))return S_OK;
        clauses.clear();composition.copy_from(view);Extent(view);*accept=TRUE;return S_OK;
    }
    STDMETHODIMP OnUpdateComposition(ITfCompositionView* view,ITfRange*) override {if(view!=composition.get())return E_INVALIDARG;Extent(view);clauses.clear();dirty=true;return S_OK;}
    STDMETHODIMP OnEndComposition(ITfCompositionView* view) override {if(view!=composition.get())return E_INVALIDARG;Extent(view);if(Diagnostics())fprintf(stderr,"FennPageIME end extent=%ld,%ld lock=%lu\n",compositionStart,compositionLength,lock);FinishComposition();return S_OK;}
    STDMETHODIMP OnEndEdit(ITfContext*,TfEditCookie cookie,ITfEditRecord*) override {if(composition) {Extent(composition.get());CaptureClauses(cookie);dirty=true;}if(!lock)Flush();return S_OK;}
};
// Per thread: each thread that owns page windows has its own stores and TSF thread manager (M5c).
inline std::map<HWND,winrt::com_ptr<Store>>& Stores() {static thread_local std::map<HWND,winrt::com_ptr<Store>> stores;return stores;}
inline Store* Get(HWND hwnd) {auto it=Stores().find(hwnd);return it==Stores().end() ? nullptr : it->second.get();}
inline HRESULT WINAPI Publish(HWND hwnd,const WinUIPageInput* input) noexcept {
    try {
        if(!input || GetWindowThreadProcessId(hwnd,nullptr)!=GetCurrentThreadId())return E_INVALIDARG;
        HWND endpoint=static_cast<HWND>(GetPropW(hwnd,WINUI_PANE_ENDPOINT_PROPERTY));if(!endpoint)return E_INVALIDARG;
        auto& entry=Stores()[hwnd];
        if(!entry) {
            entry.attach(new Store(hwnd,endpoint));
            if(Mailboxed()) {
                auto box=std::make_shared<Mailbox>();box->page=hwnd;
                {std::lock_guard guard(MailboxesLock());Mailboxes()[GetAncestor(hwnd,GA_PARENT)]=box;}
                entry->mailbox=box;
            }
        }
        return entry->Publish(*input);
    }catch(...){return winrt::to_hresult();}
}
inline HRESULT WINAPI Drain(HWND hwnd,WinUIPageInputEvent* output,WCHAR* text,uint32_t capacity) noexcept {
    if(!output || !text || GetWindowThreadProcessId(hwnd,nullptr)!=GetCurrentThreadId())return E_INVALIDARG;
    auto store=Get(hwnd);if(!store)return S_FALSE;store->wake=false;ShellPageInputEvent event{};
    if(shell_core_page_input_event(store->core.get(),&event)!=SHELL_STATUS_OK)return S_FALSE;
    if(event.length>capacity)return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    if(!Fill(event,*output))return E_UNEXPECTED;
    if(event.length)memcpy(text,event.text,event.length*sizeof(WCHAR));store->Apply(5);return S_OK;
}
inline HRESULT WINAPI Geometry(HWND hwnd,const WinUIPageGeometry* input) noexcept {
    if(!input || GetWindowThreadProcessId(hwnd,nullptr)!=GetCurrentThreadId())return E_INVALIDARG;
    auto store=Get(hwnd);return store ? store->Geometry(*input) : S_FALSE;
}
inline HRESULT WINAPI Commit(HWND hwnd,BOOL cancel) noexcept {if(GetWindowThreadProcessId(hwnd,nullptr)!=GetCurrentThreadId())return E_INVALIDARG;auto store=Get(hwnd);return store ? store->Commit(cancel!=FALSE) : S_FALSE;}
inline void Remove(HWND hwnd) {
    auto it=Stores().find(hwnd);if(it==Stores().end())return;
    if(auto box=it->second->mailbox) {
        std::lock_guard guard(MailboxesLock());
        for(auto m=Mailboxes().begin();m!=Mailboxes().end();++m) if(m->second==box) {Mailboxes().erase(m);break;}
    }
    it->second->Close();Stores().erase(it);
}
inline bool Key(MSG* message) {auto store=Get(message->hwnd);return store && store->Key(message);}
inline void Focus(HWND hwnd) {auto store=Get(hwnd);if(store && store->enabled && store->manager)store->manager->SetFocus(store->document.get());}
inline void Blur(HWND hwnd) {auto store=Get(hwnd);if(store)store->Commit(false);}
inline void Pump(HWND hwnd,WPARAM endpoint) {auto store=Get(hwnd);if(store && reinterpret_cast<HWND>(endpoint)==store->endpoint && store->pendingLock){HRESULT status;store->RequestLock(store->pendingLock,&status);}}
}
