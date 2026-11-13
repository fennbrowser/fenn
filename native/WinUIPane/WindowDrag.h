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
    weak_ref<Controls::Button> addButton;
    bool visible{}, updating{}, checkingLayout{};
    // A caption press on empty strip waits here for the drag threshold; see Proc.
    bool pendingCaption{};
    POINT pendingStart{};
    std::vector<Rect> ownedPass, caption, pass;
#if !defined(FENN_SHELL_PROCESS) || defined(WINUI_PANE_TESTING)
    bool pendingContextMenu{};
#endif
#ifndef FENN_SHELL_PROCESS
    std::vector<Rect> ownedCaption;
#endif
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
        for (auto r : systemClose)
            if (Contains(r, screen)) return HTCLOSE;
        for (auto r : systemMaximize)
            if (Contains(r, screen)) return HTMAXBUTTON;
        for (auto r : systemMinimize)
            if (Contains(r, screen)) return HTMINBUTTON;
        return HTNOWHERE;
    }
    bool OwnsCaptionPoint(POINT screen) const {
        if (!visible || SystemButtonAt(screen) != HTNOWHERE) return false;
        ScreenToClient(owner, &screen);
        return std::none_of(pass.begin(), pass.end(), [&](auto r) { return Contains(r, screen); }) &&
               std::any_of(caption.begin(), caption.end(), [&](auto r) { return Contains(r, screen); });
    }
    bool ShowCaptionMenu(POINT screen) {
        if (!OwnsCaptionPoint(screen)) return false;
        ScreenToClient(owner, &screen);
        auto bounds = Bounds(strip->root);
        auto scale = strip->root.XamlRoot().RasterizationScale();
        Windows::Foundation::Point position{float((screen.x - bounds.X) / scale), float((screen.y - bounds.Y) / scale)};
        auto weak = std::weak_ptr<int>(lifetime);
        strip->root.DispatcherQueue().TryEnqueue([this, weak, position] {
            if (weak.expired() || !visible) return;
            try {
                strip->ShowBarMenu(position);
            } catch (...) {}
        });
        return true;
    }
#if !defined(FENN_SHELL_PROCESS) || defined(WINUI_PANE_TESTING)
    bool TranslateContextMenu(MSG const& message) {
        // The SDK caption-input child can handle the click without forwarding
        // WM_NCRBUTTONUP to the owner. Route our own caption regions before
        // dispatch, without identifying/subclassing private SDK child classes.
        if (GetAncestor(message.hwnd, GA_ROOT) != owner) return false;
        if (message.message == WM_RBUTTONDOWN || message.message == WM_NCRBUTTONDOWN) {
            pendingContextMenu = OwnsCaptionPoint(message.pt);
            if (pendingContextMenu && GetForegroundWindow() != owner) SetForegroundWindow(owner);
            return pendingContextMenu;
        }
        if ((message.message == WM_RBUTTONUP || message.message == WM_NCRBUTTONUP) && pendingContextMenu) {
            pendingContextMenu = false;
            ShowCaptionMenu(message.pt);
            return true;
        }
        return false;
    }
#endif
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) noexcept {
        auto self = reinterpret_cast<NativeWindowDrag*>(data);
        try {
            if ((msg == WM_NCLBUTTONDOWN || msg == WM_NCLBUTTONUP || msg == WM_SYSCOMMAND) &&
                GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0))
                fprintf(stderr, "WinUIPane system-caption msg=%x hit=%llu region=%d\n", msg,
                        static_cast<unsigned long long>(w), self->SystemButtonAt(ScreenPoint(l)));
#ifndef FENN_SHELL_PROCESS
            // The separate shell's WinUI Window owns non-client hit testing.
            // The rollback host still needs to reconcile Gecko's hit targets
            // with the SDK regions registered for the native strip.
            if (msg == WM_NCHITTEST && self->visible) {
                if (auto button = self->SystemButtonAt(ScreenPoint(l)); button != HTNOWHERE) return button;
                auto normal = DefSubclassProc(hwnd, msg, w, l);
                // Gecko can retain hidden DOM caption-button rectangles after
                // tab-row replacement. Only the SDK regions above own buttons.
                if (normal == HTMINBUTTON || normal == HTMAXBUTTON || normal == HTCLOSE) normal = HTCLIENT;
                if (normal >= HTLEFT && normal <= HTBOTTOMRIGHT) return normal;
                POINT p = ScreenPoint(l);
                ScreenToClient(hwnd, &p);
                for (auto r : self->pass)
                    if (Contains(r, p)) return normal;
                for (auto r : self->caption)
                    if (Contains(r, p)) return HTCAPTION;
                return normal;
            }
            // Empty strip space is HTCAPTION. Gecko's custom-titlebar handling
            // turns non-client moves into DOM mouse moves at the same client
            // point, which lights up whatever retained toolbar control sits
            // beneath the native strip. The strip owns that space: keep the
            // move from reaching Gecko, but leave buttons and passthrough rects
            // (and the SDK's system-caption tracking) on the normal path.
            if (msg == WM_NCMOUSEMOVE && self->visible && w == HTCAPTION &&
                self->SystemButtonAt(ScreenPoint(l)) == HTNOWHERE) {
                POINT p = ScreenPoint(l);
                ScreenToClient(hwnd, &p);
                const bool passthrough =
                    std::any_of(self->pass.begin(), self->pass.end(), [&](auto rect) { return Contains(rect, p); });
                if (!passthrough) return 0;
            }
#endif
            if ((msg == WM_NCRBUTTONUP || msg == WM_CONTEXTMENU) && self->ShowCaptionMenu(ScreenPoint(l))) return 0;
#if !defined(FENN_SHELL_PROCESS) || defined(WINUI_PANE_TESTING)
            if (msg == WM_CANCELMODE || (msg == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE))
                self->pendingContextMenu = false;
#endif
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
                    POINT now{};
                    GetCursorPos(&now);
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
#ifdef FENN_SHELL_PROCESS
                // Let WinUI finish positioning the content bridge first.
                const auto weak = std::weak_ptr<int>(self->lifetime);
                const auto result = DefSubclassProc(hwnd, msg, w, l);
                if (!weak.expired()) {
                    try {
                        self->windowCaption.Update();
                    } catch (...) {}
                }
                return result;
#else
                self->windowCaption.Update();
#endif
            }
            if (msg == WM_NCDESTROY) {
                self->windowCaption.Close(false);
                RemoveWindowSubclass(hwnd, Proc, id);
                self->owner = nullptr;
            }
        } catch (...) {}
        return DefSubclassProc(hwnd, msg, w, l);
    }
    Rect Bounds(FrameworkElement const& element) const {
        auto local = element.TransformToVisual(nullptr).TransformBounds(
            {0, 0, float(element.ActualWidth()), float(element.ActualHeight())});
        auto screen = element.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
        POINT p{screen.X, screen.Y};
        ScreenToClient(owner, &p);
        return {p.x, p.y, screen.Width, screen.Height};
    }
    void Exclusions(DependencyObject const& node, std::vector<Rect>& rects) {
        if (auto element = node.try_as<FrameworkElement>(); element && element.Visibility() != Visibility::Visible)
            return;
        // TabView can retain departed containers through a layout transition.
        // Only the exact current row owns input; never exclude its retired tree.
        if (auto row = node.try_as<Controls::TabViewItem>()) {
            const auto id = unbox_value_or<uint32_t>(row.Tag(), 0);
            auto current = strip->rows.find(id);
            if (current == strip->rows.end() || current->second != row) return;
        }
        if (auto button = node.try_as<Controls::Button>(); button && button.Name() == L"AddButton") addButton = button;
        bool exclude = node.try_as<Controls::Primitives::ButtonBase>() ||
                       node.try_as<Controls::Primitives::ScrollBar>() || node.try_as<Controls::TabViewItem>();
        if (exclude) {
            if (auto element = node.try_as<FrameworkElement>()) rects.push_back(Bounds(element));
            return;
        }
        for (int i = 0; i < Media::VisualTreeHelper::GetChildrenCount(node); ++i)
            Exclusions(Media::VisualTreeHelper::GetChild(node, i), rects);
    }
    bool GeometryChanged() {
        // Observe bounded, actual owner-client geometry. Root/scroller dimensions
        // alone do not detect the final New Tab arrangement after a row closes.
        // Cache its template object during an already-required exclusion scan.
        ShellLayoutInputs inputs{};
        auto sample = [&](size_t at, FrameworkElement const& element) {
            if (!element || !element.IsLoaded()) return;
            auto r = Bounds(element);
            inputs.values[at] = r.X;
            inputs.values[at + 1] = r.Y;
            inputs.values[at + 2] = r.Width;
            inputs.values[at + 3] = r.Height;
        };
        sample(0, strip->root);
        sample(4, addButton.get());
        auto scroller = strip->inlineGroups.layoutScroller.get();
        sample(8, scroller);
        if (scroller && scroller.IsLoaded()) {
            inputs.values[12] = scroller.HorizontalOffset();
            inputs.values[13] = scroller.ExtentWidth();
            inputs.values[14] = int(scroller.ComputedHorizontalScrollBarVisibility());
        }
        inputs.values[15] =
            int(strip->root.Visibility()) + 2 * int(strip->root.IsLoaded()) + 4 * int(strip->view.FlowDirection());
        return strip->LayoutWork(SHELL_LAYOUT_WORK_CAPTION, inputs, strip->captionDirty || strip->ambient.observing);
    }
    void Replace(Kind kind, std::vector<Rect>& owned, std::vector<Rect> const& next) {
        if (owned == next) return;
        std::vector<Rect> all;
        for (auto r : input.GetRegionRects(kind))
            if (std::find(owned.begin(), owned.end(), r) == owned.end()) all.push_back(r);
        for (auto r : next)
            if (std::find(all.begin(), all.end(), r) == all.end()) all.push_back(r);
        owned = next;
        input.SetRegionRects(kind, all);
    }
    void RefreshCache() {
        auto c = input.GetRegionRects(Kind::Caption), p = input.GetRegionRects(Kind::Passthrough);
        caption.assign(c.begin(), c.end());
        pass.assign(p.begin(), p.end());
        auto min = input.GetRegionRects(Kind::Minimize), max = input.GetRegionRects(Kind::Maximize),
             close = input.GetRegionRects(Kind::Close);
        systemMinimize.assign(min.begin(), min.end());
        systemMaximize.assign(max.begin(), max.end());
        systemClose.assign(close.begin(), close.end());
    }
    void Update() {
        // Minimized HWND coordinates are placeholders, not caption geometry.
        // Keep the dirty state until restore publishes real client bounds.
        if (IsIconic(owner)) return;
        if (!strip || !strip->root.IsLoaded() || updating || !strip->captionDirty) return;
        if (!input && !AttachInput()) return;
        updating = true;
        try {
#ifdef WINUI_PANE_TESTING
            ++strip->captionPasses;
#endif
            windowCaption.Update();
            std::vector<Rect> excluded;
#ifndef FENN_SHELL_PROCESS
            std::vector<Rect> area;
#endif
            if (visible) {
#ifndef FENN_SHELL_PROCESS
                auto bounds = Bounds(strip->root);
                if (bounds.Width > 0 && bounds.Height > 0) area.push_back(bounds);
#endif
                Exclusions(strip->root, excluded);
                excluded.insert(excluded.end(), clipped.begin(), clipped.end());
            }
            // Passthrough holes keep real WinUI buttons and tab rows in
            // charge of their own input, even inside the caption rectangle.
            Replace(Kind::Passthrough, ownedPass, excluded);
#ifndef FENN_SHELL_PROCESS
            Replace(Kind::Caption, ownedCaption, area);
#endif
            RefreshCache();
            strip->captionDirty = false;
        } catch (...) {
            updating = false;
            throw;
        }
        updating = false;
    }
    void SetVisible(bool value) {
        windowCaption.SetVisible(value);
        if (visible != value && strip) strip->captionDirty = true;
        visible = value;
        Update();
    }
    // Exclusions are relative to the strip's region, whose client rectangle is `strip`.
    void SetClip(RECT const& stripBounds, std::vector<RECT> const& exclusions) {
        std::vector<Rect> next;
        for (auto r : exclusions)
            next.push_back({stripBounds.left + r.left, stripBounds.top + r.top, r.right - r.left, r.bottom - r.top});
        if (next == clipped) return;
        clipped = std::move(next);
        if (strip) {
            strip->captionDirty = true;
            Update();
        }
    }
    void Open(HWND hwnd, NativeTabStrip* value) {
        owner = hwnd;
        strip = value;
        windowCaption.Open(hwnd, strip->root);
        // Caption buttons activate an inactive window on press; a XAML Button
        // otherwise waits for its release. The Button handles Pressed itself.
        strip->allTabs.button.AddHandler(
            UIElement::PointerPressedEvent(),
            box_value(Microsoft::UI::Xaml::Input::PointerEventHandler([hwnd](auto const&, auto const&) {
                const HWND top = GetAncestor(hwnd, GA_ROOT);
                if (top && GetForegroundWindow() != top) SetForegroundWindow(top);
            })),
            true);
        auto weak = std::weak_ptr<int>(lifetime);
        strip->captionGeometryChanged = [this, weak] {
            if (weak.expired()) return;
            strip->captionDirty = true;
            Update();
        };
        strip->root.LayoutUpdated([this, weak](auto const&, auto const&) {
            if (weak.expired() || updating || checkingLayout) return;
            // A title edit can raise this global event without moving any
            // input target. Topology/realization invalidates explicitly; the
            // sample observes resize, scroll, DPI, placement and active motion.
            checkingLayout = true;
            try {
                if (GeometryChanged()) {
                    strip->captionDirty = true;
                    Update();
                }
            } catch (...) {
                strip->captionDirty = true;
            }
            checkingLayout = false;
        });
        strip->root.SizeChanged([this, weak](auto const&, auto const&) {
            if (!weak.expired()) strip->captionDirty = true;
        });
    }
    bool AttachInput() {
        input = Input::GetForWindowId(Microsoft::UI::GetWindowIdFromWindow(owner));
        if (!input) return false;  // Gecko has not initialized this AppWindow yet.
        check_bool(SetWindowSubclass(owner, Proc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)));
        auto weak = std::weak_ptr<int>(lifetime);
        regions = input.RegionsChanged([this, weak](auto const&, auto const&) {
            if (!weak.expired()) {
                try {
                    RefreshCache();
                } catch (...) {}
            }
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
            if (input) {
#ifndef FENN_SHELL_PROCESS
                Replace(Kind::Caption, ownedCaption, {});
#endif
                Replace(Kind::Passthrough, ownedPass, {});
            }
        } catch (...) {}
        input = nullptr;
        strip = nullptr;
        owner = nullptr;
    }
    ~NativeWindowDrag() { Close(); }
};
