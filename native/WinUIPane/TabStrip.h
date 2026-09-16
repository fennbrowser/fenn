#pragma once
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <winrt/Microsoft.UI.Input.h>
#include "JsonValidation.h"
#include "TabGroupsButton.h"

// Strip-only experiment: TabView owns presentation, Firefox owns all mutations.
// No content pages or template internals are used. CanDragTabs/CanReorderTabs
// are OFF permanently: enabling them reliably fail-fasts inside
// Microsoft.UI.Xaml.dll's own native drag-drop implementation the moment a
// drag starts in the full embedded browser, though not in a minimal
// island-only test host -- see the tab strip layout notes'
// drag-to-reorder section for the crash-dump analysis and upstream reports
// (microsoft/microsoft-ui-xaml#7007, #8450) showing this is a long-standing
// WinUI bug, not something fixable on our side by configuration. Reordering
// is instead hand-rolled below (DragState/PointerPressed and friends): plain
// pointer capture and a manual RenderTransform never touch WinUI's native
// OLE drag-drop path, so the crash can never trigger.
struct NativeTabStrip {
    Controls::Grid root{nullptr};
    Controls::TabView view{nullptr};
    NativeTabMenu& menu;
    std::function<void(uint32_t)> select, close;
    std::function<void()> add;
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
    // The completed drag becomes one validated request (pendingReorder,
    // drained like groupsButton.pending); Firefox's own moveTabTo/
    // moveTabBefore is the sole authority on the result, and the next
    // Sync() that actually reorders TabItems() clears the drag's leftover
    // visual offsets -- correcting anything the gesture visually allowed
    // that Firefox didn't apply (crossing the pinned boundary, leaving a
    // group/split), same as the rest of this strip's optimistic actions.
    bool dragging{};
    hstring pendingReorder;
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
    };
    DragState drag;
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
        pendingReorder = action.Stringify();
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

    // Moves the dragged row to follow the cursor (clamped to the strip's own
    // width) and re-projects the drop position against the stable snapshot
    // taken when the drag began -- never against live (already-shifted)
    // sibling positions, which would feed back into the hit-test.
    void UpdateDragVisual(double rawDeltaX) {
        auto row = rows.at(drag.tabId);
        double origin = drag.originalLeftEdge[drag.originalIndex];
        double minX = -origin, maxX = view.ActualWidth() - drag.tabWidth - origin;
        double clampedX = std::clamp(rawDeltaX, std::min(minX, maxX), std::max(minX, maxX));
        RowTransform(row).X(clampedX);
        auto projected = ProjectDropIndex(origin + drag.tabWidth / 2 + clampedX);
        if (projected == drag.projectedIndex) return;
        drag.projectedIndex = projected;
        for (uint32_t i = 0; i < drag.originalLeftEdge.size(); ++i) {
            if (i != drag.originalIndex) ShiftSibling(i);
        }
    }

    // Ends whatever gesture is in progress for `id`: a real drop (commits via
    // CompleteReorder at the last projected index), a plain click that never
    // crossed the drag threshold (nothing to commit -- selection already
    // happened normally), or capture lost mid-drag (treated as a drop at the
    // last known position, not a cancel: losing capture is not itself a
    // request to abandon the gesture).
    void FinishDrag(uint32_t id) {
        if (drag.tabId != id || drag.phase == DragPhase::None) return;
        bool wasDragging = drag.phase == DragPhase::Dragging;
        auto targetIndex = drag.projectedIndex;
        drag.phase = DragPhase::None;
        dragging = false;
        if (wasDragging && rows.count(id)) {
            auto row = rows.at(id);
            auto beforeRow = targetIndex < view.TabItems().Size()
                                  ? view.TabItems().GetAt(targetIndex).as<Controls::TabViewItem>()
                                  : nullptr;
            // ShiftSibling only ever tracks the OTHER rows nudged to open a
            // gap; the dragged row's own offset needs the same eventual
            // reset once Sync() sees the real reorder land, or it would sit
            // at its dropped position forever.
            drag.affectedRowIds.push_back(id);
            drag.awaitingConfirm = true;
            drag.confirmTabId = id;
            drag.confirmBeforeId = beforeRow && beforeRow != row ? unbox_value<uint32_t>(beforeRow.Tag()) : 0;
            CompleteReorderAt(row, beforeRow);
        }
    }

    // Escape: abandon the gesture with no request sent, snapping every
    // touched row back immediately -- nothing has been committed yet, so
    // there is nothing to await from Firefox before resetting.
    void CancelDrag() {
        if (drag.phase == DragPhase::Dragging && rows.count(drag.tabId)) {
            auto row = rows.at(drag.tabId);
            for (auto pointer : row.PointerCaptures()) row.ReleasePointerCapture(pointer);
            for (uint32_t i = 0; i < drag.originalLeftEdge.size(); ++i) {
                auto sibling = view.TabItems().GetAt(i).as<Controls::TabViewItem>();
                ResetRowOffset(sibling, unbox_value<uint32_t>(sibling.Tag()));
            }
        }
        drag.phase = DragPhase::None;
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
        // Any successfully applied presentation revokes a queued reorder from
        // before it, same as groupsButton.Apply does for a queued toggle.
        pendingReorder = L"";
        presentationRevision = revision;
        presentationFingerprint = fingerprint;
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
        if (dragging) {
            if (rows.count(active)) {
                syncing = true;
                view.SelectedItem(rows.at(active));
                syncing = false;
            }
            return;
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
                                // never be observed crossing.
                                if (!row.CapturePointer(args.Pointer())) return;
                                drag.phase = DragPhase::Armed;
                                drag.tabId = id;
                                drag.startPoint = point.Position();
                            })),
                        true);
                    row.AddHandler(
                        UIElement::PointerMovedEvent(),
                        box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                            [this, alive, id](auto const&,
                                              Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                                if (alive.expired() || syncing || drag.tabId != id || drag.phase == DragPhase::None)
                                    return;
                                auto point = args.GetCurrentPoint(view).Position();
                                double deltaX = point.X - drag.startPoint.X;
                                if (drag.phase == DragPhase::Armed) {
                                    constexpr double kDragThreshold = 4.0;
                                    if (std::abs(deltaX) < kDragThreshold || !rows.count(id)) return;
                                    auto& row = rows.at(id);
                                    drag.phase = DragPhase::Dragging;
                                    // Any row still easing from a prior
                                    // settle or sibling-shift (this row's
                                    // own drop settle if it's grabbed again
                                    // quickly, or a sibling shift that
                                    // hadn't finished) must be stopped
                                    // before the snapshot below, or this
                                    // drag's grid math would be built on
                                    // stale, mid-animation coordinates --
                                    // and for this row specifically, leaving
                                    // its own leftover settle running would
                                    // let it later force the transform back
                                    // to 0 once it completes, fighting this
                                    // drag's own direct manipulation.
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
                                        drag.originalLeftEdge.push_back(
                                            item.TransformToVisual(view).TransformPoint({0, 0}).X);
                                    }
                                    dragging = true;
                                }
                                UpdateDragVisual(deltaX);
                            })),
                        true);
                    row.AddHandler(
                        UIElement::PointerReleasedEvent(),
                        box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                            [this, alive, id](auto const&,
                                              Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
                                if (alive.expired() || drag.tabId != id || drag.phase == DragPhase::None) return;
                                if (rows.count(id)) rows.at(id).ReleasePointerCapture(args.Pointer());
                                FinishDrag(id);
                            })),
                        true);
                    row.AddHandler(
                        UIElement::PointerCaptureLostEvent(),
                        box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                            [this, alive, id](auto const&, auto const&) {
                                if (!alive.expired()) FinishDrag(id);
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
