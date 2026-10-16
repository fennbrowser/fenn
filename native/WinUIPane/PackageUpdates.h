#pragma once
#include <memory>
#include <map>
#include <functional>
#include <string>
#include <shobjidl.h>
#include <shlwapi.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Services.Store.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#pragma comment(lib, "shlwapi.lib")

// OS servicing stays outside ShellCore. Use an owned WinUI window: dialogs
// within the toolbar's XamlRoot would be confined to its 40-DIP island.
struct NativePackageUpdates : std::enable_shared_from_this<NativePackageUpdates> {
    using Labels = std::map<std::wstring, winrt::hstring>;
    using Action = winrt::Windows::Foundation::IAsyncAction;
    enum class Source { None, Store, Feed };
    struct Result {
        Source source{Source::None};
        bool available{};
        bool revoked{};
        std::wstring status{L"unavailable"};
        winrt::Windows::Foundation::Uri feed{nullptr};
    };
    static Source Select(bool storeSigned, bool hasFeed) {
        return storeSigned ? Source::Store : hasFeed ? Source::Feed : Source::None;
    }
    static Action Query(HWND owner, std::shared_ptr<Result> result) {
        using namespace winrt::Windows;
        try {
            auto package = ApplicationModel::Package::Current();
            const bool store = package.SignatureKind() == ApplicationModel::PackageSignatureKind::Store;
            auto info = store ? ApplicationModel::AppInstallerInfo(nullptr) : package.GetAppInstallerInfo();
            result->source = Select(store, bool(info));
            if (result->source == Source::Store) {
                auto context = Services::Store::StoreContext::GetDefault();
                winrt::check_hresult(context.as<IInitializeWithWindow>()->Initialize(owner));
                auto updates = co_await context.GetAppAndOptionalStorePackageUpdatesAsync();
                result->available = updates.Size() != 0;
                result->status = result->available ? L"available-store" : L"current";
            } else if (result->source == Source::Feed) {
                result->feed = info.Uri();
                if (result->feed.SchemeName() != L"file" && result->feed.SchemeName() != L"https")
                    winrt::throw_hresult(E_INVALIDARG);
                auto update = co_await package.CheckUpdateAvailabilityAsync();
                using Availability = ApplicationModel::PackageUpdateAvailability;
                switch (update.Availability()) {
                case Availability::Available:
                case Availability::Required:
                    result->available = true; result->status = L"available-feed"; break;
                case Availability::NoUpdates: result->status = L"current"; break;
                default: result->status = L"failed"; break;
                }
            }
        } catch (winrt::hresult_error const& error) {
            result->status = error.code() == HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE)
                ? L"unavailable" : L"failed";
        }
    }
    static Action Launch(std::shared_ptr<Result> result) {
        using namespace winrt::Windows;
        if (result->revoked) co_return;
        bool opened = false;
        if (result->source == Source::Store) {
            opened = co_await System::Launcher::LaunchUriAsync(Foundation::Uri(L"ms-windows-store://downloadsandupdates"));
        } else if (result->source == Source::Feed && result->feed) {
            auto feed = result->feed;
            if (feed.SchemeName() == L"file") {
                wchar_t path[32768];
                DWORD length = ARRAYSIZE(path);
                winrt::check_hresult(PathCreateFromUrlW(feed.AbsoluteUri().c_str(), path, &length, 0));
                auto file = co_await Storage::StorageFile::GetFileFromPathAsync(path);
                if (result->revoked) co_return;
                opened = co_await System::Launcher::LaunchFileAsync(file);
            } else if (feed.SchemeName() == L"https") {
                // ms-appinstaller: is disabled by default. Use the supported
                // download/open flow for the registered feed file instead.
                opened = co_await System::Launcher::LaunchUriAsync(feed);
            }
        }
        if (!opened) winrt::throw_hresult(E_FAIL);
    }

    HWND owner{};
    bool running{};
    uint64_t generation{};
    winrt::Microsoft::UI::Xaml::Window window{nullptr};
    std::shared_ptr<Result> current;
    // Native tests inject OS operations without installing packages.
    std::function<Action(HWND, std::shared_ptr<Result>)> query = Query;
    std::function<Action(std::shared_ptr<Result>)> launch = Launch;
    void Cancel() {
        ++generation;
        running = false;
        if (current) current->revoked = true;
        current.reset();
        auto old = window;
        window = nullptr;
        if (old) old.Close();
    }
    static winrt::hstring Text(Labels const& labels, wchar_t const* key) {
        const auto found = labels.find(key);
        return found == labels.end() ? winrt::hstring() : found->second;
    }
    static winrt::fire_and_forget Handoff(std::shared_ptr<NativePackageUpdates> self,
        uint64_t epoch, std::shared_ptr<Result> result, Labels labels,
        winrt::Microsoft::UI::Xaml::Controls::TextBlock status,
        winrt::Microsoft::UI::Xaml::Controls::Button button) {
        if (self->generation != epoch || !self->window) co_return;
        button.IsEnabled(false);
        try { co_await self->launch(result); }
        catch (...) {
            if (self->generation == epoch && self->window) {
                status.Text(Text(labels, L"failed"));
                button.IsEnabled(true);
            }
        }
    }
    static winrt::fire_and_forget Check(std::shared_ptr<NativePackageUpdates> self, Labels labels) {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace Controls;
        if (self->running || Text(labels, L"title").empty()) co_return;
        self->running = true;
        const auto epoch = ++self->generation;
        try {
            Window window;
            self->window = window;
            window.Title(Text(labels, L"title"));
            auto appWindow = window.AppWindow();
            const auto hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(appWindow.Id());
            SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(self->owner));
            appWindow.Resize({560, 320});
            StackPanel panel;
            panel.Padding({24, 24, 24, 24});
            panel.Spacing(16);
            Automation::AutomationProperties::SetAutomationId(panel, L"FennUpdateDialog");
            TextBlock status;
            status.TextWrapping(TextWrapping::Wrap);
            status.Text(Text(labels, L"checking"));
            Automation::AutomationProperties::SetAutomationId(status, L"FennUpdateStatus");
            Automation::AutomationProperties::SetLiveSetting(status, Automation::Peers::AutomationLiveSetting::Polite);
            panel.Children().Append(status);
            Button primary;
            primary.Visibility(Visibility::Collapsed);
            Automation::AutomationProperties::SetAutomationId(primary, L"FennUpdateOpen");
            panel.Children().Append(primary);
            Button close;
            close.Content(winrt::box_value(Text(labels, L"close")));
            Automation::AutomationProperties::SetAutomationId(close, L"FennUpdateClose");
            panel.Children().Append(close);
            auto weak = std::weak_ptr<NativePackageUpdates>(self);
            close.Click([weak](auto const&, auto const&) { if (auto state = weak.lock()) state->Cancel(); });
            panel.KeyDown([weak](auto const&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& event) {
                if (event.Key() == winrt::Windows::System::VirtualKey::Escape) {
                    event.Handled(true);
                    if (auto state = weak.lock()) state->Cancel();
                }
            });
            window.Closed([weak, epoch](auto const&, auto const&) {
                if (auto state = weak.lock(); state && state->generation == epoch) {
                    if (state->current) state->current->revoked = true;
                    state->current.reset();
                    ++state->generation; state->running = false; state->window = nullptr;
                }
            });
            auto scroll = Markup::XamlReader::Load(
                LR"(<ScrollViewer xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource ApplicationPageBackgroundThemeBrush}"/>)").as<ScrollViewer>();
            scroll.Content(panel);
            scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            window.Content(scroll);
            window.Activate();
            close.Focus(FocusState::Programmatic);
            auto result = std::make_shared<Result>();
            self->current = result;
            try { co_await self->query(hwnd, result); }
            catch (...) { result->available = false; result->status = L"failed"; }
            if (self->generation != epoch || !self->window) co_return;
            status.Text(Text(labels, result->status.c_str()));
            if (result->available) {
                primary.Content(winrt::box_value(Text(labels,
                    result->source == Source::Store ? L"open-store" : L"open-feed")));
                primary.Visibility(Visibility::Visible);
                primary.Click([weak, epoch, result, labels, status](auto const& sender, auto const&) {
                    if (auto state = weak.lock()) Handoff(state, epoch, result, labels, status, sender.template as<Button>());
                });
            }
        } catch (...) {
            if (self->generation == epoch) self->Cancel();
        }
    }
};
