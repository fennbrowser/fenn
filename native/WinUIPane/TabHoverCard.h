#pragma once
#include <chrono>
#include <functional>
#include <map>
#include "JsonValidation.h"
#include "ShellCore.h"

// The tab hover card: Firefox's tab preview presented in a stock Flyout. It
// shows the title, the site's registrable domain, the row's container/group
// detail and a thumbnail of the page, centered under the tab wherever the
// pointer is on it.
//
// A ToolTip was the first choice, but a pointer-opened ToolTip positions
// itself from the pointer, not its owner, and has no SystemBackdrop. A
// transient Flyout shown at the row with Bottom placement anchors to the row,
// takes no focus, hosts Desktop Acrylic itself, and lets input through to the
// strip. Opening is therefore this card's job: a hover delay, reopening at
// once while moving between tabs (as Firefox's preview does), and closing on
// exit, press, drag or removal.
//
// Firefox owns the domain and the thumbnail. Hovering a row asks for them
// with one coalescing request; the answer is bound to that request number, so
// an answer for a tab the pointer has already left is validated and dropped.
// Thumbnails are released when a card closes rather than kept per tab.
struct TabHoverCard {
    static constexpr double kPreviewWidth = 280, kPreviewHeight = 140;
    // Device pixels of one thumbnail: 280x140 DIPs up to 1.5x, scaled down
    // beyond that. Hex-encoded that stays under the 1 MiB publication limit.
    static constexpr NativeJson::ImageLimits kPreviewLimits{640, 100000};
    // Firefox's ZERO_DELAY_ACTIVATION_TIME: hovering another tab this soon
    // after a card closed opens its card without waiting again.
    static constexpr std::chrono::milliseconds kReshowWindow{300};

    // Queues a strip-preview request for (tab, request, rasterization scale).
    std::function<void(uint32_t, uint64_t, double)> send;
    // True while a strip gesture owns the pointer; no card opens then.
    std::function<bool()> suppressed;
    // Input on this element passes through an open card's dismissal layer.
    UIElement passThrough{nullptr};

    // The strip's root: its input passes through an open card, and any key
    // pressed in it closes the card, so a keyboard-invoked menu never meets one.
    void Bind(UIElement const& root) {
        passThrough = root;
        root.AddHandler(UIElement::KeyDownEvent(),
            box_value(Microsoft::UI::Xaml::Input::KeyEventHandler(
                [this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                    if (alive.expired()) return;
                    if (waiting) Hide(waiting);
                    if (shown) Hide(shown);
                })),
            true);
    }

    struct Card {
        Controls::Flyout flyout{nullptr};
        Controls::TextBlock title{nullptr}, domain{nullptr}, detail{nullptr};
        Controls::Border frame{nullptr};
        Controls::Image image{nullptr};
        weak_ref<Controls::TabViewItem> row;
    };
    std::map<uint32_t, Card> cards;
    // The row whose card is wanted, and the request its answer must name.
    uint32_t tab{};
    uint64_t pending{};
    ShellCore* core{};
    bool open{};
    // The row whose card is showing, and the one waiting out the hover delay.
    uint32_t shown{}, waiting{};
    // A pressed row shows no card until the pointer really leaves it, as
    // Firefox's preview does after a click.
    uint32_t pressed{};
    std::chrono::steady_clock::time_point closedAt{};
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    Style presenterStyle{nullptr};
    // The timer can outlive the strip in the dispatcher queue.
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    ~TabHoverCard() {
        CancelRequest();
        lifetime.reset();
        if (timer) timer.Stop();
        try {
            if (auto found = cards.find(shown); found != cards.end()) found->second.flyout.Hide();
        } catch (...) { /* The island may already be closing. */ }
    }

    // The presenter is transparent so the Flyout's own SystemBackdrop shows;
    // its stock border and shadow stay. High contrast covers the acrylic with
    // the system window color, since the backdrop's fallback is not a
    // contrast color.
    Card Create() {
        if (!presenterStyle) {
            presenterStyle = Markup::XamlReader::Load(LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="FlyoutPresenter">
  <Setter Property="Background" Value="Transparent"/>
  <Setter Property="Padding" Value="0"/>
  <Setter Property="MinWidth" Value="0"/>
  <Setter Property="MinHeight" Value="0"/>
  <Setter Property="MaxWidth" Value="320"/>
  <Setter Property="CornerRadius" Value="8"/>
  <Setter Property="IsTabStop" Value="False"/>
  <!-- The card is display-only. When it overlaps the row (pointer near the
       row's bottom edge) it must not take the pointer: that exits the row,
       closes the card, re-enters and reopens it, in a loop. -->
  <Setter Property="IsHitTestVisible" Value="False"/>
  <Setter Property="ScrollViewer.HorizontalScrollBarVisibility" Value="Disabled"/>
  <Setter Property="ScrollViewer.VerticalScrollBarVisibility" Value="Disabled"/>
</Style>)").as<Style>();
        }
        Card card;
        auto content = Markup::XamlReader::Load(LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
  <Grid.Resources>
    <ResourceDictionary>
      <ResourceDictionary.ThemeDictionaries>
        <ResourceDictionary x:Key="Default"><SolidColorBrush x:Key="FirefoxHoverCardFill" Color="Transparent"/></ResourceDictionary>
        <ResourceDictionary x:Key="Light"><SolidColorBrush x:Key="FirefoxHoverCardFill" Color="Transparent"/></ResourceDictionary>
        <ResourceDictionary x:Key="HighContrast"><StaticResource x:Key="FirefoxHoverCardFill" ResourceKey="SystemColorWindowColorBrush"/></ResourceDictionary>
      </ResourceDictionary.ThemeDictionaries>
    </ResourceDictionary>
  </Grid.Resources>
  <Border CornerRadius="7" Padding="9,6,9,8" Background="{ThemeResource FirefoxHoverCardFill}">
  <StackPanel Width="280" Spacing="2">
    <TextBlock FontWeight="SemiBold" TextWrapping="Wrap" MaxLines="2" TextTrimming="CharacterEllipsis"/>
    <TextBlock FontSize="12" Foreground="{ThemeResource TextFillColorSecondaryBrush}" TextTrimming="CharacterEllipsis" Visibility="Collapsed"/>
    <TextBlock FontSize="12" Foreground="{ThemeResource TextFillColorSecondaryBrush}" TextTrimming="CharacterEllipsis" Visibility="Collapsed"/>
    <Border Margin="0,6,0,2" Width="280" Height="140" CornerRadius="4" Background="{ThemeResource ControlFillColorSecondaryBrush}" Visibility="Collapsed" AutomationProperties.AccessibilityView="Raw">
      <Image Stretch="UniformToFill" HorizontalAlignment="Center" VerticalAlignment="Top" AutomationProperties.AccessibilityView="Raw"/>
    </Border>
  </StackPanel>
  </Border>
</Grid>)").as<Controls::Grid>();
        auto panel = content.Children().GetAt(0).as<Controls::Border>().Child().as<Controls::StackPanel>();
        card.title = panel.Children().GetAt(0).as<Controls::TextBlock>();
        card.domain = panel.Children().GetAt(1).as<Controls::TextBlock>();
        card.detail = panel.Children().GetAt(2).as<Controls::TextBlock>();
        card.frame = panel.Children().GetAt(3).as<Controls::Border>();
        card.image = card.frame.Child().as<Controls::Image>();
        card.flyout = Controls::Flyout();
        card.flyout.Content(content);
        card.flyout.FlyoutPresenterStyle(presenterStyle);
        card.flyout.Placement(Controls::Primitives::FlyoutPlacementMode::Bottom);
        card.flyout.ShowMode(Controls::Primitives::FlyoutShowMode::Transient);
        card.flyout.ShouldConstrainToRootBounds(false);
        card.flyout.LightDismissOverlayMode(Controls::LightDismissOverlayMode::Off);
        card.flyout.SystemBackdrop(Media::DesktopAcrylicBackdrop());
        return card;
    }

    static void SetText(Controls::TextBlock const& block, hstring const& text) {
        if (block.Text() != text) block.Text(text);
        block.Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
    }

    void CancelRequest() {
        if (core) shell_core_preview_dismiss(core, pending);
        tab = 0; pending = 0;
    }
    void Request(uint32_t id, Controls::TabViewItem const& row) {
        uint64_t request = 0;
        if (!core || shell_core_preview_begin(core, id, &request) != SHELL_STATUS_OK) {
            CancelRequest(); return;
        }
        tab = id; pending = request;
        const double scale = row.XamlRoot() ? row.XamlRoot().RasterizationScale() : 1.0;
        if (send) send(id, pending, scale);
    }

    static std::chrono::milliseconds HoverDelay() {
        UINT ms = 0;
        return SystemParametersInfoW(SPI_GETMOUSEHOVERTIME, 0, &ms, 0) && ms ? std::chrono::milliseconds(ms)
                                                                            : std::chrono::milliseconds(400);
    }

    void Show(uint32_t id) {
        waiting = 0;
        if (timer) timer.Stop();
        auto found = cards.find(id);
        if (found == cards.end() || (suppressed && suppressed())) return;
        auto row = found->second.row.get();
        if (!row || !row.XamlRoot() || !row.IsLoaded()) return;
        if (shown && shown != id) Hide(shown);
        // Like Firefox's preview, a card never opens over another popup: a
        // menu or panel the user opened takes precedence.
        for (auto popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(row.XamlRoot())) {
            auto presenter = popup.Child().try_as<Controls::FlyoutPresenter>();
            if (!presenter) return;
            // Any of this strip's own cards is not "another popup": a card
            // just hidden is still in the open list while it animates out,
            // and moving between tabs would otherwise silently open nothing.
            bool ours = false;
            for (auto const& [otherId, other] : cards)
                if (presenter.Content() == other.flyout.Content()) { ours = true; break; }
            if (!ours) return;
        }
        // Queuing can wake a host that republishes at once and drops rows, so
        // nothing from the map is held across the request.
        if (tab != id) Request(id, row);
        if (tab != id) return;
        found = cards.find(id);
        if (found == cards.end()) return;
        auto flyout = found->second.flyout;
        if (passThrough) flyout.OverlayInputPassThroughElement(passThrough);
        Controls::Primitives::FlyoutShowOptions options;
        options.Placement(Controls::Primitives::FlyoutPlacementMode::Bottom);
        options.ShowMode(Controls::Primitives::FlyoutShowMode::Transient);
        flyout.ShowAt(row, options);
        shown = id;
    }

    void Hide(uint32_t id) {
        if (tab == id) CancelRequest();
        if (waiting == id) {
            waiting = 0;
            if (timer) timer.Stop();
        }
        if (shown != id) {
            // A capture can finish during the hover delay, before any Flyout
            // opens. No Closed event will release that pending bitmap.
            if (auto found = cards.find(id); found != cards.end()) {
                found->second.image.Source(nullptr);
                found->second.frame.Visibility(Visibility::Collapsed);
            }
            return;
        }
        shown = 0;
        closedAt = std::chrono::steady_clock::now();
        if (auto found = cards.find(id); found != cards.end()) found->second.flyout.Hide();
    }

    // The pointer arrived on a row: open its card after
    // the system hover delay, or at once if a card is open or just closed.
    void Enter(uint32_t id, Controls::TabViewItem const& row) {
        if (!cards.count(id) || pressed == id || (suppressed && suppressed())) return;
        // Ask before the delay runs out, so the answer is usually here by the
        // time the card appears.
        if (tab != id) Request(id, row);
        if (tab != id) return;
        if (shown == id) return;
        if (shown || std::chrono::steady_clock::now() - closedAt < kReshowWindow) {
            Show(id);
            return;
        }
        waiting = id;
        if (!timer) {
            timer = row.DispatcherQueue().CreateTimer();
            timer.IsRepeating(false);
            timer.Tick([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (!alive.expired() && waiting) Show(waiting);
            });
        }
        timer.Stop();
        timer.Interval(HoverDelay());
        timer.Start();
    }

    void Attach(Controls::TabViewItem const& row, uint32_t id, std::weak_ptr<int> alive) {
        auto card = Create();
        card.row = make_weak(row);
        auto leave = [this, alive, id](auto&&...) {
            if (!alive.expired()) Hide(id);
        };
        row.PointerEntered([this, alive, id](auto const& sender, auto&&) {
            if (!alive.expired()) Enter(id, sender.template as<Controls::TabViewItem>());
        });
        // A menu's or card's dismissal layer also raises PointerExited while
        // the pointer stays on the row; only a real departure ends the press.
        row.PointerExited([this, alive, id](auto const& sender, auto const& args) {
            if (alive.expired()) return;
            auto element = sender.template as<FrameworkElement>();
            auto at = args.GetCurrentPoint(element).Position();
            const bool outside = at.X < 0 || at.Y < 0 || at.X >= element.ActualWidth() || at.Y >= element.ActualHeight();
            // The card's own dismissal layer (and a layout shift) raises
            // PointerExited with the pointer still on the row. Closing here
            // lost the card for good: no PointerEntered follows, so it never
            // reopened until the pointer left and came back.
            if (!outside) return;
            Hide(id);
            if (pressed == id) pressed = 0;
        });
        row.PointerCanceled(leave);
        row.PointerCaptureLost(leave);
        // A press starts a selection, a drag or a context menu; the card gets
        // out of the way and stays away. Reopening it at once here (the press
        // closed it within the reshow window) raced the row's context flyout,
        // whose opening the reopened card then light-dismissed.
        row.AddHandler(UIElement::PointerPressedEvent(),
            box_value(Microsoft::UI::Xaml::Input::PointerEventHandler([this, alive, id](auto&&, auto&&) {
                if (alive.expired()) return;
                pressed = id;
                Hide(id);
            })), true);
        card.flyout.Opened([this, alive, id](auto const&, auto const&) {
            if (!alive.expired() && cards.count(id)) open = true;
        });
        card.flyout.Closed([this, alive, id](auto const&, auto const&) {
            if (alive.expired() || !cards.count(id)) return;
            auto& closed = cards.at(id);
            closed.image.Source(nullptr);
            closed.frame.Visibility(Visibility::Collapsed);
            // A light dismissal is a close this card did not ask for.
            if (shown == id) { shown = 0; closedAt = std::chrono::steady_clock::now(); }
            if (tab == id) CancelRequest();
            if (!tab) open = false;
        });
        cards.insert_or_assign(id, card);
    }

    // Called with the row's current title and detail on every header update.
    void Update(Controls::TabViewItem const& row, uint32_t id, hstring const& title, hstring const& detail,
                std::weak_ptr<int> alive) {
        if (!cards.count(id)) Attach(row, id, alive);
        auto& card = cards.at(id);
        const bool retitled = card.title.Text() != title;
        SetText(card.title, title);
        SetText(card.detail, detail);
        // A new title usually means a navigation: refresh an open card's
        // domain and thumbnail the way Firefox's preview follows its tab.
        if (retitled && open && tab == id) Request(id, row);
    }

    // The strip reordered or replaced rows: an open or pending card was placed
    // for the old layout. Firefox likewise keeps previews away while tabs move.
    void Moved() {
        if (waiting) Hide(waiting);
        if (shown) Hide(shown);
    }

    void Forget(uint32_t id) {
        Hide(id);
        if (pressed == id) pressed = 0;
        cards.erase(id);
        if (tab == id) CancelRequest();
        if (!tab) open = false;
    }

    // {"tab":id,"request":n,"domain":"..","thumbnail":bool,"image":{..}?}
    // Fully validated before anything changes; a valid answer to a request
    // that is no longer current is ignored.
    void Apply(NativeJson::JsonObject const& answer) {
        const auto id = NativeJson::RequireId(answer, L"tab");
        const auto request = NativeJson::RequireRevision(answer, L"request");
        const auto domain = answer.GetNamedString(L"domain");
        NativeJson::RequireLength(domain, NativeJson::kMaxShortLabel);
        const bool thumbnail = answer.GetNamedBoolean(L"thumbnail");
        size_t pixels = 0;
        NativeJson::ValidateImage(answer, pixels, kPreviewLimits);
        const bool hasImage = answer.HasKey(L"image") &&
            answer.GetNamedValue(L"image").ValueType() != Windows::Data::Json::JsonValueType::Null;
        const auto fingerprint = answer.Stringify();
        const uint32_t phase = answer.HasKey(L"image") ? 2 : 1;
        bool apply = false;
        auto resolve = [&](bool commit) {
            check_hresult(shell_core_preview_publication(core, id, uint64_t(request), phase,
                reinterpret_cast<uint16_t const*>(fingerprint.c_str()), fingerprint.size(), commit, &apply)
                == SHELL_STATUS_OK ? S_OK : E_INVALIDARG);
        };
        resolve(false);
        if (!apply || !cards.count(id)) return;
        // Decode before commit: a failed allocation leaves acceptance unchanged.
        Media::Imaging::WriteableBitmap bitmap{nullptr};
        if (hasImage) bitmap = NativeJson::DecodeImage(answer.GetNamedObject(L"image"));
        resolve(true);
        if (!apply) return;
        auto& card = cards.at(id);
        SetText(card.domain, domain);
        if (hasImage) card.image.Source(bitmap);
        else if (!thumbnail) card.image.Source(nullptr);
        // Reserve the frame as soon as Firefox says a thumbnail is coming, so
        // the card does not grow when the capture lands.
        card.frame.Visibility(thumbnail || hasImage ? Visibility::Visible : Visibility::Collapsed);
    }
};
