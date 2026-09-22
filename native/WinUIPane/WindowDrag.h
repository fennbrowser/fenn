#pragma once
#include <commctrl.h>
#include <windowsx.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Interop.h>
#pragma comment(lib, "comctl32.lib")

// Windows/WinUI integration only. Firefox owns eligibility and adoption; the
// coordinator's decisions live in ShellCore. Never synthesize a caption press
// from PointerPressed: the SDK input region must own the original press.
struct NativeWindowDrag {
    using Rect = Windows::Graphics::RectInt32;
    using Input = Microsoft::UI::Input::InputNonClientPointerSource;
    using Kind = Microsoft::UI::Input::NonClientRegionKind;
    HWND owner{};
    NativeTabStrip* strip{};
    Input input{nullptr};
    Controls::Border mergePreview{nullptr};
    std::vector<Rect> clipped;
    bool visible{}, updating{}, inMove{}, released{}, cancelled{}, soleTab{}, movingOperation{};
    HHOOK filter{}, continuationFilter{};
    bool mergePosted{}, endingForMerge{}, continuationQueued{};
    std::vector<Rect> ownedCaption, ownedPass, caption, pass;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    std::function<void(POINT)> begin, move;
    std::function<void(POINT, bool)> finish;
    std::function<bool(POINT)> canMerge;
    std::function<void()> continueReady, abortContinuation;
    // Explicit tokens: the current SDK input source does not supply a usable
    // weak reference for C++/WinRT auto_revokers. Revoke before releasing it.
    event_token regions{};
    inline static NativeWindowDrag* waiting{};
    inline static NativeWindowDrag* active{}; // scoped owning-thread hook

    static bool Contains(Rect r, POINT p) {
        return p.x >= r.X && p.y >= r.Y && int64_t(p.x) < int64_t(r.X) + r.Width &&
               int64_t(p.y) < int64_t(r.Y) + r.Height;
    }
    static POINT ScreenPoint(LPARAM l) { return {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; }
    static LRESULT CALLBACK Filter(int code, WPARAM w, LPARAM l) noexcept {
        if (code >= 0 && w == PM_REMOVE && active) {
            auto msg = reinterpret_cast<MSG*>(l);
            if (msg->message == WM_LBUTTONUP || msg->message == WM_NCLBUTTONUP) active->released = true;
            if ((msg->message == WM_KEYDOWN && (msg->wParam == VK_ESCAPE || msg->wParam == VK_RETURN)) ||
                (msg->message == WM_CANCELMODE && !active->endingForMerge)) active->cancelled = true;
        }
        if (code >= 0 && w == PM_REMOVE && waiting) {
            auto msg = reinterpret_cast<MSG*>(l);
            if (msg->message == WM_LBUTTONUP || msg->message == WM_NCLBUTTONUP ||
                msg->message == WM_LBUTTONDOWN || msg->message == WM_NCLBUTTONDOWN ||
                (msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE)) {
                if (waiting->abortContinuation) waiting->abortContinuation();
            }
        }
        return CallNextHookEx(nullptr, code, w, l);
    }
    static UINT MergeMessage() {
        static const UINT value = RegisterWindowMessageW(L"FirefoxWinUIPane.WindowDrag.Merge.1");
        return value;
    }
    void RequestMerge() {
        if (inMove && !mergePosted && !endingForMerge && !cancelled) {
            mergePosted = PostMessageW(owner, MergeMessage(), 0, 0) != FALSE;
        }
    }
    void QueueContinuation() {
        if (!continuationFilter || continuationQueued || !continueReady || !strip) return;
        continuationQueued = true;
        auto weak = std::weak_ptr<int>(lifetime);
        if (!strip->root.DispatcherQueue().TryEnqueue([this, weak] {
            if (weak.expired()) return;
            continuationQueued = false;
            if (continuationFilter && continueReady) continueReady();
        })) continuationQueued = false;
    }
    void StopWaiting() noexcept {
        if (continuationFilter) UnhookWindowsHookEx(continuationFilter);
        continuationFilter = nullptr;
        if (waiting == this) waiting = nullptr;
        if (GetCapture() == owner) ReleaseCapture();
    }
    bool StartWaiting() {
        if (waiting && waiting != this && waiting->abortContinuation) waiting->abortContinuation();
        StopWaiting();
        waiting = this;
        continuationFilter = SetWindowsHookExW(WH_GETMESSAGE, Filter, nullptr, GetCurrentThreadId());
        SetCapture(owner);
        return continuationFilter != nullptr;
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) noexcept {
        auto self = reinterpret_cast<NativeWindowDrag*>(data);
        try {
            if (msg == MergeMessage()) {
                const bool requested = self->mergePosted;
                POINT point{}; GetCursorPos(&point);
                if (requested && self->inMove && !self->cancelled &&
                    (GetAsyncKeyState(VK_LBUTTON) & 0x8000) && self->canMerge && self->canMerge(point)) {
                    // End only our own move loop, without fabricating a button-up.
                    self->endingForMerge = true;
                    if (!PostMessageW(hwnd, WM_CANCELMODE, 0, 0)) self->endingForMerge = false;
                }
                self->mergePosted = false;
                return 0;
            }
            if (msg == WM_SYSCOMMAND) self->movingOperation = (w & 0xfff0) == SC_MOVE;
            if (msg == WM_ENTERSIZEMOVE && self->movingOperation && !self->inMove) {
                self->released = self->cancelled = self->mergePosted = self->endingForMerge = false;
                self->inMove = true; active = self;
                self->filter = SetWindowsHookExW(WH_GETMESSAGE, Filter, nullptr, GetCurrentThreadId());
                if (!self->filter) self->cancelled = true;
                POINT point{}; GetCursorPos(&point);
                if (self->begin) self->begin(point);
            }
            if (msg == WM_EXITSIZEMOVE && self->inMove) {
                const bool commit = (self->released || self->endingForMerge) && !self->cancelled;
                self->StopFilter();
                self->movingOperation = false;
                self->strip->captionDirty = true;
                POINT point{}; GetCursorPos(&point);
                if (self->finish) self->finish(point, commit);
                self->endingForMerge = false;
            }
            if (msg == WM_MOVING && self->inMove) {
                POINT point{}; GetCursorPos(&point);
                if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) self->cancelled = true;
                if (self->move) self->move(point);
            }
            if (msg == WM_NCHITTEST && self->visible) {
                const auto normal = DefSubclassProc(hwnd, msg, w, l);
                if (normal >= HTLEFT && normal <= HTBOTTOMRIGHT) return normal;
                POINT p = ScreenPoint(l); ScreenToClient(hwnd, &p);
                for (auto r : self->pass) if (Contains(r, p)) return normal;
                for (auto r : self->caption) if (Contains(r, p)) return HTCAPTION;
                return normal;
            }
            if (msg == WM_NCLBUTTONDOWN || msg == WM_NCLBUTTONDBLCLK ||
                msg == WM_NCRBUTTONUP || msg == WM_CONTEXTMENU || msg == WM_NCMBUTTONUP) {
                if (auto tab = self->TabAt(ScreenPoint(l))) {
                    if (msg == WM_NCLBUTTONDBLCLK) return 0;
                    if (msg == WM_NCLBUTTONDOWN &&
                        ((GetKeyState(VK_CONTROL) | GetKeyState(VK_SHIFT)) & 0x8000)) {
                        self->strip->select(tab);
                        return 0;
                    }
                    if (msg == WM_NCRBUTTONUP || msg == WM_CONTEXTMENU) {
                        auto weak = std::weak_ptr<int>(self->lifetime);
                        self->strip->root.DispatcherQueue().TryEnqueue([self, weak, tab] {
                            if (weak.expired() || !self->strip->rows.count(tab)) return;
                            auto row = self->strip->rows.at(tab);
                            try {
                                row.Focus(FocusState::Pointer);
                                row.ContextFlyout().ShowAt(row);
                            } catch (...) {
                                if (GetEnvironmentVariableW(L"FIREFOX_WINUI_PANE_DIAGNOSTICS", nullptr, 0))
                                    fprintf(stderr, "WinUIPane caption-menu failed hr=%08x\n", unsigned(to_hresult()));
                            }
                        });
                        return 0;
                    }
                    if (msg == WM_NCMBUTTONUP) { self->strip->close(tab); return 0; }
                }
            }
            if ((msg == WM_CANCELMODE && !self->endingForMerge) || (msg == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE)) self->cancelled = true;
            if (msg == WM_CAPTURECHANGED && self->inMove && l && HWND(l) != hwnd) self->cancelled = true;
            if (msg == WM_WINDOWPOSCHANGED && self->strip) self->strip->captionDirty = true;
            if (msg == WM_NCDESTROY) {
                self->cancelled = true;
                self->StopFilter();
                RemoveWindowSubclass(hwnd, Proc, id);
                self->owner = nullptr;
            }
        } catch (...) { self->cancelled = true; }
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
    uint32_t TabAt(POINT screen) const {
        if (!visible || !strip || !strip->windowDragTabId || strip->rows.size() != 1 ||
            !strip->rows.count(strip->windowDragTabId)) return 0;
        ScreenToClient(owner, &screen);
        for (auto r : pass) if (Contains(r, screen)) return 0;
        return Contains(Bounds(strip->rows.at(strip->windowDragTabId)), screen) ? strip->windowDragTabId : 0;
    }
    Rect Bounds(FrameworkElement const& element) const {
        auto local = element.TransformToVisual(nullptr).TransformBounds(
            {0, 0, float(element.ActualWidth()), float(element.ActualHeight())});
        auto screen = element.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
        POINT p{screen.X, screen.Y}; ScreenToClient(owner, &p);
        return {p.x, p.y, screen.Width, screen.Height};
    }
    void Exclusions(DependencyObject const& node, std::vector<Rect>& rects, bool sole) {
        if (auto element = node.try_as<FrameworkElement>(); element && element.Visibility() != Visibility::Visible) return;
        bool exclude = node.try_as<Controls::Primitives::ButtonBase>() || node.try_as<Controls::Primitives::ScrollBar>() ||
                       (!sole && node.try_as<Controls::TabViewItem>());
        if (exclude) {
            if (auto element = node.try_as<FrameworkElement>()) rects.push_back(Bounds(element));
            return;
        }
        for (int i = 0; i < Media::VisualTreeHelper::GetChildrenCount(node); ++i)
            Exclusions(Media::VisualTreeHelper::GetChild(node, i), rects, sole);
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
    }
    void Update() {
        if (!strip || !strip->root.IsLoaded() || updating || !strip->captionDirty) return;
        if (!input && !AttachInput()) return;
        updating = true;
        try {
            const bool sole = visible && strip->windowDragTabId && strip->rows.size() == 1 &&
                              strip->rows.count(strip->windowDragTabId);
            if (inMove && soleTab && !sole) cancelled = true;
            soleTab = sole;
            std::vector<Rect> excluded, area;
            if (visible) {
                auto bounds = Bounds(strip->root);
                if (bounds.Width > 0 && bounds.Height > 0) area.push_back(bounds);
                Exclusions(strip->root, excluded, sole);
                excluded.insert(excluded.end(), clipped.begin(), clipped.end());
            }
            // Passthrough holes keep real WinUI buttons and multi-tab rows in
            // charge of their own input, even inside the caption rectangle.
            Replace(Kind::Passthrough, ownedPass, excluded);
            Replace(Kind::Caption, ownedCaption, area);
            RefreshCache();
            strip->captionDirty = false;
        } catch (...) { updating = false; throw; }
        updating = false;
    }
    void SetVisible(bool value) {
        if (visible != value && strip) strip->captionDirty = true;
        visible = value;
        Update();
    }
    void Preview(bool show) {
        if (mergePreview) mergePreview.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
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
    void StopFilter() noexcept {
        if (filter) UnhookWindowsHookEx(filter);
        filter = nullptr;
        if (active == this) active = nullptr;
        inMove = false;
    }
    void Open(HWND hwnd, NativeTabStrip* value) {
        owner = hwnd; strip = value;
        mergePreview = Markup::XamlReader::Load(
            LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" BorderBrush="{ThemeResource AccentFillColorDefaultBrush}" BorderThickness="2" CornerRadius="4" Margin="1" IsHitTestVisible="False" Visibility="Collapsed" AutomationProperties.AutomationId="FirefoxWinUIWindowMergePreview" AutomationProperties.AccessibilityView="Raw"/>)").as<Controls::Border>();
        strip->root.Children().Append(mergePreview);
        auto weak = std::weak_ptr<int>(lifetime);
        strip->root.LayoutUpdated([this, weak](auto const&, auto const&) {
            if (weak.expired()) return;
            // Child controls can move or appear without changing the root size.
            strip->captionDirty = true;
            try { Update(); QueueContinuation(); } catch (...) { cancelled = true; }
        });
        strip->root.SizeChanged([this, weak](auto const&, auto const&) { if (!weak.expired()) strip->captionDirty = true; });
    }
    bool AttachInput() {
        input = Input::GetForWindowId(Microsoft::UI::GetWindowIdFromWindow(owner));
        if (!input) return false; // Gecko has not initialized this AppWindow yet.
        check_bool(SetWindowSubclass(owner, Proc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)));
        auto weak = std::weak_ptr<int>(lifetime);
        regions = input.RegionsChanged([this, weak](auto const&, auto const&) {
            if (!weak.expired()) { try { RefreshCache(); } catch (...) { cancelled = true; } }
        });
        return true;
    }
    void Close() noexcept {
        lifetime.reset();
        StopWaiting();
        StopFilter();
        if (input) {
            input.RegionsChanged(regions);
        }
        if (owner) RemoveWindowSubclass(owner, Proc, reinterpret_cast<UINT_PTR>(this));
        try {
            if (input) { Replace(Kind::Caption, ownedCaption, {}); Replace(Kind::Passthrough, ownedPass, {}); }
        } catch (...) {}
        input = nullptr; mergePreview = nullptr; strip = nullptr; owner = nullptr;
    }
    ~NativeWindowDrag() { Close(); }
};
