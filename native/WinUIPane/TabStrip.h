#pragma once
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <winrt/Microsoft.UI.Input.h>
#include "JsonValidation.h"
#include "TabGroupsButton.h"
#include "TabDragImage.h"

// Strip-only experiment: TabView owns presentation, Firefox owns all mutations.
// No content pages or template internals are used. CanDragTabs/CanReorderTabs
// are OFF permanently: enabling them reliably fail-fasts inside
// Microsoft.UI.Xaml.dll's own native drag-drop implementation the moment a
// drag starts in the full embedded browser, though not in a minimal
// island-only test host. Crash-dump analysis and upstream reports
// (microsoft/microsoft-ui-xaml#7007, #8450) show this is a long-standing
// WinUI bug, not something fixable on our side by configuration. Reordering
// is instead hand-rolled below (DragState/PointerPressed and friends): plain
// pointer capture and manual sibling-shift animations never touch WinUI's
// native OLE drag-drop path, so the crash can never trigger. Real OS-level
// DoDragDrop was tried too and hit
// the exact same fail-fast from a different, WinUI-internal entry point
// (SystemDropOperationTargetAdapter, automatically bridging any real OLE
// drag over any XAML Island) -- so this strip now drives the whole gesture
// itself, never touching OLE at all. See Pane.cpp's TabDragSession/
// TickDragSession for the process-wide, capture-independent tracking this
// depends on.
struct NativeTabStrip {
    Controls::Grid root{nullptr};
    Controls::TabView view{nullptr};
    NativeTabMenu& menu;
    std::function<void(uint32_t)> select, close;
    std::function<void()> add;
    // This window's own real top-level HWND, set by Pane.cpp at
    // construction -- identifies "this window" in cross-window adopt
    // requests (QueueIncomingAdopt/AcceptForeignDrop below) and is the
    // target of the raw ::SetCapture a drag-arm takes (Sync()'s
    // PointerPressed handler), so mouse input keeps routing to this
    // process for the gesture's duration regardless of where the cursor
    // ends up on screen.
    HWND ownerHwnd{};
    // Set by Pane.cpp at construction: armDragSession is called once, at
    // PointerPressed, to create the process-wide TabDragSession
    // (Pane.cpp) that tracks this gesture from here on independent of any
    // one WinUI event actually firing. tickDragSession is Pane.cpp's own
    // TickDragSession function, called directly by PointerMoved while
    // already Dragging as a low-latency accelerant on top of the same
    // polling Pane.cpp's Translate() hook already drives -- both routes
    // converge on that one function, so which one fires first never
    // matters for correctness.
    std::function<void(uint32_t)> armDragSession;
    std::function<void()> tickDragSession;
    std::map<uint32_t, Controls::TabViewItem> rows;
    // Favicons are the small end of what NativeAddress's row images (104,
    // 65536) allow: one glyph-sized icon per tab, but potentially many more
    // tabs per snapshot than address rows, so the per-snapshot total is its
    // own budget rather than reusing kIconImageLimits or NativeAddress's.
    static constexpr NativeJson::ImageLimits kTabImageLimits{32, 262144};
    struct Presentation {
        hstring detail;
        uint32_t flags{};
        hstring status;
        hstring imagePixels;
        uint32_t imageWidth{}, imageHeight{};
    };
    std::map<uint32_t, Presentation> presentation;
    TabGroupsButton groupsButton;
    uint32_t presentationRevision{};
    hstring presentationFingerprint;
    bool syncing{};
    // While a hand-rolled drag is in progress, Sync must not touch
    // TabItems() at all -- the drag only ever moves rows visually (a
    // RenderTransform), so there is nothing in TabItems() for Sync to fight,
    // but a Firefox update landing mid-gesture (another tab's title, a new
    // tab) must not restyle rows out from under the drag's own bookkeeping.
    // The completed drag becomes one validated request, handed straight to
    // the pane's queue by `dispatch`; Firefox's own moveTabTo/moveTabBefore
    // is the sole authority on the result, and the next Sync() that actually
    // reorders TabItems() clears the drag's leftover visual offsets --
    // correcting anything the gesture visually allowed that Firefox didn't
    // apply (crossing the pinned boundary, leaving a group/split), same as
    // the rest of this strip's optimistic actions.
    bool dragging{};

    // Hands one completed gesture (reorder, detach, incoming adopt) to the
    // owning pane's request queue, which wakes the host. `tab` is this
    // window's own id for the tab being acted on, or 0 when the gesture names
    // no tab of this window's (an incoming adopt names the source window's).
    std::function<void(uint32_t tab, hstring const& action)> dispatch;
    // Coalesces the mid-drag active-tab highlight below: at most one apply is
    // ever queued on the dispatcher at a time, and a Sync() landing before it
    // runs just updates the target id rather than queuing a second one.
    bool highlightQueued{};
    uint32_t queuedActive{};
    // The active tab as Firefox last stated it, so an incremental apply can
    // tell "active" from "also selected" without a snapshot.
    uint32_t activeTabId{};
    enum class DragPhase { None, Armed, Dragging };
    struct DragState {
        DragPhase phase{};
        uint32_t tabId{};
        uint32_t originalIndex{};
        Windows::Foundation::Point startPoint{};
        double tabWidth{};
        uint32_t projectedIndex{};
        // Left edge of each row in TabItems() order, captured once when the
        // drag threshold is crossed and never re-read mid-gesture: sibling
        // shift visuals only affect rendering, so hit-testing against a live
        // (already-shifted) position would feed back into itself.
        std::vector<double> originalLeftEdge;
        // Rows touched by this gesture (the dragged one, any shifted
        // siblings) whose transform needs resetting once the real reorder
        // this gesture requested actually lands via Sync().
        std::vector<uint32_t> affectedRowIds;
        // Set at commit time; Sync() only clears affectedRowIds' transforms
        // once the incoming authoritative order actually places tabId
        // immediately before beforeId (0 meaning "at the end") -- resetting
        // on whatever Sync() call happens to run next, before Firefox's
        // confirmation has actually landed, would snap the row back to its
        // pre-drag spot for a frame.
        bool awaitingConfirm{};
        uint32_t confirmTabId{};
        uint32_t confirmBeforeId{};
        // Set once the pointer clears the strip's own bounds by a real
        // margin -- past that point this is a tear-off, not a reorder:
        // siblings stop making room (CloseDragGap below). Whether it's a
        // genuine detach (CommitDetach) or a drop into a different pane's
        // own strip (AcceptForeignDrop) is resolved geometrically by
        // Pane.cpp's TickDragSession against every open pane's own
        // StripScreenBounds(), not by this flag -- this flag only ever
        // decides the visual on THIS strip (make room vs. don't).
        bool detaching{};
    };
    DragState drag;
    // Self-driven window-move state for empty-strip-space dragging -- see
    // the PointerPressed handler's own comment (constructor, below) for why
    // this doesn't just hand off to Windows' own WM_NCLBUTTONDOWN/HTCAPTION
    // mechanism. Fully independent of DragState above: a tab drag arms on a
    // row's own PointerPressed, this arms on `root`'s, and OriginatesOnInteractive
    // keeps the two from ever both claiming the same physical press.
    struct WindowMoveState {
        bool active{};
        POINT startCursor{};
        RECT startWindowRect{};
        // Own double-click-to-maximize/restore detection, since owning this
        // gesture end to end means DefWindowProc's own NC-caption handling
        // (which tracks consecutive HTCAPTION clicks itself) never runs.
        DWORD lastClickTime{};
        POINT lastClickCursor{};
    };
    WindowMoveState windowMove;
    struct RowAnimation {
        Media::Animation::Storyboard storyboard{nullptr};
        double target{};
    };
    // One in-flight sibling-shift Storyboard per row id, so a row re-targeted
    // (gap opens, closes, reopens) before its current slide finishes gets
    // stopped and restarted rather than fighting itself -- except when the
    // new target is the same one already in flight, which is left alone
    // rather than restarted from scratch: ShiftSibling recomputes every
    // *other* row on every projected-index change even though most of their
    // targets didn't actually change, and restarting an already-correct
    // slide on every one of those calls kept resetting its easing progress,
    // so it never looked like it actually finished moving. Also stopped
    // before any direct (non-animated) transform reset -- an active
    // Storyboard otherwise keeps driving the property and the reset would
    // be ignored.
    std::map<uint32_t, RowAnimation> rowAnimations;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    NativeTabStrip(NativeTabMenu& model) : menu(model) {
        root =
            Markup::XamlReader::Load(
                LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource SolidBackgroundFillColorBaseBrush}"><TabView TabWidthMode="Equal" CanDragTabs="False" CanReorderTabs="False" AllowDrop="False" VerticalAlignment="Top" AutomationProperties.AutomationId="FirefoxWinUITabStrip" AutomationProperties.Name="Tabs in this window" /></Grid>)")
                .as<Controls::Grid>();
        view = root.Children().GetAt(0).as<Controls::TabView>();
        view.TabStripHeader(groupsButton.button);
        groupsButton.currentRevision = [this] { return presentationRevision; };
        auto alive = std::weak_ptr<int>(lifetime);
        view.SelectionChanged([this, alive](auto const&, auto const&) {
            if (alive.expired() || syncing || !view.SelectedItem()) return;
            // Modified gestures are observed on the exact tab below. Never let
            // TabView's single selection replace Firefox's multiselected set.
            if (GetKeyState(VK_CONTROL) & 0x8000 || GetKeyState(VK_SHIFT) & 0x8000) return;
            auto row = view.SelectedItem().as<Controls::TabViewItem>();
            select(unbox_value<uint32_t>(row.Tag()));
        });
        view.TabCloseRequested([this, alive](auto const&, Controls::TabViewTabCloseRequestedEventArgs const& args) {
            if (!alive.expired() && !syncing) close(unbox_value<uint32_t>(args.Tab().Tag()));
        });
        view.AddTabButtonClick([this, alive](auto const&, auto const&) {
            if (!alive.expired() && !syncing) add();
        });
        // A native top bar should move the window when dragged by its own
        // empty space, the same as any ordinary title bar -- but the JS
        // side deliberately sets -moz-window-dragging:no-drag on this
        // pane's own hosting element (browser-winui-pane.js), since the
        // XAML Island sitting on top of that DOM region would otherwise
        // fight Gecko's own CSS-driven drag detection for the same input.
        // That leaves nothing to move the window at all once the island
        // takes over that whole area, so this replaces it.
        //
        // The first attempt forwarded the press to the real top-level
        // window as WM_NCLBUTTONDOWN/HTCAPTION (the message a real title
        // bar generates, letting DefWindowProc's own modal move loop take
        // over). That doesn't work reliably here: reentering
        // DefWindowProc's own move-tracking loop from inside WinUI's own
        // PointerPressed dispatch means it's racing WinUI's own,
        // independent pointer-tracking for the exact same physical
        // gesture. Confirmed live: nothing moved while the button was
        // actually held, then sometime after release the window snapped to
        // the cursor and kept tracking it, unbounded, until the next click
        // -- consistent with DefWindowProc's loop never actually seeing the
        // real button-up (WinUI's own tracking still had it), only later
        // picking up on some unrelated stray mouse-move that happened to
        // reach the top-level window instead of the Island, then having no
        // real terminating condition left to end on.
        //
        // Uses the same fix the tab-drag session itself needed for the
        // equivalent problem: don't depend on any one event-delivery path
        // WinUI/Gecko might contest -- track the physical gesture with raw
        // Win32 state instead (TickWindowMove, ticked from Pane.cpp's
        // Translate() hook the same way TickDragSession is), so which
        // event-delivery mechanism does or doesn't fire never matters for
        // correctness. Since this drives the whole gesture itself, it also
        // has to detect its own double-click-to-maximize/restore --
        // DefWindowProc's own HTCAPTION handling would have covered that
        // for free.
        root.AddHandler(
            UIElement::PointerPressedEvent(),
            box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                [this, alive](auto const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                    if (alive.expired() || syncing) return;
                    auto point = args.GetCurrentPoint(root);
                    if (!point.Properties().IsLeftButtonPressed()) return;
                    if (OriginatesOnInteractive(args.OriginalSource())) return;
                    args.Handled(true);
                    POINT cursor{};
                    ::GetCursorPos(&cursor);
                    DWORD now = ::GetTickCount();
                    bool doubleClick =
                        windowMove.lastClickTime && now - windowMove.lastClickTime <= ::GetDoubleClickTime() &&
                        std::abs(cursor.x - windowMove.lastClickCursor.x) <= ::GetSystemMetrics(SM_CXDOUBLECLK) &&
                        std::abs(cursor.y - windowMove.lastClickCursor.y) <= ::GetSystemMetrics(SM_CYDOUBLECLK);
                    windowMove.lastClickTime = now;
                    windowMove.lastClickCursor = cursor;
                    if (doubleClick) {
                        windowMove.lastClickTime = 0;  // don't chain into a triple-click toggling back
                        WINDOWPLACEMENT placement{sizeof(placement)};
                        ::GetWindowPlacement(ownerHwnd, &placement);
                        ::ShowWindow(ownerHwnd,
                                     placement.showCmd == SW_SHOWMAXIMIZED ? SW_RESTORE : SW_SHOWMAXIMIZED);
                        return;
                    }
                    ::SetCapture(ownerHwnd);
                    windowMove.active = true;
                    windowMove.startCursor = cursor;
                    ::GetWindowRect(ownerHwnd, &windowMove.startWindowRect);
                })),
            true);
        // TabView's internal ListView plays its default add/remove theme
        // transitions (fade out then remove, fade in on insert) whenever a
        // row's position in TabItems() changes via the RemoveAt+InsertAt
        // pattern Sync() uses below -- for any reorder, not just a
        // hand-rolled drag. Left in place, every settle looks like the
        // moved row (or whichever other row's slot shifted) vanishing and
        // fading back in. The template isn't applied yet at construction
        // time, so the inner ListView can only be found once Loaded fires.
        view.Loaded([this, alive](auto const&, auto const&) {
            if (alive.expired()) return;
            if (auto listView = FindTabListView(view)) listView.ItemContainerTransitions(Media::Animation::TransitionCollection{});
        });
    }

    static Controls::ListViewBase FindTabListView(DependencyObject const& node) {
        if (!node) return nullptr;
        if (auto listView = node.try_as<Controls::ListViewBase>()) return listView;
        int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int i = 0; i < count; ++i)
            if (auto found = FindTabListView(Media::VisualTreeHelper::GetChild(node, i))) return found;
        return nullptr;
    }
    ~NativeTabStrip() { lifetime.reset(); }

    // Queues one validated reorder request naming `tab`'s real neighbor: the
    // row currently after it in TabItems(), or 0 ("move to end") if it's
    // last. Correct whenever TabItems()'s own order already reflects the
    // completed move -- used by the test-only WinUIPaneTestCompleteReorder
    // hook (Pane.cpp) to drive this logic without a real pointer gesture.
    void CompleteReorder(Controls::TabViewItem const& tab) {
        uint32_t index{};
        if (!tab || !view.TabItems().IndexOf(tab, index)) return;
        CompleteReorderAt(tab, index + 1 < view.TabItems().Size()
                                    ? view.TabItems().GetAt(index + 1).as<Controls::TabViewItem>()
                                    : nullptr);
    }

    // Same, but for the hand-rolled drag below: TabItems()'s real order never
    // changes during a visual-only gesture, so the neighbor is resolved from
    // the caller's own hit-tested target index (still valid against the
    // unchanged TabItems() order at commit time) rather than looked up from
    // TabItems() itself.
    void CompleteReorder(Controls::TabViewItem const& tab, uint32_t targetIndex) {
        if (!tab) return;
        CompleteReorderAt(tab, targetIndex < view.TabItems().Size()
                                    ? view.TabItems().GetAt(targetIndex).as<Controls::TabViewItem>()
                                    : nullptr);
    }

    void CompleteReorderAt(Controls::TabViewItem const& tab, Controls::TabViewItem const& beforeRow) {
        uint32_t beforeId = beforeRow && beforeRow != tab ? unbox_value<uint32_t>(beforeRow.Tag()) : 0;
        Windows::Data::Json::JsonObject action;
        action.SetNamedValue(L"kind", Windows::Data::Json::JsonValue::CreateStringValue(L"strip-reorder"));
        action.SetNamedValue(L"revision", Windows::Data::Json::JsonValue::CreateNumberValue(presentationRevision));
        action.SetNamedValue(L"id", Windows::Data::Json::JsonValue::CreateNumberValue(unbox_value<uint32_t>(tab.Tag())));
        action.SetNamedValue(L"beforeId", Windows::Data::Json::JsonValue::CreateNumberValue(beforeId));
        if (dispatch) dispatch(unbox_value<uint32_t>(tab.Tag()), action.Stringify());
    }

    // Queues one validated detach request naming `tab` and the release
    // point in screen coordinates, on the same transport as
    // CompleteReorderAt above (just a different request `kind`) -- Firefox
    // opens the new window at that point via gBrowser.replaceTabWithWindow's
    // own left/top options, so there is nothing to compute here beyond the
    // cursor's current screen position.
    void CompleteDetach(Controls::TabViewItem const& tab) {
        POINT screenPoint{};
        GetCursorPos(&screenPoint);
        Windows::Data::Json::JsonObject action;
        action.SetNamedValue(L"kind", Windows::Data::Json::JsonValue::CreateStringValue(L"strip-detach"));
        action.SetNamedValue(L"revision", Windows::Data::Json::JsonValue::CreateNumberValue(presentationRevision));
        action.SetNamedValue(L"id", Windows::Data::Json::JsonValue::CreateNumberValue(unbox_value<uint32_t>(tab.Tag())));
        action.SetNamedValue(L"screenX", Windows::Data::Json::JsonValue::CreateNumberValue(double(screenPoint.x)));
        action.SetNamedValue(L"screenY", Windows::Data::Json::JsonValue::CreateNumberValue(double(screenPoint.y)));
        if (dispatch) dispatch(unbox_value<uint32_t>(tab.Tag()), action.Stringify());
    }

    // For a cross-window drop landing at `localX` (already converted to
    // this strip's own local coordinate space by the caller, which alone
    // has the HWND that conversion needs). This strip's own TabItems() are
    // never touched by another window's drag, so this is a plain hit-test
    // against each row's real current position -- unlike ProjectDropIndex,
    // there is no dragged row of its own to skip or snapshot to hit-test
    // against instead of a live (but here, unmoving) layout.
    uint32_t ProjectExternalDropIndex(double localX) const {
        uint32_t index = 0;
        for (uint32_t i = 0; i < view.TabItems().Size(); ++i) {
            auto row = view.TabItems().GetAt(i).as<Controls::TabViewItem>();
            auto left = row.TransformToVisual(view).TransformPoint({0, 0}).X;
            if (left + row.ActualWidth() / 2 < localX) index = i + 1;
        }
        return index;
    }

    // Queues a validated adopt request on this strip's own transport, on
    // behalf of a tab dropped here from a different window's drag -- same
    // shape as CompleteReorderAt/CompleteDetach above, just naming the
    // source window instead of a beforeId, since the tab doesn't exist in
    // this window's own rows yet for Sync()'s usual confirm-tracking to
    // watch (Firefox adopting it in will simply appear as an ordinary new
    // row on this window's next Sync(), same as any other opened tab).
    void QueueIncomingAdopt(HWND sourceHwnd, uint32_t tabId, uint32_t index) {
        Windows::Data::Json::JsonObject action;
        action.SetNamedValue(L"kind", Windows::Data::Json::JsonValue::CreateStringValue(L"strip-adopt"));
        action.SetNamedValue(L"revision", Windows::Data::Json::JsonValue::CreateNumberValue(presentationRevision));
        action.SetNamedValue(L"sourceHwnd", Windows::Data::Json::JsonValue::CreateNumberValue(
                                                 double(reinterpret_cast<uintptr_t>(sourceHwnd))));
        action.SetNamedValue(L"id", Windows::Data::Json::JsonValue::CreateNumberValue(tabId));
        action.SetNamedValue(L"index", Windows::Data::Json::JsonValue::CreateNumberValue(index));
        // The adopted tab belongs to the source window, not this one, so this
        // request names no tab in this window's id space.
        if (dispatch) dispatch(0, action.Stringify());
    }

    static Media::TranslateTransform RowTransform(Controls::TabViewItem const& row) {
        return row.RenderTransform().as<Media::TranslateTransform>();
    }

    // True if `source` is (or is inside) a button between it and `row` --
    // lets the stock close button keep its normal input untouched by either
    // the multiselect click handler or the drag-arm handler below.
    static bool OriginatesOnButton(winrt::Windows::Foundation::IInspectable const& source, Controls::TabViewItem const& row) {
        auto node = source.try_as<DependencyObject>();
        while (node && node != row) {
            if (node.try_as<Controls::Primitives::ButtonBase>()) return true;
            node = Media::VisualTreeHelper::GetParent(node);
        }
        return false;
    }

    // True if `source` is (or is inside) a tab row or a button anywhere up
    // to `root` -- the window-move handler above only wants to catch a
    // press on genuinely empty strip background, not one that happens to
    // land on (or bubble from) an actual tab, its close button, the add-tab
    // button, or the groups button.
    bool OriginatesOnInteractive(winrt::Windows::Foundation::IInspectable const& source) const {
        auto node = source.try_as<DependencyObject>();
        while (node && node != root) {
            if (node.try_as<Controls::TabViewItem>() || node.try_as<Controls::Primitives::ButtonBase>()) return true;
            node = Media::VisualTreeHelper::GetParent(node);
        }
        return false;
    }

    // Finds the TabItems() index the dragged tab should land before, given
    // its current visual center, by counting how many OTHER rows' original
    // (untransformed) centers it has now passed. That count is the target's
    // position among the OTHER rows alone; translating back to a real
    // TabItems() index just has to skip over the dragged row's own original
    // slot, since that order is otherwise unchanged for the whole gesture.
    uint32_t ProjectDropIndex(double centerX) const {
        uint32_t before = 0;
        for (uint32_t i = 0; i < drag.originalLeftEdge.size(); ++i) {
            if (i == drag.originalIndex) continue;
            if (drag.originalLeftEdge[i] + drag.tabWidth / 2 < centerX) ++before;
        }
        return before < drag.originalIndex ? before : before + 1;
    }

    // Storyboard::Stop() doesn't leave a property at its current mid-flight
    // value -- it reverts to whatever the value was before that Storyboard
    // began. Capture the true current value first and restore it right
    // after, or anything that stops an in-flight slide (a retarget, a new
    // drag grabbing a row whose previous drop is still easing in) snaps the
    // row backward first, fighting itself instead of continuing smoothly.
    void StopRowAnimation(Controls::TabViewItem const& row, uint32_t id) {
        auto it = rowAnimations.find(id);
        if (it == rowAnimations.end()) return;
        double current = RowTransform(row).X();
        it->second.storyboard.Stop();
        RowTransform(row).X(current);
        rowAnimations.erase(it);
    }

    // Slides a row's TranslateTransform.X to `target` over a short ease-out,
    // matching the original design's ~120-150ms Storyboard spec.
    void AnimateRowOffset(Controls::TabViewItem const& row, uint32_t id, double target) {
        if (auto it = rowAnimations.find(id); it != rowAnimations.end()) {
            if (it->second.target == target) return;
            StopRowAnimation(row, id);
        }
        Media::Animation::DoubleAnimation animation;
        animation.To(target);
        // Duration is a plain {TimeSpan, DurationType} struct, not a class
        // with a real constructor -- brace-initializing with just a TimeSpan
        // only sets the first field and leaves Type at its default,
        // DurationType::Automatic (0), which silently ignores the TimeSpan
        // and falls back to XAML's own built-in 1-second default. Type must
        // be set explicitly, or the duration below is set for nothing.
        animation.Duration(
            Duration{Windows::Foundation::TimeSpan{std::chrono::milliseconds(135)}, DurationType::TimeSpan});
        Media::Animation::CubicEase ease;
        ease.EasingMode(Media::Animation::EasingMode::EaseOut);
        animation.EasingFunction(ease);
        Media::Animation::Storyboard::SetTarget(animation, RowTransform(row));
        Media::Animation::Storyboard::SetTargetProperty(animation, L"X");
        Media::Animation::Storyboard storyboard;
        storyboard.Children().Append(animation);
        auto alive = std::weak_ptr<int>(lifetime);
        storyboard.Completed([this, alive, id](auto const&, auto const&) {
            if (!alive.expired()) rowAnimations.erase(id);
        });
        rowAnimations[id] = RowAnimation{storyboard, target};
        storyboard.Begin();
    }

    // Stops any in-flight slide for `id` and sets its offset immediately --
    // for resets where the row must land exactly on Sync()'s own timing
    // (once a reorder is confirmed) rather than ease into place.
    void ResetRowOffset(Controls::TabViewItem const& row, uint32_t id) {
        if (auto it = rowAnimations.find(id); it != rowAnimations.end()) {
            it->second.storyboard.Stop();
            rowAnimations.erase(it);
        }
        RowTransform(row).X(0);
        // Y only ever moves for a tear-off gesture (UpdateDragVisual below);
        // resetting it unconditionally is a no-op for every other row.
        RowTransform(row).Y(0);
    }

    // Applies (or clears) the one-tab-width visual offset that opens a gap
    // for the dragged tab at its current projected position. Rows between
    // the dragged tab's original slot and its projected target shift toward
    // that slot to make room; everything else sits at its original position.
    void ShiftSibling(uint32_t originalIndex) {
        auto row = view.TabItems().GetAt(originalIndex).as<Controls::TabViewItem>();
        double target = 0;
        if (originalIndex < drag.originalIndex && originalIndex >= drag.projectedIndex) target = drag.tabWidth;
        else if (originalIndex > drag.originalIndex && originalIndex < drag.projectedIndex) target = -drag.tabWidth;
        auto id = unbox_value<uint32_t>(row.Tag());
        AnimateRowOffset(row, id, target);
        if (target != 0 &&
            std::find(drag.affectedRowIds.begin(), drag.affectedRowIds.end(), id) == drag.affectedRowIds.end())
            drag.affectedRowIds.push_back(id);
    }

    // Re-derives the reorder/tear-off state from the cursor's current
    // position, clamped the same way a row confined to this strip's own
    // width would be -- but doesn't touch the dragged row's own visual at
    // all anymore. Pane.cpp's floating popup (TabDragImage.h) is the sole
    // visual for the dragged tab for the whole gesture now, not just once
    // it leaves a strip; the dragged row itself just sits hidden
    // (TryPromote hides it once that popup's own snapshot is ready, and
    // every commit/cancel path below restores it). Keeping one mechanism
    // instead of switching between "real row" and "floating bitmap" at
    // each hover transition is what actually fixed the class of bugs that
    // switch kept producing (a duplicate visual, an inconsistent flush/
    // elevated look depending on which one currently owned the row, two
    // independently-tuned clamps disagreeing with each other) -- there's
    // no transition left to get wrong. `point` is the pointer's current
    // position relative to `view`, used both for the horizontal reorder
    // math and to notice the gesture has cleared the strip's own vertical
    // bounds and become a tear-off.
    void UpdateDragVisual(Windows::Foundation::Point const& point) {
        double origin = drag.originalLeftEdge[drag.originalIndex];
        double minX = -origin, maxX = view.ActualWidth() - drag.tabWidth - origin;
        double rawDeltaX = point.X - drag.startPoint.X;
        double clampedX = std::clamp(rawDeltaX, std::min(minX, maxX), std::max(minX, maxX));
        // A real margin past the strip's own top/bottom, not just a small
        // wobble while reordering within it -- matches the "drag it clearly
        // off the strip" feel of a real browser's tear-off.
        double stripHeight = view.ActualHeight();
        bool wasDetaching = drag.detaching;
        drag.detaching = stripHeight > 0 && (point.Y < -stripHeight || point.Y > stripHeight * 2);
        if (drag.detaching) {
            // Just left the strip with a reorder gap still open -- close it,
            // since nothing should be making room for a tab that's leaving.
            if (!wasDetaching && drag.projectedIndex != drag.originalIndex) {
                drag.projectedIndex = drag.originalIndex;
                for (uint32_t i = 0; i < drag.originalLeftEdge.size(); ++i)
                    if (i != drag.originalIndex) ShiftSibling(i);
            }
            return;
        }
        auto projected = ProjectDropIndex(origin + drag.tabWidth / 2 + clampedX);
        if (projected == drag.projectedIndex) return;
        drag.projectedIndex = projected;
        for (uint32_t i = 0; i < drag.originalLeftEdge.size(); ++i) {
            if (i != drag.originalIndex) ShiftSibling(i);
        }
    }

    // Undoes whatever TryPromote's render-completion callback hid (see its
    // own comment) once a gesture actually ends. A no-op if the row is
    // already gone, or was never actually hidden (a gesture that ends
    // before the popup's own snapshot finished rendering never got this
    // far in the first place).
    void RestoreRowVisibility(uint32_t id) {
        if (rows.count(id)) rows.at(id).Opacity(1);
    }

    // Moves this pane's own top-level window to follow the cursor, for as
    // long as the button the PointerPressed handler above armed on stays
    // physically down. Called on every message Pane.cpp's Translate() hook
    // sees, the same way TabDragSession's own TickDragSession is -- ground
    // truth is GetCursorPos()/GetAsyncKeyState(), never dependent on any
    // one WinUI/Gecko event actually firing, for the same reason that
    // matters for tab dragging (see the constructor's own comment on why
    // handing this off to WM_NCLBUTTONDOWN didn't work reliably).
    void TickWindowMove() {
        if (!windowMove.active) return;
        if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
            windowMove.active = false;
            if (GetCapture() == ownerHwnd) ReleaseCapture();
            return;
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        int dx = cursor.x - windowMove.startCursor.x, dy = cursor.y - windowMove.startCursor.y;
        SetWindowPos(ownerHwnd, nullptr, windowMove.startWindowRect.left + dx, windowMove.startWindowRect.top + dy, 0,
                     0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // Ends an armed-but-never-promoted gesture: a plain click (selection
    // already happened normally on press, nothing to commit) or capture
    // lost while still Armed (nothing has moved yet, so there's nothing to
    // recover). Once a gesture is promoted to Dragging, Pane.cpp's
    // TabDragSession owns resolving it end to end (CommitReorder/
    // AcceptForeignDrop/CommitDetach/CancelDrag below) -- this is also
    // reused there, for the outcome where a foreign pane's own strip
    // accepted the drop, to reset this strip's own phase/dragging flag.
    void FinishDrag(uint32_t id) {
        if (drag.tabId != id) return;
        RestoreRowVisibility(id);
        drag.phase = DragPhase::None;
        dragging = false;
    }

    // Promotes Armed -> Dragging using `point` (view-local DIPs, same space
    // PointerMoved's own GetCurrentPoint(view) uses) as the drag's live
    // position. Two independent callers can each observe the same 4 DIP
    // threshold crossing and call this: PointerMoved below (the low-latency
    // path, for as long as WinUI's own capture on this row happens to
    // survive) and Pane.cpp's TickDragSession (the GetCursorPos/
    // GetAsyncKeyState-driven poll that doesn't depend on it at all, see
    // the tab drag redesign). Idempotent once already
    // Dragging (or once `id` no longer matches), so calling it from both is
    // safe -- whichever notices first wins.
    bool TryPromote(uint32_t id, Windows::Foundation::Point const& point) {
        if (drag.tabId != id || drag.phase != DragPhase::Armed || !rows.count(id)) return false;
        constexpr double kDragThreshold = 4.0;
        // Either axis can arm the drag -- a tear-off gesture often starts as
        // a mostly-vertical pull, which a horizontal-only threshold would
        // never observe crossing.
        if (std::max(std::abs(point.X - drag.startPoint.X), std::abs(point.Y - drag.startPoint.Y)) < kDragThreshold)
            return false;
        auto& row = rows.at(id);
        drag.phase = DragPhase::Dragging;
        drag.detaching = false;
        // Any row still easing from a prior settle or sibling-shift (this
        // row's own drop settle if it's grabbed again quickly, or a sibling
        // shift that hadn't finished) must be stopped before the snapshot
        // below, or this drag's grid math would be built on stale,
        // mid-animation coordinates -- and for this row specifically,
        // leaving its own leftover settle running would let it later force
        // the transform back to 0 once it completes, fighting this drag's
        // own direct manipulation.
        {
            std::vector<uint32_t> activeIds;
            for (auto& [animId, anim] : rowAnimations) activeIds.push_back(animId);
            for (auto animId : activeIds)
                if (rows.count(animId)) StopRowAnimation(rows.at(animId), animId);
        }
        drag.originalIndex = 0;
        view.TabItems().IndexOf(row, drag.originalIndex);
        drag.projectedIndex = drag.originalIndex;
        drag.tabWidth = row.ActualWidth();
        drag.originalLeftEdge.clear();
        for (uint32_t i = 0; i < view.TabItems().Size(); ++i) {
            auto item = view.TabItems().GetAt(i).as<Controls::TabViewItem>();
            drag.originalLeftEdge.push_back(item.TransformToVisual(view).TransformPoint({0, 0}).X);
        }
        dragging = true;
        UpdateDragVisual(point);
        return true;
    }

    // Closes any open reorder gap -- called once the cursor leaves this
    // strip's own bounds (Pane.cpp's TickDragSession, geometrically, via
    // StripScreenBounds) without having dropped here, mirroring
    // UpdateDragVisual's own detach-branch logic: nothing should keep
    // making room for a tab that's no longer hovering.
    void CloseDragGap() {
        if (drag.phase != DragPhase::Dragging || drag.projectedIndex == drag.originalIndex) return;
        drag.projectedIndex = drag.originalIndex;
        for (uint32_t i = 0; i < drag.originalLeftEdge.size(); ++i)
            if (i != drag.originalIndex) ShiftSibling(i);
    }

    // Commits this exact tab's own in-flight gesture as a self-reorder --
    // called by Pane.cpp's TickDragSession once release resolves as still
    // hovering this same strip.
    bool CommitReorder(uint32_t tabId, Windows::Foundation::Point const& point) {
        if (drag.tabId != tabId || drag.phase != DragPhase::Dragging || !rows.count(tabId)) return false;
        UpdateDragVisual(point);
        auto row = rows.at(tabId);
        auto targetIndex = drag.projectedIndex;
        drag.affectedRowIds.push_back(tabId);
        drag.awaitingConfirm = true;
        drag.confirmTabId = tabId;
        auto beforeRow = targetIndex < view.TabItems().Size()
                              ? view.TabItems().GetAt(targetIndex).as<Controls::TabViewItem>()
                              : nullptr;
        drag.confirmBeforeId = beforeRow && beforeRow != row ? unbox_value<uint32_t>(beforeRow.Tag()) : 0;
        CompleteReorderAt(row, beforeRow);
        RestoreRowVisibility(tabId);
        // Unlike every other exit path (FinishDrag/CancelDrag), this never
        // reset drag.phase back to None on success -- harmless in practice
        // (the next PointerPressed unconditionally overwrites it to Armed
        // regardless), except that a stale Dragging phase left on this same
        // tab could make a later, unrelated Escape keypress on it (while
        // merely focused, nothing to do with dragging) incorrectly invoke
        // CancelDrag via its own `drag.phase != DragPhase::None` guard.
        drag.phase = DragPhase::None;
        dragging = false;
        return true;
    }

    // Commits this exact tab's own in-flight gesture as a detach-to-new-
    // window -- called by Pane.cpp's TickDragSession once release resolves
    // as hovering no pane's own strip at all. `confirmBeforeId` of 0 with
    // `awaitingConfirm` set means the same thing it does for a plain
    // move-to-end reorder (CompleteReorderAt), but here it's actually
    // "landed" the moment Sync() no longer sees this tab at all: it will
    // have moved to the new window Firefox opens for it, not stayed at the
    // strip's own end. Any siblings this gesture shifted to open a gap
    // before detaching still get cleaned up the same way (CloseDragGap
    // already closes that gap as soon as the cursor left this strip, but
    // doesn't clear affectedRowIds -- this keeps the same eventual reset
    // Sync() already does for every other outcome).
    void CommitDetach(uint32_t id) {
        if (!rows.count(id)) return;
        drag.affectedRowIds.push_back(id);
        drag.awaitingConfirm = true;
        drag.confirmTabId = id;
        drag.confirmBeforeId = 0;
        CompleteDetach(rows.at(id));
        RestoreRowVisibility(id);
        // See CommitReorder's own comment: this never reset drag.phase back
        // to None on success either.
        drag.phase = DragPhase::None;
        dragging = false;
    }

    // Live "make room" preview shown in this strip while a DIFFERENT pane's
    // own drag hovers over it. Mirrors UpdateDragVisual/ShiftSibling's
    // sibling-shift half for the self-drag case, but simpler: there's no
    // dragged row of this strip's own to skip, so every row at or past the
    // projected insertion index shifts uniformly by one tab width to open a
    // single gap. Original (untransformed) positions are snapshotted once,
    // like DragState::originalLeftEdge, so repeated projection hit-tests
    // against a stable layout rather than feeding an already-shifted one
    // back into itself.
    struct ForeignDragState {
        bool active{};
        uint32_t projectedIndex{};
        double tabWidth{};
        struct Row {
            Controls::TabViewItem item{nullptr};
            uint32_t id{};
            double left{};
        };
        // Keep the exact objects we transformed. Collection indices can name
        // different rows, or no row at all, after a Firefox structural update.
        std::vector<Row> rows;
    };
    ForeignDragState foreignDrag;

    void ForeignDragOver(Windows::Foundation::Point const& point) {
        if (!foreignDrag.active) {
            // A preceding snapshot may have changed the layout. Capture the
            // new untransformed geometry once, not on every drag tick.
            view.UpdateLayout();
            foreignDrag.active = true;
            foreignDrag.projectedIndex = ~0u;
            foreignDrag.tabWidth =
                view.TabItems().Size() ? view.TabItems().GetAt(0).as<Controls::TabViewItem>().ActualWidth() : 0;
            foreignDrag.rows.clear();
            for (uint32_t i = 0; i < view.TabItems().Size(); ++i) {
                auto item = view.TabItems().GetAt(i).as<Controls::TabViewItem>();
                foreignDrag.rows.push_back({item, unbox_value<uint32_t>(item.Tag()),
                                           item.TransformToVisual(view).TransformPoint({0, 0}).X});
            }
        }
        uint32_t projected = 0;
        for (uint32_t i = 0; i < foreignDrag.rows.size(); ++i)
            if (foreignDrag.rows[i].left + foreignDrag.tabWidth / 2 < point.X) projected = i + 1;
        if (projected == foreignDrag.projectedIndex) return;
        foreignDrag.projectedIndex = projected;
        for (uint32_t i = 0; i < foreignDrag.rows.size(); ++i) {
            auto const& row = foreignDrag.rows[i];
            AnimateRowOffset(row.item, row.id, i >= projected ? foreignDrag.tabWidth : 0);
        }
    }

    // Closes the preview gap immediately -- called once the drag leaves
    // this strip's own bounds without dropping here, and once a drop here
    // is resolved (the real new row, once Firefox's adopt actually lands on
    // some later Sync(), arrives through the ordinary insert path with no
    // gap of its own to settle into; holding this preview open across that
    // round trip would need the same awaitingConfirm-style bookkeeping
    // DragState uses for a reordered EXISTING row, which a brand-new row has
    // no matching "before" state to justify).
    void EndForeignDragOver() {
        if (!foreignDrag.active) return;
        auto previous = std::exchange(foreignDrag, ForeignDragState{});
        for (auto const& row : previous.rows) ResetRowOffset(row.item, row.id);
    }

    // Commits a tab dropped here from a DIFFERENT pane's own in-flight
    // gesture -- called by Pane.cpp's TickDragSession once release resolves
    // as hovering this strip while some other pane owns the gesture.
    // `point` is already translated to this strip's own local coordinate
    // space (Pane.cpp's StripScreenBounds owns that conversion, since it
    // alone knows the hosting HWND). Always succeeds -- there is no local
    // drag state to validate against here, unlike CommitReorder; Firefox's
    // own adopt handler and the JS session's snapshot-based validation are
    // the real authority, same as every other queued request this strip
    // sends.
    bool AcceptForeignDrop(HWND sourceHwnd, uint32_t tabId, Windows::Foundation::Point const& point) {
        // Sync invalidates a preview before a structural update. Rebuild it
        // even if release arrives before the next hover tick, so a drop never
        // commits an insertion index from the previous destination row set.
        ForeignDragOver(point);
        uint32_t index = foreignDrag.projectedIndex;
        EndForeignDragOver();
        QueueIncomingAdopt(sourceHwnd, tabId, index);
        return true;
    }

    // Escape: abandon the gesture with no request sent, snapping every
    // touched row back immediately -- nothing has been committed yet, so
    // there is nothing to await from Firefox before resetting.
    void CancelDrag() {
        if (drag.phase == DragPhase::Dragging && rows.count(drag.tabId)) {
            auto row = rows.at(drag.tabId);
            for (auto pointer : row.PointerCaptures()) row.ReleasePointerCapture(pointer);
            RestoreRowVisibility(drag.tabId);
            for (uint32_t i = 0; i < drag.originalLeftEdge.size(); ++i) {
                auto sibling = view.TabItems().GetAt(i).as<Controls::TabViewItem>();
                ResetRowOffset(sibling, unbox_value<uint32_t>(sibling.Tag()));
            }
        }
        drag.phase = DragPhase::None;
        drag.detaching = false;
        drag.affectedRowIds.clear();
        dragging = false;
    }

    // Parse a complete bounded copy before changing XAML or accepting any IDs.
    void Publish(NativeTabMenu::Object const& data) {
        if (data.GetNamedNumber(L"version") != 1) throw hresult_invalid_argument();
        auto revision = NativeJson::RequireId(data, L"revision");
        auto fingerprint = data.Stringify();
        if (revision < presentationRevision ||
            (revision == presentationRevision && fingerprint != presentationFingerprint))
            throw hresult_invalid_argument();
        if (revision == presentationRevision) return;
        auto label = data.GetNamedString(L"groupsLabel");
        const bool rtl = data.GetNamedBoolean(L"rtl");
        auto entries = data.GetNamedArray(L"rows"), groups = data.GetNamedArray(L"groups");
        if (label.empty() || label.size() > NativeJson::kMaxShortLabel || entries.Size() > NativeJson::kMaxTabs ||
            groups.Size() > NativeJson::kMaxTabs)
            throw hresult_invalid_argument();
        std::map<uint32_t, Presentation> next;
        size_t totalImagePixels = 0;
        for (auto value : entries) {
            auto entry = value.GetObject();
            auto id = NativeJson::RequireId(entry, L"id");
            auto flags = entry.GetNamedNumber(L"flags");
            auto detail = entry.GetNamedString(L"detail");
            auto status = entry.GetNamedString(L"status", L"");
            if (!std::isfinite(flags) || flags < 0 || flags > 15 || std::floor(flags) != flags ||
                detail.size() > NativeJson::kMaxDetailText || status.size() > NativeJson::kMaxDetailText)
                throw hresult_invalid_argument();
            NativeJson::ValidateImage(entry, totalImagePixels, kTabImageLimits);
            hstring imagePixels;
            uint32_t imageWidth{}, imageHeight{};
            if (entry.HasKey(L"image") &&
                entry.GetNamedValue(L"image").ValueType() != winrt::Windows::Data::Json::JsonValueType::Null) {
                auto image = entry.GetNamedObject(L"image");
                imagePixels = image.GetNamedString(L"pixels");
                imageWidth = uint32_t(image.GetNamedNumber(L"width"));
                imageHeight = uint32_t(image.GetNamedNumber(L"height"));
            }
            if (!next.emplace(id, Presentation{detail, uint32_t(flags), status, imagePixels, imageWidth, imageHeight})
                     .second)
                throw hresult_invalid_argument();
        }
        std::set<uint32_t> ids;
        for (auto value : groups) {
            auto group = value.GetObject();
            auto id = NativeJson::RequireId(group, L"id");
            auto name = group.GetNamedString(L"name"), text = group.GetNamedString(L"label");
            group.GetNamedBoolean(L"collapsed");
            if (!ids.insert(id).second || name.empty() || name.size() > NativeJson::kMaxShortLabel || text.empty() ||
                text.size() > NativeJson::kMaxDetailText)
                throw hresult_invalid_argument();
        }
        groupsButton.Apply(label, groups);
        root.FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
        presentation.swap(next);
        // A gesture formed against an older presentation is not revoked here:
        // it is already queued, carrying the revision it was formed against,
        // and Firefox refuses it explicitly on that revision.
        presentationRevision = revision;
        presentationFingerprint = fingerprint;
    }

    // Incremental entry points for the v11 delta path: update exactly one row,
    // or move the active-tab highlight, without visiting the other rows. A tab
    // with no realized row is simply absent from `rows`; the next snapshot
    // carries its title when one is realized.
    void UpdateTitle(uint32_t id, hstring const& title) {
        auto found = rows.find(id);
        if (found == rows.end()) return;
        UpdateHeader(found->second, id, title);
        Automation::AutomationProperties::SetName(found->second, title);
    }
    // The secondary (multiselection) emphasis, applied from the set Firefox
    // just published. Only rows whose membership changed are touched, so a
    // selection change costs work proportional to the selection, not to the
    // number of tabs.
    void ApplySelectedSet(std::set<uint32_t> const& previous, std::set<uint32_t> const& current) {
        if (dragging) return;
        auto emphasize = [&](uint32_t id) {
            auto row = rows.find(id);
            if (row == rows.end()) return;
            bool selected = current.count(id) != 0;
            row->second.FontWeight(Windows::UI::Text::FontWeight{uint16_t(selected ? 600 : 400)});
            Automation::AutomationProperties::SetItemStatus(
                row->second, id == activeTabId ? L"Active tab" : selected ? L"Selected tab" : L"");
        };
        for (auto id : previous)
            if (!current.count(id)) emphasize(id);
        for (auto id : current)
            if (!previous.count(id)) emphasize(id);
    }
    void ApplySelection(uint32_t previous, uint32_t active) {
        activeTabId = active;
        // A live gesture owns the visual order, and Sync()'s queued-highlight
        // path already handles the mid-drag case; do not bypass it here.
        if (dragging) return;
        auto found = rows.find(active);
        syncing = true;
        view.SelectedItem(found == rows.end() ? nullptr : found->second);
        syncing = false;
        // Only the two rows whose active-tab status can have changed. TabView
        // realizes a row per tab, so touching all of them here would put back
        // exactly the per-tab work this path exists to remove.
        for (auto id : {previous, active}) {
            auto row = rows.find(id);
            if (row == rows.end()) continue;
            bool selected = menu.selectedTabs.count(id) != 0;
            Automation::AutomationProperties::SetItemStatus(row->second, id == active ? L"Active tab"
                                                                         : selected   ? L"Selected tab"
                                                                                      : L"");
        }
    }

    void UpdateHeader(Controls::TabViewItem const& row, uint32_t id, hstring const& title) {
        auto header = row.Header().as<Controls::Grid>();
        // The loading spinner shares the favicon's slot -- Firefox's own tabs
        // work the same way -- rather than reserving its own column: while
        // busy, the spinner replaces the favicon in place; once loading ends,
        // the favicon (if any) reclaims that spot.
        auto favicon = header.Children().GetAt(0).as<Controls::Grid>();
        auto faviconImage = favicon.Children().GetAt(0).as<Controls::Image>();
        auto busy = favicon.Children().GetAt(1).as<Controls::ProgressRing>();
        // Pin/audio are SymbolIcon glyphs at a fixed intrinsic size that
        // Width/Height alone would only clip, not scale; a Viewbox scales
        // the glyph down to fit the 16x16 box instead.
        auto pinBox = header.Children().GetAt(1).as<Controls::Viewbox>();
        auto pin = pinBox.Child().as<Controls::SymbolIcon>();
        auto text = header.Children().GetAt(2).as<Controls::StackPanel>();
        auto name = text.Children().GetAt(0).as<Controls::TextBlock>();
        auto detail = text.Children().GetAt(1).as<Controls::TextBlock>();
        auto audioBox = header.Children().GetAt(3).as<Controls::Viewbox>();
        auto audio = audioBox.Child().as<Controls::SymbolIcon>();
        auto meta = presentation.count(id) ? presentation.at(id) : Presentation{};
        // Compare against the value last applied to this Image, not against
        // any prior Presentation -- the map is replaced wholesale on every
        // Publish(), so it cannot itself remember what is currently shown.
        // No favicon (not yet resolved, or the page genuinely has none) shows
        // no icon at all -- there is no placeholder glyph to fall back to.
        const bool hasImage = !meta.imagePixels.empty();
        const bool isBusy = (meta.flags & 2) != 0;
        auto shown = winrt::unbox_value_or<hstring>(faviconImage.Tag(), L"");
        if (hasImage && shown != meta.imagePixels) {
            faviconImage.Source(NativeJson::DecodeImage(int(meta.imageWidth), int(meta.imageHeight), meta.imagePixels));
            faviconImage.Tag(box_value(meta.imagePixels));
        } else if (!hasImage && !shown.empty()) {
            faviconImage.Source(nullptr);
            faviconImage.Tag(box_value(hstring{}));
        }
        faviconImage.Visibility(!isBusy && hasImage ? Visibility::Visible : Visibility::Collapsed);
        busy.Visibility(isBusy ? Visibility::Visible : Visibility::Collapsed);
        busy.IsActive(isBusy);
        favicon.Visibility(isBusy || hasImage ? Visibility::Visible : Visibility::Collapsed);
        if (name.Text() != title) name.Text(title);
        if (detail.Text() != meta.detail) detail.Text(meta.detail);
        detail.Visibility(meta.detail.empty() ? Visibility::Collapsed : Visibility::Visible);
        pinBox.Visibility(meta.flags & 1 ? Visibility::Visible : Visibility::Collapsed);
        audio.Symbol(meta.flags & 4 ? Controls::Symbol::Mute : Controls::Symbol::Volume);
        audioBox.Visibility(meta.flags & 12 ? Visibility::Visible : Visibility::Collapsed);
        Controls::ToolTipService::SetToolTip(row, box_value(meta.detail.empty() ? title : title + L"\n" + meta.detail));
        Automation::AutomationProperties::SetHelpText(
            row, meta.detail + (meta.detail.empty() || meta.status.empty() ? L"" : L" · ") + meta.status);
    }

    void AttachMenu(Controls::TabViewItem const& row, uint32_t id) {
        Controls::MenuFlyout flyout;
        flyout.Items().Append(NativeTabMenu::Placeholder());
        auto alive = std::weak_ptr<int>(lifetime);
        flyout.Opening([this, alive, id, weak = make_weak(flyout)](auto const&, auto const&) {
            if (!alive.expired() && rows.count(id)) menu.Open(weak.get(), id);
        });
        flyout.Opened([this, alive, weak = make_weak(flyout)](auto const&, auto const&) {
            if (!alive.expired() && weak.get() == menu.active) menu.FocusFirst();
        });
        flyout.Closed([this, alive, id, weak = make_weak(flyout), target = make_weak(row)](auto const&, auto const&) {
            if (alive.expired()) return;
            if (weak.get() == menu.active) menu.Dismiss();
            if (auto row = target.get(); row && rows.count(id) && row.ContextFlyout() == weak.get())
                AttachMenu(row, id);
        });
        row.ContextFlyout(flyout);
    }

    void Sync(const WinUITab* tabs, uint32_t count, uint32_t active) {
        activeTabId = active;
        // A drag in progress only ever moves rows visually; skip touching
        // TabItems()/content entirely rather than risk restyling a row out
        // from under the gesture's own bookkeeping (e.g. tabWidth captured
        // at drag-start). The next Sync() once the gesture ends catches up
        // in full and clears the drag's leftover visual offsets below.
        //
        // The active-tab highlight is the one exception: mousedown already
        // asked Firefox to switch tabs before the drag armed, and that
        // confirmation can land mid-gesture on a fast drag. Applying it here
        // doesn't touch TabItems() order or row content, so it's safe to let
        // through -- without it, the highlight would silently stay on the
        // pre-drag tab until some unrelated Sync() happened to run once the
        // gesture ends, which a same-spot drop (no real reorder) may never
        // trigger.
        //
        // Applying it inline, though, means TabView's own selection-changed
        // visual-state/focus work (real layout, not just a transform) runs
        // synchronously on this same call stack -- which on a fast drag can
        // land in the middle of PointerMoved's own high-frequency
        // RenderTransform updates and stutter that frame. Queue it on the
        // dispatcher instead, so it lands on a later tick once the current
        // input processing is done rather than competing with it.
        if (dragging) {
            if (rows.count(active)) {
                queuedActive = active;
                if (!highlightQueued) {
                    highlightQueued = true;
                    auto alive = std::weak_ptr<int>(lifetime);
                    Dispatching::DispatcherQueue::GetForCurrentThread().TryEnqueue([this, alive] {
                        if (alive.expired()) return;
                        highlightQueued = false;
                        if (rows.count(queuedActive)) {
                            syncing = true;
                            view.SelectedItem(rows.at(queuedActive));
                            syncing = false;
                        }
                    });
                }
            }
            return;
        }
        if (foreignDrag.active) {
            bool changed = count != foreignDrag.rows.size();
            for (uint32_t i = 0; !changed && i < count; ++i)
                changed = tabs[i].id != foreignDrag.rows[i].id;
            // Clear transforms while the old rows still belong to the strip.
            // The next hover (or release) captures the new order and geometry.
            if (changed) EndForeignDragOver();
        }
        syncing = true;
        try {
            if (!drag.affectedRowIds.empty()) {
                // Only clear once the incoming authoritative order actually
                // places the dragged tab where this gesture requested --
                // resetting on whatever Sync() happens to run first (which
                // may still carry the pre-drop order, e.g. an unrelated
                // favicon update racing the reorder's own IPC round trip)
                // would snap the row back to its old spot for a frame
                // before the real confirmation lands right after.
                bool landed = true;
                if (drag.awaitingConfirm) {
                    landed = true;  // tab no longer present: nothing left to confirm
                    for (uint32_t i = 0; i < count; ++i) {
                        if (tabs[i].id != drag.confirmTabId) continue;
                        landed = drag.confirmBeforeId == 0 ? i + 1 == count
                                                            : i + 1 < count && tabs[i + 1].id == drag.confirmBeforeId;
                        break;
                    }
                }
                if (landed) {
                    for (auto id : drag.affectedRowIds) {
                        if (!rows.count(id)) continue;
                        auto row = rows.at(id);
                        // The dropped tab's own offset is wherever the
                        // cursor happened to be, not necessarily flush with
                        // the target slot's exact grid position (unlike a
                        // shifted sibling, whose offset is always exactly
                        // one tab width and so already matches the reorder
                        // below exactly). TabItems() is about to jump
                        // straight to that grid position a few lines down,
                        // so ease from the equivalent residual offset
                        // (current position minus that upcoming jump)
                        // rather than snapping to it.
                        if (id == drag.confirmTabId && !drag.originalLeftEdge.empty() &&
                            drag.originalIndex < drag.originalLeftEdge.size()) {
                            // projectedIndex is the index of the row this tab
                            // was inserted BEFORE in the pre-reorder
                            // TabItems() (ProjectDropIndex/FinishDrag's own
                            // convention) -- not this tab's own post-reorder
                            // index. Removing this tab from its original slot
                            // shifts everything after it back by one, so a
                            // rightward move (projectedIndex > originalIndex)
                            // actually lands one slot earlier than
                            // projectedIndex; a leftward move lands exactly
                            // at projectedIndex. Either way the result is
                            // always a valid originalLeftEdge index, so
                            // "move to end" needs no separate out-of-range
                            // case.
                            uint32_t landingIndex = drag.projectedIndex > drag.originalIndex
                                                         ? drag.projectedIndex - 1
                                                         : drag.projectedIndex;
                            double targetLeftEdge = drag.originalLeftEdge[landingIndex];
                            double gridDelta = targetLeftEdge - drag.originalLeftEdge[drag.originalIndex];
                            RowTransform(row).X(RowTransform(row).X() - gridDelta);
                            AnimateRowOffset(row, id, 0);
                        } else {
                            ResetRowOffset(row, id);
                        }
                    }
                    drag.affectedRowIds.clear();
                    drag.awaitingConfirm = false;
                }
            }
            std::set<uint32_t> current;
            for (uint32_t i = 0; i < count; ++i) {
                const auto id = tabs[i].id;
                current.insert(id);
                if (!rows.count(id)) {
                    Controls::TabViewItem row;
                    row.Tag(box_value(id));
                    row.IsClosable(true);
                    row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
                    row.RenderTransform(Media::TranslateTransform{});
                    row.Header(Markup::XamlReader::Load(
                        LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" ColumnSpacing="6"><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition Width="Auto"/><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions><Grid Width="16" Height="16"><Image Stretch="Uniform" AutomationProperties.AccessibilityView="Raw"/><ProgressRing Width="16" Height="16" IsActive="False" AutomationProperties.AccessibilityView="Raw"/></Grid><Viewbox Grid.Column="1" Width="16" Height="16"><SymbolIcon Symbol="Pin"/></Viewbox><StackPanel Grid.Column="2" MaxWidth="220" VerticalAlignment="Center"><TextBlock TextTrimming="CharacterEllipsis"/><TextBlock FontSize="10" Foreground="{ThemeResource TextFillColorSecondaryBrush}" TextTrimming="CharacterEllipsis"/></StackPanel><Viewbox Grid.Column="3" Width="16" Height="16"><SymbolIcon Symbol="Volume"/></Viewbox></Grid>)"));
                    Automation::AutomationProperties::SetAutomationId(row, L"FirefoxWinUITab-" + to_hstring(id));
                    auto alive = std::weak_ptr<int>(lifetime);
                    row.AddHandler(
                        UIElement::PointerPressedEvent(),
                        box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                            [this, alive, id](auto const&,
                                              Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                                if (alive.expired() || syncing || !rows.count(id) ||
                                    !args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed())
                                    return;
                                if (!(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000)) return;
                                if (OriginatesOnButton(args.OriginalSource(), rows.at(id))) return;
                                select(id);
                            })),
                        true);
                    // Arms a hand-rolled drag on a plain left press. Uses
                    // AddHandler(handledEventsToo=true) like the multiselect
                    // handler above: TabViewItem's stock ListView-derived
                    // selection handling marks PointerPressed handled for
                    // its own purposes, and a plainly-registered handler
                    // never sees an already-handled event. Never itself
                    // marks the event handled, so stock click-to-select via
                    // SelectionChanged still fires normally; PointerMoved
                    // promotes this to an actual drag only once the pointer
                    // clears a small threshold, matching a plain click's
                    // tolerance for incidental movement.
                    row.AddHandler(
                        UIElement::PointerPressedEvent(),
                        box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                            [this, alive, id](auto const&,
                                              Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                                if (alive.expired() || syncing || !rows.count(id)) return;
                                auto point = args.GetCurrentPoint(view);
                                if (!point.Properties().IsLeftButtonPressed()) return;
                                if (GetKeyState(VK_CONTROL) & 0x8000 || GetKeyState(VK_SHIFT) & 0x8000) return;
                                auto& row = rows.at(id);
                                if (OriginatesOnButton(args.OriginalSource(), row)) return;
                                // Browsers activate a tab the instant the
                                // mouse goes down on it, even if a drag
                                // follows -- not on release, which is where
                                // TabView's own stock SelectionChanged (this
                                // constructor, above) would otherwise leave
                                // it. SelectionChanged still fires normally
                                // once the stock control catches up on
                                // release; re-selecting the same id there is
                                // a no-op.
                                select(id);
                                // Capture immediately, before knowing this is
                                // a real drag: without capture, PointerMoved
                                // only fires while the cursor is physically
                                // over this row, and once it leaves those
                                // bounds (or the OS coalesces several moves
                                // into one that jumps straight past them)
                                // this row would simply stop seeing the
                                // gesture, and the threshold below could
                                // never be observed crossing. Raw
                                // ::SetCapture on ownerHwnd, alongside this,
                                // is the actual ground truth this drag's
                                // correctness depends on from here on:
                                // row.CapturePointer only ever feeds the
                                // low-latency accelerant path.
                                if (!row.CapturePointer(args.Pointer())) return;
                                ::SetCapture(ownerHwnd);
                                drag.phase = DragPhase::Armed;
                                drag.tabId = id;
                                drag.startPoint = point.Position();
                                if (armDragSession) armDragSession(id);
                            })),
                        true);
                    row.AddHandler(
                        UIElement::PointerMovedEvent(),
                        box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                            [this, alive, id](auto const&,
                                              Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                                // Armed: try the same threshold-crossing
                                // promotion Pane.cpp's TickDragSession polls
                                // for independently -- whichever notices
                                // first wins (TryPromote is idempotent).
                                // Dragging: this is purely an accelerant,
                                // feeding the same tick Pane.cpp's own
                                // Translate() hook already drives, just
                                // sooner when WinUI's capture on this row
                                // happens to still be alive. Neither path is
                                // required for correctness; losing this
                                // event entirely (WinUI capture stolen out
                                // from under it) costs latency, not the
                                // gesture itself.
                                if (alive.expired() || syncing || drag.tabId != id || drag.phase == DragPhase::None)
                                    return;
                                if (drag.phase == DragPhase::Armed) {
                                    TryPromote(id, args.GetCurrentPoint(view).Position());
                                    return;
                                }
                                if (tickDragSession) tickDragSession();
                            })),
                        true);
                    row.AddHandler(
                        UIElement::PointerReleasedEvent(),
                        box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                            [this, alive, id](auto const&,
                                              Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                                // Only the still-Armed case is handled here.
                                // Once Dragging, Pane.cpp's TickDragSession
                                // (raw GetAsyncKeyState polling, not this
                                // event) is the sole authority on when the
                                // gesture ends -- see FinishDrag's own
                                // comment. In practice this guard is
                                // usually moot by the time this handler
                                // runs at all: TickDragSession sees the same
                                // WM_LBUTTONUP first, via Pane.cpp's
                                // Translate() pretranslate hook, and will
                                // already have reset drag.phase to None.
                                if (alive.expired() || drag.tabId != id || drag.phase != DragPhase::Armed) return;
                                if (rows.count(id)) rows.at(id).ReleasePointerCapture(args.Pointer());
                                FinishDrag(id);
                            })),
                        true);
                    row.AddHandler(
                        UIElement::PointerCaptureLostEvent(),
                        box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                            [this, alive, id](auto const&, auto const&) {
                                // Only meaningful while still Armed -- once
                                // Dragging, nothing treats this event as
                                // authoritative anymore (raw ::SetCapture,
                                // not this row's own WinUI-level capture, is
                                // what the gesture's correctness depends on
                                // from that point on).
                                if (!alive.expired() && drag.tabId == id && drag.phase == DragPhase::Armed)
                                    FinishDrag(id);
                            })),
                        true);
                    row.PreviewKeyDown(
                        [this, alive, id](auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                            if (alive.expired() || syncing || !rows.count(id)) return;
                            if (args.Key() == Windows::System::VirtualKey::Escape && drag.tabId == id &&
                                drag.phase != DragPhase::None) {
                                CancelDrag();
                                args.Handled(true);
                                return;
                            }
                            if (args.Key() == Windows::System::VirtualKey::Space ||
                                args.Key() == Windows::System::VirtualKey::Enter) {
                                if (args.OriginalSource() != rows.at(id)) return;
                                select(id);
                                args.Handled(true);
                            }
                        });
                    rows.emplace(id, row);
                    AttachMenu(row, id);
                }
                auto row = rows.at(id);
                UpdateHeader(row, id, tabs[i].title);
                Automation::AutomationProperties::SetName(row, tabs[i].title);
                // Secondary selection is explicit and theme-aware; SelectedItem
                // remains solely the Firefox active tab.
                bool selected = menu.selectedTabs.count(id) != 0;
                row.FontWeight(Windows::UI::Text::FontWeight{uint16_t(selected ? 600 : 400)});
                Automation::AutomationProperties::SetItemStatus(row, id == active ? L"Active tab"
                                                                     : selected   ? L"Selected tab"
                                                                                  : L"");
                uint32_t old;
                if (!view.TabItems().IndexOf(row, old) || old != i) {
                    if (view.TabItems().IndexOf(row, old)) view.TabItems().RemoveAt(old);
                    view.TabItems().InsertAt(i, row);
                }
            }
            while (view.TabItems().Size() > count) view.TabItems().RemoveAtEnd();
            for (auto it = rows.begin(); it != rows.end();) {
                if (!current.count(it->first))
                    it = rows.erase(it);
                else
                    ++it;
            }
            view.SelectedItem(rows.count(active) ? rows.at(active) : nullptr);
        } catch (...) {
            syncing = false;
            throw;
        }
        syncing = false;
    }
};
