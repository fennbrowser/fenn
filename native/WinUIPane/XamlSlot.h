#pragma once
#include <windowsx.h>
#include <winrt/Microsoft.UI.Input.h>

// Where one part of the pane's XAML lives, with the part of DesktopWindowXamlSource's interface the
// pane uses: content, placement and visibility, focus entry, and leaving focus by Tab. Copies share
// state. Two backings:
//  - region: the parent is a native host Window (NativeWindowHost::Context::root). The part is a
//    region of the Window's single XAML tree, placed at a rectangle Firefox chose; all regions share
//    the Window's XAML content window (Context::bridge), below Gecko's compositor.
//  - island: any other parent, such as Gecko's own window when the independent host is off. The
//    part is its own DesktopWindowXamlSource island, stacked above the compositor and clipped to
//    Firefox's exclusions, as before the Window existed.

// COM identity: C++/WinRT's == compares interface pointers, which differ between a Border and the
// DependencyObject it also is.
inline bool SameObject(winrt::Windows::Foundation::IInspectable const& a, winrt::Windows::Foundation::IInspectable const& b) {
    return a && b && a.as<winrt::Windows::Foundation::IUnknown>() == b.as<winrt::Windows::Foundation::IUnknown>();
}

struct XamlSlotBridge {
    HWND top{}, bridge{};
    Controls::Border slot{nullptr};
    Microsoft::UI::Content::DesktopChildSiteBridge site{nullptr};
    Microsoft::UI::WindowId WindowId() const { return GetWindowIdFromWindow(bridge); }
    float Scale() const {
        if (slot) if (auto root = slot.XamlRoot()) return float(root.RasterizationScale());
        const UINT dpi = GetDpiForWindow(top);
        return dpi ? dpi / 96.0f : 1.0f;
    }
    // Physical client pixels, as the island's MoveAndResize took them.
    void MoveAndResize(Windows::Graphics::RectInt32 const& rect) const {
        if (site) { site.MoveAndResize(rect); return; }
        const float scale = Scale();
        Controls::Canvas::SetLeft(slot, rect.X / scale);
        Controls::Canvas::SetTop(slot, rect.Y / scale);
        slot.Width(rect.Width / scale);
        slot.Height(rect.Height / scale);
    }
    void Show() const { if (site) site.Show(); else slot.Visibility(Visibility::Visible); }
    void Hide() const { if (site) site.Hide(); else slot.Visibility(Visibility::Collapsed); }
    bool Visible() const { return site ? IsWindowVisible(bridge) != FALSE : slot && slot.Visibility() == Visibility::Visible; }
    // A region shares the window's one rasterization scale and cannot override it.
    void OverrideScale(float scale) const { if (site) site.OverrideScale(scale); else throw hresult_not_implemented(); }
};

struct XamlSlot {
    struct State {
        HWND top{}, bridge{};
        Controls::Border slot{nullptr};
        DesktopWindowXamlSource island{nullptr};
        std::function<void()> takeFocus;
        event_token focusToken{};
    };
    std::shared_ptr<State> state;

    XamlSlot() = default;
    XamlSlot(std::nullptr_t) {}
    static XamlSlot New() { XamlSlot slot; slot.state = std::make_shared<State>(); return slot; }
    explicit operator bool() const { return state && (state->slot || state->island); }
    bool operator==(XamlSlot const& other) const { return state == other.state; }
    bool IsIsland() const { return state && state->island; }

    void Initialize(Microsoft::UI::WindowId id) {
        auto& s = *state;
        s.top = GetWindowFromWindowId(id);
        auto* host = NativeWindowHost::Get(s.top);
        if (!host || !host->root) {
            s.island = DesktopWindowXamlSource();
            s.island.Initialize(id);
            s.bridge = GetWindowFromWindowId(s.island.SiteBridge().WindowId());
            return;
        }
        s.bridge = host->bridge;
        s.slot = Controls::Border();
        s.slot.Visibility(Visibility::Collapsed);
        host->root.Children().Append(s.slot);
        // Tab past the region's last control (or before its first) leaves it, as it left an island. XAML's
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
        return {state->top, state->bridge, state->slot, state->island ? state->island.SiteBridge() : nullptr};
    }
    void Content(UIElement const& content) const { if (state->island) state->island.Content(content); else state->slot.Child(content); }
    UIElement Content() const { return state->island ? state->island.Content() : state->slot.Child(); }
    // A region's backdrop belongs to the window, and a region never removes it: only the window's end does.
    void SystemBackdrop(Media::SystemBackdrop const& backdrop) const {
        if (state->island) { state->island.SystemBackdrop(backdrop); return; }
        if (!backdrop) return;
        if (auto* host = NativeWindowHost::Get(state->top); host && host->window) {
            host->window.SystemBackdrop(backdrop);
            // Gecko must not replace the WinUI Window's backdrop/frame after
            // binding. The property belongs to this HWND and dies with it.
            check_bool(SetPropW(state->top,L"Fenn.NativeBackdrop",reinterpret_cast<HANDLE>(1)));
        }
    }
    Media::SystemBackdrop SystemBackdrop() const {
        if (state->island) return state->island.SystemBackdrop();
        auto* host = NativeWindowHost::Get(state->top);
        return host && host->window ? host->window.SystemBackdrop() : nullptr;
    }
    template <class Handler> event_token TakeFocusRequested(Handler handler) const {
        if (state->island) return state->island.TakeFocusRequested(handler);
        state->takeFocus = [handler] { handler(nullptr, nullptr); };
        return {1};
    }
    void TakeFocusRequested(event_token const& token) const {
        if (state->island) state->island.TakeFocusRequested(token);
        else state->takeFocus = nullptr;
    }
    Microsoft::UI::Content::ContentIsland Island() const {
        auto content = Content();
        auto root = content ? content.XamlRoot() : nullptr;
        return root ? root.ContentIsland() : nullptr;
    }
    bool Contains(DependencyObject const& node) const { return Within(*state, node); }
    // Keyboard focus is on a control of this part: in its island, or in the window's XAML content window
    // and inside this region.
    bool HasFocus() const {
        if (state->island) return state->island.HasFocus();
        const HWND focused = GetFocus();
        if (!state->bridge || !focused || (focused != state->bridge && !IsChild(state->bridge, focused))) return false;
        if (!state->slot.XamlRoot()) return false;
        return Contains(Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(state->slot.XamlRoot()).try_as<DependencyObject>());
    }
    // True when a control of this part took keyboard focus.
    bool NavigateFocus(XamlSourceFocusNavigationRequest const& request) const {
        if (state->island) return state->island.NavigateFocus(request).WasFocusMoved();
        if (auto island = Island()) Microsoft::UI::Input::InputFocusController::GetForIsland(island).TrySetFocus();
        auto target = request.Reason() == XamlSourceFocusNavigationReason::Last
            ? Microsoft::UI::Xaml::Input::FocusManager::FindLastFocusableElement(state->slot)
            : Microsoft::UI::Xaml::Input::FocusManager::FindFirstFocusableElement(state->slot);
        auto element = target.try_as<UIElement>();
        return element && element.Focus(FocusState::Keyboard);
    }
    // A window message for `target` is about this part: it is in the part's island, or in the window's
    // XAML with the pointer over this region when the message was posted.
    bool Holds(HWND target) const {
        if (!*this || !target || !state->bridge || (target != state->bridge && !IsChild(state->bridge, target))) return false;
        if (state->island) return true;
        if (state->slot.Visibility() != Visibility::Visible) return false;
        const DWORD at = GetMessagePos();
        POINT point{GET_X_LPARAM(at), GET_Y_LPARAM(at)};
        ScreenToClient(state->top, &point);
        const float scale = SiteBridge().Scale();
        const double left = Controls::Canvas::GetLeft(state->slot) * scale, top = Controls::Canvas::GetTop(state->slot) * scale;
        return point.x >= left && point.y >= top &&
               point.x < left + state->slot.Width() * scale && point.y < top + state->slot.Height() * scale;
    }
    // A region's physical client rectangle, or empty while hidden. Islands are not regions.
    RECT Bounds() const {
        if (!state || !state->slot || state->slot.Visibility() != Visibility::Visible) return {};
        const float scale = SiteBridge().Scale();
        const LONG left = LONG(Controls::Canvas::GetLeft(state->slot) * scale + .5), top = LONG(Controls::Canvas::GetTop(state->slot) * scale + .5);
        return {left, top, left + LONG(state->slot.Width() * scale + .5), top + LONG(state->slot.Height() * scale + .5)};
    }
    // An island sits above Gecko's compositor, a sibling covering the client area. Asks the window
    // where it is rather than remembering: a restack by anything else still has to be undone. Returns
    // whether the compositor had to move. A region needs nothing: the window keeps its own order.
    bool Restack() const {
        if (!state || !state->island) return false;
        // Relative ordering is sufficient. HWND_TOP hides the SDK caption input window and breaks
        // native dragging. Never depend on SDK HWND class names.
        for (HWND sibling = GetWindow(state->bridge, GW_HWNDPREV); sibling; sibling = GetWindow(sibling, GW_HWNDPREV)) {
            wchar_t name[128]{};
            GetClassNameW(sibling, name, 128);
            if (wcscmp(name, L"MozillaCompositorWindowClass") == 0) {
                check_bool(SetWindowPos(sibling, state->bridge, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
                return true;
            }
        }
        return false;
    }
    // An island is clipped where Gecko's own controls show through it (island-relative exclusions);
    // Windows owns the region on success and it clips both rendering and hit testing. A region leaves
    // this to the page input window's region.
    void Clip(std::vector<RECT> const& exclusions) const {
        if (!state || !state->island) return;
        RECT bounds{};
        check_bool(GetClientRect(state->bridge, &bounds));
        HRGN region = CreateRectRgnIndirect(&bounds);
        if (!region) throw hresult_error(E_OUTOFMEMORY);
        for (auto exclusion : exclusions) {
            if (GetWindowLongPtrW(state->bridge, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) {
                const LONG left = exclusion.left;
                exclusion.left = bounds.right - exclusion.right;
                exclusion.right = bounds.right - left;
            }
            HRGN cut = CreateRectRgnIndirect(&exclusion);
            const int result = cut ? CombineRgn(region, region, cut, RGN_DIFF) : ERROR;
            if (cut) DeleteObject(cut);
            if (result == ERROR) { DeleteObject(region); throw hresult_error(E_FAIL); }
        }
        if (!SetWindowRgn(state->bridge, region, TRUE)) { DeleteObject(region); throw hresult_error(E_FAIL); }
    }
    // An island goes above the compositor and its sibling island, as the toolbar island did.
    void BringToTop() const {
        if (state && state->island && GetWindow(state->bridge, GW_HWNDPREV))
            check_bool(SetWindowPos(state->bridge, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
    }
    void Close() {
        if (!*this) return;
        auto& s = *state;
        if (s.island) {
            s.island.Close();
            s.island = nullptr;
            return;
        }
        // An island took keyboard focus with it, to the window and so to the page. A region shares the
        // window's XAML, so it hands focus to the page itself.
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
