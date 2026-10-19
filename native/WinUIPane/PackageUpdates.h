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
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Dispatching.h>
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
        bool ready{};
        std::function<void()> progress;
        winrt::Windows::Services::Store::StoreContext store{nullptr};
        winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Services::Store::StorePackageUpdate> packages{nullptr};
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
                result->store = context;
                result->packages = co_await context.GetAppAndOptionalStorePackageUpdatesAsync();
                result->available = result->packages.Size() != 0;
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
    static void ValidateUri(winrt::Windows::Foundation::Uri const& uri, bool localFeed) {
        if ((uri.SchemeName() != L"https" && !(localFeed && uri.SchemeName() == L"file")) ||
            !uri.UserName().empty() || !uri.Password().empty()) winrt::throw_hresult(E_INVALIDARG);
    }
    static uint64_t PackageVersionValue(winrt::Windows::ApplicationModel::PackageVersion const& version) {
        return (uint64_t(version.Major) << 48) | (uint64_t(version.Minor) << 32) |
               (uint64_t(version.Build) << 16) | version.Revision;
    }
    // The App Installer schema uses four UInt16 components, matching PackageId.Version.
    static uint64_t ReadPackageVersion(winrt::hstring const& text) {
        if (text.empty() || text.size() > 23) winrt::throw_hresult(E_INVALIDARG);
        uint64_t packed = 0;
        uint32_t value = 0, parts = 0;
        bool digit = false;
        for (auto character : text) {
            if (character == L'.') {
                if (!digit || parts == 3) winrt::throw_hresult(E_INVALIDARG);
                packed = (packed << 16) | value; value = 0; digit = false; ++parts;
            } else {
                if (character < L'0' || character > L'9') winrt::throw_hresult(E_INVALIDARG);
                value = value * 10 + uint32_t(character - L'0'); digit = true;
                if (value > 65535) winrt::throw_hresult(E_INVALIDARG);
            }
        }
        if (!digit || parts != 3) winrt::throw_hresult(E_INVALIDARG);
        return (packed << 16) | value;
    }
    static void DownloadStarted(std::shared_ptr<Result> const& result) {
        result->status = L"downloading";
        if (result->progress) result->progress();
    }
    // Keep the Windows cost flags independent: an unrestricted plan can still
    // report roaming or an exceeded allowance. The interface also lets the
    // native contract fixture cover provider-only states without faking a modem.
    static bool DeferFeedDownload(winrt::Windows::Networking::Connectivity::IConnectionCost const& cost) {
        using winrt::Windows::Networking::Connectivity::NetworkCostType;
        return cost.Roaming() || cost.OverDataLimit() ||
            cost.NetworkCostType() == NetworkCostType::Fixed ||
            cost.NetworkCostType() == NetworkCostType::Variable;
    }
    // These operations are Windows servicing APIs, not Firefox's updater.
    // Staging never registers the package or closes a running browser.
    static Action Download(HWND, std::shared_ptr<Result> result) {
        using namespace winrt::Windows;
        if (result->revoked || !result->available) co_return;
        if (result->source == Source::Store) {
            if (!result->store || !result->store.CanSilentlyDownloadStorePackageUpdates()) co_return;
            DownloadStarted(result);
            auto downloaded = co_await result->store.TrySilentDownloadStorePackageUpdatesAsync(result->packages);
            result->ready = downloaded.OverallState() == Services::Store::StorePackageUpdateState::Completed;
            if (!result->ready) winrt::throw_hresult(E_FAIL);
        } else if (result->source == Source::Feed) {
            const bool local = result->feed && result->feed.SchemeName() == L"file";
            ValidateUri(result->feed, local);
            Storage::Streams::IInputStream input{nullptr};
            if (!local) {
                auto profile = Networking::Connectivity::NetworkInformation::GetInternetConnectionProfile();
                if (profile) {
                    if (DeferFeedDownload(profile.GetConnectionCost())) co_return;
                }
            }
            Web::Http::Filters::HttpBaseProtocolFilter filter;
            filter.AllowAutoRedirect(false);
            Web::Http::HttpClient client(filter);
            if (local) {
                wchar_t path[32768]; DWORD length = ARRAYSIZE(path);
                winrt::check_hresult(PathCreateFromUrlW(result->feed.AbsoluteUri().c_str(), path, &length, 0));
                auto file = co_await Storage::StorageFile::GetFileFromPathAsync(path);
                input = co_await file.OpenReadAsync();
            } else input = co_await client.GetInputStreamAsync(result->feed);
            Storage::Streams::DataReader reader(input);
            constexpr uint32_t limit = 65536;
            const auto size = co_await reader.LoadAsync(limit + 1);
            if (size == 0 || size > limit || result->revoked) winrt::throw_hresult(E_INVALIDARG);
            winrt::Windows::Data::Xml::Dom::XmlLoadSettings settings;
            settings.ProhibitDtd(true); settings.ResolveExternals(false); settings.MaxElementDepth(8);
            winrt::Windows::Data::Xml::Dom::XmlDocument document;
            document.LoadXmlFromBuffer(reader.ReadBuffer(size), settings);
            const auto root = document.DocumentElement();
            const auto ns = winrt::unbox_value_or<winrt::hstring>(root.NamespaceUri(), L"");
            if (winrt::unbox_value_or<winrt::hstring>(root.LocalName(), L"") != L"AppInstaller" ||
                (ns != L"http://schemas.microsoft.com/appx/appinstaller/2017" &&
                 ns != L"http://schemas.microsoft.com/appx/appinstaller/2017/2" &&
                 ns != L"http://schemas.microsoft.com/appx/appinstaller/2018" &&
                 ns != L"http://schemas.microsoft.com/appx/appinstaller/2021")) winrt::throw_hresult(E_INVALIDARG);
            auto package = ApplicationModel::Package::Current();
            auto main = document.SelectNodes(L"/*/*[local-name()='MainPackage' or local-name()='MainBundle']");
            if (main.Size() != 1) winrt::throw_hresult(E_INVALIDARG);
            auto entry = main.GetAt(0).as<winrt::Windows::Data::Xml::Dom::XmlElement>();
            if (entry.GetAttribute(L"Name") != package.Id().Name() ||
                entry.GetAttribute(L"Publisher") != package.Id().Publisher() ||
                ReadPackageVersion(entry.GetAttribute(L"Version")) <= PackageVersionValue(package.Id().Version()) ||
                winrt::unbox_value_or<winrt::hstring>(entry.NamespaceUri(), L"") != ns) winrt::throw_hresult(E_INVALIDARG);
            Foundation::Uri uri(entry.GetAttribute(L"Uri")); ValidateUri(uri, local);
            Management::Deployment::StagePackageOptions options;
            auto dependencies = document.SelectNodes(L"/*/*[local-name()='Dependencies']/*");
            if (dependencies.Size() > 32) winrt::throw_hresult(E_INVALIDARG);
            for (auto node : dependencies) {
                auto dependency = node.as<winrt::Windows::Data::Xml::Dom::XmlElement>();
                if (winrt::unbox_value_or<winrt::hstring>(dependency.LocalName(), L"") != L"Package" ||
                    winrt::unbox_value_or<winrt::hstring>(dependency.NamespaceUri(), L"") != ns) winrt::throw_hresult(E_INVALIDARG);
                Foundation::Uri source(dependency.GetAttribute(L"Uri")); ValidateUri(source, local);
                options.DependencyPackageUris().Append(source);
            }
            if (result->revoked) co_return;
            DownloadStarted(result);
            auto staged = co_await Management::Deployment::PackageManager().StagePackageByUriAsync(uri, options);
            winrt::check_hresult(staged.ExtendedErrorCode());
            result->ready = true;
        }
    }
    static Action Install(HWND owner, std::shared_ptr<Result> result) {
        using namespace winrt::Windows;
        if (result->revoked || !result->ready) co_return;
        // Firefox registers its original restart arguments, including profile
        // selection. Never replace them with an empty command line.
        wchar_t arguments[RESTART_MAX_CMD_LINE]{};
        DWORD length = RESTART_MAX_CMD_LINE, flags = 0;
        winrt::check_hresult(GetApplicationRestartSettings(GetCurrentProcess(), arguments, &length, &flags));
        if (!arguments[0] || (flags & RESTART_NO_PATCH)) winrt::throw_hresult(E_FAIL);
        if (result->source == Source::Store) {
            auto store = Services::Store::StoreContext::GetDefault();
            winrt::check_hresult(store.as<IInitializeWithWindow>()->Initialize(owner));
            if (!store.CanSilentlyDownloadStorePackageUpdates()) winrt::throw_hresult(E_ACCESSDENIED);
            auto installed = co_await store.TrySilentDownloadAndInstallStorePackageUpdatesAsync(result->packages);
            if (installed.OverallState() != Services::Store::StorePackageUpdateState::Completed) winrt::throw_hresult(E_FAIL);
        } else if (result->source == Source::Feed) {
            auto installed = co_await Management::Deployment::PackageManager().AddPackageByAppInstallerFileAsync(
                result->feed, Management::Deployment::AddPackageByAppInstallerOptions::ForceTargetAppShutdown, nullptr);
            winrt::check_hresult(installed.ExtendedErrorCode());
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
    bool checking{};
    bool manualCheck{};
    bool showingResult{};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer resultTimer{nullptr};
    winrt::event_token resultTick{};
    bool installing{};
    Labels windowLabels;
    winrt::Microsoft::UI::Xaml::Controls::TextBlock statusControl{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button primaryControl{nullptr};
    std::function<void()> changed;
    // Firefox must flush and persist its session before Windows can stop it.
    std::function<Action()> prepare;
    std::function<void()> installFailed;
    uint64_t generation{};
    winrt::Microsoft::UI::Xaml::Window window{nullptr};
    std::shared_ptr<Result> current;
    // Native tests inject OS operations without installing packages.
    std::function<Action(HWND, std::shared_ptr<Result>)> query = Query;
    std::function<Action(std::shared_ptr<Result>)> launch = Launch;
    std::function<Action(HWND, std::shared_ptr<Result>)> download = Download;
    std::function<Action(HWND, std::shared_ptr<Result>)> install = Install;
    void Refresh() {
        using namespace winrt::Microsoft::UI::Xaml;
        if (window && current && statusControl && primaryControl) {
            statusControl.Text(Text(windowLabels, current->status.c_str()));
            primaryControl.Visibility(current->available ? Visibility::Visible : Visibility::Collapsed);
            primaryControl.IsEnabled(!checking && !installing);
            primaryControl.Content(winrt::box_value(Text(windowLabels, current->ready ? L"install" :
                current->source == Source::Store ? L"open-store" : L"open-feed")));
        }
        if (changed) changed();
    }
    void DismissWindow() {
        auto old = window;
        window = nullptr; running = false;
        statusControl = nullptr; primaryControl = nullptr;
        if (old) old.Close();
    }
    static winrt::fire_and_forget Probe(std::shared_ptr<NativePackageUpdates> self, bool manual = false) {
        if (self->checking) {
            // An explicit click can expose an already-running silent check.
            // Keep the same OS operation instead of starting another one.
            if (manual && !self->manualCheck) { self->manualCheck = true; self->Refresh(); }
            co_return;
        }
        if (self->showingResult || self->installing || (self->current && self->current->ready)) co_return;
        self->manualCheck = manual;
        const auto epoch = ++self->generation;
        auto result = std::make_shared<Result>();
        result->status = L"checking";
        self->current = result; self->checking = true; self->Refresh();
        try {
            co_await self->query(self->owner, result);
            if (self->generation != epoch || result->revoked) co_return;
            if (result->available) {
                const auto fallback = result->status;
                result->progress = [weak = std::weak_ptr<NativePackageUpdates>(self), epoch] {
                    if (auto state = weak.lock(); state && state->generation == epoch) state->Refresh();
                };
                co_await self->download(self->owner, result);
                result->status = result->ready ? L"ready" : fallback;
            }
        } catch (...) { result->ready = false; result->status = L"failed"; }
        if (self->generation != epoch || result->revoked) co_return;
        self->checking = false;
        self->showingResult = self->manualCheck && !result->ready;
        self->Refresh();
        if (self->showingResult) {
            self->resultTimer = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
            self->resultTimer.Interval(std::chrono::seconds(3));
            self->resultTimer.IsRepeating(false);
            self->resultTick = self->resultTimer.Tick([weak = std::weak_ptr<NativePackageUpdates>(self), epoch](auto const&, auto const&) {
                if (auto state = weak.lock(); state && state->generation == epoch) {
                    state->StopResultTimer();
                    state->showingResult = false;
                    state->Refresh();
                }
            });
            self->resultTimer.Start();
        }
    }
    static winrt::fire_and_forget InstallReady(std::shared_ptr<NativePackageUpdates> self) {
        if (!self->current || !self->current->ready || self->checking || self->installing) co_return;
        const auto epoch = self->generation;
        auto result = self->current;
        self->installing = true; result->status = L"installing"; self->Refresh();
        try {
            if (!self->prepare) winrt::throw_hresult(E_ACCESSDENIED);
            co_await self->prepare();
            if (self->generation != epoch || result->revoked) co_return;
            co_await self->install(self->owner, result);
            if (self->generation != epoch || result->revoked) co_return;
            result->ready = false; result->available = false; result->status = L"current";
        } catch (...) { if (self->generation == epoch) {
            result->status = L"failed";
            if (self->installFailed) self->installFailed();
        } }
        if (self->generation == epoch) { self->installing = false; self->Refresh(); }
    }
    void StopResultTimer() {
        if (!resultTimer) return;
        resultTimer.Stop();
        resultTimer.Tick(resultTick);
        resultTimer = nullptr;
    }
    void Cancel() {
        StopResultTimer();
        if (installing && installFailed) installFailed();
        ++generation;
        running = false; checking = false; manualCheck = false; showingResult = false; installing = false;
        if (current) current->revoked = true;
        current.reset();
        DismissWindow();
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
            close.Click([weak](auto const&, auto const&) { if (auto state = weak.lock()) state->DismissWindow(); });
            panel.KeyDown([weak](auto const&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& event) {
                if (event.Key() == winrt::Windows::System::VirtualKey::Escape) {
                    event.Handled(true);
                    if (auto state = weak.lock()) state->DismissWindow();
                }
            });
            window.Closed([weak](auto const& sender, auto const&) {
                if (auto state = weak.lock(); state && state->window == sender) {
                    state->window = nullptr; state->running = false;
                    state->statusControl = nullptr; state->primaryControl = nullptr;
                }
            });
            auto scroll = Markup::XamlReader::Load(
                LR"(<ScrollViewer xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource ApplicationPageBackgroundThemeBrush}"/>)").as<ScrollViewer>();
            scroll.Content(panel);
            scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            window.Content(scroll);
            window.Activate();
            close.Focus(FocusState::Programmatic);
            self->windowLabels = labels;
            self->statusControl = status; self->primaryControl = primary;
            primary.Click([weak](auto const&, auto const&) {
                if (auto state = weak.lock(); state && state->current) {
                    if (state->current->ready) InstallReady(state);
                    else Handoff(state, state->generation, state->current, state->windowLabels,
                        state->statusControl, state->primaryControl);
                }
            });
            self->Refresh();
            Probe(self);
        } catch (...) { self->DismissWindow(); }
    }
};
