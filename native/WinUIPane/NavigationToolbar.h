#pragma once
#include <array>
#include <functional>
#include "JsonValidation.h"

// WinUI presentation only. Firefox owns eligibility, labels and commands.
struct NativeNavigationToolbar {
    using JObject = winrt::Windows::Data::Json::JsonObject;
    using Button = Microsoft::UI::Xaml::Controls::Button;
    struct State { bool enabled{}, stopping{}; winrt::hstring label, tooltip; };
    using States = std::array<State, 3>;
    static constexpr wchar_t const* commands[] = {L"back", L"forward", L"reload"};
    Microsoft::UI::Xaml::Controls::StackPanel panel;
    std::array<Button, 3> buttons;
    std::array<Microsoft::UI::Xaml::Controls::SymbolIcon, 3> icons;
    bool stopping{};
    std::array<uint32_t, 3> inputModifiers{};
    std::function<void(wchar_t const*, uint32_t)> activate;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    NativeNavigationToolbar() {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        panel.Orientation(Orientation::Horizontal);
        panel.Spacing(16);
        panel.Margin({8, 0, 0, 0});
        panel.VerticalAlignment(VerticalAlignment::Center);
        panel.Visibility(Visibility::Collapsed);
        const Symbol symbols[] = {Symbol::Back, Symbol::Forward, Symbol::Refresh};
        const wchar_t* names[] = {L"Back", L"Forward", L"Reload"};
        const wchar_t* ids[] = {L"FirefoxWinUIBack", L"FirefoxWinUIForward", L"FirefoxWinUIReload"};
        for (size_t i = 0; i < buttons.size(); ++i) {
            auto button = Markup::XamlReader::Load(
                LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" Width="32" Height="32" MinWidth="0" MinHeight="0" Padding="0"/>)").as<Button>();
            SymbolIcon icon(symbols[i]);
            icons[i] = icon;
            // Keep the stock symbol and button behavior at Explorer-like density.
            Viewbox iconHost;
            iconHost.Width(12);
            iconHost.Height(12);
            iconHost.Child(icon);
            button.Content(iconHost);
            button.IsEnabled(false);
            Automation::AutomationProperties::SetAutomationId(button, ids[i]);
            Automation::AutomationProperties::SetName(button, names[i]);
            ToolTipService::SetToolTip(button, winrt::box_value(winrt::hstring(names[i])));
            // Click has no modifier data. Preserve the originating WinUI event
            // instead of sampling keys after an asynchronously delivered click.
            button.AddHandler(UIElement::PointerPressedEvent(), winrt::box_value(
                Microsoft::UI::Xaml::Input::PointerEventHandler([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const& args) {
                    if (!alive.expired()) inputModifiers[i] = uint32_t(args.KeyModifiers()) & 15u;
                })), true);
            for (auto event : {UIElement::PointerReleasedEvent(), UIElement::PointerCanceledEvent(), UIElement::PointerCaptureLostEvent()}) {
                button.AddHandler(event, winrt::box_value(
                    Microsoft::UI::Xaml::Input::PointerEventHandler([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                        if (!alive.expired()) inputModifiers[i] = 0;
                    })), true);
            }
            button.PreviewKeyDown([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (alive.expired()) return;
                inputModifiers[i] = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) |
                    ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) |
                    ((GetKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u) |
                    ((GetKeyState(VK_LWIN) & 0x8000) ? 8u : 0u);
            });
            button.Click([this, i, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (alive.expired() || !activate || !buttons[i].IsEnabled() || panel.Visibility() != Visibility::Visible) return;
                const uint32_t modifiers = inputModifiers[i];
                inputModifiers[i] = 0;
                activate(i == 2 && stopping ? L"stop" : commands[i], modifiers);
            });
            buttons[i] = button;
            panel.Children().Append(button);
        }
    }
    ~NativeNavigationToolbar() { lifetime.reset(); }
    static States Read(JObject const& snapshot) {
        States result{};
        if (!snapshot.HasKey(L"navigation")) return result;
        auto navigation = snapshot.GetNamedObject(L"navigation");
        for (size_t i = 0; i < result.size(); ++i) {
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
        stopping = visible && state[2].stopping;
        icons[2].Symbol(stopping ? Controls::Symbol::Cancel : Controls::Symbol::Refresh);
        for (size_t i = 0; i < buttons.size(); ++i) {
            buttons[i].IsEnabled(visible && state[i].enabled);
            if (!state[i].label.empty()) Automation::AutomationProperties::SetName(buttons[i], state[i].label);
            if (!state[i].tooltip.empty()) Controls::ToolTipService::SetToolTip(buttons[i], winrt::box_value(state[i].tooltip));
        }
    }
};
