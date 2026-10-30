#pragma once

namespace {
void NotifyStartup(Pane& pane) noexcept {
    ShellStartupTransfer transfer{};
    if(shell_core_startup_transfer(pane.shell.core,0,0,0,0,0,0,0,false,&transfer)!=SHELL_STATUS_OK ||
        !transfer.generation || transfer.lost)return;
    auto call=[&](uint32_t op){return shell_core_startup_transfer(pane.shell.core,op,0,
        transfer.session,transfer.generation,0,0,0,false,&transfer);};
    if(!pane.notifySink){call(10);return;}
    if(call(9)==SHELL_STATUS_OK && !pane.notifySink(pane.notifyContext))call(10);
}
}

// HWND/JSON boundary only. Rust owns leases, ordering and completion semantics.
HRESULT WINAPI StartupTransfer(HWND hwnd, const wchar_t* input, wchar_t* output,
                              uint32_t capacity) noexcept {
    try {
        check_hresult(Check(hwnd));
        if (!input || !output || capacity != 262144 || wcsnlen(input,131073)>131072) return E_INVALIDARG;
        auto found=state().panes.find(hwnd);
        if(found==state().panes.end())return E_INVALIDARG;
        auto& pane=*found->second;
        using JObject=Windows::Data::Json::JsonObject;
        using JValue=Windows::Data::Json::JsonValue;
        const auto message=JObject::Parse(input);
        if(message.GetNamedNumber(L"version")!=1)return E_INVALIDARG;
        const auto number=message.GetNamedNumber(L"operation");
        if(number<0 || number>19 || number==9 || number==10 || number!=static_cast<uint32_t>(number))return E_INVALIDARG;
        const auto operation=static_cast<uint32_t>(number);
        if(operation!=11 && operation!=19 && wcsnlen(input,4097)>4096)return E_INVALIDARG;
        if(operation==19) {
            if(wcsnlen(input,8193)>8192)return E_INVALIDARG;
            check_hresult(PublishStartupPresentation(hwnd,message));
            output[0]=L'{';output[1]=L'}';output[2]=0;return S_OK;
        }
        auto parseId=[](hstring const& text) -> uint64_t {
            if(text.empty() || text.size()>20 || (text.size()>1 && text[0]==L'0'))throw_hresult(E_INVALIDARG);
            uint64_t result=0;
            for(auto c:text){
                if(c<L'0' || c>L'9' || result>(UINT64_MAX-(c-L'0'))/10)throw_hresult(E_INVALIDARG);
                result=result*10+(c-L'0');
            }
            return result;
        };
        auto id=[&](const wchar_t* name){return parseId(message.GetNamedString(name,L"0"));};
        if(operation==15) {
            ShellStartupTransfer current{};
            if(shell_core_startup_transfer(pane.shell.core,0,0,0,0,0,0,0,false,&current)!=SHELL_STATUS_OK)return E_NOT_VALID_STATE;
            JObject result;
            result.Insert(L"early",JValue::CreateBooleanValue(pane.startupPresenter && pane.addressSource &&
                GetPropW(hwnd,L"Fenn.InteractiveStartup") && !pane.startupContext));
            result.Insert(L"session",JValue::CreateStringValue(to_hstring(current.session)));
            const auto json=result.Stringify();
            memcpy(output,json.c_str(),(json.size()+1)*sizeof(wchar_t));return S_OK;
        }
        const auto context=id(L"context"),session=id(L"session"),generation=id(L"generation");
        const auto request=id(L"request"),draft=id(L"draft"),browser=id(L"browser");
        const bool accepted=message.GetNamedBoolean(L"accepted",false);
        if(!context || (operation!=1 && context!=pane.startupContext))return E_INVALIDARG;
        if(operation==11) {
            const auto revision=id(L"revision"),selected=id(L"selected");
            if(!revision)return E_INVALIDARG;
            const auto array=message.GetNamedArray(L"ids");
            if(array.Size()>4096)return E_INVALIDARG;
            std::vector<uint64_t> ids;ids.reserve(array.Size());
            for(auto const& value:array)ids.push_back(parseId(value.GetString()));
            std::vector<ShellStartupRow> rows(SHELL_STARTUP_ROW_CAPACITY);uint32_t count{};
            if(shell_core_startup_rows(pane.shell.core,session,generation,revision,ids.data(),ids.size(),selected,
                rows.data(),rows.size(),&count)!=SHELL_STATUS_OK)return E_NOT_VALID_STATE;
            JObject result;
            result.Insert(L"context",JValue::CreateStringValue(to_hstring(context)));
            result.Insert(L"session",JValue::CreateStringValue(to_hstring(session)));
            result.Insert(L"generation",JValue::CreateStringValue(to_hstring(generation)));
            Windows::Data::Json::JsonArray projected;
            for(uint32_t i=0;i<count;++i) {
                Windows::Data::Json::JsonArray row;
                row.Append(JValue::CreateNumberValue(rows[i].presentation));
                row.Append(JValue::CreateStringValue(to_hstring(rows[i].draft)));
                row.Append(JValue::CreateStringValue(to_hstring(rows[i].browser)));
                row.Append(JValue::CreateBooleanValue(rows[i].selected));
                projected.Append(row);
            }
            result.Insert(L"rows",projected);
            const auto json=result.Stringify();
            if(json.size()>=capacity)return E_BOUNDS;
            memcpy(output,json.c_str(),(json.size()+1)*sizeof(wchar_t));
            if(pane.startupRender)pane.startupRender();
            return S_OK;
        }
        ShellStartupTransfer transfer{};
        // Observe the actual field before considering retirement. Text/composition
        // notifications may still be pending on this same owner thread.
        const bool observed=(operation!=14 && operation!=8 && operation!=6) || !pane.startupObserve || pane.startupObserve();
        if(!observed && operation==6)return E_PENDING;
        const auto status=shell_core_startup_transfer(pane.shell.core,!observed?0:(operation==12 || operation==16 || operation==18)?0:operation==17?15:operation==13?11:operation,context,session,
            generation,request,draft,browser,accepted,&transfer);
        // Ineligible placeholder adoption is a normal no-op, never a disconnect.
        if(status!=SHELL_STATUS_OK && operation==14) {
            if(shell_core_startup_transfer(pane.shell.core,0,0,0,0,0,0,0,false,&transfer)!=SHELL_STATUS_OK)
                return E_NOT_VALID_STATE;
            if(session!=transfer.session || generation!=transfer.generation || transfer.lost)return E_NOT_VALID_STATE;
        } else if(status!=SHELL_STATUS_OK)return E_NOT_VALID_STATE;
        if(operation==14 && (session!=transfer.session || !generation || generation!=transfer.generation || transfer.lost))
            return E_NOT_VALID_STATE;
        if(operation==16 || operation==18) {
            if(session!=transfer.session || !generation || generation!=transfer.generation || transfer.lost ||
                !pane.startupPresenter || !pane.addressSource || !GetPropW(hwnd,L"Fenn.InteractiveStartup"))return E_NOT_VALID_STATE;
            if(operation==16)check_bool(SetPropW(hwnd,L"Fenn.InteractiveStartupReady",reinterpret_cast<HANDLE>(1)));
        }
        if(operation==12) {
            if(session!=transfer.session || !generation || generation!=transfer.generation || transfer.lost)
                return E_NOT_VALID_STATE;
            check_hresult(BeginStartupPresenter(hwnd));
        }
        if(operation==1)pane.startupContext=context;
        JObject result;
        if(operation==18)result.Insert(L"normal",JValue::CreateBooleanValue(FinishStartupPresenter(hwnd)));
        auto put=[&](const wchar_t* name,uint64_t value){result.Insert(name,JValue::CreateStringValue(to_hstring(value)));};
        put(L"context",context);put(L"session",transfer.session);put(L"generation",transfer.generation);
        put(L"request",transfer.request);put(L"draft",transfer.draft);put(L"browser",transfer.browser);
        put(L"submission",transfer.submission);put(L"revision",transfer.revision);
        put(L"editorBrowser",transfer.editor_browser);
        result.Insert(L"adoptionReady",JValue::CreateBooleanValue(transfer.adoption_ready));
        result.Insert(L"selectionStart",JValue::CreateNumberValue(transfer.selection_start));
        result.Insert(L"selectionLength",JValue::CreateNumberValue(transfer.selection_length));
        result.Insert(L"kind",JValue::CreateNumberValue(transfer.kind));
        result.Insert(L"modifiers",JValue::CreateNumberValue(transfer.modifiers));
        result.Insert(L"mappings",JValue::CreateNumberValue(transfer.mappings));
        result.Insert(L"lost",JValue::CreateBooleanValue(transfer.lost));
        result.Insert(L"text",JValue::CreateStringValue(transfer.length ?
            hstring(reinterpret_cast<const wchar_t*>(transfer.text),uint32_t(transfer.length)) : hstring{}));
        const auto json=result.Stringify();
        if(json.size()>=capacity)return E_BOUNDS;
        memcpy(output,json.c_str(),(json.size()+1)*sizeof(wchar_t));
        if((operation==14 || operation==6) && pane.startupRender)pane.startupRender();
        if(operation==2) {
            if(GetPropW(hwnd,L"Fenn.InteractiveStartup"))check_bool(SetPropW(hwnd,L"Fenn.InteractiveStartupBound",reinterpret_cast<HANDLE>(1)));
            NotifyStartup(pane);
        }
        return S_OK;
    } catch (...) {
        const auto error=to_hresult();
#ifdef WINUI_PANE_TESTING
        fprintf(stderr,"Startup transfer failed: %08x\n",unsigned(error));
#endif
        return error;
    }
}
