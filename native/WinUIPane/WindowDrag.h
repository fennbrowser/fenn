#pragma once
#include "WindowCaption.h"
#include <commctrl.h>
#include <windowsx.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Interop.h>
#pragma comment(lib, "comctl32.lib")

// Windows/WinUI integration only: caption/passthrough regions and the system
// caption buttons. Tabs always keep their own XAML pointer input; only empty
// strip space is draggable. Never synthesize a caption press from
// PointerPressed: the SDK input region must own the original press.
struct NativeWindowDrag {
    using Rect = Windows::Graphics::RectInt32;
    using Input = Microsoft::UI::Input::InputNonClientPointerSource;
    using Kind = Microsoft::UI::Input::NonClientRegionKind;
    HWND owner{};
    NativeTabStrip* strip{};
    NativeWindowCaption windowCaption;
    Input input{nullptr};
    std::vector<Rect> clipped;
    bool visible{}, updating{};
    // A caption press on empty strip waits here for the drag threshold; see Proc.
    bool pendingCaption{};
    POINT pendingStart{};
    std::vector<Rect> ownedCaption, ownedPass, caption, pass;
    std::vector<Rect> systemMinimize, systemMaximize, systemClose;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    // Explicit tokens: the current SDK input source does not supply a usable
    // weak reference for C++/WinRT auto_revokers. Revoke before releasing it.
    event_token regions{};

    static bool Contains(Rect r, POINT p) {
        return p.x >= r.X && p.y >= r.Y && int64_t(p.x) < int64_t(r.X) + r.Width &&
               int64_t(p.y) < int64_t(r.Y) + r.Height;
    }
    static POINT ScreenPoint(LPARAM l) { return {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; }
    int SystemButtonAt(POINT screen) const {
        if (!visible) return HTNOWHERE;
        ScreenToClient(owner, &screen);
        for (auto r : systemClose) if (Contains(r, screen)) return HTCLOSE;
        for (auto r : systemMaximize) if (Contains(r, screen)) return HTMAXBUTTON;
        for (auto r : systemMinimize) if (Contains(r, screen)) return HTMINBUTTON;
        return HTNOWHERE;
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) noexcept {
        auto self = reinterpret_cast<NativeWindowDrag*>(data);
        try {
            if ((msg == WM_NCLBUTTONDOWN || msg == WM_NCLBUTTONUP || msg == WM_SYSCOMMAND) &&
                GetEnvironmentVariableW(L"FIREFOX_WINUI_PANE_DIAGNOSTICS", nullptr, 0))
                fprintf(stderr, "WinUIPane system-caption msg=%x hit=%llu region=%d\n", msg,
                        static_cast<unsigned long long>(w), self->SystemButtonAt(ScreenPoint(l)));
            if (msg == WM_NCHITTEST && self->visible) {
                if (auto button = self->SystemButtonAt(ScreenPoint(l)); button != HTNOWHERE) return button;
                const auto normal = DefSubclassProc(hwnd, msg, w, l);
                if (normal >= HTLEFT && normal <= HTBOTTOMRIGHT) return normal;
                POINT p = ScreenPoint(l); ScreenToClient(hwnd, &p);
                for (auto r : self->pass) if (Contains(r, p)) return normal;
                for (auto r : self->caption) if (Contains(r, p)) return HTCAPTION;
                return normal;
            }
            if (msg == WM_NCRBUTTONUP || msg == WM_CONTEXTMENU) {
                if (self->visible && self->SystemButtonAt(ScreenPoint(l)) == HTNOWHERE) {
                    POINT point = ScreenPoint(l);
                    ScreenToClient(self->owner, &point);
                    const bool excluded = std::any_of(self->pass.begin(), self->pass.end(),
                        [&](auto rect) { return Contains(rect, point); });
                    const bool caption = std::any_of(self->caption.begin(), self->caption.end(),
                        [&](auto rect) { return Contains(rect, point); });
                    if (caption && !excluded) {
                        auto bounds = self->Bounds(self->strip->root);
                        auto scale = self->strip->root.XamlRoot().RasterizationScale();
                        Windows::Foundation::Point position{float((point.x - bounds.X) / scale),
                                                            float((point.y - bounds.Y) / scale)};
                        auto weak = std::weak_ptr<int>(self->lifetime);
                        self->strip->root.DispatcherQueue().TryEnqueue([self, weak, position] {
                            if (weak.expired() || !self->visible) return;
                            try { self->strip->ShowBarMenu(position); } catch (...) {}
                        });
                        return 0;
                    }
                }
            }
            // Handing a plain caption press to DefWindowProc starts the system move
            // loop at once. That loop blocks this thread's dispatcher and island
            // rendering until release, so activation fades land late. Hold the press
            // under our own capture and start the system move only once the pointer
            // passes the drag threshold; a click never enters the loop.
            if (msg == WM_NCLBUTTONDOWN && w == HTCAPTION && self->visible && !self->pendingCaption &&
                (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
                self->pendingCaption = true;
                self->pendingStart = ScreenPoint(l);
                // DefWindowProc activates an inactive window while handling this
                // press; we are skipping it, so activate here.
                if (GetForegroundWindow() != hwnd) SetForegroundWindow(hwnd);
                SetCapture(hwnd);
                return 0;
            }
            if (self->pendingCaption) {
                const bool lost = msg == WM_CANCELMODE || (msg == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) ||
                                  (msg == WM_CAPTURECHANGED && HWND(l) != hwnd) || msg == WM_NCDESTROY;
                if (msg == WM_MOUSEMOVE) {
                    POINT now{}; GetCursorPos(&now);
                    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
                        self->pendingCaption = false;
                        if (GetCapture() == hwnd) ReleaseCapture();
                        return 0;
                    }
                    if (std::abs(now.x - self->pendingStart.x) > GetSystemMetrics(SM_CXDRAG) ||
                        std::abs(now.y - self->pendingStart.y) > GetSystemMetrics(SM_CYDRAG)) {
                        self->pendingCaption = false;
                        if (GetCapture() == hwnd) ReleaseCapture();
                        return DefSubclassProc(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(now.x, now.y));
                    }
                    return 0;
                }
                if (msg == WM_LBUTTONUP) {
                    self->pendingCaption = false;
                    if (GetCapture() == hwnd) ReleaseCapture();
                    return 0;
                }
                if (lost) self->pendingCaption = false;
            }
            if (msg == WM_ACTIVATE && self->strip) self->strip->allTabs.SetWindowActive(LOWORD(w) != WA_INACTIVE);
            if (msg == WM_WINDOWPOSCHANGED && self->strip) {
                self->strip->captionDirty = true;
                self->windowCaption.Update();
            }
            if (msg == WM_NCDESTROY) {
                self->windowCaption.Close(false);
                RemoveWindowSubclass(hwnd, Proc, id);
                self->owner = nullptr;
            }
        } catch (...) {}
        return DefSubclassProc(hwnd, msg, w, l);
    }
    static void Restack(HWND island) {
        // Relative ordering is sufficient. HWND_TOP hides the SDK caption input
        // window and breaks native dragging. Never depend on SDK HWND class names.
        for (HWND sibling = GetWindow(island, GW_HWNDPREV); sibling; sibling = GetWindow(sibling, GW_HWNDPREV)) {
            wchar_t name[128]{};
            GetClassNameW(sibling, name, 128);
            if (wcscmp(name, L"MozillaCompositorWindowClass") == 0) {
                check_bool(SetWindowPos(sibling, island, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
                break;
            }
        }
    }
    Rect Bounds(FrameworkElement const& element) const {
        auto local = element.TransformToVisual(nullptr).TransformBounds(
            {0, 0, float(element.ActualWidth()), float(element.ActualHeight())});
        auto screen = element.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
        POINT p{screen.X, screen.Y}; ScreenToClient(owner, &p);
        return {p.x, p.y, screen.Width, screen.Height};
    }
    void Exclusions(DependencyObject const& node, std::vector<Rect>& rects) {
        if (auto element = node.try_as<FrameworkElement>(); element && element.Visibility() != Visibility::Visible) return;
        bool exclude = node.try_as<Controls::Primitives::ButtonBase>() || node.try_as<Controls::Primitives::ScrollBar>() ||
                       node.try_as<Controls::TabViewItem>();
        if (exclude) {
            if (auto element = node.try_as<FrameworkElement>()) rects.push_back(Bounds(element));
            return;
        }
        for (int i = 0; i < Media::VisualTreeHelper::GetChildrenCount(node); ++i)
            Exclusions(Media::VisualTreeHelper::GetChild(node, i), rects);
    }
    void Replace(Kind kind, std::vector<Rect>& owned, std::vector<Rect> const& next) {
        if (owned == next) return;
        std::vector<Rect> all;
        for (auto r : input.GetRegionRects(kind))
            if (std::find(owned.begin(), owned.end(), r) == owned.end()) all.push_back(r);
        for (auto r : next) if (std::find(all.begin(), all.end(), r) == all.end()) all.push_back(r);
        owned = next;
        input.SetRegionRects(kind, all);
    }
    void RefreshCache() {
        auto c = input.GetRegionRects(Kind::Caption), p = input.GetRegionRects(Kind::Passthrough);
        caption.assign(c.begin(), c.end()); pass.assign(p.begin(), p.end());
        auto min = input.GetRegionRects(Kind::Minimize), max = input.GetRegionRects(Kind::Maximize), close = input.GetRegionRects(Kind::Close);
        systemMinimize.assign(min.begin(), min.end());
        systemMaximize.assign(max.begin(), max.end());
        systemClose.assign(close.begin(), close.end());
    }
    void Update() {
        if (!strip || !strip->root.IsLoaded() || updating || !strip->captionDirty) return;
        if (!input && !AttachInput()) return;
        updating = true;
        try {
            windowCaption.Update();
            std::vector<Rect> excluded, area;
            if (visible) {
                auto bounds = Bounds(strip->root);
                if (bounds.Width > 0 && bounds.Height > 0) area.push_back(bounds);
                Exclusions(strip->root, excluded);
                excluded.insert(excluded.end(), clipped.begin(), clipped.end());
            }
            // Passthrough holes keep real WinUI buttons and tab rows in
            // charge of their own input, even inside the caption rectangle.
            Replace(Kind::Passthrough, ownedPass, excluded);
            Replace(Kind::Caption, ownedCaption, area);
            RefreshCache();
            strip->captionDirty = false;
        } catch (...) { updating = false; throw; }
        updating = false;
    }
    void SetVisible(bool value) {
        windowCaption.SetVisible(value);
        if (visible != value && strip) strip->captionDirty = true;
        visible = value;
        Update();
    }
    void SetClip(HWND island, std::vector<RECT> const& exclusions) {
        std::vector<Rect> next;
        RECT bounds{}; check_bool(GetWindowRect(island, &bounds));
        for (auto r : exclusions) {
            POINT p{bounds.left + r.left, bounds.top + r.top}; ScreenToClient(owner, &p);
            next.push_back({p.x, p.y, r.right - r.left, r.bottom - r.top});
        }
        if (next == clipped) return;
        clipped = std::move(next);
        if (strip) { strip->captionDirty = true; Update(); }
    }
    void Open(HWND hwnd, NativeTabStrip* value) {
        owner = hwnd; strip = value;
        windowCaption.Open(hwnd, strip->root, strip->allTabs.button, AllTabsButton::kWidth);
        // Caption buttons activate an inactive window on press; a XAML Button
        // otherwise waits for its release. The Button handles Pressed itself.
        strip->allTabs.button.AddHandler(UIElement::PointerPressedEvent(),
            box_value(Microsoft::UI::Xaml::Input::PointerEventHandler([hwnd](auto const&, auto const&) {
                const HWND top = GetAncestor(hwnd, GA_ROOT);
                if (top && GetForegroundWindow() != top) SetForegroundWindow(top);
            })), true);
        auto weak = std::weak_ptr<int>(lifetime);
        strip->root.LayoutUpdated([this, weak](auto const&, auto const&) {
            if (weak.expired()) return;
            // Child controls can move or appear without changing the root size.
            strip->captionDirty = true;
            try { Update(); } catch (...) {}
        });
        strip->root.SizeChanged([this, weak](auto const&, auto const&) { if (!weak.expired()) strip->captionDirty = true; });
    }
    bool AttachInput() {
        input = Input::GetForWindowId(Microsoft::UI::GetWindowIdFromWindow(owner));
        if (!input) return false; // Gecko has not initialized this AppWindow yet.
        check_bool(SetWindowSubclass(owner, Proc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)));
        auto weak = std::weak_ptr<int>(lifetime);
        regions = input.RegionsChanged([this, weak](auto const&, auto const&) {
            if (!weak.expired()) { try { RefreshCache(); } catch (...) {} }
        });
        return true;
    }
    void Close() noexcept {
        lifetime.reset();
        windowCaption.Close();
        if (input) {
            input.RegionsChanged(regions);
        }
        if (owner) RemoveWindowSubclass(owner, Proc, reinterpret_cast<UINT_PTR>(this));
        try {
            if (input) { Replace(Kind::Caption, ownedCaption, {}); Replace(Kind::Passthrough, ownedPass, {}); }
        } catch (...) {}
        input = nullptr; strip = nullptr; owner = nullptr;
    }
    ~NativeWindowDrag() { Close(); }
};
