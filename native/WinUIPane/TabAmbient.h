#pragma once
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include "TabGlowDiffusion.h"

// Decorative Composition children cannot take focus, enter UIA's control tree,
// or affect TabView measurement. Both fields use ordinary source-over alpha.
struct TabAmbient {
    using Rect = winrt::Windows::Foundation::Rect;
    using Sprite = winrt::Microsoft::UI::Composition::SpriteVisual;
    using Gradient = winrt::Microsoft::UI::Composition::CompositionGradientBrush;
    struct Field {
        Sprite visual{nullptr}, left{nullptr}, middle{nullptr}, right{nullptr};
        Gradient brush{nullptr}, leftBrush{nullptr}, rightBrush{nullptr};
        uint32_t color{}; Rect bounds{}; bool capped{};
    };
    winrt::Microsoft::UI::Xaml::Controls::Canvas layer{nullptr};
    winrt::Microsoft::UI::Composition::ContainerVisual bottom{nullptr}, top{nullptr};
    winrt::Microsoft::UI::Composition::LayerVisual diffusion{nullptr};
    std::map<uint32_t, Field> groups, containers;
    winrt::Windows::UI::ViewManagement::AccessibilitySettings accessibility;
    winrt::Windows::UI::ViewManagement::UISettings settings;
    winrt::event_token contrastToken{}, layoutToken{}, renderingToken{};
    std::shared_ptr<int> life = std::make_shared<int>(0);
    std::function<void()> refresh;
    bool observing{}, updating{}, reportedFailure{}, queued{};
    void ScheduleUpdate() {
        if (queued) return;
        queued = true;
        if (!layer.DispatcherQueue().TryEnqueue([this, weak = std::weak_ptr<int>(life)] {
            if (weak.expired()) return;
            queued = false;
            Update();
        })) queued = false;
    }

    void Create(winrt::Microsoft::UI::Xaml::Controls::Grid const& root) {
        using namespace winrt::Microsoft::UI::Xaml;
        layer = Markup::XamlReader::Load(LR"(<Canvas xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Grid.ColumnSpan="3" IsHitTestVisible="False" AutomationProperties.AccessibilityView="Raw" FlowDirection="LeftToRight"/>)").as<Controls::Canvas>();
        root.Children().Append(layer);
        auto weak = std::weak_ptr<int>(life);
        layer.Loaded([this, weak](auto&&, auto&&) {
            if (weak.expired()) return;
            if (!layoutToken.value) layoutToken = layer.LayoutUpdated([this, weak](auto&&, auto&&) { if (!weak.expired()) Update(); });
            Update();
        });
        contrastToken = settings.ColorValuesChanged([this, weak, queue = root.DispatcherQueue()](auto&&, auto&&) {
            queue.TryEnqueue([this, weak] { if (!weak.expired()) Update(); });
        });
    }
    void Update() {
        if (updating || !refresh || !layer.IsLoaded()) return;
        updating = true;
        try {
            if (!bottom) {
                auto compositor = winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(layer).Compositor();
                diffusion = compositor.CreateLayerVisual();
                diffusion.Effect(compositor.CreateEffectFactory(winrt::make<TabGlowDiffusion>()).CreateBrush());
                diffusion.Opacity(.9f);
                auto parent = diffusion;
                bottom = compositor.CreateContainerVisual(); top = compositor.CreateContainerVisual();
                parent.Children().InsertAtTop(bottom); parent.Children().InsertAtTop(top);
                winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::SetElementChildVisual(layer, parent);
            }
            refresh();
        } catch (...) {
#ifdef WINUI_PANE_TESTING
            if (!reportedFailure) {
                reportedFailure = true;
                try { throw; }
                catch (winrt::hresult_error const& error) { fwprintf(stderr, L"Ambient refresh: %08x %s\n", unsigned(error.code()), error.message().c_str()); }
                catch (...) { fprintf(stderr, "Ambient refresh failed\n"); }
            }
#endif
            if (bottom) bottom.IsVisible(false);
            if (top) top.IsVisible(false);
        }
        updating = false;
    }
    // Only layout/scroll invalidation runs at rest. Follow existing tab motion
    // for its bounded lifetime; never keep an ambient render loop alive.
    void Motion(bool moving) {
        using namespace winrt::Microsoft::UI::Xaml;
        if (moving == observing) return;
        observing = moving;
        if (!moving) { Media::CompositionTarget::Rendering(renderingToken); return; }
        renderingToken = Media::CompositionTarget::Rendering([this, weak = std::weak_ptr<int>(life)](auto&&, auto&&) {
            if (!weak.expired()) Update();
        });
    }
    void Clip(Rect viewport, bool visible) {
        visible = visible && !accessibility.HighContrast();
        bottom.IsVisible(visible); top.IsVisible(visible);
        const winrt::Windows::Foundation::Numerics::float2 size{
            std::max(0.f, viewport.X + viewport.Width), std::max(0.f, viewport.Y + viewport.Height)};
        if (diffusion.Size() != size) diffusion.Size(size);
        // Avoid invalidating XAML layout from LayoutUpdated.
        auto old = layer.Clip();
        if (!old) {
            old = winrt::Microsoft::UI::Xaml::Media::RectangleGeometry();
            old.Rect(viewport);
            layer.Clip(old);
        } else if (old.Rect() != viewport) old.Rect(viewport);
    }
    void Put(std::map<uint32_t, Field>& fields, uint32_t id, uint32_t color, Rect bounds, bool upper, bool connected = false) {
        const bool capped = !upper || connected;
        if (upper) {
            // Expose the upper half of the same light source above the tab.
            // Keep its peak at the original tab edge and its existing falloff
            // below that edge; only the enclosing strip clips the spill.
            bounds.Y -= .8f * bounds.Height;
            bounds.Height *= 1.8f;
        }
        auto& field = fields[id];
        if (field.visual && field.capped != capped) {
            (upper ? top : bottom).Children().Remove(field.visual);
            field = Field{};
        }
        if (!field.visual) {
            field.capped = capped;
            auto owner = upper ? top : bottom;
            auto compositor = owner.Compositor();
            field.visual = compositor.CreateSpriteVisual();
            field.visual.Comment((upper ? L"container:" : L"group:") + winrt::to_hstring(id));
            auto radial = [&](float x, float y, float radius) {
                auto brush = compositor.CreateRadialGradientBrush();
                brush.EllipseCenter({x, y});
                brush.EllipseRadius({radius, upper ? .8f / 1.8f : .8f});
                return brush;
            };
            if (!capped) {
                auto brush = radial(.5f, .8f / 1.8f, .5f);
                brush.EllipseRadius({.5f, .8f / 1.8f});
                field.brush = brush;
                field.visual.Brush(field.brush);
            } else {
                // A shallow elliptical crown avoids a flat colored band. Its
                // edge has radial distance 1/2; the wider end caps meet at that
                // same distance, so their alpha matches at every height. Keep
                // the caps bounded rather than dimming entire outer tabs as
                // one ellipse stretched across a large group would do.
                const float y = upper ? .8f / 1.8f : 1.f;
                auto center = radial(.5f, y, 1.f);
                field.brush = center;
                field.leftBrush = radial(2.f, y, 2.f);
                field.rightBrush = radial(-1.f, y, 2.f);
                field.left = compositor.CreateSpriteVisual();
                field.middle = compositor.CreateSpriteVisual();
                field.right = compositor.CreateSpriteVisual();
                field.left.Brush(field.leftBrush); field.middle.Brush(center); field.right.Brush(field.rightBrush);
                field.middle.Comment(upper ? L"container-center" : L"group-center");
                field.visual.Children().InsertAtTop(field.left);
                field.visual.Children().InsertAtTop(field.middle);
                field.visual.Children().InsertAtTop(field.right);
            }
            // A broad luminous shoulder makes both fields legible on light
            // tabs. Keep the horizontal radius inside the bounds so the ends
            // still disappear completely, rather than clipping a wider ellipse.
            // Radial alpha: 36%, 35%, 24%, 9%, 2%, then transparent.
            for (auto brush : {field.brush, field.leftBrush, field.rightBrush}) {
                if (!brush) continue;
                for (auto [offset, alpha] : {std::pair{0.f, 92}, {.12f, 88}, {.4f, 61}, {.65f, 23}, {.85f, 5}, {1.f, 0}})
                    brush.ColorStops().Append(compositor.CreateColorGradientStop(offset, {uint8_t(alpha), 0, 0, 0}));
            }
            owner.Children().InsertAtTop(field.visual);
        }
        if (field.color != color) {
            field.color = color;
            for (auto brush : {field.brush, field.leftBrush, field.rightBrush}) {
                if (!brush) continue;
                for (auto stop : brush.ColorStops()) {
                    auto value = stop.Color();
                    value.R = uint8_t(color >> 16); value.G = uint8_t(color >> 8); value.B = uint8_t(color);
                    stop.Color(value);
                }
            }
        }
        if (field.bounds != bounds) {
            field.bounds = bounds;
            field.visual.Offset({bounds.X, bounds.Y, 0});
            field.visual.Size({bounds.Width, bounds.Height});
            if (capped) {
                const auto cap = std::min(upper ? 32.f : 64.f, bounds.Width / 4);
                field.left.Size({cap, bounds.Height});
                field.middle.Offset({cap, 0, 0});
                field.middle.Size({bounds.Width - 2 * cap, bounds.Height});
                field.right.Offset({bounds.Width - cap, 0, 0});
                field.right.Size({cap, bounds.Height});
            }
        }
    }
    void Prune(std::map<uint32_t, Field>& fields, std::set<uint32_t> const& live, bool upper) {
        for (auto it = fields.begin(); it != fields.end();) {
            if (live.count(it->first)) { ++it; continue; }
            (upper ? top : bottom).Children().Remove(it->second.visual);
            it = fields.erase(it);
        }
    }
    ~TabAmbient() {
        life.reset();
        settings.ColorValuesChanged(contrastToken);
        if (layer && layoutToken.value) layer.LayoutUpdated(layoutToken);
        if (observing) winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(renderingToken);
    }
};
