#pragma once
#include <shobjidl_core.h>
#include <functional>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.h>

// Only OS presentation lives here. Firefox supplies already-authorized page data.
// The owner revokes the callback before destroying its pane; a late DataRequested
// also revalidates the document/session and bounded request deadline.
struct NativeWindowsShare {
    winrt::Windows::ApplicationModel::DataTransfer::DataTransferManager manager{nullptr};
    winrt::event_token requested{};
    bool subscribed{};
    void Close() noexcept {
        if(manager && subscribed) {try{manager.DataRequested(requested);}catch(...) {}}
        subscribed=false;manager=nullptr;
    }
    ~NativeWindowsShare(){Close();}
    HRESULT Open(HWND owner,winrt::hstring title,winrt::hstring url,std::function<bool()> current) noexcept {
        Close();
        try {
            using namespace winrt::Windows::ApplicationModel::DataTransfer;
            auto interop=winrt::get_activation_factory<DataTransferManager,IDataTransferManagerInterop>();
            winrt::check_hresult(interop->GetForWindow(owner,winrt::guid_of<DataTransferManager>(),winrt::put_abi(manager)));
            requested=manager.DataRequested([title,url,current](auto const&,DataRequestedEventArgs const& args) {
                if(!current())return;
                auto data=args.Request().Data();
                data.Properties().Title(title);
                data.SetText(L"");
                data.SetWebLink(winrt::Windows::Foundation::Uri(url));
            });
            subscribed=true;
            winrt::check_hresult(interop->ShowShareUIForWindow(owner));
            return S_OK;
        }catch(...){const auto hr=winrt::to_hresult();Close();return hr;}
    }
};
