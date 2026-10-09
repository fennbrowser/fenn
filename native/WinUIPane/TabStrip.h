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
#include "TabHoverCard.h"

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
    // Ticks an armed or live tab drag. Once XAML has the pointer, WinUI can
    // deliver input inside Gecko's PeekMessage without returning a message,
    // so the pump's Translate hook alone may not run until release.
    Microsoft::UI::Dispatching::DispatcherQueueTimer tabDragTimer{nullptr};
    uint32_t groupingDelay{240}, groupCandidate{}, armedGroupTarget{};
    uint32_t joinGroup{}, joinTarget{};
    bool joinBefore{};
    // Group surface motion during a drag (the destination opening toward a
    // tail gap, a lifted member's group holding its pick-up extent, a joined
    // group held until Firefox publishes the join) is timed and tracked in
    // ShellCore's group_surface; RefreshAmbient measures and applies it.
    // Whether the last refresh held a surface or left one still moving.
    ShellGroupSurfaceResult surfaceMotion{};
    static double SteadyMs() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    Windows::Foundation::Point lastDragPoint{};
    Windows::Foundation::Rect dragSlot{};
    // Face IDs for empty drag slots inside a group surface; Firefox tab IDs
    // never reach this range. Narrower spans are ordinary spacing.
    static constexpr uint32_t SlotFaceBase = 0xFFFFFF00u;
    static constexpr float MinSlotGap = 6.f;
    // Total width of those slot faces in the last refresh (test inspection).
    float slotFaceWidth{};
    uint32_t DraggedMemberGroup() {
        if (!core || Drag().phase != DragPhase::Dragging) return 0;
        auto meta = presentation.find(Drag().tab);
        if (meta == presentation.end() || !meta->second.groupSource) return 0;
        auto slot = std::find_if(groupLayout.begin(), groupLayout.end(), [&](auto value) { return value.tab == Drag().tab; });
        if (slot == groupLayout.end() || !slot->group) return 0;
        auto run = inlineGroups.runs.find(slot->group);
        return run == inlineGroups.runs.end() || run->second.collapsed ? 0 : slot->group;
    }
    uint64_t joinFrame{~0ull};
    double joinClock{};
    // The open gap's width and destination group when it lies past that
    // group's last visible member; {0, 0} otherwise.
    std::pair<uint32_t, float> JoinTail() {
        if (!joinGroup || !core || Drag().phase != DragPhase::Dragging || Drag().original >= drag.originalWidth.size()) return {};
        auto run = inlineGroups.runs.find(joinGroup);
        if (run == inlineGroups.runs.end() || run->second.collapsed) return {};
        uint32_t last{};
        for (auto const& slot : groupLayout)
            if (slot.group == joinGroup && slot.tab && slot.tab != Drag().tab) last = slot.tab;
        const bool atEnd = last ? joinTarget == last && !joinBefore : joinTarget == 0;
        return atEnd ? std::pair{joinGroup, float(drag.originalWidth[Drag().original])} : std::pair<uint32_t, float>{};
    }

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
    // The tab order of the last full Sync.
    std::vector<uint32_t> publishedOrder;
    GroupSlide groupSlide;
    GroupDragVisual groupDragVisual;
    GroupDragShift groupDragShift;
    GroupDragShift tabDragHeaders;
    TabAmbient ambient;
    TabGroupSurface groupSurface;
    bool groupGlow{}, containerGlow{}, containerDot{}, groupHover{};
    // Opt-in outer halo for group glow; off unless Firefox publishes it.
    bool groupHalo{};
    // Opt-in light-up motion for group glow (ambient.motion.option holds
    // Firefox's groupGlowAnimations). Groups already lit when a full snapshot
    // arrives, or when motion turns on, are seeded rather than lit up again.
    TabGlowIgnition::Lit glowLit, haloLit;
    bool glowSeed{true}, glowAnimated{};
    // Settled horizontal extent of each lit group in scroll-content space,
    // so scrolling alone never reads as growth.
    std::map<uint32_t, std::pair<float, float>> glowSettled;
    // While a group's light is being revealed, tab backgrounds fade between
    // visual states rather than snapping. A drag that forms a group leaves the
    // pointer over a dropped tab, whose stock pointer-over background would
    // otherwise switch on in one frame mid-reveal (measured: 210 ms after
    // release). Uses the template's own TabContainer and the supported
    // BackgroundTransition, and restores stock instant states afterwards.
    // Selection changes during the reveal fade too, as they set the same
    // background.
    static constexpr float HoverFadeMs = 400;
    std::map<uint32_t, Controls::Grid> fadedTabs;
    // Lit groups whose color changed this refresh (glow motion).
    std::vector<uint32_t> pendingRecolors;
    void FadeTabBackgrounds(bool on) {
        if (!on) {
            for (auto& [id, container] : fadedTabs) container.BackgroundTransition(nullptr);
            fadedTabs.clear();
            return;
        }
        for (auto const& [id, row] : rows) {
            if (fadedTabs.count(id) || !row.IsLoaded() || !Media::VisualTreeHelper::GetChildrenCount(row)) continue;
            auto root = Media::VisualTreeHelper::GetChild(row, 0).try_as<FrameworkElement>();
            auto container = root ? root.FindName(L"TabContainer").try_as<Controls::Grid>() : Controls::Grid{nullptr};
            if (!container) continue;
            BrushTransition fade;
            fade.Duration(TabGlowIgnition::Ms(HoverFadeMs));
            container.BackgroundTransition(fade);
            fadedTabs.emplace(id, container);
        }
    }
    void DetachGlow() {
        for (auto* fields : {&ambient.groups, &ambient.groupSpill, &ambient.groupHalo})
            for (auto& [id, field] : *fields)
                if (field.animated) { ambient.motion.Attach(field.Root(), false); field.animated = false; }
        for (auto& [id, light] : groupSurface.lights)
            if (light.animated) { ambient.motion.Attach(light.visual, false); light.animated = false; }
        ambient.EndReveals(); ambient.EndRecolors(); groupSurface.EndReveals();
        pendingRecolors.clear();
        FadeTabBackgrounds(false);
        glowSettled.clear();
    }
    // Positioned light trails only one-off geometry changes. During
    // continuous motion it follows the geometry directly; re-place it from
    // this pass's geometry after stopping any trailing in flight.
    void FollowGlow(bool on) {
        auto& motion = ambient.motion;
        if (motion.following == on) return;
        motion.following = on;
        for (auto& [id, field] : ambient.groupHalo)
            if (field.animated) { motion.Follow(field.rimShape, on); if (!on) field.bounds = {}; }
        for (auto& [id, light] : groupSurface.lights)
            if (light.animated) { motion.Follow(light.visual, on); if (!on) light.visual.Size({}); }
    }
    void AnimateGlow(std::set<uint32_t> const& groups, std::vector<uint32_t> lit, std::vector<uint32_t> const& haloLitNow,
                     std::map<uint32_t, Windows::Foundation::Rect> const& bounds, bool moving, Controls::ScrollViewer const& scroller) {
        auto& motion = ambient.motion;
        auto outlineOf = [&](uint32_t id) {
            auto light = groupSurface.lights.find(id);
            return light == groupSurface.lights.end() ? Microsoft::UI::Composition::ShapeVisual{nullptr} : light->second.visual;
        };
        auto visualOf = [](std::map<uint32_t, TabAmbient::Field> const& fields, uint32_t id) {
            auto field = fields.find(id);
            return field == fields.end() ? Microsoft::UI::Composition::Visual{nullptr} : field->second.Root();
        };
        auto haloOf = [&](uint32_t id) {
            auto visual = visualOf(ambient.groupHalo, id);
            return visual ? visual.as<Microsoft::UI::Composition::ShapeVisual>() : Microsoft::UI::Composition::ShapeVisual{nullptr};
        };
        // The blurred light (wash, spill, halo) is revealed over the group
        // plus the halo and blur's reach; the front starts at the primary pool.
        auto revealArea = [](Windows::Foundation::Rect rect) {
            const float margin = TabAmbient::HaloReach + TabAmbient::RevealMargin;
            return Windows::Foundation::Rect{rect.X - margin, rect.Y - margin, rect.Width + 2 * margin, rect.Height + 2 * margin};
        };
        for (auto const& [id, reveal] : std::map<uint32_t, TabAmbient::Reveal>(ambient.reveals))
            if (auto rect = bounds.find(id); rect != bounds.end()) ambient.MoveReveal(id, revealArea(rect->second));
        // Several groups lighting together start left to right.
        std::sort(lit.begin(), lit.end(), [&](uint32_t a, uint32_t b) { return bounds.at(a).X < bounds.at(b).X; });
        for (size_t index = 0; index < lit.size(); ++index) {
            const auto id = lit[index];
            const auto rect = bounds.at(id);
            std::vector<Microsoft::UI::Composition::CompositionRadialGradientBrush> fronts{
                ambient.BeginReveal(id, revealArea(rect), {rect.X + TabGlowIgnition::OriginX * rect.Width,
                                                           rect.Y + TabGlowIgnition::OriginY * rect.Height})};
            if (auto outline = groupSurface.BeginReveal(id)) fronts.push_back(outline);
            motion.Reveal(id, fronts, float(std::min(index, TabGlowIgnition::StaggerCap)) * TabGlowIgnition::Stagger,
                [this, id, alive = std::weak_ptr<int>(lifetime)] {
                    if (alive.expired()) return;
                    ambient.EndReveal(id); groupSurface.EndReveal(id);
                    if (ambient.reveals.empty()) FadeTabBackgrounds(false);
                });
        }
        for (auto id : haloLitNow)
            if (std::find(lit.begin(), lit.end(), id) == lit.end()) motion.IgniteHalo(id, haloOf(id));
        // A recolor's fronts grow like a light-up's; the old color gives way.
        for (auto id : std::exchange(pendingRecolors, {})) {
            auto rect = bounds.find(id);
            if (rect == bounds.end() || !ambient.recolors.count(id)) { ambient.EndRecolor(id); continue; }
            const auto area = revealArea(rect->second);
            const Windows::Foundation::Numerics::float2 origin{rect->second.X + TabGlowIgnition::OriginX * rect->second.Width,
                                                               rect->second.Y + TabGlowIgnition::OriginY * rect->second.Height};
            const float radius = TabGlowIgnition::Reach({origin.x - area.X, origin.y - area.Y}, {area.Width, area.Height});
            auto fronts = ambient.BeginRecolor(id, origin, radius);
            for (auto const& outline : groupSurface.BeginRecolor(id, ambient.recolors.at(id).color)) fronts.push_back(outline);
            motion.Reveal(id, fronts, 0, [this, id, alive = std::weak_ptr<int>(lifetime)] {
                if (alive.expired()) return;
                ambient.EndRecolor(id); groupSurface.EndReveal(id);
                // Reattach the stop cross-fades to the restored brushes.
                for (auto* fields : {&ambient.groups, &ambient.groupSpill})
                    if (auto field = fields->find(id); field != fields->end()) {
                        ambient.motion.Attach(field->second.Root(), true); field->second.animated = true;
                    }
            });
        }
        FadeTabBackgrounds(!ambient.reveals.empty());
        // Implicit motion attaches after a field's first color and geometry,
        // so creating or re-realizing a field never animates from nothing.
        for (auto id : groups) {
            for (auto* fields : {&ambient.groups, &ambient.groupSpill, &ambient.groupHalo}) {
                auto field = fields->find(id);
                if (field == fields->end() || field->second.animated) continue;
                motion.Attach(field->second.Root(), true); field->second.animated = true;
            }
            auto light = groupSurface.lights.find(id);
            if (light != groupSurface.lights.end() && !light->second.animated) {
                motion.Attach(light->second.visual, true); light->second.animated = true;
            }
        }
        for (auto it = glowSettled.begin(); it != glowSettled.end();)
            it = glowLit.ids.count(it->first) ? std::next(it) : glowSettled.erase(it);
        // Drags, slides and joins are followed by the implicit motion; compare
        // extents only once they settle, never on every frame.
        if (moving) return;
        const float offset = scroller ? float(scroller.HorizontalOffset()) : 0.f;
        const bool rtl = root.FlowDirection() == FlowDirection::RightToLeft;
        for (auto id : groups) {
            const auto& rect = bounds.at(id);
            const float left = rect.X + (rtl ? -offset : offset), right = left + rect.Width;
            auto [settled, inserted] = glowSettled.try_emplace(id, left, right);
            if (inserted) continue;
            const auto [oldLeft, oldRight] = settled->second;
            settled->second = {left, right};
            if ((right - left) - (oldRight - oldLeft) < TabGlowIgnition::RecatchGrowth || motion.Active(id)) continue;
            motion.Recatch(outlineOf(id), oldLeft - left >= TabGlowIgnition::RecatchEdge,
                right - oldRight >= TabGlowIgnition::RecatchEdge);
        }
    }
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
    // ShellCore maps the preference names to these DIP values and owns the default.
    static constexpr float LargeGroupReach = 5.f;
    float groupReach{LargeGroupReach};
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
        // With diagnostics, report UI-thread refreshes long enough to delay
        // the reveal masks' geometry while a reveal runs.
        struct RefreshReport {
            TabAmbient& ambient; double start;
            ~RefreshReport() {
                if (!start || ambient.reveals.empty()) return;
                const double took = TabGlowIgnition::Now() - start;
                if (took > 4) fprintf(stderr, "WinUIPane glow refresh took=%.1f at=%.1f\n", took, start);
            }
        } reportRefresh{ambient, TabGlowIgnition::Diagnostics() ? TabGlowIgnition::Now() : 0};
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
        // A torn-off group is drawn only by its floating window. Its button
        // stays laid out under the transparent overlay, so leave its outline
        // and light out explicitly rather than relying on element opacity.
        const auto groupDrag = GroupDrag();
        const uint32_t tornOff = groupDrag.phase == 2 && groupDrag.detaching ? groupDrag.group : 0;
        for (auto const& slot : groupLayout) {
            if (!slot.group || slot.group == tornOff) continue;
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
        // ShellCore opens the destination group toward the dragged tab's
        // insertion gap, holds a lifted member's group at its pick-up extent
        // while the pointer stays within it, extends every surface slightly
        // past its first and last member (neighbors share the gap and never
        // overlap), and holds a dropped join's surface until Firefox publishes
        // it, so the outline, wash and halo all follow the drag.
        // Sample the clock once per rendered frame. Layout-triggered refreshes
        // in between see the same extension, so the width cannot change with
        // each pass and keep layout from ever settling (a layout cycle).
        if (joinFrame != ambient.frames) { joinFrame = ambient.frames; joinClock = SteadyMs(); }
        {
            const auto [tailGroup, tailWidth] = JoinTail();
            ShellGroupSurfaceFrame frame{};
            frame.frame_ms = joinClock; frame.wall_ms = SteadyMs();
            frame.tail_group = tailGroup; frame.tail_width = tailWidth;
            frame.source_group = DraggedMemberGroup();
            frame.drag_slot_x = dragSlot.X; frame.drag_slot_width = dragSlot.Width;
            if (frame.source_group)
                frame.pointer_x = view.TransformToVisual(ambient.layer).TransformPoint(lastDragPoint).X;
            frame.source_released = (core && Drag().detaching) || joinGroup;
            frame.dragging = IsDragging(); frame.join_group = joinGroup;
            frame.animate = groupSlide.Enabled();
            frame.right_to_left = root.FlowDirection() == FlowDirection::RightToLeft;
            frame.reach = groupReach; frame.separation = GroupSeparation;
            std::vector<ShellGroupSurface> measured;
            measured.reserve(surfaceBounds.size());
            for (auto const& [id, rect] : surfaceBounds) measured.push_back({id, rect.X, rect.Y, rect.Width, rect.Height});
            surfaceMotion = {};
            if (core && shell_core_group_surface_frame(core, &frame, groupLayout.data(), groupLayout.size(),
                                                       measured.data(), measured.size(), &surfaceMotion) == SHELL_STATUS_OK)
                for (auto const& value : measured) surfaceBounds[value.group] = {value.x, value.y, value.width, value.height};
        }
        const bool followingMotion = surfaceMotion.following || IsDragging() || GroupDrag().phase || !foreignGroupShift.entries.empty() || groupSlide.pending || groupSlide.storyboard || !rowAnimations.empty();
        // Before this pass places any light, so continuous motion never
        // starts a trailing animation.
        if (ambient.motion.Enabled()) FollowGlow(!followingMotion);
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
                const float haloOverlap = groupGlow && groupHalo ? TabAmbient::HaloReach : 0.f;
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
        // While a tab is dragged, a group surface can hold an empty slot: the
        // insertion gap between members, the tail opened past the last one,
        // or a lifted member's held slot. With no face there, full light
        // shone through it beside its neighbors' attenuated faces, cut off
        // sharply at their edges. Give each such slot an inactive tab face
        // shaped like its neighbors, so the group reads as one run of tabs.
        if (IsDragging() || surfaceMotion.holding) {
            std::vector<TabLightContour::Face> slots;
            uint32_t slotId = SlotFaceBase;
            for (auto const& [group, surface] : surfaceBounds) {
                std::vector<std::pair<float, float>> occupied;
                TabLightContour::Face const* sample{};
                for (auto const& face : faces) {
                    const auto& b = face.bounds;
                    if (b.X + b.Width <= surface.X || b.X >= surface.X + surface.Width) continue;
                    occupied.push_back({b.X, b.X + b.Width});
                    if (!sample || (sample->selected && !face.selected)) sample = &face;
                }
                if (!sample) continue;
                if (auto run = inlineGroups.runs.find(group); run != inlineGroups.runs.end()) {
                    auto button = run->second.button;
                    if (button && button.IsLoaded() && button.Visibility() == Visibility::Visible && button.Opacity() > 0) {
                        const auto header = boundsOf(button);
                        occupied.push_back({header.X, header.X + header.Width});
                    }
                }
                std::sort(occupied.begin(), occupied.end());
                auto fill = [&](float from, float to) {
                    if (to - from < MinSlotGap) return;
                    slots.push_back({slotId++, {from, sample->bounds.Y, to - from, sample->bounds.Height},
                        sample->leftRadius, sample->rightRadius, false});
                };
                // The reach past the outer members stays lit, as at rest.
                float cursor = surface.X + groupReach;
                for (auto [left, right] : occupied) { fill(cursor, left); cursor = std::max(cursor, right); }
                fill(cursor, surface.X + surface.Width - groupReach);
            }
            slotFaceWidth = 0;
            for (auto const& slot : slots) slotFaceWidth += slot.bounds.Width;
            faces.insert(faces.end(), slots.begin(), slots.end());
        } else {
            slotFaceWidth = 0;
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
            // A lit group changing color spreads the new color over the old
            // (glow motion): stop the per-stop cross-fade so the new color
            // applies at once beneath the reveal, and keep old-color copies.
            const auto shown = ambient.groups.find(id);
            const uint32_t previousColor = shown == ambient.groups.end() ? 0 : shown->second.color;
            if (previousColor && previousColor != color && ambient.motion.Enabled() && glowLit.ids.count(id) &&
                !ambient.reveals.count(id) && !ambient.recolors.count(id)) {
                for (auto* fields : {&ambient.groups, &ambient.groupSpill})
                    if (auto field = fields->find(id); field != fields->end() && field->second.animated) {
                        ambient.motion.Attach(field->second.Root(), false); field->second.animated = false;
                    }
                if (auto light = groupSurface.lights.find(id); light != groupSurface.lights.end() && light->second.animated) {
                    ambient.motion.Attach(light->second.visual, false); light->second.animated = false;
                }
                ambient.recolors[id].color = previousColor;
                pendingRecolors.push_back(id);
            }
            const auto radius = float(groupSurface.surfaces.at(id).CornerRadius().TopLeft);
            auto placeWash = [&](std::map<uint32_t, TabAmbient::Field>& fields, uint32_t tint) {
                ambient.Put(fields, id, tint, bounds, false);
                auto& field = fields.at(id);
                // A soft wash belongs inside the rounded group, not under each tab
                // divider or beyond the ends of the new shared surface.
                if (field.visual.Opacity() != .65f) field.visual.Opacity(.65f);
                if (!field.roundedClip) {
                    auto compositor = field.visual.Compositor();
                    field.roundedClip = compositor.CreateRoundedRectangleGeometry();
                    field.visual.Clip(compositor.CreateGeometricClip(field.roundedClip));
                }
                const Windows::Foundation::Numerics::float2 size{bounds.Width, bounds.Height}, corners{radius, radius};
                if (field.roundedClip.Size() != size) field.roundedClip.Size(size);
                if (field.roundedClip.CornerRadius() != corners) field.roundedClip.CornerRadius(corners);
            };
            auto placeSpill = [&](std::map<uint32_t, TabAmbient::Field>& fields, uint32_t tint) {
                ambient.Spill(fields, id, tint, bounds, false);
                auto spill = fields.at(id).visual;
                if (spill.Opacity() != .65f) spill.Opacity(.65f);
            };
            placeWash(ambient.groups, color);
            placeSpill(ambient.groupSpill, color);
            if (auto recolor = ambient.recolors.find(id); recolor != ambient.recolors.end()) {
                placeWash(ambient.retiringWash, recolor->second.color);
                placeSpill(ambient.retiringSpill, recolor->second.color);
            }
            if (groupHalo) ambient.Halo(id, color, bounds, radius);
        }
        // Decide which groups newly lit or went out before lighting and
        // pruning, so either can animate. Culling alone is neither.
        const bool animate = ambient.motion.Enabled();
        if (animate != glowAnimated) {
            glowAnimated = animate;
            if (animate) glowSeed = true;
            else { ambient.motion.Cancel(); DetachGlow(); }
        }
        std::set<uint32_t> eligible, haloEligible, gone, haloGone;
        for (auto value : currentGroups) {
            auto group = value.GetObject();
            if (!groupGlow || !uint32_t(group.GetNamedNumber(L"color", 0))) continue;
            const auto id = uint32_t(group.GetNamedNumber(L"id"));
            eligible.insert(id);
            if (groupHalo) haloEligible.insert(id);
        }
        const bool seed = glowSeed || !animate;
        glowSeed = false;
        auto lit = glowLit.Advance(eligible, groups, seed, gone);
        auto haloLitNow = haloLit.Advance(haloEligible, groupHalo ? groups : std::set<uint32_t>{}, seed, haloGone);
        haloGone.insert(gone.begin(), gone.end());
        // Light going out ends any reveal first, so it dims or goes whole.
        for (auto id : gone) {
            if (TabGlowIgnition::Diagnostics() && ambient.reveals.count(id))
                fprintf(stderr, "WinUIPane glow reveal cancelled group=%u at=%.1f\n", id, TabGlowIgnition::Now());
            ambient.EndReveal(id); ambient.EndRecolor(id); groupSurface.EndReveal(id);
        }
        groupSurface.Lighting(outlineColors, &ambient.motion, gone);
        groupSurface.Depart(&ambient.motion, gone);
        ambient.Prune(ambient.groups, groups, false, gone);
        ambient.Prune(ambient.containers, containers, true);
        ambient.PruneSpill(ambient.groupSpill, groups, gone);
        ambient.PruneHalo(groupHalo ? groups : std::set<uint32_t>{}, haloGone);
        ambient.PruneSpill(ambient.containerSpill, {});
        if (animate) AnimateGlow(groups, std::move(lit), haloLitNow, surfaceBounds, followingMotion, scroller);
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
    // A group leaving for another window keeps the look the drag gave it --
    // members collapsed, header slot empty -- until Firefox publishes a layout
    // without it, so it never pops back while the new window opens. ShellCore
    // owns the identity and outcome; only these visuals live here.
    std::map<uint32_t, Visibility> departVisibility;
    uint32_t departHeader{};
    Microsoft::UI::Dispatching::DispatcherQueueTimer departTimer{nullptr};
    ShellGroupDeparture GroupDeparture() const {
        ShellGroupDeparture state{};
        if (core) shell_core_group_departure_state(core,&state);
        return state;
    }
    bool DepartGroup(uint32_t group, uint64_t request) {
        if (!core || shell_core_group_departure_begin(core,group,request)!=SHELL_STATUS_OK) return false;
        departVisibility=std::move(groupDragVisibility);
        groupDragVisibility.clear();
        if (auto run=inlineGroups.runs.find(group); run!=inlineGroups.runs.end() && run->second.button) {
            run->second.button.Opacity(0);
            run->second.button.IsHitTestVisible(false);
            departHeader=group;
        }
        // Firefox normally removes the group well within this. If it is still
        // here, the move did not happen; show the group again.
        if (!departTimer) {
            departTimer=root.DispatcherQueue().CreateTimer();
            departTimer.IsRepeating(false);
            departTimer.Interval(std::chrono::seconds(8));
            departTimer.Tick([this, alive=std::weak_ptr<int>(lifetime)](auto&&,auto&&) {
                if (!alive.expired()) ReleaseGroupDeparture();
            });
        }
        departTimer.Start();
        return true;
    }
    void ReleaseGroupDeparture() {
        if (departTimer) departTimer.Stop();
        for (auto [id, visibility] : departVisibility)
            if (rows.count(id)) rows.at(id).Visibility(visibility);
        departVisibility.clear();
        if (departHeader) {
            if (auto run=inlineGroups.runs.find(departHeader); run!=inlineGroups.runs.end() && run->second.button) {
                run->second.button.Opacity(1);
                run->second.button.IsHitTestVisible(true);
            }
            departHeader=0;
        }
        if (core) shell_core_group_departure_end(core);
    }
    // After request outcomes: this strip's own detach was refused.
    void ReconcileGroupDeparture() {
        const auto state=GroupDeparture();
        if (state.group && state.result==2) ReleaseGroupDeparture();
    }
    // Another window's strip reports that Firefox refused its adopt.
    void RejectGroupDeparture(uint32_t group) {
        bool changed=false;
        if (core && shell_core_group_departure_reject(core,group,&changed)==SHELL_STATUS_OK && changed) ReleaseGroupDeparture();
    }
    ULONGLONG groupScrollTick{}, groupScrollHold{};
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
    // `foreign`: another window's strip queued the adopt for this group.
    void EndGroupDrag(bool commit = false, bool foreign = false) {
        const auto state=GroupDrag();
        // Torn off from a window it fills, Firefox moves the window instead of
        // handing the group over: nothing departs. Another window still adopts it.
        size_t tabCount{}, groupTabs{};
        for (auto slot : groupLayout) if (slot.tab) { ++tabCount; if (slot.group==state.group) ++groupTabs; }
        const bool movesWindow=groupTabs && groupTabs==tabCount;
        const bool detach=commit && state.phase==2 && state.detaching && dispatch;
        const bool departing=state.phase==2 && (foreign || (detach && !movesWindow));
        groupDragGeometry.clear();
        groupDragShift.Reset();
        if (!departing) RestoreDraggingGroup();
        if (!state.phase) { groupDragVisual.Hide(); return; }
        shell_core_group_drag_end(core);
        groupDragVisual.Hide();
        if (state.phase==2 && rows.count(activeTabId)) {
            const auto previous=syncing;
            syncing=true;
            view.SelectedItem(rows.at(activeTabId));
            syncing=previous;
        }
        // The group re-expands and the browser reorders it a moment later; keep
        // the scroll position through both.
        if (state.phase==2) groupScrollHold=GetTickCount64()+600;
        if (state.phase==2 || (state.phase && !commit)) inlineGroups.suppressClick=state.group;
        if (foreign) {
            if (!departing || !DepartGroup(state.group,0)) RestoreDraggingGroup();
        } else if (detach) {
            // Firefox opens the new window at the release point; the group stays
            // hidden here until that window adopts it.
            POINT screenPoint{};
            GetCursorPos(&screenPoint);
            using Value=Windows::Data::Json::JsonValue;
            Windows::Data::Json::JsonObject action;
            action.SetNamedValue(L"kind",Value::CreateStringValue(L"strip-group-detach"));
            action.SetNamedValue(L"revision",Value::CreateNumberValue(presentationRevision));
            action.SetNamedValue(L"session",Value::CreateNumberValue(windowDragSession));
            action.SetNamedValue(L"group",Value::CreateNumberValue(state.group));
            action.SetNamedValue(L"screenX",Value::CreateNumberValue(double(screenPoint.x)));
            action.SetNamedValue(L"screenY",Value::CreateNumberValue(double(screenPoint.y)));
            const auto request=dispatch(0,action.Stringify());
            if (!departing || !request || !DepartGroup(state.group,request)) RestoreDraggingGroup();
        } else if (commit && state.phase==2 && state.valid && dispatch) {
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
    // `foreign`: another window's strip claims the pointer; Pane.cpp resolves
    // a release there itself, so this strip only takes the group off itself.
    void TickGroupDrag(Windows::Foundation::Point point, bool down, bool cancel, bool foreign = false) {
        const auto old=GroupDrag();
        if (!old.phase) return;
        if (cancel || !root.IsLoaded() || root.Visibility()!=Visibility::Visible) { EndGroupDrag(); return; }
        const bool geometryReady=old.phase==2 && !groupSlide.pending && !groupSlide.storyboard;
        if (geometryReady && groupDragGeometry.empty()) view.UpdateLayout();
        auto scroller=GroupScroller(view);
        auto viewport = scroller ? scroller.TransformToVisual(view).TransformBounds({0,0,float(scroller.ActualWidth()),float(scroller.ActualHeight())})
                                : Windows::Foundation::Rect{0,0,float(view.ActualWidth()),float(view.ActualHeight())};
        const bool inside=point.X>=viewport.X && point.X<=view.ActualWidth() && point.Y>=0 && point.Y<=view.ActualHeight();
        // The pointer may be past the scroller's own edge, over the trailing
        // controls: holding it there at the strip's end must keep scrolling.
        const bool nearStrip=point.X>=0 && point.X<=view.ActualWidth() && point.Y>=0 && point.Y<=view.ActualHeight();
        if (old.phase==2 && down && nearStrip && scroller && scroller.ScrollableWidth()>0 && GetTickCount64()-groupScrollTick>=16) {
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
        if (shell_core_group_drag_update(core,point.X,point.Y,inside,viewport.X,viewport.X+viewport.Width,slots.data(),slots.size())!=SHELL_STATUS_OK ||
            shell_core_group_drag_detach(core,point.X,point.Y,view.ActualWidth(),view.ActualHeight(),foreign)!=SHELL_STATUS_OK) { EndGroupDrag(); return; }
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

    // The dragged group's slot width, for another window's destination gap.
    double GroupDragSlotWidth() const {
        if (!groupDragVisual.placeholder) return 0;
        return groupDragVisual.placeholder.Width()+groupDragVisual.margin.Left+groupDragVisual.margin.Right;
    }
    // This strip as the destination of another window's whole-group drag.
    // ShellCore picks an insertion point between whole blocks and holds the
    // gap after the drop; only the WinUI transforms live here.
    GroupDragShift foreignGroupShift;
    // The window a held adopt takes its group from, told if Firefox refuses.
    struct ForeignGroupSource { NativeTabStrip* strip{}; std::weak_ptr<int> lifetime; uint32_t group{}; } foreignGroupSource;
    ShellForeignGroup ForeignGroup() const {
        ShellForeignGroup state{};
        if (core) shell_core_group_foreign_state(core,&state);
        return state;
    }
    bool ForeignGroupOver(Windows::Foundation::Point point, double width) {
        if (!core || !root.IsLoaded() || root.Visibility()!=Visibility::Visible || IsDragging() || GroupDrag().phase) return false;
        if (ForeignGroup().request) EndForeignGroupOver(true);
        if (foreignGroupShift.entries.empty()) view.UpdateLayout();
        std::vector<ShellGroupDragSlot> slots;
        struct Item { uint64_t key; FrameworkElement element; size_t block; };
        std::vector<Item> items;
        for (auto slot : groupLayout) {
            FrameworkElement item{nullptr};
            if (!slot.tab) {
                auto found=inlineGroups.runs.find(slot.group);
                if (found==inlineGroups.runs.end() || !found->second.button.IsLoaded()) continue;
                item=found->second.button;
            } else {
                if (!rows.count(slot.tab) || !rows.at(slot.tab).IsLoaded() ||
                    rows.at(slot.tab).Visibility()==Visibility::Collapsed) continue;
                if (presentation.count(slot.tab) && (presentation.at(slot.tab).flags&1)) continue;
                item=rows.at(slot.tab);
            }
            const auto key=slot.tab ? uint64_t(slot.tab) : GroupSlide::GroupKey(slot.group);
            auto bounds=item.TransformToVisual(view).TransformBounds({0,0,float(item.ActualWidth()),float(item.ActualHeight())});
            // Unshifted geometry: opening the gap must not move its own target.
            const double start=bounds.X-foreignGroupShift.Offset(key), end=start+bounds.Width;
            if (slot.tab && slot.group) {
                if (!slots.empty() && slots.back().group==slot.group) {
                    slots.back().start=std::min(slots.back().start,start);
                    slots.back().end=std::max(slots.back().end,end);
                } else slots.push_back({slot.group,0,start,end});
            } else slots.push_back({slot.group,slot.tab,start,end});
            items.push_back({key,item,slots.size()-1});
        }
        if (shell_core_group_foreign_hover(core,point.X,slots.data(),slots.size())!=SHELL_STATUS_OK) {
            EndForeignGroupOver(true);
            return false;
        }
        for (auto const& item : items) {
            double offset{};
            if (shell_core_group_foreign_offset(core,item.block,width,&offset)==SHELL_STATUS_OK)
                foreignGroupShift.Apply(item.key,item.element,offset,groupSlide.Enabled());
        }
        ambient.Update();
        return true;
    }
    void EndForeignGroupOver(bool force = false) {
        bool ended=false;
        if (core) shell_core_group_foreign_end(core,force,&ended);
        if (ended || !core) foreignGroupShift.Reset();
    }
    // Releases a held gap once Firefox has rejected the adopt.
    void ReconcileForeignGroup() {
        const auto state=ForeignGroup();
        if (state.result==2 && foreignGroupSource.strip && !foreignGroupSource.lifetime.expired())
            foreignGroupSource.strip->RejectGroupDeparture(foreignGroupSource.group);
        if (!state.active || state.result!=0) foreignGroupSource={};
        if (!state.active || state.result==2) EndForeignGroupOver();
    }
    // Queues the adopt on this window's transport. The group ID belongs to the
    // source window's session, which Firefox resolves from its HWND.
    uint64_t AcceptForeignGroupDrop(NativeTabStrip& source, HWND sourceHwnd, uint32_t sourceSession, uint32_t group,
                                    Windows::Foundation::Point point, double width) {
        if (!dispatch || !ForeignGroupOver(point,width)) { EndForeignGroupOver(true); return 0; }
        const auto target=ForeignGroup();
        using Value=Windows::Data::Json::JsonValue;
        Windows::Data::Json::JsonObject action;
        action.SetNamedValue(L"kind",Value::CreateStringValue(L"strip-group-adopt"));
        action.SetNamedValue(L"revision",Value::CreateNumberValue(presentationRevision));
        action.SetNamedValue(L"session",Value::CreateNumberValue(windowDragSession));
        action.SetNamedValue(L"sourceHwnd",Value::CreateNumberValue(double(reinterpret_cast<uintptr_t>(sourceHwnd))));
        action.SetNamedValue(L"sourceSession",Value::CreateNumberValue(sourceSession));
        action.SetNamedValue(L"group",Value::CreateNumberValue(group));
        action.SetNamedValue(L"beforeGroup",Value::CreateNumberValue(target.before_group));
        action.SetNamedValue(L"beforeTab",Value::CreateNumberValue(target.before_tab));
        const auto request=dispatch(0,action.Stringify());
        if (!request || shell_core_group_foreign_hold(core,request)!=SHELL_STATUS_OK) EndForeignGroupOver(true);
        else foreignGroupSource={&source,source.lifetime,group};
        return request;
    }

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
    // The accepted revision as gestures report it. ShellCore owns acceptance and
    // the exact-replay fingerprint.
    uint32_t presentationRevision{};
    hstring groupsLabel;
    // Accessible names, tooltips and states published by Firefox from its own
    // localization. English is only the fallback before the first publication.
    struct Labels {
        hstring tabs{L"Tabs in this window"}, active{L"Active tab"}, selected{L"Selected tab"};
        hstring allTabs{L"List all tabs"}, allTabsTip{L"List all tabs"};
        hstring newTab{L"New tab"}, newTabTip{L"New tab (Ctrl+T)"};
    } labels;
    hstring StatusFor(bool active, bool selected) const {
        return active ? labels.active : selected ? labels.selected : hstring{};
    }
    static Labels ParseLabels(Windows::Data::Json::JsonObject const& data, Labels next) {
        auto text = [](Windows::Data::Json::JsonObject const& object, wchar_t const* key, hstring& value) {
            if (!object.HasKey(key)) return;
            auto candidate = object.GetNamedString(key);
            if (candidate.empty()) throw hresult_invalid_argument();
            NativeJson::RequireLength(candidate, NativeJson::kMaxShortLabel);
            value = candidate;
        };
        text(data, L"tabs", next.tabs);
        text(data, L"active", next.active);
        text(data, L"selected", next.selected);
        if (data.HasKey(L"allTabs")) {
            auto entry = data.GetNamedObject(L"allTabs");
            text(entry, L"label", next.allTabs);
            text(entry, L"tooltip", next.allTabsTip);
        }
        if (data.HasKey(L"newTab")) {
            auto entry = data.GetNamedObject(L"newTab");
            text(entry, L"label", next.newTab);
            text(entry, L"tooltip", next.newTabTip);
        }
        return next;
    }
    void ApplyAddButtonLabels() {
        if (auto button = FindAddButton(view)) {
            Automation::AutomationProperties::SetName(button, labels.newTab);
            Controls::ToolTipService::SetToolTip(button, box_value(labels.newTabTip));
        }
    }
    void ApplyLabels(Labels const& next) {
        const bool statusChanged = next.active != labels.active || next.selected != labels.selected;
        labels = next;
        Automation::AutomationProperties::SetName(view, labels.tabs);
        Automation::AutomationProperties::SetName(allTabs.button, labels.allTabs);
        Controls::ToolTipService::SetToolTip(allTabs.button, box_value(labels.allTabsTip));
        ApplyAddButtonLabels();
        if (statusChanged)
            for (auto const& [id, row] : rows)
                Automation::AutomationProperties::SetItemStatus(row,
                    StatusFor(id == activeTabId, menu.selectedTabs.count(id) != 0));
    }
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
    // Work-accounting hooks for the drop round trip: release commits a reorder,
    // and the drop is over once Firefox's snapshot places the tab. Unset when
    // accounting is off.
    std::function<void()> dropCommitted, dropLanded;
    // Hands one hover-card request to the pane's coalescing preview queue.
    std::function<void(hstring const& action)> preview;
    TabHoverCard hoverCard;
    // Firefox's answer to the newest hover-card request; see TabHoverCard.
    void ApplyPreview(NativeTabMenu::Object const& answer) { hoverCard.Apply(answer); }
    // Only peer object lifetimes stay native. Rust owns tickets, request
    // outcomes and source departure tokens until their UI effects are applied.
    struct TransferPeer { NativeTabStrip* source; std::weak_ptr<int> lifetime; };
    std::map<uint64_t, TransferPeer> transferPeers;
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
        // Where the scroller's content sat, in view coordinates, when the
        // geometry above was measured. ShellCore keeps that snapshot for the
        // whole gesture, so live pointer input is mapped into its frame.
        double contentOrigin{};
        double shift{};
        ULONGLONG scrollTick{};
    } drag;
    ShellTabDrag Drag() const {
        ShellTabDrag state{};
        if (core) shell_core_tab_drag_state(core, &state);
        return state;
    }
    void StartTabDragTimer() {
        if (!tabDragTimer) {
            tabDragTimer = root.DispatcherQueue().CreateTimer();
            tabDragTimer.Interval(std::chrono::milliseconds(16));
            tabDragTimer.Tick([this, alive = std::weak_ptr<int>(lifetime)](auto const& timer, auto const&) {
                if (alive.expired()) return;
                // Like the pump's Translate hook: an exception escaping a
                // timer callback would fail-fast the browser.
                try {
                    if (Drag().phase == DragPhase::None) { timer.Stop(); return; }
                    if (tickDragSession) tickDragSession();
                } catch (...) {}
            });
        }
        tabDragTimer.Start();
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
    uint32_t windowDragSession{};
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
    Media::Animation::Storyboard addSlide{nullptr};
    uint64_t closeGeneration{};
    // The tab most recently opened beside existing ones, and until when
    // TabView bringing it into view counts as part of its entry motion.
    uint32_t enteringTab{};
    uint64_t enteringUntil{};
    Media::Transform addPrevious{nullptr};
    bool addHeld{};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    NativeTabStrip(NativeTabMenu& model) : menu(model) {
        root =
            Markup::XamlReader::Load(
                LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="Transparent"><Grid.Resources><ResourceDictionary><ResourceDictionary.ThemeDictionaries>
<ResourceDictionary x:Key="Default" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"><SolidColorBrush x:Key="TabViewItemHeaderBackgroundSelected" Color="{ThemeResource LayerOnMicaBaseAltFillColorDefault}"/></ResourceDictionary>
<ResourceDictionary x:Key="Light" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"><SolidColorBrush x:Key="TabViewItemHeaderBackgroundSelected" Color="{ThemeResource LayerOnMicaBaseAltFillColorDefault}"/></ResourceDictionary>
<ResourceDictionary x:Key="HighContrast" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"><SolidColorBrush x:Key="TabViewItemHeaderBackgroundSelected" Color="{ThemeResource SystemColorWindowColor}"/></ResourceDictionary>
</ResourceDictionary.ThemeDictionaries></ResourceDictionary></Grid.Resources><TabView TabWidthMode="Equal" CanDragTabs="False" CanReorderTabs="False" AllowDrop="False" VerticalAlignment="Top" AutomationProperties.AutomationId="FennTabStrip" AutomationProperties.Name="Tabs in this window" /></Grid>)")
                .as<Controls::Grid>();
        view = root.Children().GetAt(0).as<Controls::TabView>();
        ambient.Create(root);
        separatorTheme = Markup::XamlReader::Load(LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource TabViewItemSeparator}" Visibility="Collapsed" IsHitTestVisible="False" AutomationProperties.AccessibilityView="Raw"/>)").as<Controls::Border>();
        ambient.layer.Children().Append(separatorTheme);
        ambient.refresh = [this] { RefreshAmbient(); };
        // Firefox opens its preview only in the active window and never while
        // tabs move (tab-hover-preview.mjs, shouldActivate). A card is a windowed
        // popup: one opened from a background window's visible strip edge floats
        // over the window in front and takes its input.
        hoverCard.suppressed = [this] {
            return IsDragging() || GroupDrag().phase || groupSlide.pending || groupSlide.storyboard ||
                   !rowAnimations.empty() || !foreignGroupShift.entries.empty() ||
                   (ownerHwnd && GetAncestor(ownerHwnd, GA_ROOT) != GetForegroundWindow());
        };
        hoverCard.Bind(root);
        hoverCard.send = [this](uint32_t tab, uint64_t request, double scale) {
            if (!preview) return;
            using Value = Windows::Data::Json::JsonValue;
            Windows::Data::Json::JsonObject action;
            action.SetNamedValue(L"kind", Value::CreateStringValue(L"strip-preview"));
            action.SetNamedValue(L"session", Value::CreateNumberValue(windowDragSession));
            action.SetNamedValue(L"id", Value::CreateNumberValue(tab));
            action.SetNamedValue(L"request", Value::CreateNumberValue(double(request)));
            action.SetNamedValue(L"scale", Value::CreateNumberValue(scale));
            preview(action.Stringify());
        };
        inlineGroups.realized = [this] { ambient.ScheduleUpdate(); };
        inlineGroups.holdScroll = [this] {
            const auto state = GroupDrag();
            return state.phase == 2 || state.awaiting || groupSlide.resizing || GetTickCount64() < groupScrollHold;
        };
        inlineGroups.holdCaps = [this] { return groupSlide.resizing; };
        inlineGroups.fixedWidth = [this](Controls::TabViewItem const& row) { return IsPinnedRow(row); };
        inlineGroups.animateBring = [this](Controls::TabViewItem const& tab) {
            if (!enteringTab || GetTickCount64() > enteringUntil || !groupSlide.Enabled()) return false;
            auto found = rows.find(enteringTab);
            return found != rows.end() && found->second == tab;
        };
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
            if (!core || syncing || !armGroupDragSession || (GetKeyState(VK_CONTROL)&0x8000) || (GetKeyState(VK_SHIFT)&0x8000) ||
                GroupDeparture().group==id) return;
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
        inlineGroups.highlight = [this](uint32_t id) { groupSurface.Hover(groupHover ? id : 0); };
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
            if ((key==Key::Left || key==Key::Right || key==Key::Home || key==Key::End) && !args.Handled() &&
                !IsDragging() && Navigate(args.OriginalSource(), key))
                args.Handled(true);
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
            // Firefox keeps the active tab for these gestures, so return
            // TabView's visual selection to it instead of the clicked tab.
            if (GetKeyState(VK_CONTROL) & 0x8000 || GetKeyState(VK_SHIFT) & 0x8000) {
                RestoreActiveSelection();
                // The stock ListView is still mid-selection here and can leave
                // the clicked row looking selected; repeat once it settles.
                Dispatching::DispatcherQueue::GetForCurrentThread().TryEnqueue([this, alive] {
                    if (!alive.expired()) RestoreActiveSelection();
                });
                return;
            }
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
            ApplyAddButtonLabels();
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
        Automation::AutomationProperties::SetAutomationId(button, L"FennStripNewTab");
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
        if (tabDragTimer) tabDragTimer.Stop();
        lifetime.reset();
        if (core) shell_core_transfer_cancel(core);
        try { ReconcileTransfers(); } catch (...) { /* An island may also be closing. */ }
        inlineGroups.Release(view);
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

    uint64_t CompleteReorderAt(Controls::TabViewItem const& tab, Controls::TabViewItem const& beforeRow,
                               bool ungroup = false, bool unpin = false) {
        uint32_t beforeId = beforeRow && beforeRow != tab ? unbox_value<uint32_t>(beforeRow.Tag()) : 0;
        Windows::Data::Json::JsonObject action;
        action.SetNamedValue(L"kind", Windows::Data::Json::JsonValue::CreateStringValue(L"strip-reorder"));
        action.SetNamedValue(L"revision", Windows::Data::Json::JsonValue::CreateNumberValue(presentationRevision));
        action.SetNamedValue(L"id", Windows::Data::Json::JsonValue::CreateNumberValue(unbox_value<uint32_t>(tab.Tag())));
        action.SetNamedValue(L"beforeId", Windows::Data::Json::JsonValue::CreateNumberValue(beforeId));
        action.SetNamedValue(L"ungroup", Windows::Data::Json::JsonValue::CreateBooleanValue(ungroup));
        action.SetNamedValue(L"unpin", Windows::Data::Json::JsonValue::CreateBooleanValue(unpin));
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
    uint64_t QueueIncomingAdopt(HWND sourceHwnd, uint32_t tabId, uint32_t index) {
        Windows::Data::Json::JsonObject action;
        action.SetNamedValue(L"kind", Windows::Data::Json::JsonValue::CreateStringValue(L"strip-adopt"));
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
        row.Clip(nullptr);
        rowAnimations.erase(it);
    }

    // Runs `work` once, from the next frame's Rendering callback: after the
    // layout that follows the current change, before that frame is presented.
    void AfterNextLayout(std::function<void()> work) {
        auto token = std::make_shared<winrt::event_token>();
        *token = Media::CompositionTarget::Rendering([token, work = std::move(work)](auto&&, auto&&) {
            Media::CompositionTarget::Rendering(*token);
            work();
        });
    }

    // A tab created after the strip was already populated is revealed with a
    // wipe from the edge it shares with the tab before it; the tab itself stays
    // in its slot. Skipped for the first tab and under reduced motion.
    // The closed tab's row is gone by the time layout runs, so its geometry and
    // face are captured beforehand and shown by a visual-only stand-in that
    // wipes closed toward the tab before it while the others close the gap.
    struct ClosingTab {
        bool valid{};
        Windows::Foundation::Rect bounds{};
        hstring title;
        Media::ImageSource image{nullptr};
        bool selected{};
    };
    Controls::Canvas closeOverlay{nullptr};
    std::vector<Media::Animation::Storyboard> closeStoryboards;

    void AddCloseGhost(ClosingTab const& closing) {
        if (!closing.valid) return;
        if (!closeOverlay) {
            closeOverlay = Controls::Canvas();
            closeOverlay.IsHitTestVisible(false);
            Automation::AutomationProperties::SetAccessibilityView(closeOverlay, Automation::Peers::AccessibilityView::Raw);
            Controls::Grid::SetColumnSpan(closeOverlay, 3);
            Controls::Canvas::SetZIndex(closeOverlay, 10);
            root.Children().Append(closeOverlay);
        }
        Media::RectangleGeometry viewClip;
        const auto viewBounds = view.TransformToVisual(root).TransformBounds(
            {0, 0, float(view.ActualWidth()), float(view.ActualHeight())});
        viewClip.Rect(viewBounds);
        closeOverlay.Clip(viewClip);
        Controls::StackPanel face;
        face.Orientation(Controls::Orientation::Horizontal);
        face.Spacing(6);
        if (closing.image) {
            Controls::Image icon;
            icon.Width(16); icon.Height(16);
            icon.Source(closing.image);
            face.Children().Append(icon);
        }
        Controls::TextBlock name;
        name.Text(closing.title);
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        name.VerticalAlignment(VerticalAlignment::Center);
        face.Children().Append(name);
        Controls::TabViewItem ghost;
        ghost.Header(face);
        ghost.IsClosable(true);
        ghost.IsSelected(closing.selected);
        ghost.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        ghost.Width(closing.bounds.Width);
        ghost.Height(closing.bounds.Height);
        Media::ScaleTransform scale;
        Media::RectangleGeometry clip;
        clip.Rect({0, 0, closing.bounds.Width, closing.bounds.Height});
        clip.Transform(scale);
        ghost.Clip(clip);
        Controls::Canvas::SetLeft(ghost, closing.bounds.X);
        Controls::Canvas::SetTop(ghost, closing.bounds.Y);
        closeOverlay.Children().Append(ghost);
        ghost.UpdateLayout();
        GroupSlide::MakeVisualOnly(ghost);
        Media::Animation::DoubleAnimation animation;
        animation.From(1.0);
        animation.To(0.0);
        animation.Duration(
            Duration{Windows::Foundation::TimeSpan{std::chrono::milliseconds(135)}, DurationType::TimeSpan});
        Media::Animation::CubicEase ease;
        ease.EasingMode(Media::Animation::EasingMode::EaseOut);
        animation.EasingFunction(ease);
        Media::Animation::Storyboard::SetTarget(animation, scale);
        Media::Animation::Storyboard::SetTargetProperty(animation, L"ScaleX");
        Media::Animation::Storyboard storyboard;
        storyboard.Children().Append(animation);
        storyboard.Completed([this, alive = std::weak_ptr<int>(lifetime), ghost](auto const& sender, auto const&) {
            if (alive.expired()) return;
            uint32_t index;
            if (closeOverlay && closeOverlay.Children().IndexOf(ghost, index)) closeOverlay.Children().RemoveAt(index);
            std::erase_if(closeStoryboards, [&](auto const& item) { return item == sender; });
        });
        closeStoryboards.push_back(storyboard);
        storyboard.Begin();
    }

    // Existing tabs change width when a tab is added. Their layout jumps at
    // once; a composition-only offset and scale (from the left edge) puts each
    // back on its old geometry, then eases it to rest, so nothing else fights
    // the row's XAML RenderTransform. Bounds and composition offsets share one
    // unmirrored space in both flow directions (a row's Visual.Offset is the
    // same in each), so the left-edge formula holds in right-to-left too; the
    // right-edge anchor tried first was wrong.
    struct Neighbor { Controls::TabViewItem row{nullptr}; float dx{}, sx{}; };

    // Opt-in (FENN_PANE_DIAGNOSTICS) trace of the new-tab entry motion.
    template <typename... Args> static void EntryLog(char const* format, Args... args) {
        static const bool enabled = GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0) != 0;
        if (!enabled) return;
        fprintf(stderr, format, args...);
        fflush(stderr);
    }

    std::vector<Neighbor> HoldNeighbors(std::map<uint32_t, std::pair<float, float>> const& before, uint32_t newId) {
        std::vector<Neighbor> held;
        for (auto const& [id, row] : rows) {
            auto old = before.find(id);
            // Grouped-source rows can be arranged while IsLoaded stays false.
            if (id == newId || old == before.end() || (!row.IsLoaded() && row.ActualWidth() <= 0)) continue;
            const auto now = row.TransformToVisual(root).TransformBounds(
                {0, 0, float(row.ActualWidth()), float(row.ActualHeight())});
            // Rows outside the strip before and after cannot be seen moving; a
            // recycled container's bounds there are not meaningful either.
            const auto stripWidth = float(root.ActualWidth());
            auto outside = [stripWidth](float x, float width) { return x + width <= 0 || x >= stripWidth; };
            if (outside(now.X, now.Width) && outside(old->second.first, old->second.second)) continue;
            const float dx = old->second.first - now.X, sx = now.Width > 0 ? old->second.second / now.Width : 1.f;
            if (std::abs(dx) < .5f && std::abs(sx - 1.f) < .002f) continue;
            auto visual = winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(row);
            winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::SetIsTranslationEnabled(row, true);
            visual.Properties().InsertVector3(L"Translation", {dx, 0, 0});
            visual.CenterPoint({0, 0, 0});
            visual.Scale({sx, 1, 1});
            held.push_back({row, dx, sx});
        }
        return held;
    }

    static void SettleNeighbors(std::vector<Neighbor> const& held, bool animate) {
        for (auto const& item : held) {
            try {
                auto visual = winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(item.row);
                if (!animate) {
                    visual.Properties().InsertVector3(L"Translation", {0, 0, 0});
                    visual.Scale({1, 1, 1});
                    continue;
                }
                auto compositor = visual.Compositor();
                auto ease = compositor.CreateCubicBezierEasingFunction({0.215f, 0.61f}, {0.355f, 1.f});
                const Windows::Foundation::TimeSpan duration{std::chrono::milliseconds(135)};
                auto scale = compositor.CreateScalarKeyFrameAnimation();
                scale.InsertKeyFrame(1.f, 1.f, ease);
                scale.Duration(duration);
                visual.StartAnimation(L"Scale.X", scale);
                auto shift = compositor.CreateScalarKeyFrameAnimation();
                shift.InsertKeyFrame(1.f, 0.f, ease);
                shift.Duration(duration);
                visual.Properties().StartAnimation(L"Translation.X", shift);
            } catch (...) {}
        }
    }

    // `addBefore` is the New Tab button's X (in root space) before this
    // publication, or NaN. The button eases from there to its new position with
    // the reveal, so it keeps pace with the tab opening beside it.
    void RevealNewRow(Controls::TabViewItem const& row, uint32_t id, double addBefore,
                      std::map<uint32_t, std::pair<float, float>> const& before = {}) {
        EntryLog("WinUIPane entry reveal id=%u enabled=%d dragging=%d slide=%d width=%.1f loaded=%d before=%zu\n",
            id, int(groupSlide.Enabled()), int(IsDragging()), int(bool(groupSlide.storyboard)), row.ActualWidth(), int(row.IsLoaded()), before.size());
        if (!groupSlide.Enabled() || IsDragging() || groupSlide.storyboard) return;
        if (row.ActualWidth() < 1) {
            // Once the strip overflows, the new container is arranged a layout
            // pass after the first frame. Reveal it from its first real size,
            // which is raised after that layout and before it is drawn.
            auto token = std::make_shared<winrt::event_token>();
            *token = row.SizeChanged([this, alive = std::weak_ptr<int>(lifetime), generation = closeGeneration,
                                      token, id, addBefore, before](auto const& sender, auto&&) {
                auto sized = sender.template as<Controls::TabViewItem>();
                EntryLog("WinUIPane entry sized id=%u width=%.1f\n", id, sized.ActualWidth());
                if (sized.ActualWidth() < 1) return;
                sized.SizeChanged(*token);
                if (alive.expired() || generation != closeGeneration) return;
                auto found = rows.find(id);
                if (found == rows.end() || found->second != sized) return;
                try { RevealNewRow(sized, id, addBefore, before); } catch (...) {}
            });
            return;
        }
        const double addDelta = HoldAddButton(addBefore);
        auto neighbors = std::make_shared<std::vector<Neighbor>>(HoldNeighbors(before, id));
        {
            double offset = -1;
            if (auto list = FindTabListView(view))
                if (Media::VisualTreeHelper::GetChildrenCount(list))
                    if (auto border = Media::VisualTreeHelper::GetChild(list, 0).try_as<FrameworkElement>())
                        if (auto scroller = border.FindName(L"ScrollViewer").try_as<Controls::ScrollViewer>()) offset = scroller.HorizontalOffset();
            const auto at = row.TransformToVisual(root).TransformPoint({0, 0});
            EntryLog("WinUIPane entry held id=%u x=%.1f neighbors=%zu addDelta=%.1f scroll=%.1f first=%.1f/%.3f\n", id, at.X, neighbors->size(), addDelta,
                offset, neighbors->empty() ? 0.f : neighbors->front().dx, neighbors->empty() ? 1.f : neighbors->front().sx);
        }
        if (row.IsLoaded()) { StartReveal(row, id); StartAddSlide(addDelta); SettleNeighbors(*neighbors, true); return; }
        // Loaded fires after layout returns, and a storyboard cannot run on an
        // unloaded row. Hold the row invisible until then.
        row.Opacity(0);
        auto token = std::make_shared<winrt::event_token>();
        auto settled = std::make_shared<bool>(false);
        *token = row.Loaded([this, alive = std::weak_ptr<int>(lifetime), id, token, settled, addDelta, neighbors](auto const& sender, auto&&) {
            auto loaded = sender.template as<Controls::TabViewItem>();
            loaded.Loaded(*token);
            if (alive.expired() || *settled) return;
            *settled = true;
            EntryLog("WinUIPane entry loaded id=%u\n", id);
            loaded.Opacity(1);
            auto found = rows.find(id);
            if (found == rows.end() || found->second != loaded || IsDragging() || !groupSlide.Enabled()) {
                StartAddSlide(0);
                SettleNeighbors(*neighbors, false);
                return;
            }
            StartReveal(loaded, id);
            StartAddSlide(addDelta);
            SettleNeighbors(*neighbors, true);
        });
        // Grouped-source rows can be arranged without ever raising Loaded.
        // Never leave such a tab hidden: show it in place without the reveal.
        auto timer = root.DispatcherQueue().CreateTimer();
        timer.IsRepeating(false);
        timer.Interval(std::chrono::milliseconds(250));
        // The handler keeps the timer alive until it fires.
        auto keep = std::make_shared<winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer>(timer);
        timer.Tick([this, alive = std::weak_ptr<int>(lifetime), weakRow = make_weak(row), token, settled, neighbors, keep](auto const& sender, auto&&) {
            sender.Stop();
            *keep = nullptr;
            if (alive.expired() || *settled) return;
            *settled = true;
            EntryLog("WinUIPane entry never loaded\n");
            if (auto row = weakRow.get()) { row.Loaded(*token); row.Opacity(1); }
            StartAddSlide(0);
            SettleNeighbors(*neighbors, false);
        });
        timer.Start();
    }

    // Places the button at its old position at once (its layout slot has
    // already moved) and returns that offset for StartAddSlide to ease out.
    double HoldAddButton(double before) {
        auto add = FindAddButton(view);
        if (std::isnan(before) || !add || !add.IsLoaded()) return 0;
        // A RenderTransform follows bounds space in both flow directions
        // (confirmed by eye); only composition offsets are unmirrored.
        const auto delta = before - add.TransformToVisual(root).TransformPoint({0, 0}).X;
        if (std::abs(delta) < .5) return 0;
        StartAddSlide(0);
        addPrevious = add.RenderTransform();
        addHeld = true;
        Media::TranslateTransform transform;
        transform.X(delta);
        add.RenderTransform(transform);
        return delta;
    }

    void StartAddSlide(double delta) {
        auto add = FindAddButton(view);
        if (delta == 0) {
            // Nothing to animate (or the reveal was cancelled): restore.
            if (addSlide) { try { addSlide.Stop(); } catch (...) {} addSlide = nullptr; }
            if (addHeld && add) add.RenderTransform(addPrevious);
            addHeld = false; addPrevious = nullptr;
            return;
        }
        if (!add || !add.RenderTransform().try_as<Media::TranslateTransform>()) return;
        auto transform = add.RenderTransform().as<Media::TranslateTransform>();
        Media::Animation::DoubleAnimation animation;
        animation.From(delta);
        animation.To(0.0);
        animation.Duration(
            Duration{Windows::Foundation::TimeSpan{std::chrono::milliseconds(135)}, DurationType::TimeSpan});
        Media::Animation::CubicEase ease;
        ease.EasingMode(Media::Animation::EasingMode::EaseOut);
        animation.EasingFunction(ease);
        Media::Animation::Storyboard::SetTarget(animation, transform);
        Media::Animation::Storyboard::SetTargetProperty(animation, L"X");
        Media::Animation::Storyboard storyboard;
        storyboard.Children().Append(animation);
        storyboard.Completed([this, alive = std::weak_ptr<int>(lifetime)](auto const& sender, auto const&) {
            if (alive.expired() || addSlide != sender) return;
            addSlide = nullptr;
            if (auto add = FindAddButton(view); add && addHeld) add.RenderTransform(addPrevious);
            addHeld = false; addPrevious = nullptr;
        });
        addSlide = storyboard;
        storyboard.Begin();
    }

    void StartReveal(Controls::TabViewItem const& row, uint32_t id) {
        if (rowAnimations.count(id)) return;
        const auto width = row.ActualWidth();
        // Local geometry is unmirrored: x = 0 is the edge shared with the
        // previous tab in both flow directions.
        Media::ScaleTransform scale;
        scale.CenterX(0);
        scale.ScaleX(1);
        Media::RectangleGeometry clip;
        clip.Rect({0, 0, float(width), float(row.ActualHeight())});
        clip.Transform(scale);
        row.Clip(clip);
        Media::Animation::DoubleAnimation animation;
        animation.From(0.0);
        animation.To(1.0);
        animation.Duration(
            Duration{Windows::Foundation::TimeSpan{std::chrono::milliseconds(135)}, DurationType::TimeSpan});
        Media::Animation::CubicEase ease;
        ease.EasingMode(Media::Animation::EasingMode::EaseOut);
        animation.EasingFunction(ease);
        Media::Animation::Storyboard::SetTarget(animation, scale);
        Media::Animation::Storyboard::SetTargetProperty(animation, L"ScaleX");
        Media::Animation::Storyboard storyboard;
        storyboard.Children().Append(animation);
        storyboard.Completed([this, alive = std::weak_ptr<int>(lifetime), id, weakRow = make_weak(row)](auto const& sender, auto const&) {
            if (alive.expired()) return;
            auto current = rowAnimations.find(id);
            if (current == rowAnimations.end() || current->second.storyboard != sender) return;
            if (auto row = weakRow.get()) row.Clip(nullptr);
            rowAnimations.erase(current);
        });
        rowAnimations[id] = RowAnimation{storyboard, 0};
        storyboard.Begin();
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
        row.Clip(nullptr);
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
            if (shell_core_tab_drag_join_hit(core, point.X - drag.shift, 0, false, &hitGroup, &hitTab, &hitBefore) == SHELL_STATUS_OK && hitGroup) {
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
                if (shell_core_tab_drag_join_hit(core, point.X - drag.shift, id, run.collapsed, &hitGroup, &hitTab, &hitBefore) == SHELL_STATUS_OK && hitGroup) {
                    if (!join(hitGroup, hitTab, hitBefore)) return false;
                    // A collapsed group has no insertion gap to show, so
                    // outline its header button as the drop target.
                    if (run.collapsed && groupIndicator) {
                        const auto at = run.button.TransformToVisual(root).TransformPoint({0, 0});
                        groupIndicator.Margin({at.X - 3, at.Y, 0, 0});
                        groupIndicator.Width(run.button.ActualWidth() + 6);
                        groupIndicator.Height(run.button.ActualHeight());
                        groupIndicator.Visibility(Visibility::Visible);
                    }
                    return true;
                }
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
                if (point.X - drag.shift < left + width * .3 || point.X - drag.shift > left + width * .7) continue;
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

    double DragContentX() {
        auto scroller = GroupScroller(view);
        auto content = scroller ? scroller.Content().try_as<UIElement>() : nullptr;
        return content ? content.TransformToVisual(view).TransformPoint({0, 0}).X : 0.0;
    }

    // Scrolls the strip while a drag holds the pointer near either edge, so a
    // tab can be dropped among rows that overflow the visible area. Mirrors
    // TickGroupDrag's edge scroll; every drag tick lands here, so a pointer
    // held still at the edge keeps scrolling. Returns whether the strip is
    // scrolling (or only just stopped): rows stream past a still pointer then,
    // so new-group hover must not hold the projection frozen on whichever tab
    // is under it.
    bool EdgeScrollDrag(Windows::Foundation::Point const& point) {
        if (Drag().phase != DragPhase::Dragging || Drag().detaching || point.Y < 0 || point.Y > view.ActualHeight() ||
            point.X < 0 || point.X > view.ActualWidth()) return false;
        const bool recent = GetTickCount64() - drag.scrollTick < 120;
        auto scroller = GroupScroller(view);
        if (!scroller || scroller.ScrollableWidth() <= 0) return false;
        const auto viewport = scroller.TransformToVisual(view).TransformBounds({0, 0, float(scroller.ActualWidth()), float(scroller.ActualHeight())});
        const bool left = point.X < viewport.X + 24, right = point.X > viewport.X + viewport.Width - 24;
        if (left == right) return recent;
        // Content must move away from the pointer's edge. The offset grows as
        // content moves left in LTR and right in RTL.
        const double step = (right ? 12 : -12) * (view.FlowDirection() == FlowDirection::RightToLeft ? -1 : 1);
        const auto target = std::clamp(scroller.HorizontalOffset() + step, 0.0, scroller.ScrollableWidth());
        if (target == scroller.HorizontalOffset()) return recent;
        if (GetTickCount64() - drag.scrollTick < 16) return true;
        drag.scrollTick = GetTickCount64();
        inlineGroups.NoteUserScroll();
        scroller.ChangeView(box_value(target).as<Windows::Foundation::IReference<double>>(), nullptr, nullptr, true);
        scroller.UpdateLayout();
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
        lastDragPoint = point;
        const auto previous = Drag();
        const auto previousJoin = joinGroup;
        const bool scrolling = EdgeScrollDrag(point);
        // Rows were measured when the drag began; scrolling since moves them
        // by `shift`. Give ShellCore the pointer and visible width in that
        // frame, but keep tear-off decided by the live pointer.
        drag.shift = Drag().phase == DragPhase::Dragging ? DragContentX() - drag.contentOrigin : 0.0;
        const double width = view.ActualWidth(), height = view.ActualHeight();
        const bool outside = point.X < -height || point.X > width + height;
        const double coreX = outside ? point.X : std::clamp(point.X - drag.shift, -height, width - drag.shift + height);
        const double coreWidth = outside ? width : width - drag.shift;
        if (shell_core_tab_drag_update(core, coreX, point.Y, coreWidth, height, false) != SHELL_STATUS_OK) return;
        if (Drag().detaching) {
            ClearGroupHover();
        } else {
            if (scrolling) ClearGroupHover();
            if (allowGrouping && !scrolling && UpdateGroupHover(point)) {
                if (!joinGroup) {
                    if (!previousJoin) return;
                    shell_core_tab_drag_close_gap(core);
                }
            } else if (shell_core_tab_drag_update(core, coreX, point.Y, coreWidth, height, true) != SHELL_STATUS_OK) return;
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
    bool TryPromote(uint32_t id, Windows::Foundation::Point const& point) {
        inlineGroups.hover.Hide();
        if (Drag().tab != id || Drag().phase != DragPhase::Armed || !rows.count(id)) return false;
        bool ready = false;
        if (shell_core_tab_drag_ready(core, id, point.X, point.Y, &ready) != SHELL_STATUS_OK || !ready) return false;
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
        if (ambient.layer)
            dragSlot = row.TransformToVisual(ambient.layer).TransformBounds({0, 0, float(row.ActualWidth()), float(row.ActualHeight())});
        std::vector<ShellDragRow> measured;
        drag.originalLeftEdge.clear();
        drag.originalWidth.clear();
        for (uint32_t i = 0; i < view.TabItems().Size(); ++i) {
            auto item = view.TabItems().GetAt(i).as<Controls::TabViewItem>();
            drag.originalLeftEdge.push_back(item.TransformToVisual(view).TransformPoint({0, 0}).X);
            drag.originalWidth.push_back(item.ActualWidth());
            measured.push_back({drag.originalLeftEdge.back(), drag.originalWidth.back()});
        }
        drag.contentOrigin = DragContentX();
        drag.shift = 0;
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
    // Firefox's own drop unpins a pinned tab released over the unpinned tabs.
    // It decides by the drop target, not the index, so the one gap between
    // the last pinned and first unpinned tab goes by which side of the first
    // unpinned row (or its group's header) the pointer is released on.
    bool DropUnpins(uint32_t tabId, Windows::Foundation::Point const& point) {
        if (!presentation.count(tabId) || !(presentation.at(tabId).flags & 1)) return false;
        auto past = [&](FrameworkElement const& element) {
            return point.X >= element.TransformToVisual(view).TransformPoint({0, 0}).X;
        };
        auto unpinned = [&](uint32_t id) {
            auto row = rows.find(id);
            return id != tabId && row != rows.end() && row->second.Visibility() == Visibility::Visible &&
                presentation.count(id) && !(presentation.at(id).flags & 1) ? row->second : Controls::TabViewItem{nullptr};
        };
        if (hasGroupLayout) {
            for (auto slot : groupLayout) {
                if (!slot.tab) {
                    auto run = inlineGroups.runs.find(slot.group);
                    if (run != inlineGroups.runs.end() && run->second.button && run->second.button.IsLoaded())
                        return past(run->second.button);
                } else if (auto row = unpinned(slot.tab)) return past(row);
            }
            return false;
        }
        for (auto const& item : view.TabItems())
            if (auto row = unpinned(unbox_value<uint32_t>(item.as<Controls::TabViewItem>().Tag()))) return past(row);
        return false;
    }

    bool CommitReorder(uint32_t tabId, Windows::Foundation::Point const& point) {
        if (Drag().tab != tabId || Drag().phase != DragPhase::Dragging || !rows.count(tabId)) return false;
        UpdateDragVisual(point);
        if (joinGroup) {
            using Value = Windows::Data::Json::JsonValue;
            if (core) shell_core_group_surface_join(core, joinGroup, tabId, SteadyMs() + 1500);
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
        const bool unpin = DropUnpins(tabId, point);
        // Reveal the row at its destination during the browser round trip.
        ResetRowOffset(row, tabId);
        RowTransform(row).X(Drag().landing_offset);
        const auto request = CompleteReorderAt(row, beforeRow, ungroup, unpin);
        if (shell_core_tab_drag_commit(core, request, beforeId) != SHELL_STATUS_OK)
            shell_core_tab_drag_end(core, true);
        else if (request && dropCommitted) dropCommitted();
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

    // One published row as Firefox wrote it. Reading checks JSON types only;
    // ShellCore validates the values and decides which of these strings the
    // row takes. The strings stay here: ShellCore sees only their lengths and
    // the hex pixels it checks.
    struct StagedRow {
        hstring detail, status, containerName, imagePixels;
        ShellStripRowInput raw{};
        explicit StagedRow(Windows::Data::Json::JsonObject const& entry) {
            auto number = [&](wchar_t const* key) {
                return entry.HasKey(key) ? ShellStripNumber{true, entry.GetNamedNumber(key)} : ShellStripNumber{};
            };
            auto flag = [&](wchar_t const* key) {
                return entry.HasKey(key) ? ShellStripFlag{true, entry.GetNamedBoolean(key)} : ShellStripFlag{};
            };
            auto text = [&](wchar_t const* key, hstring& value) {
                if (!entry.HasKey(key)) return ShellStripText{};
                value = entry.GetNamedString(key);
                return ShellStripText{true, value.size()};
            };
            raw.id = entry.GetNamedNumber(L"id");
            raw.flags = number(L"flags");
            raw.container_color = number(L"containerColor");
            raw.group_source = flag(L"groupSource");
            raw.group_target = flag(L"groupTarget");
            raw.detail = text(L"detail", detail);
            raw.status = text(L"status", status);
            raw.container_name = text(L"containerName", containerName);
            raw.image.kind = SHELL_STRIP_IMAGE_KIND_ABSENT;
            if (entry.HasKey(L"image")) {
                if (entry.GetNamedValue(L"image").ValueType() == Windows::Data::Json::JsonValueType::Null) {
                    raw.image.kind = SHELL_STRIP_IMAGE_KIND_NULL;
                } else {
                    auto image = entry.GetNamedObject(L"image");
                    raw.image.kind = SHELL_STRIP_IMAGE_KIND_SET;
                    raw.image.width = image.GetNamedNumber(L"width");
                    raw.image.height = image.GetNamedNumber(L"height");
                    imagePixels = image.GetNamedString(L"pixels");
                }
            }
        }
        // Points into imagePixels, so it is taken once the rows stop moving.
        ShellStripRowInput Input() const {
            auto input = raw;
            if (input.image.kind == SHELL_STRIP_IMAGE_KIND_SET) {
                input.image.pixels = reinterpret_cast<uint16_t const*>(imagePixels.c_str());
                input.image.len = imagePixels.size();
            }
            return input;
        }
    };

    // The publication envelope as Firefox wrote it. Reading checks JSON types
    // only; ShellCore decides version, base/revision, replay identity, ranges,
    // names and which omitted options a delta keeps. `raw` points into the
    // names, so the struct is neither copied nor moved.
    struct StripEnvelope {
        ShellStripEnvelope raw{};
        hstring backdrop, groupReach;
        explicit StripEnvelope(NativeTabMenu::Object const& data) {
            auto number = [&](wchar_t const* key) {
                return data.HasKey(key) ? ShellStripNumber{true, data.GetNamedNumber(key)} : ShellStripNumber{};
            };
            auto flag = [&](wchar_t const* key) {
                return data.HasKey(key) ? ShellStripFlag{true, data.GetNamedBoolean(key)} : ShellStripFlag{};
            };
            auto name = [&](wchar_t const* key, hstring& value) {
                if (!data.HasKey(key)) return ShellStripName{};
                value = data.GetNamedString(key);
                return ShellStripName{true, reinterpret_cast<uint16_t const*>(value.c_str()), value.size()};
            };
            raw.version = data.GetNamedNumber(L"version");
            raw.revision = data.GetNamedNumber(L"revision");
            raw.base = number(L"base");
            raw.window_drag_session = number(L"windowDragSession");
            raw.grouping_delay = number(L"groupingDelay");
            raw.backdrop = name(L"backdrop", backdrop);
            raw.group_reach = name(L"groupReach", groupReach);
            raw.window_drag_private = flag(L"windowDragPrivate");
            raw.group_glow = flag(L"groupGlow");
            raw.container_glow = flag(L"containerGlow");
            raw.container_dot = flag(L"containerDot");
            raw.group_hover = flag(L"groupHover");
            raw.group_halo = flag(L"groupHalo");
            raw.group_glow_animations = flag(L"groupGlowAnimations");
            raw.group_glow_fade_out = flag(L"groupGlowFadeOut");
            raw.rtl = flag(L"rtl");
        }
        StripEnvelope(StripEnvelope const&) = delete;
        StripEnvelope& operator=(StripEnvelope const&) = delete;
    };
    static Backdrop BackdropOf(ShellBackdrop value) {
        switch (value) {
        case SHELL_BACKDROP_MICA: return Backdrop::Mica;
        case SHELL_BACKDROP_ACRYLIC: return Backdrop::Acrylic;
        default: return Backdrop::MicaAlt;
        }
    }

    // Parse a complete bounded copy before changing XAML or accepting any IDs.
    void Publish(NativeTabMenu::Object const& data) {
        const StripEnvelope envelope{data};
        const auto fingerprint = data.Stringify();
        auto fingerprintData = reinterpret_cast<uint16_t const*>(fingerprint.c_str());
        ShellStripOptions resolved{};
        bool unchanged{};
        if (!core || shell_core_strip_resolve(core, &envelope.raw, fingerprintData, fingerprint.size(),
                                              &resolved, &unchanged) != SHELL_STATUS_OK)
            throw hresult_invalid_argument();
        if (unchanged) return;
        const auto base = resolved.base;
        const auto revision = resolved.revision;
        const bool updateGroups = !base || data.HasKey(L"groups") || data.HasKey(L"groupsLabel");
        auto label = base ? data.GetNamedString(L"groupsLabel", groupsLabel) : data.GetNamedString(L"groupsLabel");
        auto entries = data.GetNamedArray(L"rows");
        auto groups = base && !data.HasKey(L"groups") ? currentGroups : data.GetNamedArray(L"groups");
        if (label.empty() || label.size() > NativeJson::kMaxShortLabel || groups.Size() > NativeJson::kMaxTabs)
            throw hresult_invalid_argument();
        // Parsed with the rest of the envelope so a bad label changes nothing.
        const bool labelsPublished = data.HasKey(L"labels");
        const Labels nextLabels = labelsPublished ? ParseLabels(data.GetNamedObject(L"labels"), labels) : labels;
        // Removals and rows as Firefox wrote them. ShellCore validates them with
        // the envelope and decides which retained fields each row keeps.
        std::vector<double> removals;
        if (base) for (auto value : data.GetNamedArray(L"removed")) removals.push_back(value.GetNumber());
        std::vector<StagedRow> staged;
        staged.reserve(entries.Size());
        for (auto value : entries) staged.emplace_back(value.GetObject());
        std::vector<ShellStripRowInput> inputs;
        inputs.reserve(staged.size());
        for (auto const& row : staged) inputs.push_back(row.Input());
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
        // The Rust core owns the envelope, revision/identity, row validation and
        // retention, and aggregate retained-image bounds, and accepts them
        // together with the staged layout. Everything above is staged before
        // either model or XAML changes.
        const ShellStripPublication publication{inputs.data(), inputs.size(), removals.data(), removals.size(),
                                                layout.data(), layout.size(), layoutChanged};
        ShellStripOptions options{};
        std::vector<ShellStripRow> accepted(inputs.size());
        std::vector<uint32_t> removed(removals.size());
        if (shell_core_strip_publish(core, &envelope.raw, fingerprintData, fingerprint.size(), &publication, &options,
                                     accepted.data(), removed.data()) != SHELL_STATUS_OK)
            throw hresult_invalid_argument();
        // Project the accepted rows: ShellCore decided which fields each takes,
        // keeps from the retained presentation, or leaves empty.
        std::map<uint32_t, Presentation> next;
        for (size_t i = 0; i < accepted.size(); ++i) {
            auto const& row = accepted[i];
            auto const& source = staged[i];
            const auto old = presentation.find(row.id);
            auto pick = [&](ShellStripSource from, hstring const& published, hstring Presentation::*retained) {
                if (from == SHELL_STRIP_SOURCE_TAKE) return published;
                return from == SHELL_STRIP_SOURCE_KEEP && old != presentation.end() ? old->second.*retained : hstring{};
            };
            next.insert_or_assign(row.id, Presentation{
                pick(row.detail, source.detail, &Presentation::detail), row.flags,
                pick(row.status, source.status, &Presentation::status),
                pick(row.image, source.imagePixels, &Presentation::imagePixels), row.image_width, row.image_height,
                pick(row.container_name, source.containerName, &Presentation::containerName), row.container_color,
                row.group_source, row.group_target});
        }
        if (GroupDrag().phase && (layoutChanged || !removed.empty() || !base)) EndGroupDrag();
        if (layoutChanged || !base) {
            EndForeignGroupOver(true);
            bool left=false;
            if (shell_core_group_departure_publish(core,&left)==SHELL_STATUS_OK && left) ReleaseGroupDeparture();
        }
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
            root.FlowDirection(options.rtl ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
        if (!base) presentation.swap(next);
        else {
            for (auto id : removed) presentation.erase(id);
            for (auto const& [id, value] : next) presentation.insert_or_assign(id, value);
        }
        // A gesture formed against an older presentation is not revoked here:
        // it is already queued, carrying the revision it was formed against,
        // and Firefox refuses it explicitly on that revision.
        if (groupingDelay != options.grouping_delay || layoutChanged || !removed.empty()) ClearGroupHover();
        groupingDelay = options.grouping_delay;
        if (groupCandidate && (!presentation.count(groupCandidate) || !presentation.at(groupCandidate).groupTarget ||
            !presentation.count(Drag().tab) || !presentation.at(Drag().tab).groupSource)) ClearGroupHover();
        windowDragSession = options.window_drag_session;
        windowDragPrivate = options.window_drag_private;
        captionDirty = true;
        presentationRevision = revision;
        if (labelsPublished) ApplyLabels(nextLabels);
        if (layoutChanged) {
            size_t size = layout.size();
            if (shell_core_strip_layout(core, layout.data(), layout.size(), &size) != SHELL_STATUS_OK)
                throw hresult_invalid_argument();
            groupLayout = std::move(layout);
            hasGroupLayout = true;
            // Reorders within the strip arrive here rather than through Sync.
            hoverCard.Moved();
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
        const bool dotChanged = containerDot != options.container_dot;
        containerDot = options.container_dot;
        containerGlow = options.container_glow;
        groupGlow = options.group_glow;
        if (groupHover != options.group_hover) { groupHover = options.group_hover; inlineGroups.WashesGroup(groupHover); if (!groupHover) groupSurface.Hover(0); }
        groupHalo = options.group_halo;
        ambient.motion.option = options.group_glow_animations;
        ambient.motion.fadeOut = options.group_glow_fade_out;
        // A full snapshot (window open, session restore) shows its groups
        // already lit; only later changes light up.
        if (!base) glowSeed = true;
        if (groupReach != options.group_reach) { groupReach = options.group_reach; ApplyAddSpacing(); }
        if (const auto nextBackdrop = BackdropOf(options.backdrop); nextBackdrop != backdrop) {
            backdrop = nextBackdrop;
            if (applyBackdrop) applyBackdrop(backdrop);
        }
        if (dotChanged) {
            for (auto const& [id, row] : rows)
                UpdateHeader(row, id, Automation::AutomationProperties::GetName(row));
        }
        // Metadata has its own publication now; do not wait for a title delta
        // or a structural Sync to make a changed icon/status visible.
        for (auto const& change : accepted) {
            auto row = rows.find(change.id);
            if (row == rows.end()) continue;
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
    // Multiselection outline: a layout-neutral overlay inside the row's template
    // root. A real BorderThickness on the row changes its measured size, so the
    // hover-only close button grew the row and shifted the whole strip. The
    // brush is bound to groupIndicator's live ThemeResource border brush.
    std::set<uint32_t> outlinedTabs;
    std::map<uint32_t, Controls::Border> outlines;
    void AttachOutline(uint32_t id, Controls::TabViewItem const& row) {
        const bool want = outlinedTabs.count(id) != 0;
        auto existing = outlines.find(id);
        if (!want && existing == outlines.end()) return;
        if (!Media::VisualTreeHelper::GetChildrenCount(row)) row.ApplyTemplate();
        if (!Media::VisualTreeHelper::GetChildrenCount(row)) return;
        auto root = Media::VisualTreeHelper::GetChild(row, 0).try_as<FrameworkElement>();
        if (!root) return;
        auto container = root.FindName(L"TabContainer").try_as<Controls::Grid>();
        if (!container) container = root.try_as<Controls::Grid>();
        if (!container) return;
        if (existing != outlines.end() && !want) {
            uint32_t at{};
            if (container.Children().IndexOf(existing->second, at)) container.Children().RemoveAt(at);
            outlines.erase(existing);
            return;
        }
        Controls::Border border{nullptr};
        if (existing == outlines.end()) {
            border = Markup::XamlReader::Load(LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" BorderThickness="2" CornerRadius="4" IsHitTestVisible="False" HorizontalAlignment="Stretch" VerticalAlignment="Stretch" AutomationProperties.AccessibilityView="Raw"/>)").as<Controls::Border>();
            Data::Binding binding;
            binding.Source(groupIndicator); binding.Path(PropertyPath(L"BorderBrush"));
            border.SetBinding(Controls::Border::BorderBrushProperty(), binding);
            outlines.emplace(id, border);
        } else border = existing->second;
        // A Grid lays children out inside its padding; extend over it so the
        // outline meets the tab's own edges.
        // The template changes that padding with the close button's state, so
        // follow it rather than reading it once.
        auto fit = [container, border] {
            const auto padding = container.Padding();
            border.Margin({-padding.Left, -padding.Top, -padding.Right, -padding.Bottom});
        };
        fit();
        uint32_t at{};
        if (container.Children().IndexOf(border, at)) return;
        container.RegisterPropertyChangedCallback(Controls::Grid::PaddingProperty(),
            [fit](auto const&, auto const&) { fit(); });
        Controls::Grid::SetRowSpan(border, 16);
        Controls::Grid::SetColumnSpan(border, 16);
        Controls::Canvas::SetZIndex(border, 100);
        container.Children().Append(border);
    }
    void SetSelectedOutline(Controls::TabViewItem const& row, bool selected) {
        const auto id = unbox_value<uint32_t>(row.Tag());
        if (selected) outlinedTabs.insert(id); else outlinedTabs.erase(id);
        AttachOutline(id, row);
    }
    void ApplySelectedSet(std::set<uint32_t> const& previous, std::set<uint32_t> const& current) {
        if (IsDragging()) return;
        auto emphasize = [&](uint32_t id) {
            auto row = rows.find(id);
            if (row == rows.end()) return;
            bool selected = current.count(id) != 0;
            row->second.FontWeight(Windows::UI::Text::FontWeight{uint16_t(selected ? 600 : 400)});
            // Firefox's selection always contains the active tab; outline only
            // a real multiselection.
            SetSelectedOutline(row->second, selected && current.size() > 1);
            Automation::AutomationProperties::SetItemStatus(row->second, StatusFor(id == activeTabId, selected));
        };
        // The active tab's outline depends on the set's size, so revisit
        // every member of either set, not only the ones that changed.
        for (auto id : previous) emphasize(id);
        for (auto id : current)
            if (!previous.count(id)) emphasize(id);
    }
    // Firefox's active tab stays TabView's only selected item. Modified clicks
    // only change Firefox's multiselection, never this.
    void RestoreActiveSelection() {
        if (IsDragging()) return;
        const auto active = rows.find(activeTabId);
        syncing = true;
        if (active != rows.end() && view.SelectedItem() != active->second)
            view.SelectedItem(active->second);
        for (auto const& [id, row] : rows)
            if (id != activeTabId && row.IsSelected()) row.IsSelected(false);
        syncing = false;
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
            Automation::AutomationProperties::SetItemStatus(row->second, StatusFor(id == active, selected));
        }
    }

    bool IsPinnedRow(Controls::TabViewItem const& row) const {
        const auto id = unbox_value_or<uint32_t>(row.Tag(), 0);
        auto found = presentation.find(id);
        return found != presentation.end() && (found->second.flags & 1);
    }

    // Like Firefox's own pinned tabs, a pinned row shows only its icons: no
    // title (still its UIA name and hover card) and no close button. Its
    // width follows those icons instead of TabView's Equal width.
    double pinnedChrome = 32;
    void ApplyPinnedFace(Controls::TabViewItem const& row, Controls::Grid const& header, bool pinned) {
        auto text = header.Children().GetAt(2).as<FrameworkElement>();
        const bool wasPinned = text.Visibility() == Visibility::Collapsed;
        if (pinned != wasPinned) {
            text.Visibility(pinned ? Visibility::Collapsed : Visibility::Visible);
            // Grid spacing applies between empty columns too; space only
            // the shown icons.
            header.ColumnSpacing(pinned ? 0 : 6);
            if (pinned && row.IsClosable()) row.IsClosable(false);
        }
        if (!pinned) {
            if (wasPinned) {
                for (uint32_t i : {1u, 3u, 4u}) header.Children().GetAt(i).as<FrameworkElement>().Margin({});
                row.ClearValue(FrameworkElement::MinWidthProperty());
                row.ClearValue(FrameworkElement::MaxWidthProperty());
            }
            return;
        }
        double width = 0;
        for (uint32_t i : {0u, 1u, 3u, 4u}) {
            auto icon = header.Children().GetAt(i).as<FrameworkElement>();
            const bool shown = icon.Visibility() == Visibility::Visible;
            icon.Margin({shown && width > 0 ? 6.0 : 0.0, 0, 0, 0});
            if (shown) width += icon.Margin().Left + icon.Width();
        }
        // Add the template's own space around the header (container padding
        // and border, icon slot), measured from a laid-out row because it is
        // more than the stock header padding: sizing to that padding alone
        // clipped the icon. Until a row is laid out, assume generous room.
        if (row.ActualWidth() > 0 && header.ActualWidth() > 0)
            pinnedChrome = std::max(0.0, row.ActualWidth() - header.ActualWidth());
        width += pinnedChrome;
        if (std::abs(row.MinWidth() - width) > .01) row.MinWidth(width);
        if (std::abs(row.MaxWidth() - width) > .01) row.MaxWidth(width);
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
        // Fallback/audio are SymbolIcon glyphs at a fixed intrinsic size that
        // Width/Height alone would only clip, not scale; a Viewbox scales
        // the glyph down to fit the 16x16 box instead.
        auto fallbackBox = header.Children().GetAt(1).as<Controls::Viewbox>();
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
        // A pinned tab is identified by its favicon alone, like Firefox's;
        // one without a favicon shows a generic page glyph in its place, as
        // Firefox shows its default favicon. Pinned state is in ItemStatus.
        fallbackBox.Visibility((meta.flags & 1) && !isBusy && !hasImage ? Visibility::Visible : Visibility::Collapsed);
        audio.Symbol(meta.flags & 4 ? Controls::Symbol::Mute : Controls::Symbol::Volume);
        audioBox.Visibility(meta.flags & 12 ? Visibility::Visible : Visibility::Collapsed);
        ApplyPinnedFace(row, header, (meta.flags & 1) != 0);
        hoverCard.Update(row, id, title, meta.containerName +
            (meta.containerName.empty() || meta.detail.empty() ? L"" : L" · ") + meta.detail, lifetime);
        Automation::AutomationProperties::SetHelpText(
            row, meta.detail + (meta.detail.empty() || meta.status.empty() ? L"" : L" \u00b7 ") + meta.status);
        if (!meta.containerName.empty()) Automation::AutomationProperties::SetHelpText(
            row, meta.containerName + L" " + meta.detail + L" " + meta.status);
    }

    // Tabs and inline group headers in visual order. The grouped source hosts
    // tabs in a ListView whose stock arrow handling does not cross them, so the
    // strip navigates itself.
    std::vector<Controls::Control> FocusOrder() const {
        std::vector<Controls::Control> order;
        if (inlineGroups.active) {
            for (auto id : inlineGroups.order) {
                auto found = inlineGroups.runs.find(id);
                if (found == inlineGroups.runs.end()) continue;
                auto const& run = found->second;
                if (run.button && run.button.IsLoaded()) order.push_back(run.button);
                for (auto item : run.items) order.push_back(item.as<Controls::Control>());
            }
        } else {
            for (auto item : view.TabItems())
                if (auto row = item.try_as<Controls::TabViewItem>()) order.push_back(row);
        }
        return order;
    }

    // Firefox's tab strip keys: Left/Right select the adjacent tab, wrapping at
    // the ends, and only move focus with Ctrl; Home/End go to the first and
    // last item. Shift and Alt combinations stay Firefox's (tab moving).
    bool Navigate(Windows::Foundation::IInspectable const& source, Windows::System::VirtualKey key) {
        using Key = Windows::System::VirtualKey;
        if (GetKeyState(VK_SHIFT) & 0x8000 || GetKeyState(VK_MENU) & 0x8000) return false;
        const bool focusOnly = GetKeyState(VK_CONTROL) & 0x8000;
        auto order = FocusOrder();
        auto current = std::find(order.begin(), order.end(), source.try_as<Controls::Control>());
        if (current == order.end()) return false;
        const auto count = order.size();
        size_t index = current - order.begin();
        const bool rtl = view.FlowDirection() == FlowDirection::RightToLeft;
        if (key == Key::Home) index = 0;
        else if (key == Key::End) index = count - 1;
        else if ((key == Key::Right) != rtl) index = (index + 1) % count;
        else index = (index + count - 1) % count;
        auto target = order[index];
        if (auto row = target.try_as<Controls::TabViewItem>(); row && !focusOnly)
            select(unbox_value<uint32_t>(row.Tag()));
        target.Focus(FocusState::Keyboard);
        return true;
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
        std::vector<uint32_t> createdRows;
        const size_t priorRowCount = rows.size();
        {
            std::vector<uint32_t> order(count);
            for (uint32_t i = 0; i < count; ++i) order[i] = tabs[i].id;
            if (order != publishedOrder) {
                hoverCard.Moved();
                publishedOrder = std::move(order);
            }
        }
        ClosingTab closing;
        double addBefore = std::numeric_limits<double>::quiet_NaN();
        std::map<uint32_t, std::pair<float, float>> rowsBefore;
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
                    if (dropLanded) dropLanded();
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
            std::set<uint32_t> current;
            const bool populated = !rows.empty();
            if (populated && count != rows.size()) {
                if (auto add = FindAddButton(view); add && add.IsLoaded())
                    addBefore = add.TransformToVisual(root).TransformPoint({0, 0}).X;
            }
            if (populated && count + 1 == rows.size()) {
                std::set<uint32_t> incoming;
                for (uint32_t i = 0; i < count; ++i) incoming.insert(tabs[i].id);
                for (auto const& [rowId, row] : rows) {
                    if (incoming.count(rowId) || !row.IsLoaded()) continue;
                    closing.valid = true;
                    closing.bounds = row.TransformToVisual(root).TransformBounds(
                        {0, 0, float(row.ActualWidth()), float(row.ActualHeight())});
                    closing.title = Automation::AutomationProperties::GetName(row);
                    closing.selected = row.IsSelected();
                    if (auto header = row.Header().try_as<Controls::Grid>())
                        if (auto favicon = header.Children().GetAt(0).try_as<Controls::Grid>())
                            if (auto image = favicon.Children().GetAt(0).try_as<Controls::Image>())
                                closing.image = image.Source();
                }
            }
            if (populated && count != rows.size()) {
                for (auto const& [rowId, row] : rows) {
                    if ((!row.IsLoaded() && row.ActualWidth() <= 0) || rowsBefore.size() >= 128) continue;
                    const auto bounds = row.TransformToVisual(root).TransformBounds(
                        {0, 0, float(row.ActualWidth()), float(row.ActualHeight())});
                    rowsBefore.emplace(rowId, std::make_pair(bounds.X, bounds.Width));
                }
            }
            for (uint32_t i = 0; i < count; ++i) {
                const auto id = tabs[i].id;
                current.insert(id);
                if (!rows.count(id)) {
                    // A lone new tab after the first slot: not startup, a
                    // restored batch, or a tab adopted from another window.
                    if (populated && i > 0) createdRows.push_back(id);
                    Controls::TabViewItem row;
                    row.Loaded([this, alive = std::weak_ptr<int>(lifetime)](auto const& sender, auto&&) {
                        // Grouped-source moves can load rows after the final
                        // layout notification. Refresh once realization settles.
                        if (alive.expired()) return;
                        inlineGroups.Realized(sender.template as<FrameworkElement>());
                        ambient.ScheduleUpdate();
                    });
                    row.Tag(box_value(id));
                    if (populated && inlineGroups.active) {
                        // The width caps reach a new tab only after its first
                        // layout, which could measure it wider than the others
                        // and briefly grow (and scroll) an overflowing strip.
                        // Start it at an existing tab's cap; with room to spare
                        // that cap is already at or above its Equal width.
                        for (auto const& [otherId, other] : rows) {
                            const auto cap = other.MaxWidth();
                            if (other.ActualWidth() > 0 && std::isfinite(cap) && cap > 0) { row.MaxWidth(cap); break; }
                        }
                    }
                    // The stock close button is shown only while the tab is
                    // hovered or holds keyboard focus, so narrow tabs keep the
                    // room for their title. TabViewItem collapses (and stops
                    // reserving space for) the button while IsClosable is false.
                    row.IsClosable(false);
                    {
                        struct Reveal { bool hover = false; bool focus = false; };
                        auto state = std::make_shared<Reveal>();
                        auto apply = [this, alive = std::weak_ptr<int>(lifetime), state](auto const& sender) {
                            auto item = sender.template as<Controls::TabViewItem>();
                            const bool reveal = (state->hover || state->focus) &&
                                !alive.expired() && !IsPinnedRow(item);
                            if (item.IsClosable() != reveal) item.IsClosable(reveal);
                        };
                        row.PointerEntered([state, apply](auto const& sender, auto&&) {
                            state->hover = true; apply(sender);
                        });
                        row.PointerExited([state, apply](auto const& sender, auto&&) {
                            state->hover = false; apply(sender);
                        });
                        row.GotFocus([state, apply](auto const& sender, RoutedEventArgs const&) {
                            // Pointer focus from a click must not pin the button.
                            state->focus = sender.template as<Controls::Control>().FocusState() ==
                                FocusState::Keyboard;
                            apply(sender);
                        });
                        row.LostFocus([state, apply](auto const& sender, auto&&) {
                            state->focus = false; apply(sender);
                        });
                    }
                    row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
                    row.RenderTransform(Media::TranslateTransform{});
                    row.Header(Markup::XamlReader::Load(
                        LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" ColumnSpacing="6"><Grid.ColumnDefinitions><ColumnDefinition Width="Auto"/><ColumnDefinition Width="Auto"/><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions><Grid Width="16" Height="16"><Image Stretch="Uniform" AutomationProperties.AccessibilityView="Raw"/><ProgressRing Width="16" Height="16" IsActive="False" AutomationProperties.AccessibilityView="Raw"/></Grid><Viewbox Grid.Column="1" Width="16" Height="16" AutomationProperties.AccessibilityView="Raw"><SymbolIcon Symbol="Globe"/></Viewbox><StackPanel Grid.Column="2" MaxWidth="220" VerticalAlignment="Center"><TextBlock TextTrimming="CharacterEllipsis"/></StackPanel><Viewbox Grid.Column="3" Width="16" Height="16"><SymbolIcon Symbol="Volume"/></Viewbox><Border Grid.Column="4" Width="8" Height="8" CornerRadius="4" VerticalAlignment="Center" Visibility="Collapsed" Background="{ThemeResource TextFillColorPrimaryBrush}" AutomationProperties.AccessibilityView="Raw"/></Grid>)"));
                    Automation::AutomationProperties::SetAutomationId(row, L"FennTab-" + to_hstring(id));
                    // A pinned row's width depends on its own laid-out chrome.
                    row.SizeChanged([this, alive = std::weak_ptr<int>(lifetime)](auto const& sender, auto&&) {
                        if (alive.expired()) return;
                        auto item = sender.template as<Controls::TabViewItem>();
                        if (!IsPinnedRow(item)) return;
                        if (auto header = item.Header().template try_as<Controls::Grid>())
                            ApplyPinnedFace(item, header, true);
                    });
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
                                StartTabDragTimer();
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
                    row.Loaded([this, alive, id](auto const&, auto const&) {
                        auto found = rows.find(id);
                        if (!alive.expired() && found != rows.end()) AttachOutline(id, found->second);
                    });
                    AttachMenu(row, id);
                }
                auto row = rows.at(id);
                UpdateHeader(row, id, tabs[i].title);
                Automation::AutomationProperties::SetName(row, tabs[i].title);
                // Secondary selection is explicit and theme-aware; SelectedItem
                // remains solely the Firefox active tab.
                bool selected = menu.selectedTabs.count(id) != 0;
                row.FontWeight(Windows::UI::Text::FontWeight{uint16_t(selected ? 600 : 400)});
                SetSelectedOutline(row, selected && menu.selectedTabs.size() > 1);
                Automation::AutomationProperties::SetItemStatus(row, StatusFor(id == active, selected));
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
                    outlinedTabs.erase(it->first); outlines.erase(it->first); hoverCard.Forget(it->first);
                    it = rows.erase(it);
                }
                else
                    ++it;
            }
            view.SelectedItem(rows.count(active) ? rows.at(active) : nullptr);
            if (rows.size() < priorRowCount) {
                // Stock TabView keeps the old tab widths after a removal while
                // the pointer is over the strip, so the next close button stays
                // under the cursor. Tabs close by motion here, not by that
                // lock; changing the width mode makes it recompute right away.
                view.TabWidthMode(Controls::TabViewWidthMode::SizeToContent);
                view.TabWidthMode(Controls::TabViewWidthMode::Equal);
            }
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
        if (!createdRows.empty())
            EntryLog("WinUIPane entry sync created=%zu first=%u prior=%zu rows=%zu before=%zu addBefore=%.1f inline=%d\n", createdRows.size(),
                createdRows[0], priorRowCount, rows.size(), rowsBefore.size(), addBefore, int(inlineGroups.active));
        if (createdRows.size() == 1 && rows.count(createdRows[0])) {
            enteringTab = createdRows[0];
            enteringUntil = GetTickCount64() + 1000;
            // Read geometry after the natural layout, not from a forced one
            // (see the close path below).
            const auto generation = ++closeGeneration;
            auto token = std::make_shared<winrt::event_token>();
            *token = Media::CompositionTarget::Rendering([this, alive = std::weak_ptr<int>(lifetime), generation, token,
                                                          created = createdRows[0], addBefore, rowsBefore](auto&&, auto&&) {
                Media::CompositionTarget::Rendering(*token);
                EntryLog("WinUIPane entry frame id=%u current=%d\n", created, int(!alive.expired() && generation == closeGeneration && rows.count(created)));
                if (alive.expired() || generation != closeGeneration || !rows.count(created)) return;
                try { RevealNewRow(rows.at(created), created, addBefore, rowsBefore); } catch (...) {}
            });
        }
        else if (createdRows.empty() && rows.size() < priorRowCount) {
            // The toggle above can leave TabView's next layout with every Width
            // NaN (tabs sized to their content, the group's last tab narrow until
            // the pointer moves). Repair it once that layout has run, then read the
            // motion's geometry from the layout the repaired widths produce.
            const auto generation = ++closeGeneration;
            const bool animate = !std::isnan(addBefore) && groupSlide.Enabled() && !IsDragging() && !groupSlide.storyboard;
            AfterNextLayout([this, alive = std::weak_ptr<int>(lifetime), generation, animate, addBefore, rowsBefore, closing] {
                if (alive.expired() || generation != closeGeneration) return;
                bool unsized = false;
                for (auto const& [id, row] : rows) unsized |= std::isnan(row.Width());
                if (unsized) {
                    view.TabWidthMode(Controls::TabViewWidthMode::SizeToContent);
                    view.TabWidthMode(Controls::TabViewWidthMode::Equal);
                }
                if (!animate) return;
                AfterNextLayout([this, alive, generation, addBefore, rowsBefore, closing] {
                    if (alive.expired() || generation != closeGeneration || IsDragging() || groupSlide.storyboard) return;
                    try {
                        const double addDelta = HoldAddButton(addBefore);
                        auto neighbors = HoldNeighbors(rowsBefore, 0);
                        AddCloseGhost(closing);
                        StartAddSlide(addDelta);
                        SettleNeighbors(neighbors, true);
                    } catch (...) {}
                });
            });
        }
        // A stationary cursor may produce no further input after this layout
        // change. Refresh a live hover now using the session's cursor sampler.
        if (refreshForeignPreview && tickDragSession) tickDragSession();
        ambient.Update();
    }
};
