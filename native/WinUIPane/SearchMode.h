#pragma once
#include <functional>
#include <winrt/Windows.Data.Json.h>
#include "JsonValidation.h"
#include "ShellCore.h"

// The search-mode switcher (button, flyout of engines/local providers, exit
// button). Presentation only: Firefox owns mode eligibility, ordering and
// switching. Rust tracks admitted mode requests and their priority over
// address actions until completion or local session reset.
struct NativeSearchMode {
    using JObject = winrt::Windows::Data::Json::JsonObject;
    using JValue = winrt::Windows::Data::Json::JsonValue;

    Microsoft::UI::Xaml::Controls::DropDownButton button;
    Microsoft::UI::Xaml::Controls::MenuFlyout flyout;
    Microsoft::UI::Xaml::Controls::Button exit;
    Microsoft::UI::Xaml::Controls::Image image;
    Microsoft::UI::Xaml::Controls::SymbolIcon symbol;
    Microsoft::UI::Xaml::FrameworkElement symbolHost{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock label;
    std::vector<Microsoft::UI::Xaml::Controls::MenuFlyoutItem> items;
    std::vector<winrt::hstring> ids, pixels;
    NativeShellCore& shell;
    winrt::hstring buttonPixels;
    double generation{-1}, applied{}, reveal{};
    bool revealPending{};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    // Provided by the owning NativeAddress: observe the pending text edit,
    // build a versioned envelope and return focus to
    // the address field. Kept as callbacks rather than a back-reference so
    // this struct has no dependency on NativeAddress's layout.
    std::function<void()> edit, focusField;
    std::function<JObject(wchar_t const*)> message;
    std::function<bool()> busy;
    // Queue admission records mode identity and cancels retained Enter in Rust.
    std::function<void(winrt::hstring const&)> dispatch;

    ~NativeSearchMode() {
        lifetime.reset();
        if (flyout.IsOpen()) flyout.Hide();
    }

    static void ValidateImage(JObject const& item, size_t& total) {
        NativeJson::ValidateImage(item, total, NativeJson::kIconImageLimits);
    }
    static Microsoft::UI::Xaml::Media::ImageSource DecodeImage(JObject const& item) {
        return NativeJson::DecodeImage(item.GetNamedObject(L"image"));
    }
    static Microsoft::UI::Xaml::Controls::Symbol IconFor(winrt::hstring const& symbolName) {
        using Microsoft::UI::Xaml::Controls::Symbol;
        if (symbolName == L"bookmarks") return Symbol::Favorite;
        if (symbolName == L"tabs") return Symbol::BrowsePhotos;
        if (symbolName == L"history") return Symbol::Clock;
        if (symbolName == L"actions") return Symbol::AllApps;
        if (symbolName == L"settings") return Symbol::Setting;
        if (symbolName == L"add") return Symbol::Add;
        return Symbol::Find;
    }

    void Queue(wchar_t const* kind, winrt::hstring const& id = L"") {
        if (busy()) return;
        edit();
        auto value = message(kind);
        value.SetNamedValue(L"generation", JValue::CreateNumberValue(generation));
        value.SetNamedValue(L"id", JValue::CreateStringValue(id));
        JObject intent;
        intent.SetNamedValue(L"ctrlKey", JValue::CreateBooleanValue((GetKeyState(VK_CONTROL) & 0x8000) != 0));
        intent.SetNamedValue(L"altKey", JValue::CreateBooleanValue((GetKeyState(VK_MENU) & 0x8000) != 0));
        intent.SetNamedValue(L"shiftKey", JValue::CreateBooleanValue((GetKeyState(VK_SHIFT) & 0x8000) != 0));
        intent.SetNamedValue(L"metaKey", JValue::CreateBooleanValue(false));
        value.SetNamedValue(L"intent", intent);
        auto action = value.Stringify();
        if (dispatch) dispatch(action);
    }

    void NewFlyout() {
        using namespace Microsoft::UI::Xaml;
        if (flyout.IsOpen()) flyout.Hide();
        flyout = Controls::MenuFlyout();
        button.Flyout(flyout);
        flyout.Opening([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired()) return;
            if (button.XamlRoot()) flyout.OverlayInputPassThroughElement(button.XamlRoot().Content());
            Queue(L"mode-open");
        });
        flyout.Opened([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired() && !items.empty()) items.front().Focus(FocusState::Programmatic);
        });
    }

    // Hides any open flyout and revokes local request tracking without disturbing the
    // rest of this switcher's displayed state. Used when the address surface
    // itself is disabled, a strict subset of Reset().
    void Disable() {
        if (flyout.IsOpen()) flyout.Hide();
        shell.ResetAddressMode();
    }
    // Full reset on a session change: nothing about the previous session's
    // mode selection is valid for a new one.
    void Reset() {
        Disable();
        generation = -1;
        applied = 0;
        reveal = 0;
        revealPending = false;
    }

    void Sync(JObject const& snapshot) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        const bool visible = snapshot.GetNamedBoolean(L"modeButtonVisible", true);
        if (!visible) {
            if (flyout.IsOpen()) flyout.Hide();
            if (button.FocusState() != FocusState::Unfocused || exit.FocusState() != FocusState::Unfocused)
                focusField();
        }
        button.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        if (!visible) revealPending = false;
        const auto nextReveal = snapshot.GetNamedNumber(L"modeReveal", 0);
        if (nextReveal != reveal) {
            reveal = nextReveal;
            revealPending = visible;
        }
        if (!snapshot.HasKey(L"modes") ||
            snapshot.GetNamedValue(L"modes").ValueType() == winrt::Windows::Data::Json::JsonValueType::Null) {
            button.IsEnabled(false);
            exit.Visibility(Visibility::Collapsed);
            if (flyout.IsOpen()) flyout.Hide();
            return;
        }
        auto modes = snapshot.GetNamedObject(L"modes");
        auto entries = modes.GetNamedArray(L"entries");
        auto nextGeneration = modes.GetNamedNumber(L"generation");
        if (generation != nextGeneration) {
            NewFlyout();
            items.clear();
            ids.clear();
            pixels.clear();
            generation = nextGeneration;
            bool local = false, offered = false;
            for (auto value : entries) {
                auto entry = value.GetObject();
                auto symbolName = entry.GetNamedString(L"symbol", L"");
                if ((!local && entry.GetNamedBoolean(L"local", false)) || (symbolName == L"add" && !offered) ||
                    (symbolName == L"settings" && !offered)) {
                    flyout.Items().Append(MenuFlyoutSeparator());
                    local = true;
                }
                offered = offered || symbolName == L"add";
                MenuFlyoutItem item;
                item.Text(entry.GetNamedString(L"label"));
                auto id = entry.GetNamedString(L"id");
                item.Click([this, alive = std::weak_ptr<int>(lifetime), id, nextGeneration](auto const&, auto const&) {
                    if (!alive.expired() && generation == nextGeneration) Queue(L"mode-pick", id);
                });
                Automation::AutomationProperties::SetAutomationId(item, id);
                flyout.Items().Append(item);
                items.push_back(item);
                ids.push_back(id);
                pixels.push_back(L"");
            }
        }
        for (uint32_t i = 0; i < entries.Size(); ++i) {
            auto entry = entries.GetAt(i).GetObject();
            auto encoded = entry.GetNamedValue(L"image", JValue::CreateNullValue()).Stringify();
            if (pixels[i] != encoded) {
                if (encoded != L"null") {
                    ImageIcon icon;
                    icon.Source(DecodeImage(entry));
                    items[i].Icon(icon);
                } else {
                    SymbolIcon icon;
                    icon.Symbol(IconFor(entry.GetNamedString(L"symbol", L"search")));
                    items[i].Icon(icon);
                }
                pixels[i] = encoded;
            }
        }
        auto encoded = modes.GetNamedValue(L"image", JValue::CreateNullValue()).Stringify();
        if (buttonPixels != encoded) {
            image.Source(encoded == L"null" ? nullptr : DecodeImage(modes));
            buttonPixels = encoded;
        }
        image.Visibility(encoded == L"null" ? Visibility::Collapsed : Visibility::Visible);
        symbol.Visibility(encoded == L"null" ? Visibility::Visible : Visibility::Collapsed);
        if (symbolHost) symbolHost.Visibility(symbol.Visibility());
        symbol.Symbol(IconFor(modes.GetNamedString(L"symbol", L"search")));
        auto modesLabel = modes.GetNamedString(L"label");
        Automation::AutomationProperties::SetName(button, modes.GetNamedString(L"buttonLabel", modesLabel));
        Automation::AutomationProperties::SetName(exit, modes.GetNamedString(L"exitLabel", L"Exit search mode"));
        label.Text(modes.GetNamedBoolean(L"active") ? modesLabel : L"");
        label.Visibility(label.Text().empty() ? Visibility::Collapsed : Visibility::Visible);
        label.MaxWidth(80);
        label.TextTrimming(TextTrimming::CharacterEllipsis);
        exit.Visibility(visible && snapshot.GetNamedBoolean(L"modeExitVisible", modes.GetNamedBoolean(L"active"))
                            ? Visibility::Visible
                            : Visibility::Collapsed);
        button.IsEnabled(true);
        const double nextApplied = modes.GetNamedNumber(L"applied", 0);
        if (applied != nextApplied) {
            applied = nextApplied;
            focusField();
        }
    }

    // Shows the flyout if Sync() left a reveal request pending. Called
    // unconditionally after Sync() so the check stays internal to this
    // struct rather than duplicated at each of NativeAddress's callers.
    void ShowPendingReveal() {
        if (revealPending && button.IsEnabled() && button.Visibility() == Microsoft::UI::Xaml::Visibility::Visible) {
            revealPending = false;
            flyout.ShowAt(button);
        }
    }

    explicit NativeSearchMode(NativeShellCore& core) : shell(core) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        NewFlyout();
        button.IsEnabled(false);
        button.Padding({4, 0, 4, 0});
        Automation::AutomationProperties::SetAutomationId(button, L"FennSearchMode");
        StackPanel buttonContent;
        buttonContent.Orientation(Orientation::Horizontal);
        buttonContent.Spacing(4);
        Automation::AutomationProperties::SetAccessibilityView(image, Automation::Peers::AccessibilityView::Raw);
        Automation::AutomationProperties::SetAccessibilityView(symbol, Automation::Peers::AccessibilityView::Raw);
        image.Width(16);
        image.Height(16);
        image.Stretch(Media::Stretch::Uniform);
        symbol.Symbol(Symbol::Find);
        buttonContent.Children().Append(image);
        buttonContent.Children().Append(symbol);
        buttonContent.Children().Append(label);
        button.Content(buttonContent);
        exit.Content(winrt::box_value(L"×"));
        exit.Padding({4, 0, 4, 0});
        exit.MinWidth(24);
        Automation::AutomationProperties::SetName(exit, L"Exit search mode");
        Automation::AutomationProperties::SetAutomationId(exit, L"FennExitSearchMode");
        exit.Visibility(Visibility::Collapsed);
        exit.Click([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) Queue(L"mode-exit");
        });
    }
};
