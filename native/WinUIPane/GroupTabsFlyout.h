#pragma once
#include "JsonValidation.h"
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

// A stock menu on a retained group button. Firefox owns its membership and
// selection; this class owns only hover timing and live WinUI objects.
struct GroupTabsFlyout {
    using JObject = Windows::Data::Json::JsonObject;
    using JValue = Windows::Data::Json::JsonValue;
    Controls::MenuFlyout menu{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    winrt::weak_ref<Controls::Button> anchor;
    winrt::weak_ref<FrameworkElement> presenter;
    JObject data;
    uint32_t group{};
    bool open{}, closing{}, showPending{};
    bool keyboardOpen{};
    bool pointerDismissed{};
    bool suppressed{};
    uint32_t outsideTicks{};
    uint64_t generation{};
    std::vector<uint32_t> ids;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    std::function<bool()> allowed;
    std::function<void(uint32_t,uint32_t)> activate;

    static bool UnderPointer(FrameworkElement const& element) {
        if (!element || !element.IsLoaded()) return false;
        POINT point{};
        if (!GetCursorPos(&point)) return false;
        try {
            auto bounds = element.TransformToVisual(nullptr).TransformBounds(
                {0,0,float(element.ActualWidth()),float(element.ActualHeight())});
            auto screen = element.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(bounds);
            return point.x >= screen.X && point.x < screen.X + screen.Width &&
                   point.y >= screen.Y && point.y < screen.Y + screen.Height;
        } catch (winrt::hresult_error const&) { return false; }
    }

    ~GroupTabsFlyout() { lifetime.reset(); if (timer) timer.Stop(); if (menu) { try { menu.Hide(); } catch (...) {} } }
    void Hide(bool pointerExit = false) {
        pointerDismissed = pointerExit;
        if (timer) timer.Stop();
        showPending = false;
        if (menu && open) { closing = true; menu.Hide(); }
    }
    void Delay(bool show) {
        if (open && !closing) return;
        auto button = anchor.get();
        if (!button) return;
        if (!timer) {
            timer = button.DispatcherQueue().CreateTimer();
            timer.IsRepeating(false);
            timer.Tick([this, alive = std::weak_ptr<int>(lifetime)](auto&&,auto&&) {
                if (alive.expired()) return;
                if (open && !closing) {
                    if (keyboardOpen) return;
                    if (UnderPointer(anchor.get()) || UnderPointer(presenter.get())) outsideTicks = 0;
                    else if (++outsideTicks >= 5) Hide(true);
                    return;
                }
                if (showPending) Show(); else Hide();
            });
        }
        timer.Stop();
        timer.IsRepeating(false);
        showPending = show;
        timer.Interval(std::chrono::milliseconds(show ? 400 : 250));
        timer.Start();
    }
    void Enter(uint32_t id, Controls::Button const& button, JObject const& value, bool immediate = false) {
        if (suppressed && group == id && !immediate) return;
        if (immediate || group != id) suppressed = false;
        if (!value.GetNamedBoolean(L"collapsed") || !value.HasKey(L"tabs") ||
            !value.GetNamedArray(L"tabs").Size() || (allowed && !allowed())) return;
        if (group == id && open && !closing) { outsideTicks = 0; return; }
        if (group == id && showPending && !immediate) return;
        Hide();
        group = id; anchor = button; data = value;
        if (immediate) Show(true); else Delay(true);
    }
    void Exit(uint32_t id) {
        if (group == id && !UnderPointer(anchor.get())) { suppressed = false; Delay(false); }
    }
    void Dismiss() { suppressed = true; Hide(); }
    void Refresh(uint32_t id, JObject const& value) {
        if (id != group) return;
        data = value;
        if (!value.GetNamedBoolean(L"collapsed")) { Hide(); return; }
        if (!open || closing) return;
        auto tabs = value.GetNamedArray(L"tabs", Windows::Data::Json::JsonArray());
        if (tabs.Size() != ids.size()) { Hide(); return; }
        for (uint32_t i = 0; i < tabs.Size(); ++i) {
            if (NativeJson::RequireId(tabs.GetObjectAt(i), L"id") != ids[i]) { Hide(); return; }
        }
        for (uint32_t i = 0; i < tabs.Size(); ++i) Paint(menu.Items().GetAt(i).as<Controls::MenuFlyoutItem>(), tabs.GetObjectAt(i));
    }
    static void Paint(Controls::MenuFlyoutItem const& item, JObject const& tab) {
        item.Text(tab.GetNamedString(L"title"));
        if (tab.HasKey(L"image") && tab.GetNamedValue(L"image").ValueType() == Windows::Data::Json::JsonValueType::Object) {
            Controls::ImageIcon pixels;
            pixels.Source(NativeJson::DecodeImage(tab.GetNamedObject(L"image")));
            item.Icon(pixels);
        } else {
            Controls::FontIcon icon;
            icon.Glyph(L"\uE774");
            item.Icon(icon);
        }
    }
    void Show(bool keyboard = false) {
        if (closing) { showPending = true; return; }
        showPending = false;
        auto button = anchor.get();
        if (!button || !button.IsLoaded() || (allowed && !allowed()) ||
            !data.GetNamedBoolean(L"collapsed")) return;
        if (open) return;
        auto tabs = data.GetNamedArray(L"tabs");
        if (!tabs.Size()) return;
        menu = Controls::MenuFlyout();
        menu.MenuFlyoutPresenterStyle(Markup::XamlReader::Load(LR"(
<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="MenuFlyoutPresenter">
 <Setter Property="AutomationProperties.AutomationId" Value="FennGroupTabs"/>
 <Setter Property="MaxHeight" Value="320"/><Setter Property="MaxWidth" Value="360"/>
 <Setter Property="MinWidth" Value="220"/>
</Style>)").as<Style>());
        const auto current = group;
        const auto token = ++generation;
        auto alive = std::weak_ptr<int>(lifetime);
        ids.clear();
        for (auto value : tabs) {
            auto tab = value.GetObject();
            const auto id = NativeJson::RequireId(tab,L"id");
            ids.push_back(id);
            Controls::MenuFlyoutItem item;
            Paint(item,tab);
            Automation::AutomationProperties::SetAutomationId(item,L"FennGroupTab-" + to_hstring(id));
            item.Click([this,alive,current,id,token](auto&&,auto&&) {
                if (alive.expired() || token != generation || !open || closing || group != current || (allowed && !allowed())) return;
                Hide();
                if (activate) activate(current,id);
            });
            menu.Items().Append(item);
        }
        menu.Opened([this,alive,token](auto&&,auto&&) {
            if (alive.expired() || token != generation) return;
            for (DependencyObject node = menu.Items().GetAt(0); node; node = Media::VisualTreeHelper::GetParent(node)) {
                if (auto element = node.try_as<Controls::MenuFlyoutPresenter>()) { presenter = element; break; }
            }
            if (!keyboardOpen && timer) {
                timer.Stop(); timer.IsRepeating(true);
                timer.Interval(std::chrono::milliseconds(50)); timer.Start();
            }
        });
        menu.Closed([this,alive,token](auto&&,auto&&) {
            if (!alive.expired() && token == generation) {
                open = closing = false; presenter = nullptr;
                menu = nullptr;
                if (!pointerDismissed && !showPending) suppressed = UnderPointer(anchor.get());
                if (showPending || (pointerDismissed && UnderPointer(anchor.get()))) Delay(true);
                else if (timer) timer.Stop();
            }
        });
        Controls::Primitives::FlyoutShowOptions options;
        options.Placement(Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft);
        options.ShowMode(keyboard ? Controls::Primitives::FlyoutShowMode::Standard : Controls::Primitives::FlyoutShowMode::Transient);
        open = true; closing = false;
        keyboardOpen = keyboard; outsideTicks = 0; pointerDismissed = false;
        menu.ShowAt(button,options);
    }
};
