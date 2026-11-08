#pragma once
#include <set>

// Presentation-only cache written by Firefox for the resolved profile. It never
// supplies commands, settings, profile identity or browser state.
namespace NativeStartupLabels {
inline winrt::Windows::Data::Json::JsonObject Parse(winrt::hstring const& json) {
    if(json.empty() || json.size()>32768)winrt::throw_hresult(E_INVALIDARG);
    auto data=winrt::Windows::Data::Json::JsonObject::Parse(json);
    const auto version=data.GetNamedNumber(L"version");
    if(version!=1 && version!=2 && version!=3)winrt::throw_hresult(E_INVALIDARG);
    if(version>=2) {
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
    if(version==3)for(auto key:{L"engineTitle",L"engineMessage",L"engineRetry",L"engineClose",L"engineWaiting",L"engineDraft"}) {
        auto text=labels.GetNamedString(key);
        if(text.empty() || text.size()>1024 || std::wstring_view(text).find(L'\0')!=std::wstring::npos)winrt::throw_hresult(E_INVALIDARG);
    }
    if(data.HasKey(L"toolbar")) {
        auto toolbar=data.GetNamedObject(L"toolbar");
        (void)toolbar.GetNamedBoolean(L"productMenu");
        for(auto key:{L"menuLabel",L"menuTooltip"}) {
            const auto text=toolbar.GetNamedString(key);
            if(text.size()>1024 || std::wstring_view(text).find(L'\0')!=std::wstring::npos)winrt::throw_hresult(E_INVALIDARG);
        }
        const auto controls=toolbar.GetNamedArray(L"controls");
        if(controls.Size()>11)winrt::throw_hresult(E_INVALIDARG);
        std::set<winrt::hstring> unique;
        for(auto value:controls) {
            auto control=value.GetString();
            if(control!=L"address" && control!=L"back" && control!=L"forward" && control!=L"reload" &&
               control!=L"profile" && control!=L"extensions" && control!=L"home" && control!=L"history" && control!=L"bookmarks")
                winrt::throw_hresult(E_INVALIDARG);
            if(!unique.insert(control).second)winrt::throw_hresult(E_INVALIDARG);
        }
        if(controls.Size() && !unique.contains(L"address"))winrt::throw_hresult(E_INVALIDARG);
    }
    return data;
}
// The first presentation to apply, and its direction. A full presentation (version, locale, rtl,
// labels) is applied as given. A direction-only hint (no "labels", early startup before Fluent is
// available) supplies the direction and uses the profile cache only if the cache was written for
// the same locale; a stale or missing cache leaves the native defaults. Without a presentation the
// profile cache applies as before.
inline winrt::Windows::Data::Json::JsonObject Resolve(const wchar_t* presentation, const wchar_t* profile, bool& rtl);
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
        // The optional toolbar is an inert preview, validated by the same
        // bounded parser as live initial presentation. No commands are restored.
        return Parse(winrt::to_hstring(bytes));
    } catch(...) {return nullptr;}
}
inline winrt::Windows::Data::Json::JsonObject Resolve(const wchar_t* presentation, const wchar_t* profile, bool& rtl) {
    rtl=false;
    if(!presentation) {
        auto cached=Read(profile);
        if(cached)rtl=cached.GetNamedBoolean(L"rtl",false);
        return cached;
    }
    const winrt::hstring json{presentation};
    if(json.empty() || json.size()>32768)winrt::throw_hresult(E_INVALIDARG);
    auto hint=winrt::Windows::Data::Json::JsonObject::Parse(json);
    if(hint.HasKey(L"labels")) {
        auto full=Parse(json);
        rtl=full.GetNamedBoolean(L"rtl",false);
        return full;
    }
    if(hint.GetNamedNumber(L"version")!=2)winrt::throw_hresult(E_INVALIDARG);
    rtl=hint.GetNamedBoolean(L"rtl");
    const auto locale=hint.GetNamedString(L"locale");
    if(locale.empty() || locale.size()>128)winrt::throw_hresult(E_INVALIDARG);
    auto cached=Read(profile);
    if(cached && cached.GetNamedString(L"locale",L"")==locale)return cached;
    return nullptr;
}
}
