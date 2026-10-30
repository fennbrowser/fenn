#pragma once

// Presentation-only cache written by Firefox for the resolved profile. It never
// supplies commands, settings, profile identity or browser state.
namespace NativeStartupLabels {
inline winrt::Windows::Data::Json::JsonObject Parse(winrt::hstring const& json) {
    if(json.empty() || json.size()>32768)winrt::throw_hresult(E_INVALIDARG);
    auto data=winrt::Windows::Data::Json::JsonObject::Parse(json);
    const auto version=data.GetNamedNumber(L"version");
    if(version!=1 && version!=2)winrt::throw_hresult(E_INVALIDARG);
    if(version==2) {
        (void)data.GetNamedBoolean(L"rtl");
        const auto locale=data.GetNamedString(L"locale");
        if(locale.empty() || locale.size()>128 || std::wstring_view(locale).find(L'\0')!=std::wstring::npos)winrt::throw_hresult(E_INVALIDARG);
    } else {
        data.Insert(L"rtl",winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(false));
    }
    auto labels=data.GetNamedObject(L"labels");
    for(auto key:{L"newTab",L"pending",L"full",L"composition",L"address",L"unavailable",L"recovery",L"recoveryAction"}) {
        auto text=labels.GetNamedString(key);
        if(text.empty() || text.size()>1024 || std::wstring_view(text).find(L'\0')!=std::wstring::npos)winrt::throw_hresult(E_INVALIDARG);
    }
    return data;
}
inline winrt::Windows::Data::Json::JsonObject Read(const wchar_t* profile) noexcept {
    try {
        if(!profile || !*profile)return nullptr;
        const auto path=std::wstring(profile)+L"\\fenn-startup-labels.json";
        HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return nullptr;
        struct Close {HANDLE value;~Close(){CloseHandle(value);}} close{file};
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(file,&size) || size.QuadPart<=0 || size.QuadPart>65536)return nullptr;
        std::string bytes(static_cast<size_t>(size.QuadPart),'\0');DWORD read{};
        if(!ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr) || read!=bytes.size())return nullptr;
        return Parse(winrt::to_hstring(bytes));
    } catch(...) {return nullptr;}
}
}
