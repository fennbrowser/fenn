#pragma once
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include "JsonValidation.h"
#include "TabGroupsButton.h"
#include "AllTabsButton.h"
#include "TabDragImage.h"
#include "ShellCore.h"
#include "InlineTabGroups.h"
#include "GroupSlide.h"
#include "GroupDragVisual.h"
#include "GroupDragShift.h"
#include "TabAmbient.h"
#include "TabGroupSurface.h"

// Strip-only experiment: TabView owns presentation, Firefox owns all mutations.
// Inline grouping has a localized TabListView template dependency. CanDragTabs/CanReorderTabs
// are OFF permanently: enabling them reliably fail-fasts inside
// Microsoft.UI.Xaml.dll's own native drag-drop implementation the moment a
// drag starts in the full embedded browser, though not in a minimal
// island-only test host. Crash-dump analysis and upstream reports
// (microsoft/microsoft-ui-xaml#7007, #8450) show this is a long-standing
// WinUI bug, not something fixable on our side by configuration. Reordering
// is instead hand-rolled below (DragState/PointerPressed and friends): plain
// pointer capture and manual sibling-shift animations never touch WinUI's
// native OLE drag-drop path for our own gestures. Pane.cpp separately rejects
// incoming system drags at the island's DragDropManager boundary. Real OS-level
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
    Controls::Border groupIndicator{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer groupTimer{nullptr};
    uint32_t groupingDelay{240}, groupCandidate{}, armedGroupTarget{};
    uint32_t joinGroup{}, joinTarget{};
    bool joinBefore{};

    NativeTabMenu& menu;
    ShellCore* core{};
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
        hstring containerName;
        uint32_t containerColor{};
        bool groupSource{}, groupTarget{};
    };
    std::map<uint32_t, Presentation> presentation;
    TabGroupsButton groupsButton;
    AllTabsButton allTabs;
    InlineTabGroups inlineGroups;
    GroupSlide groupSlide;
    GroupDragVisual groupDragVisual;
    GroupDragShift groupDragShift;
    GroupDragShift tabDragHeaders;
    TabAmbient ambient;
    TabGroupSurface groupSurface;
    bool groupGlow{}, containerGlow{}, containerDot{};
    // The island's system backdrop, chosen by Firefox's strip preference. The
    // pane owns the DesktopWindowXamlSource and applies a validated change.
    enum class Backdrop { Mica, MicaAlt, Acrylic };
    Backdrop backdrop{Backdrop::MicaAlt};
    std::function<void(Backdrop)> applyBackdrop;
    Controls::Border separatorTheme{nullptr};
    struct SeparatorTint {
        Controls::Border separator{nullptr};
        Media::SolidColorBrush brush{nullptr};
        uint32_t color{};
        bool tinted{}, hidden{};
    };
    std::map<uint32_t, SeparatorTint> separatorTints;
    void RestoreSeparator(SeparatorTint& tint) {
        if (!tint.tinted) return;
        // Restore a live ThemeResource through an attached proxy, rather than
        // keeping a resolved brush from a previous light/dark/contrast theme.
        Data::Binding binding;
        binding.Source(separatorTheme); binding.Path(PropertyPath(L"Background"));
        tint.separator.SetBinding(Controls::Border::BackgroundProperty(), binding);
        tint.tinted = false; tint.color = 0;
    }
    void RefreshSeparators() {
        std::set<uint32_t> live;
        const bool enabled = containerGlow && !ambient.accessibility.HighContrast();
        // The last shown member of each group ends at the group outline; its
        // trailing divider would sit directly beside that edge.
        std::set<uint32_t> groupEnds;
        if (inlineGroups.active) {
            std::map<uint32_t, uint32_t> lastMember;
            for (auto const& slot : groupLayout) {
                if (!slot.group || !slot.tab) continue;
                auto run = inlineGroups.runs.find(slot.group);
                if (run != inlineGroups.runs.end() && (!run->second.collapsed || slot.tab == activeTabId))
                    lastMember[slot.group] = slot.tab;
            }
            for (auto const& [group, tab] : lastMember) groupEnds.insert(tab);
        }
        for (auto const& [id, row] : rows) {
            if (!row.IsLoaded() || !Media::VisualTreeHelper::GetChildrenCount(row)) continue;
            auto templateRoot = Media::VisualTreeHelper::GetChild(row, 0).try_as<FrameworkElement>();
            if (!templateRoot) continue;
            auto separator = templateRoot.FindName(L"TabSeparator").try_as<Controls::Border>();
            if (!separator) continue;
            live.insert(id);
            auto& tint = separatorTints[id];
            if (tint.separator != separator) {
                RestoreSeparator(tint); tint = SeparatorTint{}; tint.separator = separator;
            }
            // TabView's visual states drive only the divider's Opacity, so its
            // Visibility is free for this presentation-only decision.
            const bool hide = groupEnds.count(id) != 0;
            if (tint.hidden != hide) {
                separator.Visibility(hide ? Visibility::Collapsed : Visibility::Visible);
                tint.hidden = hide;
            }
            auto meta = presentation.find(id);
            const uint32_t color = enabled && id != activeTabId && meta != presentation.end() &&
                !meta->second.containerName.empty() ? meta->second.containerColor : 0;
            if (!color) { RestoreSeparator(tint); continue; }
            if (tint.tinted && tint.color == color) continue;
            const Windows::UI::Color value{170, uint8_t(color >> 16), uint8_t(color >> 8), uint8_t(color)};
            if (!tint.brush) tint.brush = Media::SolidColorBrush(value);
            else tint.brush.Color(value);
            if (!tint.tinted) separator.Background(tint.brush);
            tint.tinted = true; tint.color = color;
            // TabView still owns separator opacity/visibility, including hover,
            // selected neighbors and RTL. Never create a replacement divider.
        }
        for (auto it = separatorTints.begin(); it != separatorTints.end();) {
            if (live.count(it->first)) { ++it; continue; }
            RestoreSeparator(it->second);
            if (it->second.hidden) it->second.separator.Visibility(Visibility::Visible);
            it = separatorTints.erase(it);
        }
    }
    // DIP a group surface extends beyond its outer members, from Firefox's
    // groupReach preference, and the minimum separation kept between two
    // neighboring group surfaces.
    static constexpr float GroupSeparation = 1.f;
    static constexpr float LargeGroupReach = 5.f, DefaultGroupReach = LargeGroupReach;
    float groupReach{DefaultGroupReach};
    static std::optional<float> GroupReachOf(hstring const& name) {
        if (name == L"none") return 0.f;
        if (name == L"small") return 2.f;
        if (name == L"medium") return 3.f;
        if (name == L"large") return LargeGroupReach;
        return std::nullopt;
    }
    // The stock gap before New Tab holds the smaller reaches. At the large
    // reach, move the button by that reach so the outline keeps the stock
    // tab-to-button spacing. Presentation only: the button stays in its
    // template column, which grows by the margin, and keeps its behavior.
    Controls::Button addSpacingButton{nullptr};
    Thickness addBaseMargin{};
    void ApplyAddSpacing() {
        auto add = FindAddButton(view);
        if (!add) return;
        if (add != addSpacingButton) { addSpacingButton = add; addBaseMargin = add.Margin(); }
        auto margin = addBaseMargin;
        if (groupReach >= LargeGroupReach) margin.Left += groupReach;
        if (add.Margin() != margin) add.Margin(margin);
    }
    Controls::ScrollViewer ambientScroller{nullptr};
    winrt::event_token ambientScrollToken{};
    FrameworkElement ambientContent{nullptr};
    void RefreshAmbient() {
        if (!ambient.layer) return;
        if (!root.IsLoaded() || root.Visibility() != Visibility::Visible) {
            ambient.Motion(false);
            ambient.bottom.IsVisible(false); ambient.top.IsVisible(false);
            if (ambient.rims) ambient.rims.IsVisible(false);
            if (ambient.spill) ambient.spill.IsVisible(false);
            return;
        }
        // A grouped-source update detached shown headers or rows. Keep the
        // retained light and outlines until they are realized again, rather
        // than pruning them for a frame and rebuilding them. Frames observe
        // this bounded wait; Loaded events usually end it first.
        if (inlineGroups.Realizing(ambient.frames)) {
            ambient.Motion(true);
            return;
        }
        RefreshSeparators();
        auto scroller = GroupScroller(view);
        if (scroller != ambientScroller) {
            if (ambientScroller) ambientScroller.ViewChanged(ambientScrollToken);
            ambientScroller = scroller;
            ambientContent = nullptr;
            if (scroller) ambientScrollToken = scroller.ViewChanged([this, alive = std::weak_ptr<int>(lifetime)](auto&&, auto&&) {
                if (!alive.expired()) ambient.Update();
            });
        }
        auto boundsOf = [&](FrameworkElement const& item) {
            return item.TransformToVisual(ambient.layer).TransformBounds({0, 0, float(item.ActualWidth()), float(item.ActualHeight())});
        };
        auto viewport = scroller ? boundsOf(scroller) : boundsOf(view);
        // The stock scroller template places its arrow columns beside the
        // ScrollContentPresenter that clips the tabs. While the arrows are
        // shown, keep lighting and outlines inside that presenter rather than
        // drawing across the arrows. Without arrows the bounds are unchanged.
        if (scroller && scroller.ComputedHorizontalScrollBarVisibility() == Visibility::Visible) {
            if (!ambientContent && Media::VisualTreeHelper::GetChildrenCount(scroller))
                if (auto template_ = Media::VisualTreeHelper::GetChild(scroller, 0).try_as<FrameworkElement>())
                    ambientContent = template_.FindName(L"ScrollContentPresenter").try_as<FrameworkElement>();
            if (ambientContent && ambientContent.IsLoaded() && ambientContent.ActualWidth() > 0) {
                const auto content = boundsOf(ambientContent);
                const auto left = std::max(viewport.X, content.X);
                const auto right = std::min(viewport.X + viewport.Width, content.X + content.Width);
                viewport.X = left; viewport.Width = std::max(0.f, right - left);
            }
        }
        // Follow realized XAML geometry, including finite drag/collapse motion.
        // Group identity and membership still come from the validated layout.
        std::map<uint32_t, Windows::Foundation::Rect> surfaceBounds;
        auto includeSurface = [&](uint32_t id, FrameworkElement const& element) {
            if (!element || !element.IsLoaded() || element.Visibility() != Visibility::Visible || element.Opacity() == 0) return;
            auto rect = boundsOf(element);
            if (rect.Width <= 0 || rect.Height <= 0) return;
            auto [it, inserted] = surfaceBounds.emplace(id, rect);
            if (!inserted) {
                auto& total = it->second;
                const auto right = std::max(total.X + total.Width, rect.X + rect.Width);
                const auto bottom = std::max(total.Y + total.Height, rect.Y + rect.Height);
                total.X = std::min(total.X, rect.X); total.Y = std::min(total.Y, rect.Y);
                total.Width = right - total.X; total.Height = bottom - total.Y;
            }
        };
        for (auto const& slot : groupLayout) {
            if (!slot.group) continue;
            auto run = inlineGroups.runs.find(slot.group);
            // Keep the collapsed disclosure connected to Firefox's retained
            // active member. Hidden members never enlarge the lighting.
            if (run == inlineGroups.runs.end()) continue;
            if (slot.tab) {
                if ((!run->second.collapsed || slot.tab == activeTabId) && rows.count(slot.tab))
                    includeSurface(slot.group, rows.at(slot.tab));
            } else {
                includeSurface(slot.group, run->second.button);
            }
        }
        // Extend every group surface slightly past its first and last member,
        // so the outline, wash and halo also enclose an active tab at either
        // end. Neighboring groups share the gap between them and keep a
        // visible separation; they never overlap.
        {
            std::vector<Windows::Foundation::Rect*> ordered;
            for (auto& [id, rect] : surfaceBounds) ordered.push_back(&rect);
            std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->X < b->X; });
            std::vector<float> left(ordered.size(), groupReach), right(ordered.size(), groupReach);
            for (size_t i = 1; i < ordered.size(); ++i) {
                const float gap = ordered[i]->X - (ordered[i - 1]->X + ordered[i - 1]->Width);
                const float share = std::clamp((gap - GroupSeparation) / 2, 0.f, groupReach);
                right[i - 1] = std::min(right[i - 1], share);
                left[i] = std::min(left[i], share);
            }
            for (size_t i = 0; i < ordered.size(); ++i) {
                ordered[i]->X -= left[i];
                ordered[i]->Width += left[i] + right[i];
            }
        }
        const bool followingMotion = IsDragging() || GroupDrag().phase || groupSlide.pending || groupSlide.storyboard || !rowAnimations.empty();
        auto ambientViewport = viewport;
        float fadeLeft = 0, fadeRight = 0;
        // Permit container light in the strip's top padding while retaining
        // the scroll viewport's horizontal clipping and bottom boundary.
        ambientViewport.Height += ambientViewport.Y;
        ambientViewport.Y = 0;
        // A non-overflowing TabView can size its scroller exactly to the final
        // tab. Leave room for the end's rounded light falloff rather than
        // slicing it at that tab's edge. Overflow keeps the strict viewport.
        if (scroller && scroller.ScrollableWidth() < .5) {
            float left = std::max(0.f, ambientViewport.X - 24.f);
            float right = std::min(float(root.ActualWidth()), ambientViewport.X + ambientViewport.Width + 24.f);
            if (auto add = FindAddButton(view); add && add.IsLoaded()) {
                const auto addBounds = boundsOf(add);
                // Fall to zero only in the space beyond the outermost tab or
                // header, never across it: an end tab's contour highlights
                // must match its other side. The scroller can extend past
                // that tab, so measure the realized content itself. At least
                // 1 DIP still reaches zero at the New Tab boundary.
                float contentLeft = viewport.X + viewport.Width, contentRight = viewport.X;
                auto extend = [&](FrameworkElement const& element) {
                    if (!element || !element.IsLoaded() || element.Visibility() != Visibility::Visible) return;
                    const auto bounds = boundsOf(element);
                    if (bounds.Width <= 0) return;
                    contentLeft = std::min(contentLeft, bounds.X);
                    contentRight = std::max(contentRight, bounds.X + bounds.Width);
                };
                for (auto const& [id, row] : rows) extend(row);
                for (auto const& [id, run] : inlineGroups.runs) extend(run.button);
                if (contentRight < contentLeft) { contentLeft = viewport.X; contentRight = viewport.X + viewport.Width; }
                auto fade = [](float space) { return std::clamp(space, 1.f, 16.f); };
                // A group's halo reaches past its outline by more than the gap
                // before New Tab. Let it fall off softly under the button's
                // edge rather than slicing it there.
                const float haloOverlap = groupGlow ? TabAmbient::HaloReach : 0.f;
                if (addBounds.X >= viewport.X + viewport.Width - 1) {
                    right = std::min(right, addBounds.X + std::min(haloOverlap, addBounds.Width / 2));
                    fadeRight = fade(right - contentRight);
                }
                if (addBounds.X + addBounds.Width <= viewport.X + 1) {
                    left = std::max(left, addBounds.X + addBounds.Width - std::min(haloOverlap, addBounds.Width / 2));
                    fadeLeft = fade(contentLeft - left);
                }
            }
            ambientViewport.X = left; ambientViewport.Width = std::max(0.f, right - left);
        }
        // Outlines share the light's horizontal bounds, so the extension is
        // not sliced at a scroller sized exactly to the final tab.
        groupSurface.Update(surfaceBounds, {ambientViewport.X, viewport.Y, ambientViewport.Width, viewport.Height});
        Windows::Foundation::Rect activeBounds{};
        auto activeRow = rows.find(activeTabId);
        if (activeRow != rows.end() && activeRow->second.IsLoaded() &&
            activeRow->second.Visibility() == Visibility::Visible && activeRow->second.Opacity() > 0)
            activeBounds = boundsOf(activeRow->second);
        ambient.Clip(ambientViewport, root.Visibility() == Visibility::Visible, activeBounds, fadeLeft, fadeRight);
        std::vector<TabLightContour::Face> faces;
        for (auto const& [id, row] : rows) {
            if (!row.IsLoaded() || row.Visibility() != Visibility::Visible || row.Opacity() == 0) continue;
            const auto bounds = boundsOf(row);
            if (bounds.Width <= 0 || bounds.Height <= 0 || bounds.X + bounds.Width + TabLightContour::Flare <= viewport.X ||
                bounds.X - TabLightContour::Flare >= viewport.X + viewport.Width) continue;
            const auto radius = row.CornerRadius();
            faces.push_back({id, bounds, float(radius.TopLeft), float(radius.TopRight), id == activeTabId});
        }
        // The disclosure and first inactive member have no native border at
        // their join. Give that continuous surface one attenuation profile,
        // rather than exposing full light under the button and abruptly
        // cutting it at the tab. Selected tabs retain their visible contour.
        std::set<uint32_t> joinedGroups;
        for (auto const& slot : groupLayout) {
            if (!slot.group || !slot.tab || !joinedGroups.insert(slot.group).second) continue;
            auto run = inlineGroups.runs.find(slot.group);
            if (run == inlineGroups.runs.end()) continue;
            auto button = run->second.button;
            if (!button || !button.IsLoaded() || button.Visibility() != Visibility::Visible || button.Opacity() == 0) continue;
            // Faces follow the ID-sorted row map; avoid scanning all tabs for
            // each group during layout or bounded drag updates.
            auto face = std::lower_bound(faces.begin(), faces.end(), slot.tab,
                [](auto const& value, uint32_t id) { return value.id < id; });
            if (face == faces.end() || face->id != slot.tab || face->selected) continue;
            const auto header = boundsOf(button);
            auto& bounds = face->bounds;
            const auto radius = button.CornerRadius();
            const auto margin = button.Margin();
            if (std::abs(header.X + header.Width + float(margin.Right) - bounds.X) <= 2.f) {
                bounds.Width += bounds.X - header.X; bounds.X = header.X;
                face->leftRadius = float(radius.TopLeft);
            } else if (std::abs(bounds.X + bounds.Width + float(margin.Left) - header.X) <= 2.f) {
                bounds.Width = header.X + header.Width - bounds.X;
                face->rightRadius = float(radius.TopRight);
            }
        }
        ambient.Faces(ambientViewport, faces);
        ambient.Motion(followingMotion);
        std::set<uint32_t> containers, groups;
        std::map<uint32_t, uint32_t> outlineColors;
        // Open upper-corner strokes keep container light off the central face.
        // Each tab retains its own color and the stock control's corner radii.
        for (auto const& [id, row] : rows) {
            if (!containerGlow || !row.IsLoaded() || row.Visibility() != Visibility::Visible || row.Opacity() == 0) continue;
            auto meta = presentation.find(id);
            if (meta == presentation.end() || meta->second.containerName.empty()) continue;
            auto bounds = boundsOf(row);
            if (bounds.Width <= 0 || bounds.Height <= 0 || bounds.X + bounds.Width <= viewport.X || bounds.X >= viewport.X + viewport.Width) continue;
            containers.insert(id);
            const auto radius = row.CornerRadius();
            ambient.Corners(ambient.containers[id], meta->second.containerColor, bounds, float(radius.TopLeft), float(radius.TopRight));
            ambient.containers[id].visual.Comment(L"container:" + winrt::to_hstring(id));
        }
        // The outline and light share the same full physical bounds. Scrolling
        // clips that geometry without inventing a new group edge at the viewport.
        for (auto value : currentGroups) {
            auto group = value.GetObject();
            const auto id = uint32_t(group.GetNamedNumber(L"id"));
            const auto color = uint32_t(group.GetNamedNumber(L"color", 0));
            if (!groupGlow || !color || !surfaceBounds.count(id)) continue;
            const auto bounds = surfaceBounds.at(id);
            if (bounds.X + bounds.Width <= viewport.X || bounds.X >= viewport.X + viewport.Width) continue;
            groups.insert(id);
            if (!ambient.accessibility.HighContrast()) outlineColors.emplace(id, color);
            ambient.Put(ambient.groups, id, color, bounds, false);
            auto& field = ambient.groups.at(id);
            // A soft wash belongs inside the rounded group, not under each tab
            // divider or beyond the ends of the new shared surface.
            if (field.visual.Opacity() != .65f) field.visual.Opacity(.65f);
            if (!field.roundedClip) {
                auto compositor = field.visual.Compositor();
                field.roundedClip = compositor.CreateRoundedRectangleGeometry();
                field.visual.Clip(compositor.CreateGeometricClip(field.roundedClip));
            }
            const auto radius = float(groupSurface.surfaces.at(id).CornerRadius().TopLeft);
            const Windows::Foundation::Numerics::float2 size{bounds.Width, bounds.Height}, corners{radius, radius};
            if (field.roundedClip.Size() != size) field.roundedClip.Size(size);
            if (field.roundedClip.CornerRadius() != corners) field.roundedClip.CornerRadius(corners);
            ambient.Spill(ambient.groupSpill, id, color, bounds, false);
            auto spill = ambient.groupSpill.at(id).visual;
            if (spill.Opacity() != .65f) spill.Opacity(.65f);
            ambient.Halo(id, color, bounds, radius);
        }
        groupSurface.Lighting(outlineColors);
        ambient.Prune(ambient.groups, groups, false);
        ambient.Prune(ambient.containers, containers, true);
        ambient.PruneSpill(ambient.groupSpill, groups);
        ambient.PruneHalo(groups);
        ambient.PruneSpill(ambient.containerSpill, {});
        auto container = ambient.containers.find(activeTabId);
        const bool activeVisible = activeBounds.Width > 0 && activeBounds.Height > 0;
        auto upperBounds = activeBounds;
        if (container != ambient.containers.end()) {
            upperBounds.X = container->second.bounds.X;
            upperBounds.Width = container->second.bounds.Width;
        }
        const auto radius = activeVisible ? activeRow->second.CornerRadius() : CornerRadius{};
        ambient.Corners(ambient.upperRim, activeVisible && container != ambient.containers.end() ? container->second.color : 0,
            upperBounds, float(radius.TopLeft), float(radius.TopRight), true);

    }
    // Presentation only: keep browser membership, selection and collapse state.
    // Retaining the items also avoids recycling the captured group header.
    std::map<uint32_t, Visibility> groupDragVisibility;
    void CollapseDraggingGroup(uint32_t group) {
        std::vector<ShellStripSlot> compact;
        for (auto slot : groupLayout)
            if (!slot.tab || slot.group != group) compact.push_back(slot);
        std::vector<ShellGroupMotionRow> motion(groupLayout.size());
        size_t count{};
        if (shell_core_group_motion(core,groupLayout.data(),groupLayout.size(),compact.data(),compact.size(),
                &group,1,motion.data(),motion.size(),&count)==SHELL_STATUS_OK) {
            motion.resize(count);
            try { groupSlide.Prepare(root,view,FindTabListView(view),rows,inlineGroups,FindAddButton(view),std::move(motion)); }
            catch (...) { groupSlide.Cancel(); }
        }
        for (auto slot : groupLayout) {
            if (!slot.tab || slot.group != group || !rows.count(slot.tab)) continue;
            auto row = rows.at(slot.tab);
            groupDragVisibility.emplace(slot.tab, row.Visibility());
            row.Visibility(Visibility::Collapsed);
        }
        // The browser retains these rows. Borrow their header visuals only for
        // the existing slide effect, restoring them on completion or early drop.
        auto visible=rows;
        for (auto [id,visibility] : groupDragVisibility) visible.erase(id);
        try { groupSlide.Commit(visible,inlineGroups,FindAddButton(view),activeTabId,true,group); }
        catch (...) { groupSlide.Cancel(); }
    }
    void RestoreDraggingGroup() {
        if (!groupDragVisibility.empty()) groupSlide.Cancel();
        for (auto [id, visibility] : groupDragVisibility)
            if (rows.count(id)) rows.at(id).Visibility(visibility);
        groupDragVisibility.clear();
    }
    ULONGLONG groupScrollTick{};
    // Presentation geometry captured after the temporary collapse settles.
    // Store content-space bounds so scrolling does not feed animated transforms
    // back into the whole-group insertion decision.
    std::map<uint64_t,std::pair<double,double>> groupDragGeometry;
    // Grouped-source moves can deliver deferred selection after Sync returns.
    // Keep the browser selection until acknowledged layout settles, or new input.
    bool groupSelectionGuard{}, groupSelectionObserving{};
    uint32_t groupSelectionFrames{};
    winrt::event_token groupSelectionRendering{};
    void StopGroupSelectionGuard() {
        groupSelectionGuard=false;
        if (groupSelectionObserving) {
            groupSelectionObserving=false;
            Media::CompositionTarget::Rendering(groupSelectionRendering);
        }
    }
    void BeginGroupSelectionGuard() {
        groupSelectionGuard=true; groupSelectionFrames=0;
        if (groupSelectionObserving) return;
        groupSelectionObserving=true;
        groupSelectionRendering=Media::CompositionTarget::Rendering([this,alive=std::weak_ptr<int>(lifetime)](auto&&,auto&&) {
            if (alive.expired()) return;
            if (!groupSelectionGuard) { StopGroupSelectionGuard(); return; }
            auto state=GroupDrag();
            if (state.phase || state.awaiting) { groupSelectionFrames=0; return; }
            if (rows.count(activeTabId) && view.SelectedItem()!=rows.at(activeTabId)) {
                groupSelectionFrames=0;
                view.SelectedItem(rows.at(activeTabId));
            } else if (++groupSelectionFrames>=2) StopGroupSelectionGuard();
        });
    }
    std::function<void(uint32_t)> armGroupDragSession;
    ShellGroupDrag GroupDrag() const {
        ShellGroupDrag state{};
        if (core) shell_core_group_drag_state(core,&state);
        return state;
    }
    static Controls::ScrollViewer GroupScroller(DependencyObject const& node) {
        if (auto scroll = node.try_as<Controls::ScrollViewer>(); scroll && scroll.Name() == L"ScrollViewer") return scroll;
        for (int i=0;i<Media::VisualTreeHelper::GetChildrenCount(node);++i)
            if (auto scroll=GroupScroller(Media::VisualTreeHelper::GetChild(node,i))) return scroll;
        return nullptr;
    }
    void EndGroupDrag(bool commit = false) {
        const auto state=GroupDrag();
        groupDragGeometry.clear();
        groupDragShift.Reset();
        RestoreDraggingGroup();
        if (!state.phase) { groupDragVisual.Hide(); return; }
        shell_core_group_drag_end(core);
        groupDragVisual.Hide();
        if (state.phase==2 && rows.count(activeTabId)) {
            const auto previous=syncing;
            syncing=true;
            view.SelectedItem(rows.at(activeTabId));
            syncing=previous;
        }
        if (state.phase==2 || (state.phase && !commit)) inlineGroups.suppressClick=state.group;
        if (commit && state.phase==2 && state.valid && dispatch) {
            using Value=Windows::Data::Json::JsonValue;
            Windows::Data::Json::JsonObject action;
            action.SetNamedValue(L"kind",Value::CreateStringValue(L"strip-group-reorder"));
            action.SetNamedValue(L"revision",Value::CreateNumberValue(presentationRevision));
            action.SetNamedValue(L"session",Value::CreateNumberValue(windowDragSession));
            action.SetNamedValue(L"group",Value::CreateNumberValue(state.group));
            action.SetNamedValue(L"beforeGroup",Value::CreateNumberValue(state.before_group));
            action.SetNamedValue(L"beforeTab",Value::CreateNumberValue(state.before_tab));
            const auto request=dispatch(0,action.Stringify());
            if (request) shell_core_group_drag_wait(core,request,state.group,state.before_group,state.before_tab);
        }
    }
    void TickGroupDrag(Windows::Foundation::Point point, bool down, bool cancel) {
        const auto old=GroupDrag();
        if (!old.phase) return;
        if (cancel || !root.IsLoaded() || root.Visibility()!=Visibility::Visible) { EndGroupDrag(); return; }
        const bool geometryReady=old.phase==2 && !groupSlide.pending && !groupSlide.storyboard;
        if (geometryReady && groupDragGeometry.empty()) view.UpdateLayout();
        auto scroller=GroupScroller(view);
        auto viewport = scroller ? scroller.TransformToVisual(view).TransformBounds({0,0,float(scroller.ActualWidth()),float(scroller.ActualHeight())})
                                : Windows::Foundation::Rect{0,0,float(view.ActualWidth()),float(view.ActualHeight())};
        const bool inside=point.X>=viewport.X && point.X<=view.ActualWidth() && point.Y>=0 && point.Y<=view.ActualHeight();
        if (old.phase==2 && down && inside && point.X<viewport.X+viewport.Width && scroller && scroller.ScrollableWidth()>0 && GetTickCount64()-groupScrollTick>=16) {
            groupScrollTick=GetTickCount64();
            double step=point.X<viewport.X+24 ? -12 : point.X>viewport.X+viewport.Width-24 ? 12 : 0;
            if (step) {
                // An edge scroll is explicit; stale selected-tab requests must not undo it.
                inlineGroups.NoteUserScroll();
                scroller.ChangeView(box_value(std::clamp(scroller.HorizontalOffset()+step,0.0,scroller.ScrollableWidth())).as<Windows::Foundation::IReference<double>>(),nullptr,nullptr,true);
            }
        }
        std::vector<ShellGroupDragSlot> slots;
        struct ShiftItem { uint64_t id; FrameworkElement element; size_t block; };
        std::vector<ShiftItem> shiftItems;
        auto boundsOf=[&](FrameworkElement const& item) {
            return item.TransformToVisual(view).TransformBounds({0,0,float(item.ActualWidth()),float(item.ActualHeight())});
        };
        for (auto slot:groupLayout) {
            FrameworkElement item{nullptr};
            if (!slot.tab) {
                auto found=inlineGroups.runs.find(slot.group);
                if (found==inlineGroups.runs.end()) { EndGroupDrag(); return; }
                item=slot.group==old.group && groupDragVisual.placeholder
                    ? groupDragVisual.placeholder.as<FrameworkElement>() : found->second.button.as<FrameworkElement>();
                if (!item.IsLoaded()) {
                    if (slot.group==old.group) { EndGroupDrag(); return; }
                    continue;
                }
            } else {
                if (!rows.count(slot.tab) || !rows.at(slot.tab).IsLoaded() ||
                    rows.at(slot.tab).Visibility()==Visibility::Collapsed) continue;
                if (presentation.count(slot.tab) && (presentation.at(slot.tab).flags&1)) continue;
                item=rows.at(slot.tab);
            }
            const auto key=slot.tab ? uint64_t(slot.tab) : GroupSlide::GroupKey(slot.group);
            auto bounds=boundsOf(item);
            // Targeting uses unshifted geometry so opening a gap cannot move
            // its own midpoint and reverse a stationary pointer's destination.
            bounds.X-=float(groupDragShift.Offset(key));
            double start=bounds.X;
            double end=bounds.X+bounds.Width;
            if (slot.tab && slot.group) {
                if (!slots.empty() && slots.back().group==slot.group) {
                    slots.back().start=std::min(slots.back().start,start);
                    slots.back().end=std::max(slots.back().end,end);
                } else slots.push_back({slot.group,0,start,end});
            } else slots.push_back({slot.group,slot.tab,start,end});
            shiftItems.push_back({key,item,slots.size()-1});
        }
        if (geometryReady) {
            const auto scroll=scroller ? scroller.HorizontalOffset() : 0.0;
            for (auto& slot : slots) {
                const auto key=slot.group ? GroupSlide::GroupKey(slot.group) : uint64_t(slot.tab);
                const auto [entry,inserted]=groupDragGeometry.try_emplace(key,slot.start+scroll,slot.end+scroll);
                slot.start=entry->second.first-scroll;
                slot.end=entry->second.second-scroll;
            }
        }
        if (shell_core_group_drag_update(core,point.X,point.Y,inside,viewport.X,viewport.X+viewport.Width,slots.data(),slots.size())!=SHELL_STATUS_OK) { EndGroupDrag(); return; }
        const auto state=GroupDrag();
        if (state.phase==2) {
            if (old.phase==1) {
                BeginGroupSelectionGuard();
                ::SetCapture(ownerHwnd);
                CollapseDraggingGroup(state.group);
                auto found=inlineGroups.runs.find(state.group);
                if (found!=inlineGroups.runs.end()) {
                    try { groupDragVisual.Begin(found->second.button,found->second.model); }
                    catch (...) { EndGroupDrag(); return; }
                }
            }
            inlineGroups.suppressClick=state.group;
            inlineGroups.hover.Dismiss();
            // Finish the initial collapse before taking ownership of the
            // surviving controls' transforms for the reorder preview.
            if (geometryReady && groupDragVisual.placeholder) {
                const auto width=groupDragVisual.placeholder.Width()+groupDragVisual.margin.Left+groupDragVisual.margin.Right;
                for (auto const& item : shiftItems) {
                    double offset{};
                    if (shell_core_group_drag_offset(core,item.block,width,&offset)==SHELL_STATUS_OK)
                        groupDragShift.Apply(item.id,item.element,offset,groupSlide.Enabled());
                }
            }
            auto found=inlineGroups.runs.find(state.group);
            if (found!=inlineGroups.runs.end()) {
                // WinUI already maps view coordinates into logical order in RTL.
                // The overlay is explicitly LTR, so transform into its own space.
                auto transform=view.TransformToVisual(groupDragVisual.layer);
                auto cursor=transform.TransformPoint(point);
                auto bounds=transform.TransformBounds({0,0,float(view.ActualWidth()),float(view.ActualHeight())});
                groupDragVisual.Show(cursor.X,bounds.X,bounds.Width);
            }
        }
        if (!down) EndGroupDrag(true);
    }
    std::vector<ShellStripSlot> groupLayout;
    bool hasGroupLayout{};

    bool CanApplyGroupLayout() const {
        if (!hasGroupLayout || !inlineGroups.configured || IsDragging()) return false;
        uint32_t index = 0;
        for (auto slot : groupLayout) {
            if (!slot.tab) continue;
            if (index >= view.TabItems().Size() ||
                unbox_value<uint32_t>(view.TabItems().GetAt(index++).as<Controls::TabViewItem>().Tag()) != slot.tab) return false;
        }
        return index == view.TabItems().Size();
    }
    void ApplyGroupLayout() {
        inlineGroups.Sync(view, groupLayout, rows, currentGroups);
        UpdateGroupIndicator();
        groupsButton.button.Visibility(Visibility::Collapsed);
    }
    void UpdateGroupIndicator() {
        uint32_t activeGroup = 0;
        if (activeTabId)
            for (auto slot : groupLayout) if (slot.tab == activeTabId) { activeGroup = slot.group; break; }
        inlineGroups.ActiveGroup(activeGroup, groupSlide.Enabled());
    }
    uint32_t presentationRevision{};
    hstring presentationFingerprint;
    hstring groupsLabel;
    Windows::Data::Json::JsonArray currentGroups;
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
    bool IsDragging() const {
#ifdef WINUI_PANE_TESTING
        if (testDragging) return true;
#endif
        return Drag().phase == 2;
    }
#ifdef WINUI_PANE_TESTING
    bool testDragging{};
#endif

    // Hands one completed gesture (reorder, detach, incoming adopt) to the
    // owning pane's request queue, which wakes the host. `tab` is this
    // window's own id for the tab being acted on, or 0 when the gesture names
    // no tab of this window's (an incoming adopt names the source window's).
    std::function<uint64_t(uint32_t tab, hstring const& action)> dispatch;
    // Only peer object lifetimes stay native. Rust owns tickets, request
    // outcomes and source departure tokens until their UI effects are applied.
    struct TransferPeer { NativeTabStrip* source; std::weak_ptr<int> lifetime; };
    std::map<uint64_t, TransferPeer> transferPeers;
    std::function<uint32_t()> pendingWindowDragTab;
    uint64_t ReserveTransfer(NativeTabStrip& source, uint32_t tab) {
        ShellTransfer transfer{};
        if (shell_core_transfer_reserve(core, source.core, tab, &transfer) != SHELL_STATUS_OK) return 0;
        try { transferPeers.emplace(transfer.ticket, TransferPeer{&source, source.lifetime}); }
        catch (...) {
            bool matched = false;
            shell_core_departure_match(source.core, transfer.source_endpoint, tab, transfer.source_token, true, &matched);
            shell_core_transfer_bind(core, transfer.ticket, 0, false);
            shell_core_transfer_ack(core, transfer.ticket);
            throw;
        }
        return transfer.ticket;
    }
    struct TransferAdmission {
        NativeTabStrip& destination;
        uint64_t ticket;
        bool preview, bound{};
        ~TransferAdmission() {
            if (!bound) {
                shell_core_transfer_bind(destination.core, ticket, 0, preview);
                try { destination.ReconcileTransfers(); } catch (...) {}
            }
        }
        void Bind(uint64_t request) {
            if (shell_core_transfer_bind(destination.core, ticket, request, preview) != SHELL_STATUS_OK)
                throw winrt::hresult_invalid_argument();
            bound = true;
            destination.ReconcileTransfers();
        }
    };
    void ReconcileTransfers() {
        // A closing destination visual must not prevent restoring live peers.
        try { ReconcileForeignVisuals(); } catch (...) {}
        uint64_t after = 0;
        for (;;) {
            ShellTransfer transfer{};
            if (shell_core_transfer_next(core, after, &transfer) != SHELL_STATUS_OK || !transfer.ticket) break;
            after = transfer.ticket;
            auto peer = transferPeers.find(transfer.ticket);
            try {
                if (transfer.result != 1 && peer != transferPeers.end() && !peer->second.lifetime.expired()) {
                    auto source = peer->second.source;
                    bool matched = false;
                    if (shell_core_departure_match(source->core, transfer.source_endpoint, transfer.tab,
                            transfer.source_token, false, &matched) != SHELL_STATUS_OK) continue;
                    if (matched) {
                        source->RestoreRowVisibility(transfer.tab);
                        shell_core_departure_match(source->core, transfer.source_endpoint, transfer.tab,
                            transfer.source_token, true, &matched);
                    }
                }
                if (shell_core_transfer_ack(core, transfer.ticket) == SHELL_STATUS_OK) transferPeers.erase(transfer.ticket);
            } catch (...) {
                // Keep this effect for the next drain/publication. Other peers
                // can still be reconciled if one WinUI object is unwinding.
            }
        }
    }
    void BeginDeparture(uint32_t id) {
        tabDragHeaders.Reset();
        ClearGroupHover();
        for (auto affected : drag.affectedRowIds)
            if (rows.count(affected)) ResetRowOffset(rows.at(affected), affected);
        drag.affectedRowIds.clear();
        shell_core_tab_drag_end(core, true);
        if (rows.count(id)) {
            ResetRowOffset(rows.at(id), id);
            rows.at(id).Opacity(0);
            TabDragImage::SetFloatingAppearance(rows.at(id), false);
        }
        shell_core_tab_drag_end(core, false);
    }
    // Coalesces the mid-drag active-tab highlight below: at most one apply is
    // ever queued on the dispatcher at a time, and a Sync() landing before it
    // runs just updates the target id rather than queuing a second one.
    bool highlightQueued{};
    uint32_t queuedActive{};
    // The active tab as Firefox last stated it, so an incremental apply can
    // tell "active" from "also selected" without a snapshot.
    uint32_t activeTabId{};
    struct DragPhase { static constexpr uint32_t None=0, Armed=1, Dragging=2; };
    // Presentation caches only. Rust owns gesture state and frozen geometry math.
    struct DragVisuals {
        std::vector<double> originalLeftEdge, originalWidth;
        std::vector<uint32_t> affectedRowIds;
    } drag;
    ShellTabDrag Drag() const {
        ShellTabDrag state{};
        if (core) shell_core_tab_drag_state(core, &state);
        return state;
    }
    bool ArmDrag(uint32_t tab, Windows::Foundation::Point point) {
        groupSlide.Cancel();
        return core && shell_core_tab_drag_arm(core, tab, point.X, point.Y) == SHELL_STATUS_OK;
    }
    void ReconcileDrag() {
        const auto state = Drag();
        if (state.phase == DragPhase::Dragging || state.awaiting) return;
        tabDragHeaders.Reset();
        for (auto id : drag.affectedRowIds)
            if (rows.count(id)) ResetRowOffset(rows.at(id), id);
        drag.affectedRowIds.clear();
    }
    bool captionDirty{true};
    uint32_t windowDragTabId{}, windowDragSession{};
    bool windowDragPrivate{};
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
                LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="Transparent"><TabView TabWidthMode="Equal" CanDragTabs="False" CanReorderTabs="False" AllowDrop="False" VerticalAlignment="Top" AutomationProperties.AutomationId="FirefoxWinUITabStrip" AutomationProperties.Name="Tabs in this window" /></Grid>)")
                .as<Controls::Grid>();
        view = root.Children().GetAt(0).as<Controls::TabView>();
        ambient.Create(root);
        separatorTheme = Markup::XamlReader::Load(LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource TabViewItemSeparator}" Visibility="Collapsed" IsHitTestVisible="False" AutomationProperties.AccessibilityView="Raw"/>)").as<Controls::Border>();
        ambient.layer.Children().Append(separatorTheme);
        ambient.refresh = [this] { RefreshAmbient(); };
        inlineGroups.realized = [this] { ambient.ScheduleUpdate(); };
        groupIndicator = Markup::XamlReader::Load(LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" BorderBrush="{ThemeResource AccentTextFillColorPrimaryBrush}" BorderThickness="2" CornerRadius="4" Grid.ColumnSpan="3" HorizontalAlignment="Left" VerticalAlignment="Top" IsHitTestVisible="False" Visibility="Collapsed" AutomationProperties.AccessibilityView="Raw"/>)").as<Controls::Border>();
        root.Children().Append(groupIndicator);
        groupSurface.Create(root);
        groupTimer = root.DispatcherQueue().CreateTimer();
        groupTimer.IsRepeating(false);
        auto timerLife = std::weak_ptr<int>(lifetime);
        groupTimer.Tick([this, timerLife](auto const&, auto const&) {
            if (!timerLife.expired() && IsDragging() && tickDragSession) tickDragSession();
        });
        view.TabStripHeader(groupsButton.button);
        allTabs.button.Click([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (!alive.expired()) OpenAllTabs();
        });
        groupsButton.currentRevision = [this] { return presentationRevision; };
        inlineGroups.press = [this](uint32_t id, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
            if (!core || syncing || !armGroupDragSession || (GetKeyState(VK_CONTROL)&0x8000) || (GetKeyState(VK_SHIFT)&0x8000)) return;
            armGroupDragSession(id);
            auto point=args.GetCurrentPoint(view).Position();
            if (shell_core_group_drag_arm(core,id,point.X,point.Y)!=SHELL_STATUS_OK) return;
            inlineGroups.hover.Dismiss();
            groupSlide.Cancel();
            groupDragVisual.Create(root);
            auto found=inlineGroups.runs.find(id);
            if (found!=inlineGroups.runs.end())
                groupDragVisual.Arm(found->second.button,args.GetCurrentPoint(groupDragVisual.layer).Position());
            if (!groupDragVisual.timer) {
                groupDragVisual.timer=root.DispatcherQueue().CreateTimer();
                groupDragVisual.timer.Interval(std::chrono::milliseconds(16));
                groupDragVisual.timer.Tick([this, alive=std::weak_ptr<int>(lifetime)](auto&&,auto&&) {
                    if (!alive.expired() && tickDragSession) tickDragSession();
                });
            }
            groupDragVisual.timer.Start();
            // Button retains its stock click/capture until movement promotes the drag.
        };
        inlineGroups.hover.allowed = [this] { return !IsDragging() && !GroupDrag().phase && !(GetKeyState(VK_LBUTTON) & 0x8000) && root.IsLoaded() && root.Visibility() == Visibility::Visible; };
        inlineGroups.hover.activate = [this](uint32_t group, uint32_t tab) {
            if (IsDragging() || GroupDrag().phase || !groupsButton.dispatch) return;
            using Value = Windows::Data::Json::JsonValue;
            Windows::Data::Json::JsonObject action;
            action.SetNamedValue(L"kind",Value::CreateStringValue(L"strip-group-select"));
            action.SetNamedValue(L"revision",Value::CreateNumberValue(presentationRevision));
            action.SetNamedValue(L"session",Value::CreateNumberValue(windowDragSession));
            action.SetNamedValue(L"group",Value::CreateNumberValue(group));
            action.SetNamedValue(L"id",Value::CreateNumberValue(tab));
            groupsButton.dispatch(action.Stringify());
        };
        inlineGroups.toggle = [this](uint32_t id, bool collapsed) {
            if (GroupDrag().phase==1) EndGroupDrag(true);
            if (IsDragging() || GroupDrag().phase || !groupsButton.dispatch) return;
            using JValue = Windows::Data::Json::JsonValue;
            Windows::Data::Json::JsonObject action;
            action.SetNamedValue(L"kind", JValue::CreateStringValue(L"strip-group"));
            action.SetNamedValue(L"revision", JValue::CreateNumberValue(presentationRevision));
            action.SetNamedValue(L"group", JValue::CreateNumberValue(id));
            action.SetNamedValue(L"collapsed", JValue::CreateBooleanValue(collapsed));
            groupsButton.dispatch(action.Stringify());
        };
        inlineGroups.context = [this](uint32_t id, Windows::Foundation::Rect bounds) {
            if (IsDragging() || GroupDrag().phase || !groupsButton.dispatch) return;
            // The next surface belongs to Gecko. Release island keyboard focus
            // before Firefox opens its panel and focuses the group-name field.
            if (ownerHwnd) SetFocus(ownerHwnd);
            using Value = Windows::Data::Json::JsonValue;
            Windows::Data::Json::JsonObject action;
            action.SetNamedValue(L"kind", Value::CreateStringValue(L"strip-group-context"));
            action.SetNamedValue(L"revision", Value::CreateNumberValue(presentationRevision));
            action.SetNamedValue(L"session", Value::CreateNumberValue(windowDragSession));
            action.SetNamedValue(L"group", Value::CreateNumberValue(id));
            action.SetNamedValue(L"x", Value::CreateNumberValue(bounds.X));
            action.SetNamedValue(L"y", Value::CreateNumberValue(bounds.Y));
            action.SetNamedValue(L"width", Value::CreateNumberValue(bounds.Width));
            action.SetNamedValue(L"height", Value::CreateNumberValue(bounds.Height));
            groupsButton.dispatch(action.Stringify());
        };
        auto alive = std::weak_ptr<int>(lifetime);
        root.SizeChanged([this, alive](auto&&, auto&&) { if (!alive.expired()) groupSlide.Cancel(); });
        root.Unloaded([this, alive](auto&&, auto&&) { if (!alive.expired()) { ambient.Motion(false); groupSlide.Cancel(); inlineGroups.hover.Hide(); } });
        root.PreviewKeyDown([this, alive](auto&&, auto const& args) {
            if (alive.expired() || GroupDrag().phase) return;
            const auto key=args.Key();
            using Key=Windows::System::VirtualKey;
            if (key==Key::Left || key==Key::Right || key==Key::Up || key==Key::Down ||
                key==Key::Home || key==Key::End || key==Key::Tab || key==Key::Space || key==Key::Enter)
                groupSelectionGuard=false;
        });
        view.SelectionChanged([this, alive](auto const&, auto const&) {
            if (alive.expired() || syncing || GroupDrag().phase || !view.SelectedItem()) return;
            if (groupSelectionGuard) {
                groupSelectionFrames=0;
                if (rows.count(activeTabId) && view.SelectedItem()!=rows.at(activeTabId))
                    view.SelectedItem(rows.at(activeTabId));
                return;
            }
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
            if (auto listView = FindTabListView(view)) {
                listView.ItemContainerTransitions(Media::Animation::TransitionCollection{});
                inlineGroups.Configure(listView, view);
                if (CanApplyGroupLayout()) {
                    auto selected = view.SelectedItem();
                    syncing = true;
                    ApplyGroupLayout();
                    view.SelectedItem(selected);
                    syncing = false;
                }
            }
            if (auto button = FindAddButton(view)) AttachAddMenu(button);
            ApplyAddSpacing();
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
    static Controls::Button FindAddButton(DependencyObject const& node) {
        if (auto button = node.try_as<Controls::Button>(); button && button.Name() == L"AddButton") return button;
        for (int i = 0; i < Media::VisualTreeHelper::GetChildrenCount(node); ++i)
            if (auto found = FindAddButton(Media::VisualTreeHelper::GetChild(node, i))) return found;
        return nullptr;
    }
    void AttachAddMenu(Controls::Button const& button) {
        Automation::AutomationProperties::SetAutomationId(button, L"FirefoxWinUIStripNewTab");
        Controls::MenuFlyout flyout;
        flyout.Items().Append(NativeTabMenu::Placeholder());
        auto alive = std::weak_ptr<int>(lifetime);
        flyout.Opening([this, alive, weak = make_weak(flyout)](auto const&, auto const&) {
            if (!alive.expired()) menu.Open(weak.get(), activeTabId, L"new-tab");
        });
        flyout.Opened([this, alive, weak = make_weak(flyout)](auto const&, auto const&) {
            if (!alive.expired() && weak.get() == menu.active) menu.FocusFirst();
        });
        flyout.Closed([this, alive, weak = make_weak(flyout), target = make_weak(button)](auto const&, auto const&) {
            if (alive.expired()) return;
            if (weak.get() == menu.active) menu.Dismiss();
            if (auto button = target.get(); button && button.ContextFlyout() == weak.get()) AttachAddMenu(button);
        });
        button.ContextFlyout(flyout);
    }
    ~NativeTabStrip() {
        inlineGroups.realized = nullptr;
        ambient.refresh = nullptr;
        ambient.Motion(false);
        if (ambientScroller) ambientScroller.ViewChanged(ambientScrollToken);
        groupSlide.Cancel();
        groupDragVisual.Hide();
        StopGroupSelectionGuard();
        if (groupTimer) groupTimer.Stop();
        lifetime.reset();
        if (core) shell_core_transfer_cancel(core);
        try { ReconcileTransfers(); } catch (...) { /* An island may also be closing. */ }
    }

    // Names the button's screen rectangle; Firefox opens its own all-tabs panel there.
    void OpenAllTabs() {
        if (!dispatch || IsDragging() || GroupDrag().phase || !allTabs.button.XamlRoot()) return;
        auto button = allTabs.button;
        auto local = button.TransformToVisual(nullptr).TransformBounds(
            {0, 0, float(button.ActualWidth()), float(button.ActualHeight())});
        auto screen = button.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(local);
        // The panel belongs to Gecko; release island keyboard focus first.
        if (ownerHwnd) SetFocus(ownerHwnd);
        using Value = Windows::Data::Json::JsonValue;
        Windows::Data::Json::JsonObject action;
        action.SetNamedValue(L"kind", Value::CreateStringValue(L"strip-all-tabs"));
        action.SetNamedValue(L"revision", Value::CreateNumberValue(presentationRevision));
        action.SetNamedValue(L"session", Value::CreateNumberValue(windowDragSession));
        action.SetNamedValue(L"x", Value::CreateNumberValue(screen.X));
        action.SetNamedValue(L"y", Value::CreateNumberValue(screen.Y));
        action.SetNamedValue(L"width", Value::CreateNumberValue(screen.Width));
        action.SetNamedValue(L"height", Value::CreateNumberValue(screen.Height));
        dispatch(0, action.Stringify());
    }

    Controls::MenuFlyout barMenu{nullptr};
    void ShowBarMenu(Windows::Foundation::Point position) {
        if (barMenu) barMenu.Hide();
        barMenu = Controls::MenuFlyout();
        barMenu.Items().Append(NativeTabMenu::Placeholder());
        auto alive = std::weak_ptr<int>(lifetime);
        auto weak = make_weak(barMenu);
        barMenu.Opening([this, alive, weak](auto const&, auto const&) {
            if (!alive.expired()) menu.Open(weak.get(), activeTabId, L"tab-bar");
        });
        barMenu.Opened([this, alive, weak](auto const&, auto const&) {
            if (!alive.expired() && weak.get() == menu.active) menu.FocusFirst();
        });
        barMenu.Closed([this, alive, weak](auto const&, auto const&) {
            if (!alive.expired() && weak.get() == menu.active) menu.Dismiss();
        });
        Controls::Primitives::FlyoutShowOptions options;
        options.Position(position);
        barMenu.ShowAt(root, options);
    }

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

    uint64_t CompleteReorderAt(Controls::TabViewItem const& tab, Controls::TabViewItem const& beforeRow, bool ungroup = false) {
        uint32_t beforeId = beforeRow && beforeRow != tab ? unbox_value<uint32_t>(beforeRow.Tag()) : 0;
        Windows::Data::Json::JsonObject action;
        action.SetNamedValue(L"kind", Windows::Data::Json::JsonValue::CreateStringValue(L"strip-reorder"));
        action.SetNamedValue(L"revision", Windows::Data::Json::JsonValue::CreateNumberValue(presentationRevision));
        action.SetNamedValue(L"id", Windows::Data::Json::JsonValue::CreateNumberValue(unbox_value<uint32_t>(tab.Tag())));
        action.SetNamedValue(L"beforeId", Windows::Data::Json::JsonValue::CreateNumberValue(beforeId));
        action.SetNamedValue(L"ungroup", Windows::Data::Json::JsonValue::CreateBooleanValue(ungroup));
        return dispatch ? dispatch(unbox_value<uint32_t>(tab.Tag()), action.Stringify()) : 0;
    }

    // Queues one validated detach request naming `tab` and the release
    // point in screen coordinates, on the same transport as
    // CompleteReorderAt above (just a different request `kind`) -- Firefox
    // opens the new window at that point via gBrowser.replaceTabWithWindow's
    // own left/top options, so there is nothing to compute here beyond the
    // cursor's current screen position.
    uint64_t CompleteDetach(Controls::TabViewItem const& tab) {
        POINT screenPoint{};
        GetCursorPos(&screenPoint);
        Windows::Data::Json::JsonObject action;
        action.SetNamedValue(L"kind", Windows::Data::Json::JsonValue::CreateStringValue(L"strip-detach"));
        action.SetNamedValue(L"revision", Windows::Data::Json::JsonValue::CreateNumberValue(presentationRevision));
        action.SetNamedValue(L"id", Windows::Data::Json::JsonValue::CreateNumberValue(unbox_value<uint32_t>(tab.Tag())));
        action.SetNamedValue(L"screenX", Windows::Data::Json::JsonValue::CreateNumberValue(double(screenPoint.x)));
        action.SetNamedValue(L"screenY", Windows::Data::Json::JsonValue::CreateNumberValue(double(screenPoint.y)));
        return dispatch ? dispatch(unbox_value<uint32_t>(tab.Tag()), action.Stringify()) : 0;
    }

    // For a cross-window drop landing at `localX` (already converted to
    // this strip's own local coordinate space by the caller, which alone
    // has the HWND that conversion needs). This strip's own TabItems() are
    // never touched by another window's drag, so this is a plain hit-test
    // against each row's real current position -- unlike Rust's local projection,
    // there is no dragged row of its own to skip or snapshot to hit-test
    // against instead of a live (but here, unmoving) layout.
    uint32_t ProjectExternalDropIndex(double localX) const {
        std::vector<ShellForeignRow> measured;
        for (uint32_t i = 0; i < view.TabItems().Size(); ++i) {
            auto row = view.TabItems().GetAt(i).as<Controls::TabViewItem>();
            measured.push_back({unbox_value<uint32_t>(row.Tag()),
                row.TransformToVisual(view).TransformPoint({0, 0}).X, row.ActualWidth()});
        }
        uint32_t index = 0;
        if (shell_core_foreign_external_index(core, measured.data(), measured.size(), localX, &index) != SHELL_STATUS_OK)
            throw winrt::hresult_invalid_argument();
        return index;
    }

    // Queues a validated adopt request on this strip's own transport, on
    // behalf of a tab dropped here from a different window's drag -- same
    // shape as CompleteReorderAt/CompleteDetach above, just naming the
    // source window instead of a beforeId, since the tab doesn't exist in
    // this window's own rows yet for Sync()'s usual confirm-tracking to
    // watch (Firefox adopting it in will simply appear as an ordinary new
    // row on this window's next Sync(), same as any other opened tab).
    uint64_t QueueIncomingAdopt(HWND sourceHwnd, uint32_t tabId, uint32_t index, uint32_t sourceRevision = 0, uint32_t sourceSession = 0, bool continuation = false) {
        Windows::Data::Json::JsonObject action;
        action.SetNamedValue(L"kind", Windows::Data::Json::JsonValue::CreateStringValue(L"strip-adopt"));
        if (sourceRevision) {
            action.SetNamedValue(L"continueDrag", Windows::Data::Json::JsonValue::CreateBooleanValue(continuation));
            action.SetNamedValue(L"sourceRevision", Windows::Data::Json::JsonValue::CreateNumberValue(sourceRevision));
            action.SetNamedValue(L"sourceSession", Windows::Data::Json::JsonValue::CreateNumberValue(sourceSession));
        }
        action.SetNamedValue(L"revision", Windows::Data::Json::JsonValue::CreateNumberValue(presentationRevision));
        action.SetNamedValue(L"sourceHwnd", Windows::Data::Json::JsonValue::CreateNumberValue(
                                                 double(reinterpret_cast<uintptr_t>(sourceHwnd))));
        action.SetNamedValue(L"id", Windows::Data::Json::JsonValue::CreateNumberValue(tabId));
        action.SetNamedValue(L"index", Windows::Data::Json::JsonValue::CreateNumberValue(index));
        // The adopted tab belongs to the source window, not this one, so this
        // request names no tab in this window's id space.
        return dispatch ? dispatch(0, action.Stringify()) : 0;
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

    // Storyboard::Stop() doesn't leave a property at its current mid-flight
    // value -- it reverts to whatever the value was before that Storyboard
    // began. Capture the true current value first and restore it right
    // after, or anything that stops an in-flight slide (a retarget, a new
    // drag grabbing a row whose previous gap shift is still easing in) snaps the
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
        auto transform = RowTransform(row);
        if (!row.IsLoaded()) {
            // A newly inserted/reinserted row cannot run a live storyboard yet.
            // Its first layout should already contain the insertion gap.
            transform.X(target);
            return;
        }
        const auto from = transform.X();
        if (from == target) return; // no timeline to wait for when nothing moves
        // Keep the settled value in the base property as well as the timeline.
        // Recycling/unloading a row can end the timeline without holding its
        // final value; it must not then fall back to the old offset.
        transform.X(target);
        Media::Animation::DoubleAnimation animation;
        animation.From(from);
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
        Media::Animation::Storyboard::SetTarget(animation, transform);
        Media::Animation::Storyboard::SetTargetProperty(animation, L"X");
        Media::Animation::Storyboard storyboard;
        storyboard.Children().Append(animation);
        auto alive = std::weak_ptr<int>(lifetime);
        storyboard.Completed([this, alive, id](auto const& sender, auto const&) {
            if (alive.expired()) return;
            auto current = rowAnimations.find(id);
            // A replaced timeline may still deliver its queued completion.
            if (current != rowAnimations.end() && current->second.storyboard == sender) {
                rowAnimations.erase(current);
                // Reconsider a stationary pointer only after the last slide
                // settles. A pending grouping timer never owns these offsets.
                if (rowAnimations.empty() && IsDragging() && tickDragSession) tickDragSession();
            }
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
        auto status = inlineGroups.active
            ? shell_core_tab_drag_slot_offset(core, originalIndex, false, false, false, &target)
            : shell_core_tab_drag_offset(core, originalIndex, &target);
        if (status != SHELL_STATUS_OK) return;
        auto id = unbox_value<uint32_t>(row.Tag());
        AnimateRowOffset(row, id, target);
        if (target != 0 &&
            std::find(drag.affectedRowIds.begin(), drag.affectedRowIds.end(), id) == drag.affectedRowIds.end())
            drag.affectedRowIds.push_back(id);
    }

    void ShiftTabDragHeaders() {
        if (!inlineGroups.active) return;
        const auto source = Drag().tab;
        const auto sourceSlot = std::find_if(groupLayout.begin(), groupLayout.end(),
            [source](auto slot) { return slot.tab == source; });
        const auto sourceGroup = sourceSlot == groupLayout.end() ? 0 : sourceSlot->group;
        uint32_t index = 0;
        for (auto slot : groupLayout) {
            if (slot.tab) { ++index; continue; }
            auto found = inlineGroups.runs.find(slot.group);
            if (found == inlineGroups.runs.end() || !found->second.button) continue;
            double target{};
            if (shell_core_tab_drag_slot_offset(core, index, true, slot.group == sourceGroup, slot.group == joinGroup, &target) == SHELL_STATUS_OK)
                tabDragHeaders.Apply(slot.group, found->second.button, target, groupSlide.Enabled());
        }
    }

    void ClearGroupHover() {
        if (groupTimer) groupTimer.Stop();
        if (groupIndicator) groupIndicator.Visibility(Visibility::Collapsed);
        groupCandidate = armedGroupTarget = 0;
        joinGroup = joinTarget = 0;
        bool armed{};
        if (core) shell_core_group_hover(core, 0, 0, 0, 0, &armed);
    }

    bool UpdateGroupHover(Windows::Foundation::Point const& point) {
        const auto previousJoin = joinGroup;
        joinGroup = joinTarget = 0;
        uint32_t candidate{};
        const auto source = presentation.find(Drag().tab);
        if (core && source != presentation.end() && source->second.groupSource &&
            point.X >= 0 && point.X <= view.ActualWidth() &&
            point.Y >= 0 && point.Y <= view.ActualHeight()) {
            auto join = [&](uint32_t group, uint32_t tab, bool before) {
                if (shell_core_tab_drag_join(core, group, tab, before) != SHELL_STATUS_OK) {
                    ClearGroupHover(); return false;
                }
                ClearGroupHover();
                joinGroup = group; joinTarget = tab; joinBefore = before;
                // Existing groups use the insertion gap. The outline belongs
                // only to the new-group creation dwell below.
                return true;
            };
            uint32_t hitGroup{}, hitTab{};
            bool hitBefore{};
            if (shell_core_tab_drag_join_hit(core, point.X, 0, false, &hitGroup, &hitTab, &hitBefore) == SHELL_STATUS_OK && hitGroup) {
                const auto target = presentation.find(hitTab);
                if (!hitTab || (target != presentation.end() && target->second.groupSource))
                    return join(hitGroup, hitTab, hitBefore);
            }
            // Header hits include fully collapsed groups with no visible member.
            uint32_t index{};
            const auto sourceSlot = std::find_if(groupLayout.begin(), groupLayout.end(),
                [&](auto slot) { return slot.tab == Drag().tab; });
            const auto sourceGroup = sourceSlot == groupLayout.end() ? 0 : sourceSlot->group;
            for (auto slot : groupLayout) {
                if (slot.tab) { ++index; continue; }
                const auto id = slot.group;
                const auto found = inlineGroups.runs.find(id);
                if (found == inlineGroups.runs.end()) continue;
                auto const& run = found->second;
                if (!run.button || !run.button.IsLoaded()) continue;
                double offset{};
                // Collapsed headers retain their stable acquisition region:
                // removing the source can shift the entire button by a tab width.
                if (!run.collapsed && shell_core_tab_drag_slot_offset(core,index,true,id == sourceGroup,id == previousJoin,&offset) != SHELL_STATUS_OK) continue;
                const auto left = run.button.TransformToVisual(view).TransformPoint({0,0}).X - tabDragHeaders.Offset(id) + offset;
                if (point.X < left || point.X > left + run.button.ActualWidth()) continue;
                if (shell_core_tab_drag_join_hit(core, point.X, id, run.collapsed, &hitGroup, &hitTab, &hitBefore) == SHELL_STATUS_OK && hitGroup)
                    return join(hitGroup, hitTab, hitBefore);
            }
            // New-group dwell still targets settled live rows, never a former
            // occupant of the insertion gap or a control in mid-animation.
            if (!rowAnimations.empty()) { ClearGroupHover(); return false; }
            for (uint32_t i = 0; i < drag.originalLeftEdge.size(); ++i) {
                auto row = view.TabItems().GetAt(i).as<Controls::TabViewItem>();
                const auto id = unbox_value<uint32_t>(row.Tag());
                if (id == Drag().tab) continue;
                const auto left = drag.originalLeftEdge[i] + RowTransform(row).X();
                const auto width = drag.originalWidth[i];
                if (point.X < left + width * .3 || point.X > left + width * .7) continue;
                const auto target = presentation.find(id);
                if (target != presentation.end() && target->second.groupTarget)
                    candidate = id;
                break;
            }
        }
        if (!candidate) { ClearGroupHover(); return false; }
        bool armed{};
        if (shell_core_group_hover(core, Drag().tab, candidate, GetTickCount64(), groupingDelay, &armed) != SHELL_STATUS_OK) {
            ClearGroupHover(); return false;
        }
        if (candidate != groupCandidate) {
            groupTimer.Stop();
            groupCandidate = candidate;
            groupTimer.Interval(std::chrono::milliseconds(std::max(1u, groupingDelay)));
            groupTimer.Start();
        }
        // The Win32 clock is quantized; a one-shot can arrive just before
        // the dwell deadline. Rearm only while this candidate is pending.
        if (!armed && !groupTimer.IsRunning()) {
            groupTimer.Interval(std::chrono::milliseconds(16));
            groupTimer.Start();
        }
        armedGroupTarget = armed ? candidate : 0;
        groupIndicator.Visibility(armed ? Visibility::Visible : Visibility::Collapsed);
        if (armed) {
            auto row = rows.at(candidate);
            const auto at = row.TransformToVisual(root).TransformPoint({0, 0});
            groupIndicator.Margin({at.X - 3, at.Y, 0, 0});
            groupIndicator.Width(row.ActualWidth() + 6);
            groupIndicator.Height(row.ActualHeight());
        }
        return true;
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
    // math and to notice the gesture has cleared the strip's own
    // bounds and become a tear-off.
    void UpdateDragVisual(Windows::Foundation::Point const& point, bool allowGrouping = true) {
        const auto previous = Drag();
        const auto previousJoin = joinGroup;
        if (shell_core_tab_drag_update(core, point.X, point.Y, view.ActualWidth(), view.ActualHeight(), false) != SHELL_STATUS_OK) return;
        if (Drag().detaching) {
            ClearGroupHover();
        } else {
            if (allowGrouping && UpdateGroupHover(point)) {
                if (!joinGroup) {
                    if (!previousJoin) return;
                    shell_core_tab_drag_close_gap(core);
                }
            } else if (shell_core_tab_drag_update(core, point.X, point.Y, view.ActualWidth(), view.ActualHeight(), true) != SHELL_STATUS_OK) return;
        }
        const auto current = Drag();
        if (current.projected == previous.projected && joinGroup == previousJoin) return;
        for (uint32_t i = 0; i < drag.originalLeftEdge.size(); ++i)
            if (i != current.original) ShiftSibling(i);
        ShiftTabDragHeaders();
    }

    // Undoes whatever TryPromote's render-completion callback hid (see its
    // own comment) once a gesture actually ends. A no-op if the row is
    // already gone, or was never actually hidden (a gesture that ends
    // before the popup's own snapshot finished rendering never got this
    // far in the first place).
    void RestoreRowVisibility(uint32_t id) {
        if (rows.count(id)) {
            TabDragImage::SetFloatingAppearance(rows.at(id), false);
            rows.at(id).Opacity(1);
        }
    }

    // Ends an armed-but-never-promoted gesture: a plain click (selection
    // already happened normally on press, nothing to commit) or capture
    // lost while still Armed (nothing has moved yet, so there's nothing to
    // recover). Once a gesture is promoted to Dragging, Pane.cpp's
    // TabDragSession owns resolving it end to end (CommitReorder/
    // AcceptForeignDrop/CommitDetach/CancelDrag below) -- this is also
    // used only for outcomes that have no pending browser transfer.
    void FinishDrag(uint32_t id) {
        ClearGroupHover();
        if (Drag().tab != id) return;
        RestoreRowVisibility(id);
        shell_core_tab_drag_end(core, false);
    }

    // Promotes Armed -> Dragging using `point` (view-local DIPs, same space
    // PointerMoved's own GetCurrentPoint(view) uses) as the drag's live
    // position. Routed moves and the message hook both tick the same live
    // cursor sampler in Pane.cpp. Idempotent once already Dragging.
    bool TryPromote(uint32_t id, Windows::Foundation::Point const& point, bool continuation = false) {
        inlineGroups.hover.Hide();
        if (Drag().tab != id || Drag().phase != DragPhase::Armed || !rows.count(id)) return false;
        bool ready = false;
        if (shell_core_tab_drag_ready(core, id, point.X, point.Y, continuation, &ready) != SHELL_STATUS_OK || !ready) return false;
        auto& row = rows.at(id);
        // Stop any remaining gap shifts before taking the new drag's
        // geometry snapshot, so its grid math does not use stale,
        // mid-animation coordinates or compete with a running storyboard.
        {
            std::vector<uint32_t> activeIds;
            for (auto& [animId, anim] : rowAnimations) activeIds.push_back(animId);
            for (auto animId : activeIds)
                if (rows.count(animId)) StopRowAnimation(rows.at(animId), animId);
        }
        uint32_t original = 0;
        view.TabItems().IndexOf(row, original);
        std::vector<ShellDragRow> measured;
        drag.originalLeftEdge.clear();
        drag.originalWidth.clear();
        for (uint32_t i = 0; i < view.TabItems().Size(); ++i) {
            auto item = view.TabItems().GetAt(i).as<Controls::TabViewItem>();
            drag.originalLeftEdge.push_back(item.TransformToVisual(view).TransformPoint({0, 0}).X);
            drag.originalWidth.push_back(item.ActualWidth());
            measured.push_back({drag.originalLeftEdge.back(), drag.originalWidth.back()});
        }
        if (shell_core_tab_drag_promote(core, original, measured.data(), measured.size()) != SHELL_STATUS_OK) return false;
        UpdateDragVisual(point);
        return true;
    }

    // Closes any open reorder gap -- called once the cursor leaves this
    // strip's own bounds (Pane.cpp's TickDragSession, geometrically, via
    // StripScreenBounds) without having dropped here, mirroring
    // UpdateDragVisual's own detach-branch logic: nothing should keep
    // making room for a tab that's no longer hovering.
    void CloseDragGap() {
        const bool joining = joinGroup != 0;
        ClearGroupHover();
        if (Drag().phase != DragPhase::Dragging || (!joining && Drag().projected == Drag().original)) return;
        shell_core_tab_drag_close_gap(core);
        for (uint32_t i = 0; i < drag.originalLeftEdge.size(); ++i)
            if (i != Drag().original) ShiftSibling(i);
        ShiftTabDragHeaders();
    }

    // Commits this exact tab's own in-flight gesture as a self-reorder --
    // called by Pane.cpp's TickDragSession once release resolves as still
    // hovering this same strip.
    bool CommitReorder(uint32_t tabId, Windows::Foundation::Point const& point) {
        if (Drag().tab != tabId || Drag().phase != DragPhase::Dragging || !rows.count(tabId)) return false;
        UpdateDragVisual(point);
        if (joinGroup) {
            using Value = Windows::Data::Json::JsonValue;
            Windows::Data::Json::JsonObject action;
            action.SetNamedValue(L"kind", Value::CreateStringValue(L"strip-join-group"));
            action.SetNamedValue(L"revision", Value::CreateNumberValue(presentationRevision));
            action.SetNamedValue(L"session", Value::CreateNumberValue(windowDragSession));
            action.SetNamedValue(L"id", Value::CreateNumberValue(tabId));
            action.SetNamedValue(L"group", Value::CreateNumberValue(joinGroup));
            action.SetNamedValue(L"targetId", Value::CreateNumberValue(joinTarget));
            action.SetNamedValue(L"before", Value::CreateBooleanValue(joinBefore));
            CancelDrag();
            if (dispatch) dispatch(tabId, action.Stringify());
            return true;
        }
        if (armedGroupTarget) {
            using Value = Windows::Data::Json::JsonValue;
            Windows::Data::Json::JsonObject action;
            action.SetNamedValue(L"kind", Value::CreateStringValue(L"strip-create-group"));
            action.SetNamedValue(L"revision", Value::CreateNumberValue(presentationRevision));
            action.SetNamedValue(L"session", Value::CreateNumberValue(windowDragSession));
            action.SetNamedValue(L"id", Value::CreateNumberValue(tabId));
            action.SetNamedValue(L"targetId", Value::CreateNumberValue(armedGroupTarget));
            uint32_t targetIndex{};
            view.TabItems().IndexOf(rows.at(armedGroupTarget), targetIndex);
            action.SetNamedValue(L"before", Value::CreateBooleanValue(Drag().original < targetIndex));
            CancelDrag();
            if (dispatch) dispatch(tabId, action.Stringify());
            return true;
        }
        ClearGroupHover();
        UpdateDragVisual(point, false);
        auto row = rows.at(tabId);
        auto targetIndex = Drag().projected;
        drag.affectedRowIds.push_back(tabId);
        auto beforeRow = targetIndex < view.TabItems().Size()
                              ? view.TabItems().GetAt(targetIndex).as<Controls::TabViewItem>()
                              : nullptr;
        const auto beforeId = beforeRow && beforeRow != row ? unbox_value<uint32_t>(beforeRow.Tag()) : 0;
        // A drop outside the source group's rendered members/header is an
        // explicit ungroup, even if its tab index would remain unchanged.
        bool ungroup = false;
        auto sourceSlot = std::find_if(groupLayout.begin(), groupLayout.end(),
            [tabId](auto slot) { return slot.tab == tabId; });
        if (sourceSlot != groupLayout.end() && sourceSlot->group && presentation.at(tabId).groupSource) {
            double left = view.ActualWidth(), right = 0;
            auto include = [&](FrameworkElement const& element) {
                auto at = element.TransformToVisual(view).TransformPoint({0,0});
                left = std::min(left, double(at.X));
                right = std::max(right, double(at.X) + element.ActualWidth());
            };
            for (auto slot : groupLayout) {
                if (slot.group != sourceSlot->group) continue;
                if (slot.tab && rows.count(slot.tab)) include(rows.at(slot.tab));
                if (!slot.tab) {
                    auto run = inlineGroups.runs.find(slot.group);
                    if (run != inlineGroups.runs.end() && run->second.button) include(run->second.button);
                }
            }
            // Include the reorder gap between members in the group's bounds.
            ungroup = point.X < left || point.X > right;
        }
        // Reveal the row at its destination during the browser round trip.
        ResetRowOffset(row, tabId);
        RowTransform(row).X(Drag().landing_offset);
        const auto request = CompleteReorderAt(row, beforeRow, ungroup);
        if (shell_core_tab_drag_commit(core, request, beforeId) != SHELL_STATUS_OK)
            shell_core_tab_drag_end(core, true);
        ReconcileDrag();
        RestoreRowVisibility(tabId);
        return true;
    }

    // A transfer is confirmed by source removal, not by a matching index
    // (in particular, a departing last tab must remain hidden until removal).
    void CommitDetach(uint32_t id) {
        if (!rows.count(id)) return;
        const auto ticket = ReserveTransfer(*this, id);
        if (!ticket) return;
        TransferAdmission admission{*this, ticket, false};
        BeginDeparture(id);
        admission.Bind(CompleteDetach(rows.at(id)));
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
    struct ForeignRowVisual {
        Controls::TabViewItem item{nullptr};
        uint32_t id{};
    };
    // Exact transformed objects survive removal/reordering until reset.
    std::vector<ForeignRowVisual> foreignRows;
    ShellForeignPreview ForeignPreview() const {
        ShellForeignPreview state{};
        if (core) shell_core_foreign_state(core, &state);
        return state;
    }
    void ReconcileForeignVisuals() {
        if (ForeignPreview().active) return;
        for (auto const& row : foreignRows) ResetRowOffset(row.item, row.id);
        foreignRows.clear();
    }
    void ForeignDragOver(Windows::Foundation::Point const& point) {
        if (ForeignPreview().request) EndForeignDragOver(true);
        if (!ForeignPreview().active) {
            ReconcileForeignVisuals();
            view.UpdateLayout();
            std::vector<ShellForeignRow> measured;
            std::vector<ForeignRowVisual> visuals;
            for (uint32_t i = 0; i < view.TabItems().Size(); ++i) {
                auto item = view.TabItems().GetAt(i).as<Controls::TabViewItem>();
                const auto id = unbox_value<uint32_t>(item.Tag());
                measured.push_back({id, item.TransformToVisual(view).TransformPoint({0, 0}).X, item.ActualWidth()});
                visuals.push_back({item, id});
            }
            if (shell_core_foreign_begin(core, measured.data(), measured.size()) != SHELL_STATUS_OK) return;
            foreignRows = std::move(visuals);
        }
        bool changed = false;
        if (shell_core_foreign_hover(core, point.X, &changed) != SHELL_STATUS_OK || !changed) return;
        const auto preview = ForeignPreview();
        for (uint32_t i = 0; i < foreignRows.size(); ++i)
            AnimateRowOffset(foreignRows[i].item, foreignRows[i].id, i >= preview.projected ? preview.width : 0);
    }
    void EndForeignDragOver(bool force = false) {
        if (core) shell_core_foreign_end(core, force);
        ReconcileForeignVisuals();
    }

    // Commits a tab dropped here from a DIFFERENT pane's own in-flight
    // gesture -- called by Pane.cpp's TickDragSession once release resolves
    // as hovering this strip while some other pane owns the gesture.
    // `point` is already translated to this strip's own local coordinate
    // space (Pane.cpp's StripScreenBounds owns that conversion, since it
    // alone knows the hosting HWND). Zero means queue admission failed. Firefox's
    // own adopt handler and the JS session's snapshot-based validation are
    // the real authority, same as every other queued request this strip
    // sends.
    uint64_t AcceptForeignDrop(HWND sourceHwnd, uint32_t tabId, Windows::Foundation::Point const& point) {
        // Sync invalidates a preview before a structural update. Rebuild it
        // even if release arrives before the next hover tick, so a drop never
        // commits an insertion index from the previous destination row set.
        ForeignDragOver(point);
        const auto preview = ForeignPreview();
        if (!preview.active || preview.projected == UINT32_MAX) return 0;
        return QueueIncomingAdopt(sourceHwnd, tabId, preview.projected);
    }

    // Escape: abandon the gesture with no request sent, snapping every
    // touched row back immediately -- nothing has been committed yet, so
    // there is nothing to await from Firefox before resetting.
    void CancelDrag() {
        const bool wasDragging = Drag().phase == DragPhase::Dragging;
        const auto tab = Drag().tab;
        // Revoke the local gesture before any XAML work can reenter. Reset
        // actual retained rows, not geometry indices captured by an old layout.
        shell_core_tab_drag_end(core, true);
        tabDragHeaders.Reset();
        drag.affectedRowIds.clear();
        ClearGroupHover();
        if (wasDragging) {
            RestoreRowVisibility(tab);
            for (auto const& [id, row] : rows) ResetRowOffset(row, id);
            if (rows.count(tab)) rows.at(tab).ReleasePointerCaptures();
        }
    }

    // Parse a complete bounded copy before changing XAML or accepting any IDs.
    void Publish(NativeTabMenu::Object const& data) {
        auto version = data.GetNamedNumber(L"version");
        if (version != 1 && version != 2) throw hresult_invalid_argument();
        const auto baseNumber = version == 2 ? NativeJson::RequireRevision(data, L"base") : 0;
        if (baseNumber > UINT32_MAX) throw hresult_invalid_argument();
        const auto base = uint32_t(baseNumber);
        auto revision = NativeJson::RequireId(data, L"revision");
        auto fingerprint = data.Stringify();
        if (revision < presentationRevision ||
            (revision == presentationRevision && fingerprint != presentationFingerprint))
            throw hresult_invalid_argument();
        if (revision == presentationRevision) return;
        if (base && base != presentationRevision) throw hresult_invalid_argument();
        const auto dragTab = data.HasKey(L"windowDragTabId") ? NativeJson::RequireRevision(data, L"windowDragTabId") : (base ? windowDragTabId : 0);
        if (dragTab > UINT32_MAX) throw hresult_invalid_argument();
        const auto dragSessionId = data.HasKey(L"windowDragSession") ? NativeJson::RequireId(data, L"windowDragSession") : (base ? windowDragSession : 0);
        const bool dragPrivate = data.GetNamedBoolean(L"windowDragPrivate", base ? windowDragPrivate : false);
        const auto delay = data.GetNamedNumber(L"groupingDelay", base ? groupingDelay : 240);
        const bool nextGroupGlow = data.HasKey(L"groupGlow") ? data.GetNamedBoolean(L"groupGlow") : base ? groupGlow : false;
        const bool nextContainerGlow = data.HasKey(L"containerGlow") ? data.GetNamedBoolean(L"containerGlow") : base ? containerGlow : false;
        const bool nextContainerDot = data.HasKey(L"containerDot") ? data.GetNamedBoolean(L"containerDot") : base ? containerDot : false;
        auto nextBackdrop = base ? backdrop : Backdrop::MicaAlt;
        if (data.HasKey(L"backdrop")) {
            const auto name = data.GetNamedString(L"backdrop");
            if (name == L"mica") nextBackdrop = Backdrop::Mica;
            else if (name == L"mica-alt") nextBackdrop = Backdrop::MicaAlt;
            else if (name == L"acrylic") nextBackdrop = Backdrop::Acrylic;
            else throw hresult_invalid_argument();
        }
        auto nextGroupReach = base ? groupReach : DefaultGroupReach;
        if (data.HasKey(L"groupReach")) {
            auto reach = GroupReachOf(data.GetNamedString(L"groupReach"));
            if (!reach) throw hresult_invalid_argument();
            nextGroupReach = *reach;
        }
        if (!std::isfinite(delay) || delay < 0 || delay > 60000 || std::floor(delay) != delay)
            throw hresult_invalid_argument();
        const bool updateGroups = !base || data.HasKey(L"groups") || data.HasKey(L"groupsLabel");
        auto label = base ? data.GetNamedString(L"groupsLabel", groupsLabel) : data.GetNamedString(L"groupsLabel");
        const bool rtl = base ? data.GetNamedBoolean(L"rtl", root.FlowDirection() == FlowDirection::RightToLeft)
                             : data.GetNamedBoolean(L"rtl");
        auto entries = data.GetNamedArray(L"rows");
        auto groups = base && !data.HasKey(L"groups") ? currentGroups : data.GetNamedArray(L"groups");
        if (label.empty() || label.size() > NativeJson::kMaxShortLabel || entries.Size() > NativeJson::kMaxTabs ||
            groups.Size() > NativeJson::kMaxTabs)
            throw hresult_invalid_argument();
        std::map<uint32_t, Presentation> next;
        std::vector<ShellStripChange> changes;
        std::set<uint32_t> removed;
        if (base) {
            auto removals = data.GetNamedArray(L"removed");
            if (removals.Size() > NativeJson::kMaxTabs) throw hresult_invalid_argument();
            for (auto value : removals) {
                auto number = value.GetNumber();
                if (!std::isfinite(number) || number < 1 || number > UINT32_MAX || std::floor(number) != number)
                    throw hresult_invalid_argument();
                auto id = uint32_t(number);
                if (!presentation.count(id) || !removed.insert(id).second) throw hresult_invalid_argument();
                changes.push_back({id, 0, true, false});
            }
        }
        size_t totalImagePixels = 0;
        for (auto value : entries) {
            auto entry = value.GetObject();
            auto id = NativeJson::RequireId(entry, L"id");
            if (removed.count(id)) throw hresult_invalid_argument();
            auto old = presentation.find(id);
            bool existing = base && old != presentation.end();
            auto flags = existing ? entry.GetNamedNumber(L"flags", old->second.flags) : entry.GetNamedNumber(L"flags");
            auto detail = existing ? entry.GetNamedString(L"detail", old->second.detail) : entry.GetNamedString(L"detail");
            auto status = entry.GetNamedString(L"status", existing ? old->second.status : L"");
            auto containerName = entry.GetNamedString(L"containerName", existing ? old->second.containerName : L"");
            auto containerColor = entry.GetNamedNumber(L"containerColor", existing ? old->second.containerColor : 0);
            if (containerName.size() > NativeJson::kMaxShortLabel || !std::isfinite(containerColor) ||
                containerColor < 0 || containerColor > UINT32_MAX || std::floor(containerColor) != containerColor)
                throw hresult_invalid_argument();
            if (!std::isfinite(flags) || flags < 0 || flags > 15 || std::floor(flags) != flags ||
                detail.size() > NativeJson::kMaxDetailText || status.size() > NativeJson::kMaxDetailText)
                throw hresult_invalid_argument();
            NativeJson::ValidateImage(entry, totalImagePixels, kTabImageLimits);
            hstring imagePixels;
            uint32_t imageWidth{}, imageHeight{};
            if (existing && !entry.HasKey(L"image")) {
                imagePixels = old->second.imagePixels;
                imageWidth = old->second.imageWidth;
                imageHeight = old->second.imageHeight;
            }
            if (entry.HasKey(L"image") &&
                entry.GetNamedValue(L"image").ValueType() != winrt::Windows::Data::Json::JsonValueType::Null) {
                auto image = entry.GetNamedObject(L"image");
                imagePixels = image.GetNamedString(L"pixels");
                imageWidth = uint32_t(image.GetNamedNumber(L"width"));
                imageHeight = uint32_t(image.GetNamedNumber(L"height"));
            }
            if (!next.emplace(id, Presentation{detail, uint32_t(flags), status, imagePixels, imageWidth, imageHeight,
                                               containerName, uint32_t(containerColor),
                                               entry.GetNamedBoolean(L"groupSource", existing && old->second.groupSource),
                                               entry.GetNamedBoolean(L"groupTarget", existing && old->second.groupTarget)})
                     .second)
                throw hresult_invalid_argument();
            changes.push_back({id, imageWidth * imageHeight, false,
                               entry.HasKey(L"flags") && entry.HasKey(L"detail")});
        }
        std::set<uint32_t> ids, previewIds, memberIds;
        size_t previewPixels = 0;
        for (auto value : groups) {
            auto group = value.GetObject();
            auto id = NativeJson::RequireId(group, L"id");
            auto name = group.GetNamedString(L"name"), text = group.GetNamedString(L"label");
            auto headerText = group.GetNamedString(L"headerText", L"");
            if (group.HasKey(L"members")) {
                auto members=group.GetNamedArray(L"members");
                if (members.Size()>NativeJson::kMaxTabs) throw hresult_invalid_argument();
                for (auto value:members) {
                    auto id=value.GetNumber();
                    if (!std::isfinite(id) || id<=0 || id>UINT32_MAX || std::floor(id)!=id ||
                        !memberIds.insert(uint32_t(id)).second || memberIds.size()>NativeJson::kMaxTabs) throw hresult_invalid_argument();
                }
            }
            if (group.HasKey(L"tabs")) {
                auto tabs = group.GetNamedArray(L"tabs");
                if (tabs.Size() > NativeJson::kMaxTabs) throw hresult_invalid_argument();
                for (auto value : tabs) {
                    auto tab = value.GetObject();
                    if (!previewIds.insert(NativeJson::RequireId(tab,L"id")).second || previewIds.size() > NativeJson::kMaxTabs)
                        throw hresult_invalid_argument();
                    NativeJson::RequireLength(tab.GetNamedString(L"title"), NativeJson::kMaxShortLabel);
                    NativeJson::ValidateImage(tab,previewPixels,kTabImageLimits);
                }
            }
            group.GetNamedBoolean(L"collapsed");
            const auto color = group.GetNamedNumber(L"color", 0);
            if (!std::isfinite(color) || color < 0 || color > UINT32_MAX || std::floor(color) != color)
                throw hresult_invalid_argument();
            if (!ids.insert(id).second || name.empty() || name.size() > NativeJson::kMaxShortLabel || text.empty() ||
                text.size() > NativeJson::kMaxDetailText || headerText.size() > NativeJson::kMaxShortLabel)
                throw hresult_invalid_argument();
        }
        std::vector<ShellStripSlot> layout;
        const bool layoutChanged = data.HasKey(L"layout");
        if (!base && hasGroupLayout && !layoutChanged) throw hresult_invalid_argument();
        if (layoutChanged) {
            auto slots = data.GetNamedArray(L"layout");
            if (slots.Size() > 20000) throw hresult_invalid_argument();
            for (auto value : slots) {
                auto slot = value.GetObject();
                auto group = NativeJson::RequireRevision(slot, L"group");
                auto tab = NativeJson::RequireRevision(slot, L"tab");
                if (group > UINT32_MAX || tab > UINT32_MAX || (group && !ids.count(uint32_t(group))))
                    throw hresult_invalid_argument();
                layout.push_back({uint32_t(group), uint32_t(tab)});
            }
        } else if (base && hasGroupLayout) {
            for (auto slot : groupLayout)
                if (slot.group && !ids.count(slot.group)) throw hresult_invalid_argument();
        }
        // The Rust core owns revision/identity and aggregate retained-image
        // bounds. Everything above is staged before either model or XAML changes.
        if (!core || shell_core_strip_apply_layout(core, base, revision, changes.data(), changes.size(), layout.data(), layout.size(), layoutChanged) != SHELL_STATUS_OK)
            throw hresult_invalid_argument();
        if (GroupDrag().phase && (layoutChanged || !removed.empty() || !base)) EndGroupDrag();
        if (layoutChanged) {
            std::vector<uint32_t> toggled;
            if (updateGroups && inlineGroups.active && !IsDragging()) {
                for (auto value : groups) {
                    auto group = value.GetObject();
                    const auto id = uint32_t(group.GetNamedNumber(L"id"));
                    auto old = inlineGroups.runs.find(id);
                    if (old != inlineGroups.runs.end() && old->second.collapsed != group.GetNamedBoolean(L"collapsed"))
                        toggled.push_back(id);
                }
            }
            std::vector<ShellGroupMotionRow> motion(groupLayout.size() + layout.size());
            size_t motionCount{};
            if (shell_core_group_motion(core, groupLayout.data(), groupLayout.size(), layout.data(), layout.size(),
                    toggled.data(), toggled.size(), motion.data(), motion.size(), &motionCount) == SHELL_STATUS_OK) {
                motion.resize(motionCount);
                try { groupSlide.Prepare(root, view, FindTabListView(view), rows, inlineGroups, FindAddButton(view), std::move(motion)); }
                catch (...) { groupSlide.Cancel(); }
            } else groupSlide.Cancel();
        }
        if (updateGroups && GroupDrag().phase) {
            auto id=GroupDrag().group;
            auto old=inlineGroups.runs.find(id);
            bool same=false;
            for (auto value:groups) {
                auto group=value.GetObject();
                if (uint32_t(group.GetNamedNumber(L"id"))==id && old!=inlineGroups.runs.end())
                    same=group.GetNamedArray(L"members",Windows::Data::Json::JsonArray()).Stringify()==old->second.metadata.GetNamedArray(L"members",Windows::Data::Json::JsonArray()).Stringify();
            }
            if (!same) EndGroupDrag();
        }
        if (updateGroups) {
            if (!inlineGroups.active) groupsButton.Apply(label, groups);
            else if (!IsDragging()) inlineGroups.Metadata(groups, groupSlide.Enabled());
            groupsLabel = label;
            currentGroups = groups;
        }
        if (!base || data.HasKey(L"rtl"))
            root.FlowDirection(rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
        if (!base) presentation.swap(next);
        else {
            for (auto id : removed) presentation.erase(id);
            for (auto const& [id, value] : next) presentation.insert_or_assign(id, value);
        }
        // A gesture formed against an older presentation is not revoked here:
        // it is already queued, carrying the revision it was formed against,
        // and Firefox refuses it explicitly on that revision.
        if (groupingDelay != uint32_t(delay) || layoutChanged || !removed.empty()) ClearGroupHover();
        groupingDelay = uint32_t(delay);
        if (groupCandidate && (!presentation.count(groupCandidate) || !presentation.at(groupCandidate).groupTarget ||
            !presentation.count(Drag().tab) || !presentation.at(Drag().tab).groupSource)) ClearGroupHover();
        windowDragTabId = uint32_t(dragTab);
        windowDragSession = dragSessionId;
        windowDragPrivate = dragPrivate;
        captionDirty = true;
        presentationRevision = revision;
        presentationFingerprint = fingerprint;
        if (layoutChanged) {
            size_t size = layout.size();
            if (shell_core_strip_layout(core, layout.data(), layout.size(), &size) != SHELL_STATUS_OK)
                throw hresult_invalid_argument();
            groupLayout = std::move(layout);
            hasGroupLayout = true;
            if (CanApplyGroupLayout()) {
                const bool previous = syncing;
                auto selected = view.SelectedItem();
                syncing = true;
                ApplyGroupLayout();
                view.SelectedItem(selected);
                syncing = previous;
            }
        }
        shell_core_group_drag_publish(core,groupLayout.data(),groupLayout.size());
        const bool dotChanged = containerDot != nextContainerDot;
        containerDot = nextContainerDot;
        containerGlow = nextContainerGlow;
        groupGlow = nextGroupGlow;
        if (groupReach != nextGroupReach) { groupReach = nextGroupReach; ApplyAddSpacing(); }
        if (nextBackdrop != backdrop) {
            backdrop = nextBackdrop;
            if (applyBackdrop) applyBackdrop(backdrop);
        }
        if (dotChanged) {
            for (auto const& [id, row] : rows)
                UpdateHeader(row, id, Automation::AutomationProperties::GetName(row));
        }
        // Metadata has its own publication now; do not wait for a title delta
        // or a structural Sync to make a changed icon/status visible.
        for (auto const& change : changes) {
            auto row = rows.find(change.id);
            if (change.remove || row == rows.end()) continue;
            UpdateHeader(row->second, change.id, Automation::AutomationProperties::GetName(row->second));
        }
        ambient.Update();
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
        if (IsDragging()) return;
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
        if (previous != active) groupSlide.Cancel();
        activeTabId = active;
        // A live gesture owns the visual order, and Sync()'s queued-highlight
        // path already handles the mid-drag case; do not bypass it here.
        if (IsDragging()) return;
        auto found = rows.find(active);
        syncing = true;
        view.SelectedItem(found == rows.end() ? nullptr : found->second);
        syncing = false;
        UpdateGroupIndicator();
        ambient.Update();
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
        // Incremental title/favicon updates may arrive during the short slide.
        // Return borrowed content before the normal header update accesses it.
        if (!row.Header() && !groupSlide.borrowed.empty()) groupSlide.Cancel();
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
        auto audioBox = header.Children().GetAt(3).as<Controls::Viewbox>();
        auto audio = audioBox.Child().as<Controls::SymbolIcon>();
        auto meta = presentation.count(id) ? presentation.at(id) : Presentation{};
        auto container = header.Children().GetAt(4).as<Controls::Border>();
        container.Visibility(containerDot && !meta.containerName.empty() ? Visibility::Visible : Visibility::Collapsed);
        if (unbox_value_or<uint32_t>(container.Tag(), 0) != meta.containerColor) {
            auto color = meta.containerColor;
            container.Background(Media::SolidColorBrush(Windows::UI::Color{
                255, uint8_t(color >> 16), uint8_t(color >> 8), uint8_t(color)}));
            container.Tag(box_value(color));
        }
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
        // Group/split detail remains in the tooltip and accessibility metadata;
        // tab faces show only the title.
        pinBox.Visibility(meta.flags & 1 ? Visibility::Visible : Visibility::Collapsed);
        audio.Symbol(meta.flags & 4 ? Controls::Symbol::Mute : Controls::Symbol::Volume);
        audioBox.Visibility(meta.flags & 12 ? Visibility::Visible : Visibility::Collapsed);
        Controls::ToolTipService::SetToolTip(row, box_value(meta.detail.empty() ? title : title + L"\n" + meta.detail));
        if (!meta.containerName.empty())
            Controls::ToolTipService::SetToolTip(row, box_value(title + L"\n" + meta.containerName +
                (meta.detail.empty() ? L"" : L"\n" + meta.detail)));
        Automation::AutomationProperties::SetHelpText(
            row, meta.detail + (meta.detail.empty() || meta.status.empty() ? L"" : L" \u00b7 ") + meta.status);
        if (!meta.containerName.empty()) Automation::AutomationProperties::SetHelpText(
            row, meta.containerName + L" " + meta.detail + L" " + meta.status);
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
        if (IsDragging()) {
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
        bool refreshForeignPreview = false;
        if (!groupSlide.pending) groupSlide.Cancel();
        if (ForeignPreview().active) {
            std::vector<uint32_t> order;
            for (uint32_t i = 0; i < count; ++i) order.push_back(tabs[i].id);
            shell_core_foreign_layout(core, order.data(), order.size(), &refreshForeignPreview);
        }
        ReconcileTransfers();
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
                std::vector<uint32_t> order;
                for (uint32_t i = 0; i < count; ++i) order.push_back(tabs[i].id);
                bool landed = false;
                shell_core_tab_drag_publish(core, order.data(), order.size(), &landed);
                if (landed) {
                    tabDragHeaders.Reset();
                    for (auto id : drag.affectedRowIds) {
                        if (!rows.count(id)) continue;
                        auto row = rows.at(id);
                        // Firefox has confirmed the final order. Clear the
                        // preview transforms in this same sync and let layout
                        // place the tab directly, without a post-drop slide.
                        ResetRowOffset(row, id);
                    }
                    drag.affectedRowIds.clear();
                }
            }
            const auto continuationTab = pendingWindowDragTab ? pendingWindowDragTab() : 0;
            std::set<uint32_t> current;
            for (uint32_t i = 0; i < count; ++i) {
                const auto id = tabs[i].id;
                current.insert(id);
                if (!rows.count(id)) {
                    Controls::TabViewItem row;
                    row.Loaded([this, alive = std::weak_ptr<int>(lifetime)](auto const& sender, auto&&) {
                        // Grouped-source moves can load rows after the final
                        // layout notification. Refresh once realization settles.
                        if (alive.expired()) return;
                        inlineGroups.Realized(sender.template as<FrameworkElement>());
                        ambient.ScheduleUpdate();
                    });
                    row.Tag(box_value(id));
                    row.IsClosable(true);
                    row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
                    row.RenderTransform(Media::TranslateTransform{});
                    row.Header(Markup::XamlReader::Load(
                        LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" ColumnSpacing="6"><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition Width="Auto"/><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions><Grid Width="16" Height="16"><Image Stretch="Uniform" AutomationProperties.AccessibilityView="Raw"/><ProgressRing Width="16" Height="16" IsActive="False" AutomationProperties.AccessibilityView="Raw"/></Grid><Viewbox Grid.Column="1" Width="16" Height="16"><SymbolIcon Symbol="Pin"/></Viewbox><StackPanel Grid.Column="2" MaxWidth="220" VerticalAlignment="Center"><TextBlock TextTrimming="CharacterEllipsis"/></StackPanel><Viewbox Grid.Column="3" Width="16" Height="16"><SymbolIcon Symbol="Volume"/></Viewbox><Border Grid.Column="4" Width="8" Height="8" CornerRadius="4" VerticalAlignment="Center" Visibility="Collapsed" Background="{ThemeResource TextFillColorPrimaryBrush}" AutomationProperties.AccessibilityView="Raw"/></Grid>)"));
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
                                groupSelectionGuard=false;
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
                                // Revoke the old session before overwriting its
                                // local state or taking capture for this press.
                                if (armDragSession) armDragSession(id);
                                if (!ArmDrag(id, point.Position())) return;
                                if (!row.CapturePointer(args.Pointer())) { FinishDrag(id); return; }
                                ::SetCapture(ownerHwnd);
                            })),
                        true);
                    row.AddHandler(
                        UIElement::PointerMovedEvent(),
                        box_value(Microsoft::UI::Xaml::Input::PointerEventHandler(
                            [this, alive, id](auto const&,
                                              Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {
                                // Use the same live cursor sample as the session poll.
                                // Queued routed coordinates can belong to an older
                                // layout or gesture after a transfer.
                                if (alive.expired() || syncing || !rows.count(id)) return;
                                if (Drag().tab != id || Drag().phase == DragPhase::None) {
                                    // Stock TabViewItem keeps checking for drag after
                                    // raw capture steals its PointerReleased event.
                                    // Its class handler can re-enter floating state on
                                    // a later hover even with no button held. Our session
                                    // owns dragging, so undo that stale presentation.
                                    TabDragImage::SetFloatingAppearance(rows.at(id), false);
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
                                // already have reset Drag().phase to None.
                                if (alive.expired() || Drag().tab != id || Drag().phase != DragPhase::Armed) return;
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
                                if (!alive.expired() && Drag().tab == id && Drag().phase == DragPhase::Armed)
                                    FinishDrag(id);
                            })),
                        true);
                    row.PreviewKeyDown(
                        [this, alive, id](auto const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                            if (alive.expired() || syncing || !rows.count(id)) return;
                            if (args.Key() == Windows::System::VirtualKey::Escape && Drag().tab == id &&
                                Drag().phase != DragPhase::None) {
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
                if (id == continuationTab) row.Opacity(0);
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
                if (!inlineGroups.active && (!view.TabItems().IndexOf(row, old) || old != i)) {
                    if (view.TabItems().IndexOf(row, old)) view.TabItems().RemoveAt(old);
                    view.TabItems().InsertAt(i, row);
                }
            }
            if (hasGroupLayout && inlineGroups.configured) {
                uint32_t index = 0;
                for (auto slot : groupLayout) {
                    if (!slot.tab) continue;
                    if (index >= count || tabs[index++].id != slot.tab) throw hresult_invalid_argument();
                }
                if (index != count) throw hresult_invalid_argument();
                ApplyGroupLayout();
            } else {
                while (view.TabItems().Size() > count) view.TabItems().RemoveAtEnd();
            }
            for (auto it = rows.begin(); it != rows.end();) {
                if (!current.count(it->first)) {
                    shell_core_departure_removed(core, it->first);
                    it = rows.erase(it);
                }
                else
                    ++it;
            }
            view.SelectedItem(rows.count(active) ? rows.at(active) : nullptr);
        } catch (...) {
            syncing = false;
            throw;
        }
        syncing = false;
        captionDirty = true;
        try { groupSlide.Commit(rows, inlineGroups, FindAddButton(view), active); }
        catch (...) {
#ifdef WINUI_PANE_TESTING
            fprintf(stderr, "GroupSlide Commit failed: %08x\n", unsigned(to_hresult()));
#endif
            groupSlide.Cancel();
        }
        // A stationary cursor may produce no further input after this layout
        // change. Refresh a live hover now using the session's cursor sampler.
        if (refreshForeignPreview && tickDragSession) tickDragSession();
        ambient.Update();
    }
};
