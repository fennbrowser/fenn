#pragma once
#include <array>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <functional>
#include "JsonValidation.h"
#include "PinnedExtensionButtons.h"

// WinUI presentation only. Firefox owns eligibility, labels and commands.
struct NativeNavigationToolbar {
    using JObject = winrt::Windows::Data::Json::JsonObject;
    using Button = Microsoft::UI::Xaml::Controls::Button;
    struct State { bool enabled{}, stopping{}; winrt::hstring label, tooltip; };
    using States = std::array<State, 6>;
    static constexpr wchar_t const* commands[] = {L"back", L"forward", L"reload", L"profile", L"extensions", L"appmenu"};
    Microsoft::UI::Xaml::Controls::StackPanel panel, trailing;
    std::array<Button, 6> buttons;
    std::array<Microsoft::UI::Xaml::Controls::SymbolIcon, 6> icons;
    PinnedExtensionButtons pinned;
    bool stopping{};
    std::array<Microsoft::UI::Dispatching::DispatcherQueueTimer, 2> historyTimers{nullptr, nullptr};
    std::array<bool, 2> historyHeld{};
    std::array<uint32_t, 6> inputModifiers{};
    std::function<void(wchar_t const*, uint32_t, Button const&)> activate;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    NativeNavigationToolbar() {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        pinned.activate = [this](auto command, auto modifiers, auto const& button) { if (activate) activate(command, modifiers, button); };
        panel.Orientation(Orientation::Horizontal);
        panel.Spacing(4);
        panel.Margin({8, 0, 0, 0});
        panel.VerticalAlignment(VerticalAlignment::Center);
        panel.Visibility(Visibility::Collapsed);
        trailing.Orientation(Orientation::Horizontal);
        trailing.Spacing(4);
        trailing.Margin({0, 0, 8, 0});
        trailing.VerticalAlignment(VerticalAlignment::Center);
        trailing.Visibility(Visibility::Collapsed);
        const Symbol symbols[] = {Symbol::Back, Symbol::Forward, Symbol::Refresh, Symbol::Contact, Symbol::Library, Symbol::More};
        const wchar_t* names[] = {L"Back", L"Forward", L"Reload", L"Account", L"Extensions", L"Application menu"};
        const wchar_t* ids[] = {L"FirefoxWinUIBack", L"FirefoxWinUIForward", L"FirefoxWinUIReload", L"FirefoxWinUIProfile", L"FirefoxWinUIExtensions", L"FirefoxWinUIAppMenu"};
        for (size_t i = 0; i < buttons.size(); ++i) {
            auto button = Markup::XamlReader::Load(
                LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" Width="32" Height="32" MinWidth="0" MinHeight="0" Padding="0"/>)").as<Button>();
            SymbolIcon icon(symbols[i]);
            icons[i] = icon;
            // Firefox-like density with stock icons and generous button targets.
            Viewbox iconHost;
            iconHost.Width(16);
            iconHost.Height(16);
            iconHost.Child(icon);
            if (i == 4) {
                iconHost.Child(Markup::XamlReader::Load(
                    LR"(<FontIcon xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" FontFamily="{ThemeResource SymbolThemeFontFamily}" Glyph="&#xEA86;"/>)").as<FontIcon>());
            }
            button.Content(iconHost);
            button.IsEnabled(false);
            Automation::AutomationProperties::SetAutomationId(button, ids[i]);
            Automation::AutomationProperties::SetName(button, names[i]);
            ToolTipService::SetToolTip(button, winrt::box_value(winrt::hstring(names[i])));
            // Click has no modifier data. Preserve the originating WinUI event
            // instead of sampling keys after an asynchronously delivered click.
            button.AddHandler(UIElement::PointerPressedEvent(), winrt::box_value(
                Microsoft::UI::Xaml::Input::PointerEventHandler([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& args) {
                    if (alive.expired()) return;
                    inputModifiers[i] = uint32_t(args.KeyModifiers()) & 15u;
                    if (i < 2) {
                        historyHeld[i] = false;
                        if (args.GetCurrentPoint(buttons[i]).Properties().IsLeftButtonPressed()) historyTimers[i].Start();
                    }
                })), true);
            for (auto event : {UIElement::PointerReleasedEvent(), UIElement::PointerCanceledEvent(), UIElement::PointerCaptureLostEvent()}) {
                button.AddHandler(event, winrt::box_value(
                    Microsoft::UI::Xaml::Input::PointerEventHandler([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                        if (!alive.expired()) {
                            inputModifiers[i] = 0;
                            if (i < 2) historyTimers[i].Stop();
                        }
                    })), true);
            }
            button.PreviewKeyDown([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (alive.expired()) return;
                if (i < 2) { historyHeld[i] = false; historyTimers[i].Stop(); }
                inputModifiers[i] = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) |
                    ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) |
                    ((GetKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u) |
                    ((GetKeyState(VK_LWIN) & 0x8000) ? 8u : 0u);
            });
            button.Click([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (alive.expired() || !activate || !buttons[i].IsEnabled() || panel.Visibility() != Visibility::Visible) return;
                if (i < 2 && historyHeld[i]) { historyHeld[i] = false; return; }
                const uint32_t modifiers = inputModifiers[i];
                inputModifiers[i] = 0;
                activate(i == 2 && stopping ? L"stop" : commands[i], modifiers, buttons[i]);
            });
            buttons[i] = button;
            if (i < 2) {
                historyTimers[i] = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
                historyTimers[i].IsRepeating(false);
                historyTimers[i].Interval(std::chrono::milliseconds(500));
                auto history = [this, i, alive = std::weak_ptr<int>(lifetime)] {
                    if (!alive.expired() && activate && buttons[i].IsEnabled() && panel.Visibility() == Visibility::Visible)
                        activate(i == 0 ? L"back-history" : L"forward-history", 0, buttons[i]);
                };
                historyTimers[i].Tick([this, i, history, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                    if (alive.expired() || !(GetKeyState(VK_LBUTTON) & 0x8000)) return;
                    historyHeld[i] = true;
                    history();
                });
                button.ContextRequested([history](auto const&, auto const& args) { args.Handled(true); history(); });
                button.PointerExited([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                    if (!alive.expired()) historyTimers[i].Stop();
                });
            }
            (i < 3 ? panel : trailing).Children().Append(button);
            if (i == 4) trailing.Children().Append(pinned.panel);
        }
    }
    ~NativeNavigationToolbar() { lifetime.reset(); for (auto timer : historyTimers) if (timer) timer.Stop(); }
    static States Read(JObject const& snapshot) {
        States result{};
        if (!snapshot.HasKey(L"navigation")) return result;
        auto navigation = snapshot.GetNamedObject(L"navigation");
        for (size_t i = 0; i < result.size(); ++i) {
            if (i >= 3 && !navigation.HasKey(commands[i])) continue;
            auto entry = navigation.GetNamedObject(commands[i]);
            auto& item = result[i];
            item.enabled = entry.GetNamedBoolean(L"enabled");
            if (i == 2) item.stopping = entry.GetNamedBoolean(L"stopping", false);
            item.label = entry.GetNamedString(L"label", L"");
            item.tooltip = entry.GetNamedString(L"tooltip", L"");
            NativeJson::RequireLength(item.label, NativeJson::kMaxShortLabel);
            NativeJson::RequireLength(item.tooltip, NativeJson::kMaxShortLabel);
        }
        return result;
    }
    void Apply(States const& state, bool visible) {
        using namespace Microsoft::UI::Xaml;
        panel.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        trailing.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        if (!visible) pinned.Apply({}, false);
        stopping = visible && state[2].stopping;
        icons[2].Symbol(stopping ? Controls::Symbol::Cancel : Controls::Symbol::Refresh);
        for (size_t i = 0; i < buttons.size(); ++i) {
            buttons[i].IsEnabled(visible && state[i].enabled);
            if (!state[i].label.empty()) Automation::AutomationProperties::SetName(buttons[i], state[i].label);
            if (!state[i].tooltip.empty()) Controls::ToolTipService::SetToolTip(buttons[i], winrt::box_value(state[i].tooltip));
        }
    }
};
