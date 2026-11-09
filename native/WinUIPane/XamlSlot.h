#pragma once
#include <windowsx.h>
#include <winrt/Microsoft.UI.Input.h>

// A pane region in the native host Window's single XAML tree. Both the process
// shell and in-process host use this backing; Firefox-owned HWNDs are rejected.

// COM identity: C++/WinRT's == compares interface pointers, which differ between a Border and the
// DependencyObject it also is.
inline bool SameObject(winrt::Windows::Foundation::IInspectable const& a, winrt::Windows::Foundation::IInspectable const& b) {
    return a && b && a.as<winrt::Windows::Foundation::IUnknown>() == b.as<winrt::Windows::Foundation::IUnknown>();
}

struct XamlSlotBridge {
    HWND top{}, bridge{};
    Controls::Border slot{nullptr};
    Microsoft::UI::WindowId WindowId() const { return GetWindowIdFromWindow(bridge); }
    float Scale() const {
        if (slot) if (auto root = slot.XamlRoot()) return float(root.RasterizationScale());
        const UINT dpi = GetDpiForWindow(top);
        return dpi ? dpi / 96.0f : 1.0f;
    }
    // Firefox supplies physical client pixels; the region uses XAML DIPs.
    void MoveAndResize(Windows::Graphics::RectInt32 const& rect) const {
        const float scale = Scale();
        Controls::Canvas::SetLeft(slot, rect.X / scale);
        Controls::Canvas::SetTop(slot, rect.Y / scale);
        slot.Width(rect.Width / scale);
        slot.Height(rect.Height / scale);
    }
    void Show() const { slot.Visibility(Visibility::Visible); }
    void Hide() const { slot.Visibility(Visibility::Collapsed); }
    bool Visible() const { return slot && slot.Visibility() == Visibility::Visible; }
    // A region shares the window's one rasterization scale and cannot override it.
};

struct XamlSlot {
    struct State {
        HWND top{}, bridge{};
        Controls::Border slot{nullptr};
        std::function<void()> takeFocus;
        event_token focusToken{};
    };
    std::shared_ptr<State> state;

    XamlSlot() = default;
    XamlSlot(std::nullptr_t) {}
    static XamlSlot New() { XamlSlot slot; slot.state = std::make_shared<State>(); return slot; }
    explicit operator bool() const { return state && state->slot; }
    bool operator==(XamlSlot const& other) const { return state == other.state; }

    void Initialize(Microsoft::UI::WindowId id) {
        auto& s = *state;
        s.top = GetWindowFromWindowId(id);
        auto* host = NativeWindowHost::Get(s.top);
        if (!host || !host->root) throw_hresult(E_INVALIDARG);
        s.bridge = host->bridge;
        s.slot = Controls::Border();
        s.slot.Visibility(Visibility::Collapsed);
        host->root.Children().Append(s.slot);
        // Tab past the region's last control (or before its first) leaves it. XAML's
        // own Tab navigation would move on into the window's other region or wrap; it is cancelled at
        // LosingFocus. Moves into popups (another tree root) and programmatic focus are untouched.
        std::weak_ptr<State> weak = state;
        s.focusToken = s.slot.LosingFocus([weak](auto const&, Microsoft::UI::Xaml::Input::LosingFocusEventArgs const& args) noexcept { try {
            auto self = weak.lock();
            if (!self || !self->takeFocus || args.FocusState() != FocusState::Keyboard) return;
            using Direction = Microsoft::UI::Xaml::Input::FocusNavigationDirection;
            const auto direction = args.Direction();
            if (direction != Direction::Next && direction != Direction::Previous) return;
            auto next = args.NewFocusedElement();
            bool leaving = !next;
            if (next) {
                // In the window's own tree (XAML adds its own roots above our canvas), not a popup's.
                auto* host = NativeWindowHost::Get(self->top);
                bool inWindow = false;
                for (DependencyObject node = next; node && host; node = Media::VisualTreeHelper::GetParent(node))
                    if (SameObject(node, host->root)) { inWindow = true; break; }
                if (!inWindow) return;
                if (!Within(*self, next)) leaving = true;
                else {
                    auto edge = direction == Direction::Next ? Microsoft::UI::Xaml::Input::FocusManager::FindFirstFocusableElement(self->slot)
                                                             : Microsoft::UI::Xaml::Input::FocusManager::FindLastFocusableElement(self->slot);
                    leaving = edge && SameObject(edge, next);
                }
            }
            if (!leaving || !args.TryCancel()) return;
            args.Handled(true);
            auto take = self->takeFocus;
            if (auto queue = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread()) queue.TryEnqueue([take] { take(); });
        } catch (...) { NativeWindowHost::FocusTrace("slot-tab-failed", static_cast<uintptr_t>(static_cast<uint32_t>(winrt::to_hresult()))); } });
    }
    // In the window's tree, a popup's content hangs off the popup root rather than the region that
    // opened it: follow an open popup back to its parent, or to its placement target when it has none.
    static bool Within(State const& s, DependencyObject node, int depth = 0) {
        for (auto at = node; at; at = Media::VisualTreeHelper::GetParent(at))
            if (SameObject(at, s.slot)) return true;
        if (!node || depth > 3 || !s.slot || !s.slot.XamlRoot()) return false;
        for (auto const& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(s.slot.XamlRoot())) {
            auto child = popup.Child();
            if (!child) continue;
            for (auto at = node; at; at = Media::VisualTreeHelper::GetParent(at)) {
                if (!SameObject(at, child)) continue;
                DependencyObject owner = Media::VisualTreeHelper::GetParent(popup);
                if (!owner) owner = popup.PlacementTarget();
                return owner && Within(s, owner, depth + 1);
            }
        }
        return false;
    }
    XamlSlotBridge SiteBridge() const {
        return {state->top, state->bridge, state->slot};
    }
    void Content(UIElement const& content) const { state->slot.Child(content); }
    UIElement Content() const { return state->slot.Child(); }
    // A region's backdrop belongs to the window, and a region never removes it: only the window's end does.
    void SystemBackdrop(Media::SystemBackdrop const& backdrop) const {
        if (!backdrop) return;
        if (auto* host = NativeWindowHost::Get(state->top); host && host->window) {
            host->window.SystemBackdrop(backdrop);
            if (GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0)) {
                const auto previous = reinterpret_cast<uintptr_t>(GetPropW(state->top, L"Fenn.DiagnosticBackdropUpdates"));
                SetPropW(state->top, L"Fenn.DiagnosticBackdropUpdates", reinterpret_cast<HANDLE>(previous + 1));
                uintptr_t kind = 0;
                if (auto mica = backdrop.try_as<Media::MicaBackdrop>())
                    kind = mica.Kind() == Microsoft::UI::Composition::SystemBackdrops::MicaKind::BaseAlt ? 2 : 1;
                else if (backdrop.try_as<Media::DesktopAcrylicBackdrop>()) kind = 3;
                SetPropW(state->top, L"Fenn.DiagnosticBackdropKind", reinterpret_cast<HANDLE>(kind));
            }
            // Gecko must not replace the WinUI Window's backdrop/frame after
            // binding. The property belongs to this HWND and dies with it.
            check_bool(SetPropW(state->top,L"Fenn.NativeBackdrop",reinterpret_cast<HANDLE>(1)));
        }
    }
    Media::SystemBackdrop SystemBackdrop() const {
        auto* host = NativeWindowHost::Get(state->top);
        return host && host->window ? host->window.SystemBackdrop() : nullptr;
    }
    template <class Handler> event_token TakeFocusRequested(Handler handler) const {
        state->takeFocus = [handler] { handler(nullptr, nullptr); };
        return {1};
    }
    void TakeFocusRequested(event_token const& token) const {
        state->takeFocus = nullptr;
    }
    Microsoft::UI::Content::ContentIsland Island() const {
        auto content = Content();
        auto root = content ? content.XamlRoot() : nullptr;
        return root ? root.ContentIsland() : nullptr;
    }
    bool Contains(DependencyObject const& node) const { return Within(*state, node); }
    // Keyboard focus is in the shared content window and inside this region.
    bool HasFocus() const {
        const HWND focused = GetFocus();
        if (!state->bridge || !focused || (focused != state->bridge && !IsChild(state->bridge, focused))) return false;
        if (!state->slot.XamlRoot()) return false;
        return Contains(Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(state->slot.XamlRoot()).try_as<DependencyObject>());
    }
    // True when a control of this part took keyboard focus.
    bool NavigateFocus(XamlSourceFocusNavigationRequest const& request) const {
        if (auto island = Island()) Microsoft::UI::Input::InputFocusController::GetForIsland(island).TrySetFocus();
        auto target = request.Reason() == XamlSourceFocusNavigationReason::Last
            ? Microsoft::UI::Xaml::Input::FocusManager::FindLastFocusableElement(state->slot)
            : Microsoft::UI::Xaml::Input::FocusManager::FindFirstFocusableElement(state->slot);
        auto element = target.try_as<UIElement>();
        return element && element.Focus(FocusState::Keyboard);
    }
    // The message targets the shared XAML window with the pointer over this region.
    bool Holds(HWND target) const {
        if (!*this || !target || !state->bridge || (target != state->bridge && !IsChild(state->bridge, target))) return false;
        if (state->slot.Visibility() != Visibility::Visible) return false;
        const DWORD at = GetMessagePos();
        POINT point{GET_X_LPARAM(at), GET_Y_LPARAM(at)};
        ScreenToClient(state->top, &point);
        const float scale = SiteBridge().Scale();
        const double left = Controls::Canvas::GetLeft(state->slot) * scale, top = Controls::Canvas::GetTop(state->slot) * scale;
        return point.x >= left && point.y >= top &&
               point.x < left + state->slot.Width() * scale && point.y < top + state->slot.Height() * scale;
    }
    // A region's physical client rectangle, or empty while hidden.
    RECT Bounds() const {
        if (!state || !state->slot || state->slot.Visibility() != Visibility::Visible) return {};
        const float scale = SiteBridge().Scale();
        const LONG left = LONG(Controls::Canvas::GetLeft(state->slot) * scale + .5), top = LONG(Controls::Canvas::GetTop(state->slot) * scale + .5);
        return {left, top, left + LONG(state->slot.Width() * scale + .5), top + LONG(state->slot.Height() * scale + .5)};
    }
    void Close() {
        if (!*this) return;
        auto& s = *state;
        const bool focused = HasFocus();
        s.takeFocus = nullptr;
        s.slot.LosingFocus(s.focusToken);
        s.slot.Child(nullptr);
        if (auto* host = NativeWindowHost::Get(s.top); host && host->root) {
            uint32_t index{};
            if (host->root.Children().IndexOf(s.slot, index)) host->root.Children().RemoveAt(index);
        }
        s.slot = nullptr;
        if (focused) SetFocus(NativeWindowHost::PageWindow(s.top));
    }
};
