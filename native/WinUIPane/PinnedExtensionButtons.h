#pragma once
#include "JsonValidation.h"
#include <map>
#include <set>

// Retained WinUI controls only; Firefox owns placement and extension behavior.
struct PinnedExtensionButtons {
    using JObject = winrt::Windows::Data::Json::JsonObject;
    using Button = winrt::Microsoft::UI::Xaml::Controls::Button;
    struct State {
        winrt::hstring id, label, badge, text;
        bool enabled{}, genericIcon{}, alwaysVisible{};
        JObject image{nullptr};
        winrt::Windows::UI::Color background{}, foreground{};
    };
    struct Row {
        bool enabled{}, alwaysVisible{};
        Button button{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Image image{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::FontIcon genericIcon{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::TextBlock badge{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Border badgeBorder{nullptr};
        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush background{nullptr}, foreground{nullptr};
        winrt::hstring imageKey;
    };
    winrt::Microsoft::UI::Xaml::Controls::StackPanel panel;
    std::map<winrt::hstring, Row> rows;
    std::function<void(wchar_t const*, uint32_t, Button const&)> activate;
    winrt::hstring commandPrefix{L"extension:"}, contextPrefix{L"extension-menu:"}, automationPrefix{L"FennExtension-"};
    double buttonSize{32};
    bool externalLayout{};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    PinnedExtensionButtons() {
        using namespace winrt::Microsoft::UI::Xaml;
        panel.Orientation(Controls::Orientation::Horizontal);
        panel.Spacing(4);
        panel.Visibility(Visibility::Collapsed);
    }
    ~PinnedExtensionButtons() { lifetime.reset(); }
    void SetRevealed(bool reveal) {
        panel.Opacity(1);
        for (auto const& [id,row] : rows) row.button.Opacity(reveal || row.alwaysVisible ? 1.0 : 0.0);
    }
    static std::vector<State> Read(JObject const& snapshot, wchar_t const* key = L"pinned") {
        std::vector<State> result;
        if (!snapshot.HasKey(L"navigation")) return result;
        auto navigation = snapshot.GetNamedObject(L"navigation");
        if (!navigation.HasKey(key)) return result;
        auto items = navigation.GetNamedArray(key);
        if (items.Size() > 32) throw winrt::hresult_invalid_argument();
        size_t pixels = 0;
        std::set<winrt::hstring> ids;
        auto color = [](auto const& item, wchar_t const* key) {
            auto values = item.GetNamedArray(key);
            if (values.Size() != 4) throw winrt::hresult_invalid_argument();
            uint8_t rgba[4];
            for (uint32_t i = 0; i < 4; ++i) {
                double n = values.GetNumberAt(i);
                if (!std::isfinite(n) || n < 0 || n > 255 || n != int(n)) throw winrt::hresult_invalid_argument();
                rgba[i] = uint8_t(n);
            }
            return winrt::Windows::UI::Color{rgba[3], rgba[0], rgba[1], rgba[2]};
        };
        for (auto value : items) {
            auto item = value.GetObject();
            State next;
            next.id = item.GetNamedString(L"id");
            next.label = item.GetNamedString(L"label");
            next.badge = item.GetNamedString(L"badge", L"");
            next.text = item.GetNamedString(L"text", L"");
            if (next.id.empty() || next.id.size() > 64 || !ids.insert(next.id).second ||
                next.label.size() > 4096 || next.badge.size() > 4096 || next.text.size() > 16) throw winrt::hresult_invalid_argument();
            next.enabled = item.GetNamedBoolean(L"enabled");
            next.genericIcon = item.GetNamedBoolean(L"genericIcon",false);
            next.alwaysVisible = item.GetNamedBoolean(L"alwaysVisible",false);
            next.background = color(item, L"background");
            next.foreground = color(item, L"foreground");
            NativeJson::ValidateImage(item, pixels, {32, 32768});
            if (item.HasKey(L"image") && item.GetNamedValue(L"image").ValueType() != winrt::Windows::Data::Json::JsonValueType::Null)
                next.image = item.GetNamedObject(L"image");
            result.push_back(std::move(next));
        }
        return result;
    }
    void Apply(std::vector<State> const& state, bool visible) {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        std::set<winrt::hstring> keep;
        uint32_t index = 0;
        for (auto const& item : state) {
            keep.insert(item.id);
            auto [it, added] = rows.try_emplace(item.id);
            auto& row = it->second;
            if (added) {
                row.button = Markup::XamlReader::Load(LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Style="{StaticResource SubtleButtonStyle}" Width="32" Height="32" MinWidth="0" MinHeight="0" Padding="0" HorizontalContentAlignment="Stretch" VerticalContentAlignment="Stretch"><Grid><Grid.Resources><ResourceDictionary><ResourceDictionary.ThemeDictionaries><ResourceDictionary x:Key="Default" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"><SolidColorBrush x:Key="BadgeBackground" Color="Transparent"/><SolidColorBrush x:Key="BadgeForeground" Color="Transparent"/></ResourceDictionary><ResourceDictionary x:Key="HighContrast" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"><SolidColorBrush x:Key="BadgeBackground" Color="{ThemeResource SystemColorHighlightColor}"/><SolidColorBrush x:Key="BadgeForeground" Color="{ThemeResource SystemColorHighlightTextColor}"/></ResourceDictionary></ResourceDictionary.ThemeDictionaries></ResourceDictionary></Grid.Resources><Image Width="16" Height="16" Stretch="Uniform"/><Border Background="{ThemeResource BadgeBackground}" CornerRadius="3" HorizontalAlignment="Right" VerticalAlignment="Top" MaxWidth="30"><TextBlock Foreground="{ThemeResource BadgeForeground}" FontSize="9" Padding="2,0" TextTrimming="CharacterEllipsis"/></Border></Grid></Button>)").as<Button>();
                row.button.Width(buttonSize);
                row.button.Height(buttonSize);
                auto grid = row.button.Content().as<Grid>();
                // Fit inside the stock button border instead of overflowing its inset.
                row.image = grid.Children().GetAt(0).as<Image>();
                row.badgeBorder = grid.Children().GetAt(1).as<Border>();
                row.badge = row.badgeBorder.Child().as<TextBlock>();
                row.genericIcon = Markup::XamlReader::Load(LR"(<FontIcon xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" FontFamily="{ThemeResource SymbolThemeFontFamily}" Glyph="&#xEA86;" FontSize="16" HorizontalAlignment="Center" VerticalAlignment="Center"/>)").as<FontIcon>();
                grid.Children().Append(row.genericIcon);
                Canvas::SetZIndex(row.badgeBorder,1);
                auto resources = grid.Resources().ThemeDictionaries().Lookup(winrt::box_value(L"Default")).as<ResourceDictionary>();
                row.background = resources.Lookup(winrt::box_value(L"BadgeBackground")).as<Media::SolidColorBrush>();
                row.foreground = resources.Lookup(winrt::box_value(L"BadgeForeground")).as<Media::SolidColorBrush>();
                Automation::AutomationProperties::SetAutomationId(row.button, automationPrefix + item.id);
                auto invoke = [this, id = item.id, alive = std::weak_ptr<int>(lifetime)](bool context) {
                    if (alive.expired() || !activate || !rows.contains(id) || panel.Visibility() != Visibility::Visible) return;
                    auto button = rows.at(id).button;
                    if (!context && !button.IsEnabled()) return;
                    auto command = (context ? contextPrefix : commandPrefix) + id;
                    uint32_t modifiers = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) |
                        ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) | ((GetKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u);
                    activate(command.c_str(), modifiers, button);
                };
                row.button.Click([invoke](auto const&, auto const&) { invoke(false); });
                row.button.ContextRequested([invoke](auto const&, auto const& args) { args.Handled(true); invoke(true); });
            }
            if (!externalLayout) {
            uint32_t oldIndex;
            if (panel.Children().IndexOf(row.button, oldIndex)) {
                if (oldIndex != index) { panel.Children().RemoveAt(oldIndex); panel.Children().InsertAt(index, row.button); }
            } else panel.Children().InsertAt(index, row.button);
            ++index;
            }
            row.enabled=item.enabled;row.alwaysVisible=item.alwaysVisible;row.button.IsEnabled(visible && item.enabled);
            Automation::AutomationProperties::SetName(row.button, item.label);
            Automation::AutomationProperties::SetItemStatus(row.button, item.badge);
            ToolTipService::SetToolTip(row.button, winrt::box_value(item.label));
            row.badge.Text(item.text.empty() ? item.badge : item.text);
            const auto width = item.text.empty() ? buttonSize : std::max(buttonSize, double(item.text.size() * 7 + 12));
            row.button.Width(width);
            row.image.Visibility(item.text.empty() && !item.genericIcon ? Visibility::Visible : Visibility::Collapsed);
            row.genericIcon.Visibility(item.text.empty() && item.genericIcon ? Visibility::Visible : Visibility::Collapsed);
            row.badge.FontSize(item.text.empty() ? 9 : 12);
            row.badgeBorder.MaxWidth(item.text.empty() ? 30 : width);
            row.badgeBorder.HorizontalAlignment(item.text.empty() ? HorizontalAlignment::Right : HorizontalAlignment::Center);
            row.badgeBorder.VerticalAlignment(item.text.empty() ? VerticalAlignment::Top : VerticalAlignment::Center);
            row.badgeBorder.Visibility((item.badge.empty() && item.text.empty()) ? Visibility::Collapsed : Visibility::Visible);
            row.background.Color(item.background); row.foreground.Color(item.foreground);
            auto imageKey = item.image ? item.image.GetNamedString(L"pixels") : winrt::hstring{};
            if (imageKey != row.imageKey) {
                row.image.Source(item.image ? NativeJson::DecodeImage(item.image) : nullptr);
                row.imageKey = imageKey;
            }
        }
        for (auto it = rows.begin(); it != rows.end();) {
            if (keep.contains(it->first)) { ++it; continue; }
            uint32_t position;
            if (panel.Children().IndexOf(it->second.button, position)) panel.Children().RemoveAt(position);
            it = rows.erase(it);
        }
        panel.Visibility(visible && !state.empty() ? Visibility::Visible : Visibility::Collapsed);
    }
};
