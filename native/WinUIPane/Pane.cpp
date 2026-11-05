#include "pch.h"
#include "../../bridge/WinUIPaneApi.h"
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Input.DragDrop.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <Microsoft.UI.Dispatching.Interop.h>
#include <map>
#include <deque>
#include <algorithm>
#include <set>
#include <vector>
#include <memory>
#include <objbase.h>
#include <cstdio>

using namespace winrt;
using namespace Microsoft::UI;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Hosting;

#include "Address.h"
#include "ShellCore.h"
#include "ChromeLayout.h"
#include "PageInput.h"
#include "NativeWindowHost.h"
#include "XamlSlot.h"
#include "TabMenu.h"
#include "TabStrip.h"
#include "WindowDrag.h"
#include "StartupSurface.h"
#include "StartupLabels.h"

namespace {
bool DiagnosticsEnabled();
namespace SystemDrag = Microsoft::UI::Input::DragDrop;
// The island's drop target, set before WinUI creates XAML DragEventArgs: it replaces XAML's own.
// It rejects external system drags (AllowDrop=false only rejects them after that unsafe dispatch has
// happened). In a browser window the island also holds the toolbar, so it brings the toolbar
// editor's own drag to the toolbar through `route`, which reads no drag data unless that drag is
// under way. It owns no pane, controls, drag data or deferred work, and holds the route weakly: an
// in-flight leave/drop remains safe even after the island that requested it is destroyed.
struct ChromeDropTarget : implements<ChromeDropTarget, SystemDrag::IDropOperationTarget> {
    using Operation = Windows::ApplicationModel::DataTransfer::DataPackageOperation;
    enum Phase { Enter, Over, Leave, Drop };
    // True when the drag is accepted as a move.
    using Route = std::function<bool(Phase, SystemDrag::DragInfo const&, SystemDrag::DragUIOverride const*)>;
    std::weak_ptr<Route> route;
    explicit ChromeDropTarget(std::weak_ptr<Route> route) : route(std::move(route)) {}
    Operation Call(Phase phase, SystemDrag::DragInfo const& info, SystemDrag::DragUIOverride const* ui) const noexcept {
        try {
            if (auto routed = route.lock(); routed && (*routed)(phase, info, ui)) return Operation::Move;
        } catch (...) {}
        return Operation::None;
    }
    Windows::Foundation::IAsyncOperation<Operation> EnterAsync(SystemDrag::DragInfo const& info,
                                                              SystemDrag::DragUIOverride const& ui) {
        const auto operation = Call(Enter, info, &ui);
        if (operation == Operation::None && DiagnosticsEnabled()) fprintf(stderr, "WinUIPane external drag rejected\n");
        co_return operation;
    }
    Windows::Foundation::IAsyncOperation<Operation> OverAsync(SystemDrag::DragInfo const& info,
                                                             SystemDrag::DragUIOverride const& ui) {
        co_return Call(Over, info, &ui);
    }
    Windows::Foundation::IAsyncAction LeaveAsync(SystemDrag::DragInfo const& info) {
        Call(Leave, info, nullptr);
        co_return;
    }
    Windows::Foundation::IAsyncOperation<Operation> DropAsync(SystemDrag::DragInfo const& info) {
        co_return Call(Drop, info, nullptr);
    }
};
// How much of what update() and clip() are asked to do turns out to repeat
// what the island already has. Process-wide; read by the contract test through
// WinUIPaneTestGetGeometryWork and reported at shutdown under
// FENN_PANE_DIAGNOSTICS.
// Native cost per ABI entry point. Until this existed the only measured
// numbers in the project were Firefox-side: the adapter's shims wrap each call
// and so include the time spent in here, but nothing attributed it, and work
// that never touches the publication path -- update() and clip() running per
// animation frame -- was not measured at all.
//
// Calls are always counted; that is an increment. Timing is taken only when
// accounting is asked for, because this runs on the browser's main thread and a
// QueryPerformanceCounter pair on every call is exactly the kind of unmeasured
// overhead this project exists to remove.
//
// Cumulative totals with a call count give mean and worst cost per call, which
// is what attribution needs and is independent of which window the run was
// sampling -- so no periodic snapshot is required to read them.
struct EntryWork {
    const wchar_t* name;
    uint64_t calls;
    double totalMs;
    double maxMs;
};
inline EntryWork* nativeWork() {
    // Message translation is timed like the rest even though it is called for
    // every window message on the pump: at roughly 50ns for the clock pair
    // against a measured 3.5us per call, the instrument costs about 1.4% of
    // what it reports, which is worth knowing rather than guessing at.
    static EntryWork entries[]{
        {L"updateTabStrip", 0, 0, 0}, {L"clip", 0, 0, 0},
        {L"syncTabs", 0, 0, 0},     {L"syncAddress", 0, 0, 0},    {L"syncTabMenu", 0, 0, 0},
        {L"drainRequests", 0, 0, 0},{L"completeRequests", 0, 0, 0}, {L"applyTabDelta", 0, 0, 0},
        {L"focus", 0, 0, 0},        {L"destroy", 0, 0, 0},        {L"translate", 0, 0, 0},
        // Inside the first update() for a window. Separated because one is
        // paid once per process and the other once per browser window, and a
        // single 45.9ms figure for "the first update()" cannot tell them apart.
        {L"initialize", 0, 0, 0},   {L"paneCreate", 0, 0, 0},
        // Only the ticks that had a session to advance. Every window message
        // reaches TickDragSession, and the ones with no drag in progress
        // return on a pointer test -- section 8 requires no session work after
        // a gesture completes, so the two must not be counted together or the
        // count could never show it.
        {L"dragTick", 0, 0, 0},
        // Measurement only for now: ticks that ran with the cursor exactly
        // where the previous tick left it, so every position-derived result
        // was already known. Counted before deciding whether skipping them is
        // worth changing a gesture path for.
        {L"dragTickSame", 0, 0, 0},
        {nullptr, 0, 0, 0},
    };
    return entries;
}
inline double NowMs() {
    static const double scale = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart ? 1000.0 / double(frequency.QuadPart) : 0.0;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return double(counter.QuadPart) * scale;
}
bool DiagnosticsEnabled();
// Timing and the shutdown report, without the per-call diagnostic lines.
// The two are separate flags because a measurement run needs the first and is
// distorted by the second: with FENN_PANE_DIAGNOSTICS on, every
// update() writes a line to a redirected stderr, which cost 20.5ms across 105
// calls -- a fifth of what update() itself was reported to cost. The perf
// harness turns diagnostics off for exactly that reason, so it sets this.
inline bool WorkAccountingEnabled() {
    static const bool enabled =
        GetEnvironmentVariableW(L"FENN_PANE_WORK", nullptr, 0) != 0 || DiagnosticsEnabled();
    return enabled;
}
// Counts on construction, charges elapsed time on destruction, so an entry
// point that returns early is still counted exactly once.
struct WorkScope {
    EntryWork& entry;
    bool timed;
    double started;
    explicit WorkScope(size_t index)
        : entry(nativeWork()[index]), timed(WorkAccountingEnabled()), started(timed ? NowMs() : 0.0) {
        ++entry.calls;
    }
    WorkScope(const WorkScope&) = delete;
    WorkScope& operator=(const WorkScope&) = delete;
    // Charge the time now and stop measuring. Entry points that print a
    // per-call diagnostic line call this first: that line exists only because
    // diagnostics are on, it writes to a redirected stderr, and charging it to
    // the entry point would mean the instrument was measuring itself.
    void Stop() {
        if (!timed) return;
        timed = false;
        const double elapsed = NowMs() - started;
        entry.totalMs += elapsed;
        if (elapsed > entry.maxMs) entry.maxMs = elapsed;
    }
    ~WorkScope() { Stop(); }
};
// How long a native intent actually takes, which is what the request path
// built over milestones 3 to 6 exists to make short and nobody had measured.
// Section 10 asks for enqueue, drain, execution and completion separately, so
// they are separate here rather than one round-trip number:
//
//   wakeup    the request is queued -> the host drains it. The cost of waking
//             Firefox, which is the part the pane controls.
//   execute   drained -> the host reports its outcome. Firefox's own work.
//   total     queued -> outcome. What the user waits for, at this layer.
//
// Both ends are inside this DLL, so all three use one clock and need no
// agreement with the browser about what time it is.
// "present" is the one stage that is not about the request path: it is the
// time from handing the pane a change to XAML producing the next frame. Every
// other number here stops at the outcome the browser reported, which section 10
// is explicit is not proof anything was drawn.
//
// What it does NOT say: that DWM composited that frame to the screen. It says
// XAML rendered after the change. That is strictly closer to presentation than
// publication is, and it is as far as this can be taken from inside the pane.
enum Stage : size_t { StageWakeup, StageExecute, StageTotal, StagePresent, StageDrop };
// A mean is the wrong summary here and saying so is cheaper than being
// corrected by it: the distribution is bimodal, because a modal dialog or a
// blocked queue legitimately holds a request for as long as it holds it. These
// buckets are the shape, in milliseconds, and the last one is the tail.
constexpr double kLatencyBuckets[]{1.0, 4.0, 16.0, 64.0, 256.0};
constexpr size_t kLatencyBucketCount = sizeof(kLatencyBuckets) / sizeof(kLatencyBuckets[0]) + 1;
struct StageWork {
    EntryWork work;
    uint64_t buckets[kLatencyBucketCount];
    // Samples this stage is not answerable for, counted and never averaged in.
    //
    //   blocked   the host had the queue blocked while the request waited.
    //   deferred  the request sat through one or more drains before being
    //             taken. Batches are bounded at 16 entries and 64KB so that a
    //             burst cannot monopolise the browser's main thread, so this
    //             is the bound working, not the wakeup being slow.
    //
    // Both are real waiting the user experiences; neither is a cost the wakeup
    // path could remove, which is why they are separated rather than dropped.
    uint64_t blocked;
    uint64_t deferred;
    // Samples still counted in the distribution above, but whose wait was
    // mostly the message pump not reaching this process at all. Real latency
    // the user experiences; not something the wakeup path could shorten.
    uint64_t stalled;
};
inline StageWork* requestLatency() {
    static StageWork stages[]{
        {{L"wakeup", 0, 0, 0}, {}, 0, 0, 0},  {{L"execute", 0, 0, 0}, {}, 0, 0, 0},
        {{L"total", 0, 0, 0}, {}, 0, 0, 0},   {{L"present", 0, 0, 0}, {}, 0, 0, 0},
        // Release of a tab reorder until Firefox's snapshot places the tab: the
        // span a dropped tab sits on its preview transform before it snaps.
        {{L"drop", 0, 0, 0}, {}, 0, 0, 0},
        {{nullptr, 0, 0, 0}, {}, 0, 0, 0},
    };
    return stages;
}
enum Excluded : uint32_t { NotExcluded, ExcludedBlocked, ExcludedDeferred };
// The longest the message pump went without delivering anything to us.
// Translate is called for every window message, so a long gap here means the
// browser's main thread was not running our code -- which is the only
// remaining explanation for a long wakeup once blocked and deferred samples
// are both zero. Corroboration, not proof: it says the pump stopped reaching
// us, not what it was doing instead.
inline double& pumpGapMax() {
    static double value = 0.0;
    return value;
}
// Time the pump spent not reaching us in stretches long enough to matter, as a
// running total. A request can then be asked how much of its own wait fell
// inside one, which turns "the main thread was probably busy" into a number
// attached to the sample rather than a coincidence of magnitudes.
constexpr double kPumpStallMs = 50.0;
inline double& pumpStallTotal() {
    static double value = 0.0;
    return value;
}
inline void RecordPumpGap() {
    static double last = 0.0;
    const double now = NowMs();
    if (last > 0.0) {
        const double gap = now - last;
        if (gap > pumpGapMax()) pumpGapMax() = gap;
        if (gap > kPumpStallMs) pumpStallTotal() += gap;
    }
    last = now;
}
inline double& dropCommittedAt() {
    static double value = 0.0;
    return value;
}
inline double& dropStallAtCommit() {
    static double value = 0.0;
    return value;
}
inline void RecordStage(size_t stage, double elapsed, uint32_t excluded = NotExcluded, double stalled = 0.0) {
    auto& record = requestLatency()[stage];
    // Attributed, not excluded: it happened to the user either way.
    if (elapsed > kPumpStallMs && stalled >= 0.5 * elapsed) ++record.stalled;
    if (excluded == ExcludedBlocked) {
        ++record.blocked;
        return;
    }
    if (excluded == ExcludedDeferred) {
        ++record.deferred;
        return;
    }
    ++record.work.calls;
    record.work.totalMs += elapsed;
    if (elapsed > record.work.maxMs) record.work.maxMs = elapsed;
    size_t bucket = kLatencyBucketCount - 1;
    for (size_t i = 0; i < kLatencyBucketCount - 1; ++i) {
        if (elapsed <= kLatencyBuckets[i]) {
            bucket = i;
            break;
        }
    }
    ++record.buckets[bucket];
}
enum Entry : size_t {
    EntryUpdateTabStrip,
    EntryClip,
    EntrySyncTabs,
    EntrySyncAddress,
    EntrySyncTabMenu,
    EntryDrainRequests,
    EntryCompleteRequests,
    EntryApplyTabDelta,
    EntryFocus,
    EntryDestroy,
    EntryTranslate,
    EntryInitialize,
    EntryPaneCreate,
    EntryDragTick,
    EntryDragTickSame,
};
struct GeometryWork {
    uint64_t updates{}, geometryApplied{}, restacks{}, visibilityApplied{}, clips{}, clipsApplied{};
};
inline GeometryWork& geometryWork() {
    static GeometryWork value;
    return value;
}
bool DiagnosticsEnabled() {
    static const bool enabled = GetEnvironmentVariableW(L"FENN_PANE_DIAGNOSTICS", nullptr, 0) != 0;
    return enabled;
}

struct PaneApplication : ApplicationT<PaneApplication, Markup::IXamlMetadataProvider> {
    XamlTypeInfo::XamlControlsXamlMetaDataProvider provider;
    Markup::IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const& name) {
        return provider.GetXamlType(name);
    }
    Markup::IXamlType GetXamlType(hstring const& name) { return provider.GetXamlType(name); }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return provider.GetXmlnsDefinitions(); }
};

// Process-wide: exactly one drag gesture can be active at a time, mirroring
// how DoDragDrop (the design this replaces) only ever let one run too.
// sourceHwnd doubles as the "is a gesture active at all" flag. Armed vs.
// Dragging is not duplicated here -- it's read directly off
// sourceStrip->Drag().phase wherever it matters, so there is exactly one
// place a gesture's phase can ever disagree with itself. Defined before
// struct Pane (unlike the functions below) because Pane::Close() reads its
// fields directly, which needs it to be a complete type.
struct TabDragSession {
    HWND sourceHwnd{};
    NativeTabStrip* sourceStrip{};
    uint32_t tabId{};
    bool groupSource{},listSource{};
    UIElement listVisual{nullptr};
    NativeAllTabsPanel* hoverPanel{};
    // Whichever pane's own strip is currently showing a live "make room"
    // preview: sourceStrip itself while still hovering the strip the
    // gesture started on, a different pane's own strip while hovering it
    // (ForeignDragOver), or null during a true tear-off (neither).
    NativeTabStrip* hoverStrip{};
    // A bitmap completion belongs to this gesture, not merely to the source
    // pane or an HWND that Windows may reuse after cancellation.
    std::shared_ptr<int> lifetime;
    // The floating visual owns one topmost HWND and bitmap for this gesture,
    // both on-strip and detached. Abort destroys them before XAML cleanup;
    // a new arm must abort the previous session before replacing these handles.
    HWND tearoffWindow{};
    HBITMAP tearoffBitmap{};
    int tearoffWidth{}, tearoffHeight{};
};

// Forward declarations: Pane::Create wires strip->armDragSession/
// tickDragSession to these, and Pane::Close aborts an in-progress session
// referencing this pane -- both need to be visible before struct Pane, and
// (aside from dragSession/TabDragSession, already complete above) are only
// fully defined once Pane itself is a complete type.
struct Pane;
void NotifyStartup(Pane& pane) noexcept;
TabDragSession& dragSession();
void ArmDragSession(Pane& pane, uint32_t id);
void AbortDragSession() noexcept;
void TickDragSession();

struct Pane {
    // The project-owned Rust core: bounded request queue, drain scheduling,
    // request identity and outcomes, and the incremental presentation model.
    // Declared before consumers so the core also outlives partial construction.
    NativeShellCore shell;
    HWND parent{};
    std::unique_ptr<NativeAddress> address;
    // Regions of the window's one XAML tree: the strip, and the toolbar when it is separate.
    XamlSlot source{nullptr};
    XamlSlot addressSource{nullptr};
    std::function<bool(MSG const&)> startupKey;
    std::shared_ptr<void> startupPresenter;
#ifdef WINUI_PANE_TESTING
    std::function<bool()> startupNormalIdentity;
    hstring startupInitialLabel;
    bool startupBeforeGecko{};
    bool startupInitialRtl{};
#endif
    std::function<void()> startupRender;
    std::function<bool()> startupObserve;
    std::function<bool(NativeAddress::JObject const&)> startupAddressApply;
    uint64_t startupContext{};
    HWND addressWindow{};
    RECT addressBounds{};
    bool addressBoundsApplied{};
    bool navigationBackdrop{};
    bool toolbarHitTestVisible{true};
    static void SetBackdrop(XamlSlot const& target, NativeTabStrip::Backdrop kind) {
        if (kind == NativeTabStrip::Backdrop::Acrylic) {
            target.SystemBackdrop(Media::DesktopAcrylicBackdrop());
        } else {
            Media::MicaBackdrop mica;
            mica.Kind(kind == NativeTabStrip::Backdrop::Mica ? Composition::SystemBackdrops::MicaKind::Base
                                                          : Composition::SystemBackdrops::MicaKind::BaseAlt);
            target.SystemBackdrop(mica);
        }
    }
    event_token addressTakeFocus{};
    Microsoft::UI::Content::ContentIsland addressIsland{nullptr};
    static bool InWindow(HWND window, HWND target) {
        return window && target && (window == target || IsChild(window, target));
    }
    static bool InPopups(Microsoft::UI::Content::ContentIsland const& island, HWND target) {
        if (island) {
            for (auto popup : island.Popups())
                if (InWindow(GetWindowFromWindowId(popup.WindowId()), target)) return true;
        }
        return false;
    }
    void DismissMenus() {
        if (address) { address->bookmarkPanel.LightDismiss(); address->permissionsPanel.LightDismiss(); address->notificationPanel.LightDismiss(true); address->connectionPanel.LightDismiss(); address->translationsPanel.LightDismiss(); address->selectedTranslationsPanel.LightDismiss(); address->pageContextMenu.LightDismiss(); if(address->navigation)address->navigation->backForwardMenu.LightDismiss(); }
        if (strip) strip->allTabsPanel.LightDismiss(true);
        if (tabMenu.active) tabMenu.Dismiss();
        if (strip) strip->inlineGroups.hover.Dismiss();
        if (address && address->mode.flyout.IsOpen()) address->mode.flyout.Hide();
        CloseCustomization();
    }
    // The editor is not light-dismissed (that would block the toolbar it edits),
    // so it joins the same outside-press and deactivation paths as the menus.
    void CloseCustomization() {
        if (address && address->navigation && address->navigation->customization.editing &&
            !address->navigation->customization.dragging) address->navigation->customization.Hide();
    }
    void LeaveAddress() {
        if (!address) return;
        if (address->mode.flyout.IsOpen()) address->mode.flyout.Hide();
        address->ObserveFocus(false);
        if (address->popup) address->popup.IsOpen(false);
        address->ScheduleVisibilityReport();
    }
    static LRESULT CALLBACK FocusProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                      UINT_PTR id, DWORD_PTR data) noexcept {
        auto* pane = reinterpret_cast<Pane*>(data);
        try {
            if (hwnd == pane->parent && ((msg == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE &&
                 GetAncestor(HWND(l), GA_ROOTOWNER) != GetAncestor(hwnd, GA_ROOTOWNER)) ||
                (msg == WM_ACTIVATEAPP && !w) || msg == WM_CANCELMODE)) {
                if (pane->strip) pane->strip->CloseToolTips();
                pane->DismissMenus();
                pane->LeaveAddress();
            }
            if ((msg == WM_KILLFOCUS || msg == WM_SETFOCUS) && pane->address)
                pane->address->ScheduleFocusObservation();
            if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, FocusProc, id);
        } catch (...) {}
        return DefSubclassProc(hwnd, msg, w, l);
    }
    void OutsidePress(HWND target) {
        const bool stripPopup = InPopups(contentIsland, target);
        const bool addressPopup = InPopups(addressSource ? addressIsland : contentIsland, target);
        // All submenu windows belong to the same island popup graph.
        if (!stripPopup && tabMenu.active) tabMenu.Dismiss();
        if(!stripPopup && strip)strip->allTabsPanel.LightDismiss();
        if (!stripPopup && strip && (strip->inlineGroups.hover.open || strip->inlineGroups.hover.showPending))
            strip->inlineGroups.hover.Dismiss();
        if (!addressPopup && address) { address->bookmarkPanel.LightDismiss(); address->permissionsPanel.LightDismiss(); address->notificationPanel.LightDismiss(); address->connectionPanel.LightDismiss(); address->translationsPanel.LightDismiss(); address->selectedTranslationsPanel.LightDismiss(); address->pageContextMenu.LightDismiss(); if(address->navigation)address->navigation->backForwardMenu.LightDismiss(); }
        const bool inAddress = (addressSource ? addressSource : source).Holds(target);
        if (!inAddress && !addressPopup) LeaveAddress();
        else if (!addressPopup && address && address->mode.flyout.IsOpen()) address->mode.flyout.Hide();
        if (!source.Holds(target) && !addressSource.Holds(target) &&
            !stripPopup && !addressPopup &&
            (InWindow(childWindow, GetFocus()) || InWindow(addressWindow, GetFocus()) ||
             InPopups(contentIsland, GetFocus()) || InPopups(addressIsland, GetFocus())) &&
            InWindow(parent, target)) SetFocus(NativeWindowHost::PageWindow(parent));
        if (!inAddress && !addressPopup) CloseCustomization();
        if (address) address->ScheduleFocusObservation();
    }
    // The page child takes input everywhere but the visible chrome regions, except where
    // Gecko's own controls show through the strip (Firefox's exclusions, strip-relative).
    void ApplyPageRegion() {
        if (!parent) return;
        std::vector<RECT> chrome, cut;
        for (auto const* slot : {&source, &addressSource}) {
            if (*slot) if (RECT r = slot->Bounds(); r.right > r.left && r.bottom > r.top) chrome.push_back(r);
        }
        const RECT strip = source.Bounds();
        for (auto r : appliedExclusions) {
            OffsetRect(&r, strip.left, strip.top);
            cut.push_back(r);
        }
        NativeWindowHost::SetPageRegion(parent, std::move(chrome), std::move(cut));
    }
    void CloseAddressHost() {
        startupPresenter.reset();
        if (!addressSource) return;
        if (addressWindow && addressWindow != childWindow) RemoveWindowSubclass(addressWindow, FocusProc, reinterpret_cast<UINT_PTR>(this));
        addressSource.TakeFocusRequested(addressTakeFocus);
        addressSource.SiteBridge().Hide();
        addressSource.SystemBackdrop(nullptr);
        navigationBackdrop = false;
        addressSource.Content(nullptr);
        windowDrag.windowCaption.SetToolbarSurface(nullptr);
        if (strip) strip->SetLeadingControl(nullptr);
        address.reset();
        shell.ResetAddress();
        shell.ResetAddressPublication();
        shell.ResetToolbarPublication();
        addressIsland = nullptr;
        addressWindow = nullptr;
        addressBoundsApplied = false;
        addressSource.Close();
        addressSource = nullptr;
        ApplyPageRegion();
    }
    void WireAddress() {
        address->navigation->customization.ownerHwnd=parent;
        address->navigation->ownerHwnd=parent;
        windowDrag.windowCaption.SetToolbarSurface(address->toolbarSurface);
        if (strip) {
            auto menuButton = address->navigation->productMenu.button;
            menuButton.Margin({6, 0, 4, 0});
            strip->SetLeadingControl(menuButton);
        }
        address->releaseMenuFocus = [this] { SetFocus(NativeWindowHost::PageWindow(parent)); };
        address->preparePointerFocus = [this] {
            // Revoke the other island's menu without replaying the pointer or
            // replacing TextBox selection/caret handling.
            if (tabMenu.active) tabMenu.Dismiss();
        };
        address->releaseContentFocus = [this] {
            // Never reclaim focus from another application, another island,
            // or a dialog. XAML logical focus alone is not ownership.
            const auto focused = GetFocus();
            const auto host = addressSource ? addressWindow : childWindow;
            const bool release = GetForegroundWindow() == parent &&
                (InWindow(host, focused) || InPopups(addressSource ? addressIsland : contentIsland, focused));
            NativeWindowHost::FocusTrace(release ? "release-content-focus" : "release-content-focus-skipped");
            if (release) SetFocus(NativeWindowHost::PageWindow(parent));
        };
        address->navigate = [this](hstring const& action) {
            Enqueue(NativeShellCore::NavigationAction, 0, 0, action);
        };
        address->dispatch = [this](hstring const& action) {
            return Enqueue(NativeShellCore::AddressAction, 0, address->revision, action);
        };
        address->reportVisible = [this](hstring const& action) {
            Enqueue(NativeShellCore::AddressVisibility, 0, address->revision, action);
        };
        address->mode.dispatch = [this](hstring const& action) {
            address->interaction.Interrupt(address->EditorOwned());
            Enqueue(NativeShellCore::AddressModeAction, 0, address->revision, action);
        };
        address->observeFocus = [this] {
            if (!address) return;
            bool ownsFocus = (addressSource ? addressSource : source).HasFocus();
            auto island = addressSource ? addressIsland : contentIsland;
            if (!ownsFocus && island) {
                const HWND focused = GetFocus();
                for (auto popup : island.Popups()) {
                    const HWND popupWindow = GetWindowFromWindowId(popup.WindowId());
                    if (focused == popupWindow || IsChild(popupWindow, focused)) {
                        ownsFocus = true;
                        break;
                    }
                }
            }
            address->ObserveFocus(ownsFocus &&
                GetAncestor(GetForegroundWindow(), GA_ROOTOWNER) == GetAncestor(parent, GA_ROOTOWNER));
        };
    }
    SystemDrag::DragDropManager::TargetRequested_revoker externalDropRevoker;
    std::shared_ptr<ChromeDropTarget::Route> dropRoute;
    void RejectExternalDrops() {
        if (externalDropRevoker || !strip || !strip->root.XamlRoot()) return;
        dropRoute = std::make_shared<ChromeDropTarget::Route>(
            [this](ChromeDropTarget::Phase phase, SystemDrag::DragInfo const& info, SystemDrag::DragUIOverride const* ui) {
                // Only while the editor's own drag is under way: no other drag can run meanwhile.
                if (!address || !address->navigation || !address->navigation->customization.dragging || !strip) return false;
                auto& navigation = *address->navigation;
                auto root = strip->root.XamlRoot();
                if (!root || !navigation.host || !navigation.host.XamlRoot() || !SameObject(root, navigation.host.XamlRoot()))
                    return false;
                if (phase == ChromeDropTarget::Leave) { navigation.LeaveRoot(); return false; }
                auto* host = NativeWindowHost::Get(parent);
                POINT cursor{};
                if (!host || !host->bridge || !GetCursorPos(&cursor) || !ScreenToClient(host->bridge, &cursor)) return false;
                const double scale = root.RasterizationScale() > 0 ? root.RasterizationScale() : 1.0;
                const Windows::Foundation::Point at{float(cursor.x / scale), float(cursor.y / scale)};
                return phase == ChromeDropTarget::Drop ? navigation.DropOnRoot(info.Data(), at, ui)
                                                       : navigation.DragOverRoot(info.Data(), at, ui);
            });
        auto dropManager = SystemDrag::DragDropManager::GetForIsland(strip->root.XamlRoot().ContentIsland());
        externalDropRevoker = dropManager.TargetRequested(auto_revoke,
            [route = std::weak_ptr<ChromeDropTarget::Route>(dropRoute)](auto const&, SystemDrag::DropOperationTargetRequestedEventArgs const& args) {
                args.SetTarget(make<ChromeDropTarget>(route));
            });
    }
    std::map<uint32_t, hstring> tabTitles;
    NativeTabMenu tabMenu;
    std::unique_ptr<NativeTabStrip> strip;
    event_token takeFocus{};
    WinUIPaneNotifyFn notifySink{};
    void* notifyContext{};
    // True between drainRequests and completeRequests. The host must close
    // every drain it opens; a drain still open when the next one arrives is a
    // host error, counted here rather than silently absorbed.
    bool drainOpen{};
    uint64_t abandonedDrains{};
    // What this island has actually been given. Firefox drives update() from a
    // requestAnimationFrame whenever anything is dirty, so most calls repeat
    // the previous geometry and the previous exclusions exactly, and
    // re-issuing either is measurable work for no change. Stacking is
    // deliberately not cached: it is read back from the window, because
    // anything may have restacked the child since.
    RECT appliedBounds{};
    bool boundsApplied{};
    int appliedVisible{-1};
    std::vector<RECT> appliedExclusions;
    RECT appliedClipClient{};
    bool clipApplied{};
    // Resolved once and kept, because Translate runs for every window message
    // in the browser process and both are fixed for this pane's life. Deriving
    // them per message cost two WinRT property calls plus an interop call, and
    // three more WinRT calls before the popup list: measured over one
    // integration run, caching them took message translation from 504ms to
    // 297ms across about 102,000 messages, 5.0us to 2.9us each.
    HWND childWindow{};
    Microsoft::UI::Content::ContentIsland contentIsland{nullptr};
    // Whether the host currently has the queue blocked, and how many times it
    // has entered that state. The epoch is what lets a request say "the host
    // blocked at some point while I was waiting" without keeping a history:
    // compare the epoch at enqueue with the one at drain.
    // Armed when the pane is handed a change, disarmed by the first frame
    // after it. Deliberately transient: staying subscribed to Rendering would
    // hold the UI thread in a continuous frame callback, which is the thing
    // section 6 forbids and the thing the idle measurements check for -- the
    // instrument would create the condition it is measuring.
    event_token renderingToken{};
    double presentArmedAt{};
    bool presentArmed{};
    void ArmPresentation() {
        if (!WorkAccountingEnabled() || presentArmed) return;
        presentArmedAt = NowMs();
        presentArmed = true;
        renderingToken = Media::CompositionTarget::Rendering(
            [this](Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&) {
                if (!presentArmed) return;
                RecordStage(StagePresent, NowMs() - presentArmedAt);
                presentArmed = false;
                Media::CompositionTarget::Rendering(renderingToken);
                renderingToken = {};
            });
    }
    void DisarmPresentation() {
        if (!presentArmed) return;
        presentArmed = false;
        Media::CompositionTarget::Rendering(renderingToken);
        renderingToken = {};
    }
    bool hostBlocked{};
    uint64_t blockedEpoch{};
    // Batches taken so far. A request stamped with the value this held when it
    // was queued is being taken by the first batch that could have taken it;
    // anything older sat through one, which the bounded batch size allows.
    uint64_t drainSequence{};
    void SetHostBlocked(bool blocked) {
        if (blocked && !hostBlocked) ++blockedEpoch;
        hostBlocked = blocked;
    }
    struct QueuedStamp {
        double at;
        uint64_t epoch;
        uint64_t drainSequence;
        double pumpStall;
        bool blocked;
    };
    // When each outstanding request was queued, and when it was handed over.
    // Erased on completion, and otherwise bounded at 256 entries by evicting
    // the lowest id, which is the oldest: a superseded request is evicted from
    // the queue and never completes, so its stamp would stay here forever.
    // Populated only while work accounting is on.
    std::map<uint64_t, QueuedStamp> queuedAt;
    std::map<uint64_t, double> drainedAt;
    uint32_t browserSelection{};
    bool syncing{};
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);

    // WinUI owns the island's screen origin and rasterization scale. Convert
    // through its live converter, then through the TabView/root transform;
    // parent HWND client coordinates and XAML units are different spaces.
    RECT StripScreenBounds() const {
        if (!strip || !strip->view.XamlRoot()) return {};
        auto rootBounds = strip->view.TransformToVisual(nullptr).TransformBounds(
            {0, 0, float(strip->view.ActualWidth()), float(strip->view.ActualHeight())});
        auto screen = strip->view.XamlRoot().ContentIsland().CoordinateConverter().ConvertLocalToScreen(rootBounds);
        return {screen.X, screen.Y, screen.X + screen.Width, screen.Y + screen.Height};
    }
    Windows::Foundation::Point ScreenToStrip(POINT screen) const {
        auto rootPoint = strip->view.XamlRoot().ContentIsland().CoordinateConverter().ConvertScreenToLocal(
            Windows::Graphics::PointInt32{screen.x, screen.y});
        return strip->view.TransformToVisual(nullptr).Inverse().TransformPoint(rootPoint);
    }

    // Every erase of a pane calls Close() first, but the Rendering handler
    // captures `this`, so the one invariant that must not depend on a call
    // site getting it right is stated here as well.
    ~Pane() { startupPresenter.reset(); DisarmPresentation(); }
    NativeWindowDrag windowDrag;
    void Close() {
        if (parent) RemoveWindowSubclass(parent, FocusProc, reinterpret_cast<UINT_PTR>(this));
        if (childWindow) RemoveWindowSubclass(childWindow, FocusProc, reinterpret_cast<UINT_PTR>(this));
        windowDrag.Close();
        DisarmPresentation();
        if (!source) return;
        // The tick-based session (unlike the old thread-blocking DoDragDrop
        // it replaced) can genuinely still be in progress when a window
        // closes -- abort it rather than leave a dangling sourceStrip/
        // hoverStrip pointer for the next tick to dereference.
        if (strip && (strip.get() == dragSession().sourceStrip || strip.get() == dragSession().hoverStrip || &strip->allTabsPanel == dragSession().hoverPanel))
            AbortDragSession();
        // Ordered teardown: stop accepting requests and drop the host wakeup
        // before anything the queued work refers to goes away, then release the
        // core last. The sink is cleared first so nothing can be posted to a
        // host that is already unwinding.
        notifySink = nullptr;
        notifyContext = nullptr;
        CloseAddressHost();
        drainOpen = false;
        tabMenu.Close();
        if (shell && WorkAccountingEnabled()) {
            // A refusal means Firefox and this window disagreed about a
            // publication; suites read these lines to catch it.
            static constexpr struct { uint32_t channel; wchar_t const* name; } channels[] = {
                {SHELL_PUBLICATION_CHANNEL_TAB_PREVIEW, L"tab-preview"},
                {SHELL_PUBLICATION_CHANNEL_TAB_MODEL, L"tab-model"},
                {SHELL_PUBLICATION_CHANNEL_STRIP, L"strip"},
                {SHELL_PUBLICATION_CHANNEL_TAB_MENU_STATE, L"tab-menu-state"},
                {SHELL_PUBLICATION_CHANNEL_SURFACE_MENU, L"surface-menu"},
                {SHELL_PUBLICATION_CHANNEL_ADDRESS_SESSION, L"address-session"},
                {SHELL_PUBLICATION_CHANNEL_ADDRESS_MODES, L"address-modes"},
                {SHELL_PUBLICATION_CHANNEL_ADDRESS_CONTEXT_MENU, L"address-context-menu"},
                {SHELL_PUBLICATION_CHANNEL_ADDRESS_RESULT_MENU, L"address-result-menu"},
                {SHELL_PUBLICATION_CHANNEL_DOWNLOADS_PANEL, L"downloads-panel"},
                {SHELL_PUBLICATION_CHANNEL_PROFILE_MENU, L"profile-menu"},
                {SHELL_PUBLICATION_CHANNEL_OVERFLOW_MENU, L"overflow-menu"},
                {SHELL_PUBLICATION_CHANNEL_DEVELOPER_MENU, L"developer-menu"},
                {SHELL_PUBLICATION_CHANNEL_BACK_FORWARD_MENU, L"back-forward-menu"},
                {SHELL_PUBLICATION_CHANNEL_ALL_TABS_PANEL, L"all-tabs-panel"},
                {SHELL_PUBLICATION_CHANNEL_TAB_GROUPS_MENU, L"tab-groups-menu"},
                {SHELL_PUBLICATION_CHANNEL_GROUP_HEADER, L"group-header"},
                {SHELL_PUBLICATION_CHANNEL_FORGET_PANEL, L"forget-panel"},
                {SHELL_PUBLICATION_CHANNEL_CLEAR_PRIVATE_PANEL, L"clear-private-panel"},
                {SHELL_PUBLICATION_CHANNEL_TOOLBAR_CUSTOMIZATION, L"toolbar-customization"},
            };
            for (auto const& entry : channels) {
                const auto counts = shell.PublicationCounts(entry.channel);
                fwprintf(stderr, L"WinUIPane publications channel=%s accepted=%llu unchanged=%llu ignored=%llu refused=%llu\n",
                         entry.name, counts.accepted, counts.unchanged, counts.ignored, counts.refused);
            }
        }
        if (shell.core) shell_core_close(shell.core);
        lifetime.reset();
        source.TakeFocusRequested(takeFocus);
        dropRoute.reset();
        externalDropRevoker.revoke();
        source.SystemBackdrop(nullptr);
        source.Content(nullptr);
        if (strip) strip->SetLeadingControl(nullptr);
        address.reset();
        strip.reset();
        shell.Close();
        tabTitles.clear();
        source.Close();
        source = nullptr;
    }
    // Hand one native intent to the Rust queue and, only when the queue says a
    // wakeup is now needed, ask the host to schedule exactly one. This runs on
    // XAML callback stacks, so it must never call into the browser: the sink
    // only posts a task to Firefox's own event loop.
    uint64_t Enqueue(NativeShellCore::Kind kind, uint32_t id, uint32_t contextRevision, hstring const& action) {
        bool notify = false;
        uint32_t status = 0;
        auto request = shell.Enqueue(kind, id, contextRevision, std::wstring_view(action), notify, &status);
        if (request && WorkAccountingEnabled()) {
            // A superseded request is evicted from the queue and never
            // completes, so its stamp would sit here forever. Ids are
            // monotonic, so dropping the lowest drops the oldest.
            while (queuedAt.size() >= 256) queuedAt.erase(queuedAt.begin());
            queuedAt[request] = QueuedStamp{NowMs(), blockedEpoch, drainSequence, pumpStallTotal(), hostBlocked};
        }
        if (notify) ScheduleDrain();
        if (!request && DiagnosticsEnabled())
            fwprintf(stderr, L"WinUIPane request refused kind=%u tab=%u status=%u\n", unsigned(kind), id, status);
        return request;
    }
    void ScheduleDrain() {
        // No host wakeup available (teardown, or one that could not schedule):
        // put the queue back into a state where the next request schedules a
        // fresh wakeup, rather than waiting for one that will never arrive.
        if (!notifySink || !notifySink(notifyContext)) shell.NotificationFailed();
    }
    // ShellCore owns duplicate-close suppression and optimistic selection
    // lifetime. C++ only constructs the browser action and delivers wakeups.
    void QueueClose(uint32_t id) {
        if (!id) return;
        NativeTabMenu::Object action;
        action.SetNamedValue(L"kind", NativeTabMenu::Value::CreateStringValue(L"close"));
        action.SetNamedValue(L"tab", NativeTabMenu::Value::CreateNumberValue(id));
        Enqueue(NativeShellCore::CloseTab, id, tabMenu.StateRevision(), action.Stringify());
    }
    void QueueNewTab() {
        NativeTabMenu::Object action;
        action.SetNamedValue(L"kind", NativeTabMenu::Value::CreateStringValue(L"new-tab"));
        Enqueue(NativeShellCore::NewTab, 0, tabMenu.StateRevision(), action.Stringify());
    }
    // ShellCore has already reconciled tab/address/mode bookkeeping when each
    // outcome was recorded. Deliver the remaining surface-specific effects.
    void ReconcileOutcomes() {
        shell.TakeOutcomes(); // Diagnostic reports do not own cleanup anymore.
        if(startupRender)return;
        if (strip) {
            strip->ReconcileTransfers();
            strip->ReconcileDrag();
            strip->ReconcileForeignGroup();
            strip->ReconcileGroupDeparture();
        }
    }
    // Keep the Rust presentation model in step with the authoritative snapshot,
    // so a later delta has a correct base to apply against. Structural apply to
    // the controls still happens below from the snapshot itself; the model is
    // what makes single-property updates cost one row instead of all of them.
    HRESULT Sync(const WinUITab* snapshot, uint32_t count, uint32_t selected, uint32_t revision) {
        if (shell) {
            const auto previous = shell.ModelRevision();
            if (!shell.BeginSnapshot(shell.Stats().generation, revision, count)) return E_INVALIDARG;
            for (uint32_t i = 0; i < count; ++i)
                if (!shell.PushSnapshotTab(snapshot[i].id, snapshot[i].title, snapshot[i].commands)) return E_INVALIDARG;
            uint32_t changes = 0;
            auto status = shell.CommitSnapshot(selected, changes);
            if (status != SHELL_STATUS_OK) return status == SHELL_STATUS_GAP ? E_NOT_VALID_STATE : E_INVALIDARG;
            if (previous == revision) return S_OK;
            shell.TakeChanges();
        }
        ReconcileOutcomes();
        browserSelection = selected;
        shell.SelectionPending(tabMenu.StateRevision());
        tabTitles.clear();
        for(uint32_t i=0;i<count;++i)tabTitles.emplace(snapshot[i].id,snapshot[i].title);
        if(startupRender)startupRender();
        else strip->Sync(snapshot, count, selected);
        return S_OK;
    }
    void QueueStripSelection(uint32_t id) {
        if (!tabMenu.HasTab(id)) return;
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        auto selected = control ? tabMenu.selectedTabs : std::set<uint32_t>{};
        if (control && selected.count(id) && id != browserSelection)
            selected.erase(id);
        else
            selected.insert(id);
        selected.insert(control ? browserSelection : id);
        NativeTabMenu::Array ids;
        for (auto value : selected) ids.Append(NativeTabMenu::Value::CreateNumberValue(value));
        NativeTabMenu::Object event;
        event.SetNamedValue(L"kind", NativeTabMenu::Value::CreateStringValue(L"selection"));
        event.SetNamedValue(L"revision", NativeTabMenu::Value::CreateNumberValue(tabMenu.StateRevision()));
        event.SetNamedValue(L"tab",
                            NativeTabMenu::Value::CreateNumberValue(selected.count(id) ? id : browserSelection));
        event.SetNamedValue(L"tabs", ids);
        event.SetNamedValue(L"control", NativeTabMenu::Value::CreateBooleanValue(control));
        event.SetNamedValue(L"shift", NativeTabMenu::Value::CreateBooleanValue(shift));
        Enqueue(NativeShellCore::SelectTab, id, tabMenu.StateRevision(), event.Stringify());
    }

    // Apply one incremental presentation update. Only the named tab's row is
    // touched; nothing scans the collection and no description is rebuilt.
    // A base revision the model no longer holds is reported as a gap, which the
    // producer answers with one snapshot -- never with a retry loop.
    HRESULT ApplyDelta(NativeTabMenu::Object const& delta) {
        if (!shell) return E_NOT_VALID_STATE;
        if (NativeJson::RequireId(delta, L"version") != 1) return E_INVALIDARG;
        auto base = NativeJson::RequireId(delta, L"base");
        auto revision = NativeJson::RequireId(delta, L"revision");
        // Validate the whole delta before any of it is applied.
        std::vector<std::pair<uint32_t, hstring>> titles;
        if (delta.HasKey(L"titles")) {
            auto entries = delta.GetNamedArray(L"titles");
            if (entries.Size() > NativeJson::kMaxTabs) return E_INVALIDARG;
            for (auto value : entries) {
                auto entry = value.GetObject();
                auto id = NativeJson::RequireId(entry, L"id");
                auto title = entry.GetNamedString(L"title");
                if (title.size() > NativeJson::kMaxFieldText) return E_INVALIDARG;
                // A tab the model does not hold means producer and consumer have
                // diverged: ask for a snapshot rather than guess.

                titles.emplace_back(id, title);
            }
        }
        // "selected": 0 is the valid "no active tab" value, so it is read as a
        // plain bounded number rather than as a nonzero identity.
        uint32_t nextSelected = 0;
        if (delta.HasKey(L"selected")) {
            auto value = delta.GetNamedNumber(L"selected");
            if (!std::isfinite(value) || value < 0 || value > UINT32_MAX || std::floor(value) != value)
                return E_INVALIDARG;
            nextSelected = uint32_t(value);

        }
        std::vector<ShellTabTitle> rows;
        rows.reserve(titles.size());
        for (auto const& [id, title] : titles)
            rows.push_back({id, reinterpret_cast<uint16_t const*>(title.c_str()), title.size()});
        auto status = shell.ApplyTabDelta(base, revision, rows, delta.HasKey(L"selected"), nextSelected);
        if (status != SHELL_STATUS_OK)
            return status == SHELL_STATUS_GAP || status == SHELL_STATUS_UNKNOWN_TAB ? E_NOT_VALID_STATE : E_INVALIDARG;
        for (auto const& [id, title] : titles) tabTitles[id] = title;
        if (delta.HasKey(L"selected")) browserSelection = nextSelected;
        // Read the resynchronization flag before ApplyChanges consumes the
        // change list: taking the changes is what clears it.
        const bool resync = shell.NeedsSnapshot();
        ApplyChanges();
        return resync ? S_FALSE : S_OK;
    }
    uint32_t ModelRevision() const { return shell.ModelRevision(); }

    // Turn the model's change records into the smallest native work that shows
    // them. One changed title is one row update, whatever the tab count.
    void ApplyChanges() {
        auto changes = shell.TakeChanges();
        if(startupRender){startupRender();return;}
        // Same guard as ApplyPublishedSelection: a pane that is not presenting
        // has nothing to apply to, and the model catches up at the next
        // snapshot.
        if (!changes.size() || !strip) return;
        syncing = true;
        try {
            for (size_t i = 0; i < changes.size(); ++i) {
                auto const& change = changes[i];
                if (change.kind == NativeShellCore::TitleChanged) {
                    strip->UpdateTitle(change.tab, tabTitles.count(change.tab) ? tabTitles[change.tab] : L"");
                } else if (change.kind == NativeShellCore::SelectionChanged) {
                    // `from`/`to` are the previous and new active tab ids.
                    strip->ApplySelection(change.from, change.to);
                }
            }
        } catch (...) {
            syncing = false;
            throw;
        }
        syncing = false;
    }
    // Reconcile the control's selection with the set Firefox just published.
    // Selection no longer travels on syncTabs, so a publication is the only
    // thing that moves this; applying it here keeps a selection change from
    // needing a whole snapshot. Only the rows whose membership actually
    // changed are touched.
    void ApplyPublishedSelection(std::set<uint32_t> const& previous) {
        if(startupRender)return;
        // A native selection is still awaiting its authoritative result; its
        // optimistic feedback stands until the outcome or a newer publication.
        if (shell.SelectionPending(tabMenu.StateRevision())) return;
        if (strip) strip->ApplySelectedSet(previous, tabMenu.selectedTabs);
    }
    void Create(HWND hwnd) {
        parent = hwnd;
        check_bool(SetWindowSubclass(parent, FocusProc, reinterpret_cast<UINT_PTR>(this),
                                     reinterpret_cast<DWORD_PTR>(this)));
        // Bounds for this endpoint. Zero means "the core's own default".
        // Deliberately small: these are user intents, not a data channel, and
        // an unbounded backlog of them would be a bug, not throughput.
        ShellCoreLimits limits{};
        limits.max_entries = 64;
        // JSON can encode each UTF-16 unit as six ASCII units (\uXXXX).
        // Include the bounded session and envelope, and fit a worst-case edit
        // in the queue. DrainRequests' 262144-unit buffer fits this request;
        // the batch byte bound ensures such a large request travels alone.
        limits.max_request_bytes = 2 * (6 * (NativeJson::kMaxFieldText + NativeJson::kMaxOpaqueId) + 4096);
        limits.max_queued_bytes = 512 * 1024;
        limits.max_tabs = 10000;
        limits.max_title_units = 32768;
        shell.Open(limits);
        tabMenu.core = shell.core;
        // Menu transitions (open, activate, dismiss) are ordered requests on
        // the same queue as selection, so a selection formed before an opening
        // still reaches Firefox first.
        tabMenu.dispatch = [this](uint32_t id, uint32_t contextRevision, hstring const& action) {
            Enqueue(NativeShellCore::MenuEvent, id, contextRevision, action);
        };
        source = XamlSlot::New();
        source.Initialize(GetWindowIdFromWindow(hwnd));
        childWindow = GetWindowFromWindowId(source.SiteBridge().WindowId());
        check_bool(SetWindowSubclass(childWindow, FocusProc, reinterpret_cast<UINT_PTR>(this),
                                     reinterpret_cast<DWORD_PTR>(this)));
        strip = std::make_unique<NativeTabStrip>(tabMenu);
        strip->core = shell.core;
        strip->ownerHwnd = hwnd;
        // The strip's own completed gestures ride the same queue as menu
        // transitions and selection, in the order the user made them. Each
        // carries the presentation revision it was formed against, so one
        // formed against a superseded presentation is refused by Firefox
        // rather than silently dropped here.
        if (WorkAccountingEnabled()) {
            strip->dropCommitted = [] {
                dropCommittedAt() = NowMs();
                dropStallAtCommit() = pumpStallTotal();
            };
            strip->dropLanded = [] {
                if (dropCommittedAt() <= 0.0) return;
                RecordStage(StageDrop, NowMs() - dropCommittedAt(), NotExcluded,
                            pumpStallTotal() - dropStallAtCommit());
                dropCommittedAt() = 0.0;
            };
        }
        strip->dispatch = [this](uint32_t id, hstring const& action) {
            return Enqueue(NativeShellCore::StripAction, id, strip->presentationRevision, action);
        };
        strip->groupsButton.dispatch = [this](hstring const& action) {
            Enqueue(NativeShellCore::StripAction, 0, strip->presentationRevision, action);
        };
        // Hover-card requests coalesce: only the newest hover matters, and
        // it must never displace a gesture queued before it.
        strip->preview = [this](hstring const& action) {
            Enqueue(NativeShellCore::StripPreview, 0, strip->presentationRevision, action);
        };
        strip->select = [this](uint32_t id) { QueueStripSelection(id); };
        strip->close = [this](uint32_t id) { QueueClose(id); };
        strip->add = [this] { QueueNewTab(); };
        takeFocus = source.TakeFocusRequested([this](auto const&, auto const&) { SetFocus(NativeWindowHost::PageWindow(parent)); });
        source.Content(strip->root);
        // The island owns the backdrop and its theme/activation/fallback
        // handling. The toolbar's separate island follows the same choice;
        // each host owns its backdrop instance and closes it independently.
        // Mica Alt until Firefox's first publication names its preference.
        strip->applyBackdrop = [this](NativeTabStrip::Backdrop kind) {
            SetBackdrop(source, kind);
            if (addressSource && navigationBackdrop) SetBackdrop(addressSource, kind);
        };
        strip->applyBackdrop(strip->backdrop);
        windowDrag.Open(hwnd, strip.get());
        // Subscribe after XAML installs its default target, before showing
        // the island. SetTarget substitutes a rejecting target at the system
        // boundary, before XAML drag event creation (which can fail-fast in
        // embedded Firefox). Do not Close this shared, WinUI-owned manager
        // or replace Firefox's HWND/OLE registration. Native tab gestures
        // use our independent pointer session and never enter this path.
        // A Window loads its content only once shown, so before then this waits for the strip to load,
        // which still precedes any drag.
        if (strip->root.XamlRoot()) RejectExternalDrops();
        else {
            auto token = std::make_shared<event_token>();
            *token = strip->root.Loaded([this, token, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
                if (alive.expired() || !strip) return;
                strip->root.Loaded(*token);
                RejectExternalDrops();
            });
        }
        strip->armDragSession = [this](uint32_t id) { ArmDragSession(*this, id); };
        strip->armGroupDragSession = [this](uint32_t id) { ArmDragSession(*this,id); dragSession().groupSource=true; };
        strip->tickDragSession = TickDragSession;
        strip->allTabsPanel.core=shell.core;
        strip->allTabsPanel.ownerHwnd=parent;
        strip->allTabsPanel.dispatch=[this](auto const& action){if(strip && strip->dispatch)strip->dispatch(0,action.Stringify());};
        strip->allTabsPanel.releaseFocus=[this]{SetFocus(NativeWindowHost::PageWindow(parent));};
        strip->allTabsPanel.cancelDrag=[owner=strip.get()] {
            if(dragSession().sourceStrip==owner || dragSession().hoverPanel==&owner->allTabsPanel)AbortDragSession();
        };
        strip->allTabsPanel.dragging=[] {auto* strip=dragSession().sourceStrip;return strip && (strip->Drag().phase==NativeTabStrip::DragPhase::Dragging || strip->GroupDrag().phase==2 || (!dragSession().listSource && (strip->Drag().phase==NativeTabStrip::DragPhase::Armed || strip->GroupDrag().phase==1)));};
        strip->allTabsPanel.deferOutside=[this]{
            if(!(GetAsyncKeyState(VK_LBUTTON)&0x8000))return false;POINT p{};GetCursorPos(&p);
            if(GetAncestor(WindowFromPoint(p),GA_ROOTOWNER)!=GetAncestor(parent,GA_ROOTOWNER))return false;
            for(auto const& [id,row]:strip->rows)if(row.IsLoaded() && row.Visibility()==Visibility::Visible){auto r=NativeAllTabsPanel::ScreenBounds(row);if(p.x>=r.X && p.x<r.X+r.Width && p.y>=r.Y && p.y<r.Y+r.Height)return true;}
            for(auto const& [id,run]:strip->inlineGroups.runs)if(run.button && run.button.IsLoaded()){auto r=NativeAllTabsPanel::ScreenBounds(run.button);if(p.x>=r.X && p.x<r.X+r.Width && p.y>=r.Y && p.y<r.Y+r.Height)return true;}
            return false;
        };
        strip->allTabsPanel.armDrag=[this](uint32_t tab,uint32_t group,UIElement visual){
            POINT p{};GetCursorPos(&p);auto local=ScreenToStrip(p);
            if(group){
                if(DiagnosticsEnabled())fprintf(stderr,"AllTabs arm group=%u x=%.1f y=%.1f\n",group,local.X,local.Y);
                if(shell_core_group_drag_arm(shell.core,group,local.X,local.Y)!=SHELL_STATUS_OK)return false;
                strip->groupDragVisual.Create(strip->root);
                auto found=strip->inlineGroups.runs.find(group);if(found!=strip->inlineGroups.runs.end())strip->groupDragVisual.Arm(found->second.button,found->second.button.TransformToVisual(strip->groupDragVisual.layer).TransformPoint({float(found->second.button.ActualWidth()/2),0}));
                ArmDragSession(*this,group);dragSession().groupSource=true;dragSession().listSource=true;strip->StartGroupDragTimer();return true;
            }
            if(!strip->rows.count(tab))return false;
            ArmDragSession(*this,tab);if(shell_core_tab_drag_arm(shell.core,tab,local.X,local.Y)!=SHELL_STATUS_OK){AbortDragSession();return false;}dragSession().listSource=true;dragSession().listVisual=visual;strip->StartTabDragTimer();return true;
        };
        strip->allTabsPanel.image=[this](uint32_t tab)->Media::ImageSource{
            auto found=strip->rows.find(tab);if(found==strip->rows.end() || !found->second.Header())return nullptr;
            auto header=found->second.Header().as<Controls::Grid>();
            return header.Children().GetAt(0).as<Controls::Grid>().Children().GetAt(0).as<Controls::Image>().Source();
        };
        strip->allTabsPanel.context=[this](uint32_t tab){
            Controls::MenuFlyout menu;menu.Items().Append(NativeTabMenu::Placeholder());
            auto alive=std::weak_ptr<int>(lifetime);
            menu.Opening([this,alive,tab,weak=make_weak(menu)](auto const&,auto const&){if(!alive.expired())tabMenu.Open(weak.get(),tab);});
            menu.Closed([this,alive,weak=make_weak(menu)](auto const&,auto const&){if(!alive.expired() && tabMenu.active==weak.get())tabMenu.Dismiss();});return menu;
        };

    }
};

// Blocking is checked first: a request held by a blocked queue is held
// whatever the batches did afterwards, and calling that "deferred" would
// blame the batch bound for the host's own pause.
uint32_t Classify(Pane const& pane, Pane::QueuedStamp const& stamp) {
    if (stamp.blocked || stamp.epoch != pane.blockedEpoch) return ExcludedBlocked;
    if (pane.drainSequence > stamp.drainSequence + 1) return ExcludedDeferred;
    return NotExcluded;
}
TabDragSession& dragSession() {
    static TabDragSession session;
    return session;
}

// Intentionally process-lifetime storage: no WinRT destruction under DLL detach.
struct State {
    DWORD thread{};
    bool stopped{};
    bool attempted{};
    HRESULT initialization{E_PENDING};
    Dispatching::DispatcherQueueController dispatcher{nullptr};
    Application app{nullptr};
    WindowsXamlManager manager{nullptr};
    std::map<HWND, std::unique_ptr<Pane>> panes;
};
State& state() {
    static auto value = new State;
    return *value;
}

void ArmDragSession(Pane& pane, uint32_t id) {
    // A fresh press can arrive before the old release was sampled. Dispose
    // its preview and restore its row before assigning the new identities.
    AbortDragSession();
    auto& session = dragSession();
    session.lifetime = std::make_shared<int>(0);
    session.sourceHwnd = pane.parent;
    session.sourceStrip = pane.strip.get();
    session.tabId = id;
    session.hoverStrip = pane.strip.get();
    session.tearoffWindow = nullptr;
    session.tearoffBitmap = nullptr;
    session.tearoffWidth = session.tearoffHeight = 0;
}

void DestroyTearoffWindow(TabDragSession& session) {
    if (session.tearoffBitmap) {
        DeleteObject(session.tearoffBitmap);
        session.tearoffBitmap = nullptr;
    }
    if (session.tearoffWindow) {
        DestroyWindow(session.tearoffWindow);
        session.tearoffWindow = nullptr;
    }
    session.tearoffWidth = session.tearoffHeight = 0;
}

// Tears the session down regardless of outcome -- release, cancel, a
// foreign pane accepting the drop, or (defensively) the source pane itself
// having been closed mid-gesture, which this tick-based design allows in a
// way the old, thread-blocking DoDragDrop call structurally couldn't.
void AbortDragSession() noexcept {
    // Revoke global tracking and asynchronous rendering before releasing
    // capture or touching XAML, either of which can deliver more callbacks.
    auto session = std::exchange(dragSession(), TabDragSession{});
    session.lifetime.reset();
    // OS resources cannot depend on fallible XAML cleanup. Destroy the
    // detached preview first, while its exact HWND is still owned here.
    DestroyTearoffWindow(session);
    if (session.sourceHwnd && GetCapture() == session.sourceHwnd) ReleaseCapture();
    if(session.hoverPanel)try{session.hoverPanel->ResetPreview();}catch(...){}
    auto& panes = state().panes;
    auto source = panes.find(session.sourceHwnd);
    if (source != panes.end() && source->second->strip.get() == session.sourceStrip && session.sourceStrip && session.groupSource)
        try { session.sourceStrip->EndGroupDrag(); } catch (...) {}
    if (source != panes.end() && source->second->strip.get() == session.sourceStrip && session.sourceStrip &&
        session.sourceStrip->Drag().phase != NativeTabStrip::DragPhase::None)
        try { session.sourceStrip->CancelDrag(); } catch (...) {}
    if (session.hoverStrip && session.hoverStrip != session.sourceStrip) {
        for (auto& [hwnd, pane] : panes) {
            if (pane->strip.get() == session.hoverStrip) {
                try { pane->strip->EndForeignDragOver(); } catch (...) {}
                try { pane->strip->EndForeignGroupOver(); } catch (...) {}
                break;
            }
        }
    }
}

constexpr wchar_t kTearoffClassName[] = L"FennTabTearoff";

void EnsureTearoffClassRegistered() {
    static const ATOM atom = [] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kTearoffClassName;
        return RegisterClassExW(&wc);
    }();
    (void)atom;
}

// Renders `row`'s current appearance into the session's tear-off bitmap --
// once, the first tick of Dragging, since the popup now represents the
// dragged tab for the whole gesture (TickDragSession), not just once it
// leaves a strip. A coroutine, not a blocking call, for the same reason
// TabDragImage::RenderElementToBitmapAsync itself is one (see that file).
// Guards against the session having moved on (a new gesture) by the time it
// resumes. Capture the template root, excluding the outer item's opacity,
// so the real row stays hidden throughout asynchronous capture. In particular,
// a newly adopted row must never paint in its layout slot before this is ready.
winrt::fire_and_forget RenderTearoffAsync(Controls::TabViewItem row, HWND popup, std::weak_ptr<int> alive) {
    int width{}, height{};
    uint64_t bitmap{};
    try {
        row.ApplyTemplate();
        TabDragImage::SetFloatingAppearance(row, true);
        TabDragImage::CaptureCorners corners(row);
        row.UpdateLayout();
        auto visual = Media::VisualTreeHelper::GetChild(row, 0).as<UIElement>();
        row.Opacity(0);
        bitmap = co_await TabDragImage::RenderElementToBitmapAsync(visual, &width, &height);
    } catch (...) {
        if (DiagnosticsEnabled()) fprintf(stderr, "WinUIPane drag capture failed hr=0x%08X\n", unsigned(winrt::to_hresult()));
        if (!alive.expired()) {
            TabDragImage::SetFloatingAppearance(row, false);
            row.Opacity(1);
        }
        co_return;
    }
    auto& session = dragSession();
    if (alive.expired() || !bitmap || session.tearoffWindow != popup) {
        if (bitmap) DeleteObject(HBITMAP(bitmap));
        if (!alive.expired()) {
            TabDragImage::SetFloatingAppearance(row, false);
            row.Opacity(1);
        }
        co_return;
    }
    session.tearoffBitmap = HBITMAP(bitmap);
    session.tearoffWidth = width;
    session.tearoffHeight = height;
    row.Opacity(0);
}

// Creates (lazily, on the first tick of Dragging) and repositions the
// tear-off popup -- the sole visual for the dragged tab for the whole
// gesture now (TickDragSession), so this fires immediately once promoted,
// not just once a tear-off actually happens. A plain WS_POPUP this code
// fully owns -- no OLE registration, so none of the ancestor-chain/z-order
// problems that made an overlay window unusable for OLE's own purposes
// apply here: this window only ever needs to be visually on top, never
// hit-testable by anything (WS_EX_TRANSPARENT), so it can never interfere
// with the geometric hit-testing TickDragSession does against every pane's
// own StripScreenBounds().
bool CreateTearoffWindow(TabDragSession& session) {
    EnsureTearoffClassRegistered();
    session.tearoffWindow =
        CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kTearoffClassName,
                         L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!session.tearoffWindow) return false;
    ShowWindow(session.tearoffWindow, SW_SHOWNOACTIVATE);
    SetWindowPos(session.tearoffWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return true;
}

// Overflow strip items need not have an installed template. Capture the live
// native list container for list-origin gestures, with the same gesture lease.
winrt::fire_and_forget RenderListTearoffAsync(UIElement visual, HWND popup, std::weak_ptr<int> alive) {
    int width{},height{};uint64_t bitmap{};
    try {bitmap=co_await TabDragImage::RenderElementToBitmapAsync(visual,&width,&height);}
    catch (...) {co_return;}
    auto& session=dragSession();
    if(alive.expired() || !bitmap || session.tearoffWindow!=popup){if(bitmap)DeleteObject(HBITMAP(bitmap));co_return;}
    session.tearoffBitmap=HBITMAP(bitmap);session.tearoffWidth=width;session.tearoffHeight=height;
}

void ShowOrMoveTearoff(TabDragSession& session, POINT screenPoint) {
    if (!session.tearoffWindow) {
        if (!CreateTearoffWindow(session)) return;
        if(session.listVisual)RenderListTearoffAsync(session.listVisual,session.tearoffWindow,std::weak_ptr<int>(session.lifetime));
        else if (session.sourceStrip && session.sourceStrip->rows.count(session.tabId))
            RenderTearoffAsync(session.sourceStrip->rows.at(session.tabId), session.tearoffWindow,
                                std::weak_ptr<int>(session.lifetime));
    }
    if (!session.tearoffBitmap) return;
    POINT topLeft{screenPoint.x - session.tearoffWidth / 2, screenPoint.y - session.tearoffHeight / 2};
    TabDragImage::PaintDragVisual(session.tearoffWindow, session.tearoffBitmap, session.tearoffWidth,
                                   session.tearoffHeight, topLeft);
}

// A whole group's tear-off snapshot: the overlay chip carrying the actual
// group button and its themed backing, captured once it first leaves the strip.
winrt::fire_and_forget RenderGroupTearoffAsync(FrameworkElement chip, HWND popup, std::weak_ptr<int> alive) {
    int width{}, height{};
    uint64_t bitmap{};
    try {
        bitmap = co_await TabDragImage::RenderElementToBitmapAsync(chip, &width, &height);
    } catch (...) {
        if (DiagnosticsEnabled()) fprintf(stderr, "WinUIPane group drag capture failed hr=0x%08X\n", unsigned(winrt::to_hresult()));
        co_return;
    }
    auto& session = dragSession();
    if (alive.expired() || !bitmap || session.tearoffWindow != popup) {
        if (bitmap) DeleteObject(HBITMAP(bitmap));
        co_return;
    }
    session.tearoffBitmap = HBITMAP(bitmap);
    session.tearoffWidth = width;
    session.tearoffHeight = height;
}

// Off the strip the group follows the cursor in a floating window, keeping
// the grab point; back over it, the in-strip chip takes over again. The chip
// stays visible until the snapshot is ready, so there is never a blank frame.
void UpdateGroupTearoff(TabDragSession& session, POINT screenPoint, bool detaching, RECT snap) {
    auto& visual = session.sourceStrip->groupDragVisual;
    if (!detaching || !visual.button || !visual.chip) {
        if (session.tearoffWindow) ShowWindow(session.tearoffWindow, SW_HIDE);
        visual.Detach(false);
        return;
    }
    if (!session.tearoffWindow) {
        if (!CreateTearoffWindow(session)) return;
        RenderGroupTearoffAsync(visual.chip, session.tearoffWindow, std::weak_ptr<int>(session.lifetime));
    }
    if (!session.tearoffBitmap) return;
    const double scale = visual.chip.XamlRoot() ? visual.chip.XamlRoot().RasterizationScale() : 1.0;
    const int grab = std::clamp(int(visual.grabX * scale), 0, session.tearoffWidth);
    POINT topLeft{screenPoint.x - grab, screenPoint.y - session.tearoffHeight / 2};
    // Over another strip, sit in its row band as a header would.
    if (snap.bottom > snap.top) topLeft.y = snap.top + (snap.bottom - snap.top - session.tearoffHeight) / 2;
    TabDragImage::PaintDragVisual(session.tearoffWindow, session.tearoffBitmap, session.tearoffWidth,
                                   session.tearoffHeight, topLeft);
    if (!IsWindowVisible(session.tearoffWindow)) ShowWindow(session.tearoffWindow, SW_SHOWNOACTIVATE);
    visual.Detach(true);
}

// Resolves an ended gesture (button no longer physically down) against
// wherever the cursor actually is right now: a different pane's own strip
// (AcceptForeignDrop), still this strip's own (CommitReorder), or neither
// (CommitDetach) -- the same three outcomes DoDragDrop's own return value
// used to distinguish, now decided by this code's own geometry instead of
// asking OLE to resolve a target window. `hitPane`, like TickDragSession's
// own search, only ever names a DIFFERENT pane -- whether release still
// counts as "on the source strip" is `Drag().detaching`, which TickDragSession
// only just updated (via UpdateDragVisual) for this exact point when
// `hitPane` is null.
void ResolveDragDrop(Pane& sourcePane, Pane* hitPane, POINT screenPoint) {
    // WinUI can throw while resetting transforms or releasing pointer capture.
    // Even then the independent topmost HWND must not survive the gesture.
    struct Finish { ~Finish() { AbortDragSession(); } } finish;
    auto& session = dragSession();
    auto* strip = session.sourceStrip;
    if (hitPane && hitPane->strip) {
        auto local = hitPane->ScreenToStrip(screenPoint);
        const auto tab = session.tabId;
        auto destination = hitPane->strip.get();
        const auto ticket = destination->ReserveTransfer(*strip, tab);
        if (!ticket) { strip->CancelDrag(); destination->EndForeignDragOver(true); return; }
        NativeTabStrip::TransferAdmission admission{*destination, ticket, true};
        strip->BeginDeparture(tab);
        admission.Bind(destination->AcceptForeignDrop(sourcePane.parent, tab, local));
    } else if (strip->Drag().detaching) {
        strip->CommitDetach(session.tabId);
    } else {
        auto local = sourcePane.ScreenToStrip(screenPoint);
        strip->CommitReorder(session.tabId, local);
    }
}

// The top-level window a person sees at `screenPoint`, as a drop target
// would resolve it. The source still occludes windows underneath it. Skip
// windows that are both layered and input-transparent: the drag's own preview and the
// shell's snap overlay are such windows and never take a drop.
HWND VisibleRootAt(POINT screenPoint) {
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (!IsWindowVisible(window) || IsIconic(window)) continue;
        const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((style & WS_EX_LAYERED) && (style & WS_EX_TRANSPARENT)) continue;
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
        // Top-level window rectangles include invisible resize borders.
        RECT bounds{};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) &&
            !GetWindowRect(window, &bounds)) continue;
        if (PtInRect(&bounds, screenPoint)) return GetAncestor(window, GA_ROOTOWNER);
    }
    return nullptr;
}

// The other pane whose strip claims `screenPoint`, if any. Never the source,
// a hidden or minimized window, or a strip another window covers there.
Pane* ForeignStripAt(Pane& sourcePane, POINT screenPoint) {
    const auto visible = VisibleRootAt(screenPoint);
    if (!visible) return nullptr;
    for (auto& entry : state().panes) {
        if (!entry.second->strip || entry.second.get() == &sourcePane || !entry.second->appliedVisible ||
            !IsWindowVisible(entry.first) || IsIconic(entry.first)) continue;
        RECT bounds = entry.second->StripScreenBounds();
        if (!PtInRect(&bounds, screenPoint)) continue;
        // Pane storage is keyed by HWND, not z-order. A covered pane can sort
        // before the window a person can actually see; skip it and keep
        // looking instead of turning the visible strip into empty desktop.
        if (visible != GetAncestor(entry.first, GA_ROOT)) continue;
        return entry.second.get();
    }
    return nullptr;
}

// Pane.cpp's own Translate() hook calls this unconditionally on every
// message it pretranslates, and TabStrip.h's PointerMoved calls it directly
// too while already Dragging, as a lower-latency accelerant -- both routes
// converge here. Ground truth is GetCursorPos()/GetAsyncKeyState(), never
// the triggering message's own content, so it doesn't matter which one (or
// neither) actually drove a given call: the gesture keeps advancing as long
// as this keeps getting ticked at all, which raw ::SetCapture (TabStrip.h's
// PointerPressed) exists specifically to guarantee regardless of where the
// cursor physically is.
NativeAllTabsPanel* AllTabsAt(POINT point) {
    const auto window=WindowFromPoint(point);const auto hit=GetAncestor(window,GA_ROOTOWNER);
    for(auto& [hwnd,pane]:state().panes)if(pane->strip && pane->appliedVisible && IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetAncestor(hwnd,GA_ROOTOWNER)==hit && pane->strip->allTabsPanel.Contains(point))return &pane->strip->allTabsPanel;
    return nullptr;
}

void TickDragSession() {
    // XAML layout/capture callbacks can reenter the message hook while a tick
    // is already resolving a release or creating the drag visual.
    static bool ticking{};
    if (ticking) return;
    ticking = true;
    struct TickScope { bool& active; ~TickScope() { active = false; } } scope{ticking};
    auto& session = dragSession();
    if (!session.sourceHwnd) return;
    WorkScope tick{EntryDragTick};
    auto& panes = state().panes;
    auto srcIt = panes.find(session.sourceHwnd);
    if (srcIt == panes.end() || srcIt->second->strip.get() != session.sourceStrip) {
        // The source pane was closed mid-gesture -- nothing left to resolve into.
        AbortDragSession();
        return;
    }
    Pane& sourcePane = *srcIt->second;
    if (session.groupSource) {
        POINT point{};
        GetCursorPos(&point);
        const bool down=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
        const bool cancel=(GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0 || GetAncestor(GetForegroundWindow(),GA_ROOTOWNER)!=GetAncestor(sourcePane.parent,GA_ROOTOWNER) ||
            (session.sourceStrip->GroupDrag().phase==2 && GetCapture()!=sourcePane.parent) || !sourcePane.appliedVisible;
        auto* strip = session.sourceStrip;
        if(session.listSource && strip->GroupDrag().phase==1 && !down){
            // An ordinary ListView click never promoted. Disarm without strip
            // visual restoration or capture changes before WinUI handles up.
            shell_core_group_drag_end(strip->core);
            if(strip->groupDragVisual.timer)strip->groupDragVisual.timer.Stop();
            auto candidate=std::exchange(dragSession(),TabDragSession{});
            candidate.lifetime.reset();return;
        }
        // Another window's strip wins over this strip's own margin, as for a
        // tab. Let Firefox reject incompatible private/ordinary adoption so
        // release rolls the source back; filtering that strip out here turns
        // the same release into an unintended off-strip tear-off instead.
        Pane* hitPane = nullptr;
        if (!cancel && strip->GroupDrag().phase == 2)
            hitPane = ForeignStripAt(sourcePane, point);
        NativeTabStrip* hover = hitPane ? hitPane->strip.get() : nullptr;
        if (session.hoverStrip != hover && session.hoverStrip && session.hoverStrip != strip)
            session.hoverStrip->EndForeignGroupOver();
        session.hoverStrip = hover;
        auto* panel=AllTabsAt(point);
        if(session.hoverPanel && session.hoverPanel!=panel)session.hoverPanel->ResetPreview();session.hoverPanel=panel;
        if(cancel){AbortDragSession();return;}
        if(panel && strip->GroupDrag().phase==2){
            strip->allTabsPanel.suppressClick=true;
            const bool valid=panel->Preview(point);if(!down){if(valid)panel->Drop(sourcePane.parent,strip->windowDragSession,0,strip->GroupDrag().group);strip->EndGroupDrag();AbortDragSession();return;}
        }
        if (hitPane && !down) {
            // The destination queues the adopt; the source keeps the group
            // hidden until Firefox moves it, exactly like a tear-off.
            const auto group = strip->GroupDrag().group;
            const auto width = strip->GroupDragSlotWidth();
            const auto request = hitPane->strip->AcceptForeignGroupDrop(*strip, sourcePane.parent, strip->windowDragSession,
                                                                        group, hitPane->ScreenToStrip(point), width);
            strip->EndGroupDrag(false, request != 0);
            session.hoverStrip = nullptr;
            AbortDragSession();
            return;
        }
        strip->TickGroupDrag(sourcePane.ScreenToStrip(point), down, cancel, hitPane != nullptr || panel != nullptr);
        if(session.listSource && strip->GroupDrag().phase==2)strip->allTabsPanel.suppressClick=true;
        const auto group = strip->GroupDrag();
        if (!group.phase) {const bool fromList=session.listSource;AbortDragSession();if(!fromList)sourcePane.strip->allTabsPanel.LightDismiss();return;}
        if (hitPane) {
            if (!hitPane->strip->ForeignGroupOver(hitPane->ScreenToStrip(point), strip->GroupDragSlotWidth()))
                session.hoverStrip = nullptr;
        }
        UpdateGroupTearoff(session, point, group.phase == 2 && (group.detaching || panel!=nullptr),
                           hitPane ? hitPane->StripScreenBounds() : RECT{});
        return;
    }
    if (session.sourceStrip->Drag().phase == NativeTabStrip::DragPhase::None) {
        // Ended some other way already (Escape via WinUI's own
        // PreviewKeyDown, if it happened to still fire) -- just clean up
        // the session-level state that hasn't caught up yet.
        AbortDragSession();
        return;
    }
    bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (session.sourceStrip->Drag().phase == NativeTabStrip::DragPhase::Armed) {
        POINT screenPoint{};
        GetCursorPos(&screenPoint);
        auto local = sourcePane.ScreenToStrip(screenPoint);
        if (!buttonDown) {
            if(session.listSource)shell_core_tab_drag_end(session.sourceStrip->core,false);else session.sourceStrip->FinishDrag(session.tabId);
            const bool fromList=session.listSource;AbortDragSession();if(!fromList)sourcePane.strip->allTabsPanel.LightDismiss();
        } else {
            session.sourceStrip->TryPromote(session.tabId, local);
            if(session.listSource && session.sourceStrip->Drag().phase==NativeTabStrip::DragPhase::Dragging){shell_core_tab_drag_list_origin(session.sourceStrip->core);session.sourceStrip->allTabsPanel.suppressClick=true;SetCapture(sourcePane.parent);}
        }
        return;
    }
    // Dragging.
    if (WorkAccountingEnabled()) {
        static POINT lastTick{LONG_MIN, LONG_MIN};
        POINT here{};
        GetCursorPos(&here);
        if (here.x == lastTick.x && here.y == lastTick.y) ++nativeWork()[EntryDragTickSame].calls;
        lastTick = here;
    }
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        session.sourceStrip->CancelDrag();
        AbortDragSession();
        return;
    }
    POINT screenPoint{};
    GetCursorPos(&screenPoint);
    // A DIFFERENT pane's own strip claiming this point always wins,
    // regardless of the source strip's own vertical margin below --
    // otherwise two windows sitting side by side at the same screen height
    // would never resolve as a foreign hover at all (local Y relative to
    // the source alone can't tell "moved to a horizontally different
    // window" from "still over my own strip").
    auto* panel=AllTabsAt(screenPoint);
    if(session.hoverPanel && session.hoverPanel!=panel)session.hoverPanel->ResetPreview();session.hoverPanel=panel;
    if(panel){
        if(session.hoverStrip && session.hoverStrip!=session.sourceStrip)session.hoverStrip->EndForeignDragOver();session.hoverStrip=nullptr;
        session.sourceStrip->CloseDragGap();ShowOrMoveTearoff(session,screenPoint);
        const bool valid=panel->Preview(screenPoint);if(!buttonDown){if(valid)panel->Drop(sourcePane.parent,session.sourceStrip->windowDragSession,session.tabId,0);AbortDragSession();}
        return;
    }
    Pane* hitPane = ForeignStripAt(sourcePane, screenPoint);
    NativeTabStrip* newHover = hitPane ? hitPane->strip.get() : nullptr;
    if (!hitPane) {
        // Not over any other pane -- on-strip vs. tear-off is decided by
        // the source strip's bounded margin on both axes
        // (UpdateDragVisual's Drag().detaching). Small horizontal overshoot
        // still supports moving past the last tab; farther away detaches.
        // UpdateDragVisual no longer touches the dragged row's own
        // visual at all (see its own comment) -- only the floating popup,
        // positioned below, ever represents the dragged tab now.
        auto local = sourcePane.ScreenToStrip(screenPoint);
        session.sourceStrip->UpdateDragVisual(local);
        if (!session.sourceStrip->Drag().detaching) newHover = session.sourceStrip;
    }
    if (session.hoverStrip != newHover && session.hoverStrip && session.hoverStrip != session.sourceStrip)
        session.hoverStrip->EndForeignDragOver();
    session.hoverStrip = newHover;
    // The floating popup is the sole visual for the dragged tab for the
    // whole gesture now, not just once it leaves a strip -- TabStrip.h's
    // TryPromote/UpdateDragVisual never move or show the real row anymore
    // (RenderTearoffAsync hides it once the popup's own snapshot is
    // actually ready). Keeping one mechanism instead of switching between
    // "real row" and "floating bitmap" at each hover transition is what
    // actually fixed the class of bugs that switch kept producing (a
    // duplicate visual, an inconsistent flush/elevated look depending on
    // which one currently owned the row, two independently-tuned clamps
    // disagreeing with each other) -- there's no transition left to get
    // wrong. `snapPane` is whichever pane's own strip the popup should
    // visually snap into right now (source or foreign); null means a true
    // tear-off, where it just follows the cursor directly instead.
    Pane* snapPane = hitPane ? hitPane : (newHover ? &sourcePane : nullptr);
    if (snapPane) {
        // Snap into that strip's own row band (vertically centered on it,
        // same as a real row) rather than centering on the raw cursor
        // point, so it reads as "part of this strip" the same way a real
        // row would. Horizontally it still tracks the cursor (clamped to
        // the strip's own width) -- the same direct cursor-following
        // UpdateDragVisual's own clamp math uses, not a grid-slot snap.
        RECT bounds = snapPane->StripScreenBounds();
        POINT anchor{screenPoint.x, bounds.top + (bounds.bottom - bounds.top) / 2};
        int halfWidth = session.tearoffWidth / 2;
        anchor.x = std::clamp(anchor.x, bounds.left + halfWidth, std::max(bounds.left + halfWidth, bounds.right - halfWidth));
        ShowOrMoveTearoff(session, anchor);
    } else {
        // True tear-off -- no strip to snap into, so the visual just
        // follows the cursor directly.
        ShowOrMoveTearoff(session, screenPoint);
    }
    if (hitPane) {
        session.sourceStrip->CloseDragGap();
        auto local = hitPane->ScreenToStrip(screenPoint);
        hitPane->strip->ForeignDragOver(local);
    }
    if (!buttonDown) ResolveDragDrop(sourcePane, hitPane, screenPoint);
}

HRESULT Check(HWND hwnd = nullptr) {
    auto& s = state();
    if (s.thread && s.thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    if (s.stopped) return RO_E_CLOSED;
    if (hwnd) {
        DWORD process{};
        if (!IsWindow(hwnd)) return E_INVALIDARG;
        if (GetWindowThreadProcessId(hwnd, &process) != GetCurrentThreadId() || process != GetCurrentProcessId())
            return RPC_E_WRONG_THREAD;
    }
    return S_OK;
}

void Initialize() {
    auto& s = state();
    if (s.attempted) {
        check_hresult(s.initialization);
        return;
    }
    WorkScope scope{EntryInitialize};
    s.attempted = true;
    s.thread = GetCurrentThreadId();
    const char* phase = "apartment";
    try {
        APTTYPE apartment;
        APTTYPEQUALIFIER qualifier;
        check_hresult(CoGetApartmentType(&apartment, &qualifier));
        check_bool(apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA);
        // WinUI classes resolve through the host's package graph, which holds
        // the shared runtime (package dependency or bootstrapper).
        // Never take shutdown ownership of a dispatcher created by another host.
        phase = "dispatcher";
        if (Dispatching::DispatcherQueue::GetForCurrentThread()) throw_hresult(E_UNEXPECTED);
        s.dispatcher = Dispatching::DispatcherQueueController::CreateOnCurrentThread();
        phase = "application";
        s.app = make<PaneApplication>();
        // COM teardown can unload XAML after its dispatcher has shut down.
        // Its module teardown requires process-exit semantics, like this host DLL.
        phase = "xaml-module-lifetime";
        HMODULE xaml{};
        check_bool(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"Microsoft.UI.Xaml.dll", &xaml));
        phase = "xaml-manager";
        s.manager = WindowsXamlManager::InitializeForCurrentThread();
        // Browser windows close and open through Firefox; the last one closing must not end this thread's loop.
        phase = "shutdown-mode";
        s.app.DispatcherShutdownMode(DispatcherShutdownMode::OnExplicitShutdown);
        phase = "resources";
        s.app.Resources().MergedDictionaries().Append(Controls::XamlControlsResources());
        // The same holds for the controls module these resources (and TabView)
        // live in: unloaded by COM teardown, its static destructors fault.
        phase = "controls-module-lifetime";
        HMODULE controls{};
        if (GetModuleHandleW(L"Microsoft.UI.Xaml.Controls.dll"))
            check_bool(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"Microsoft.UI.Xaml.Controls.dll", &controls));
        // Firefox caps a tab at 225 CSS px; the stock TabView default is 240.
        s.app.Resources().Insert(box_value(L"TabViewItemMaxWidth"), box_value(225.0));
        s.initialization = S_OK;
    } catch (...) {
        s.initialization = to_hresult();
        fprintf(stderr, "WinUIPane initialization phase=%s hr=0x%08X\n", phase, unsigned(s.initialization));
        throw;
    }
}

HRESULT ShowInteractiveStartup(HWND hwnd, const wchar_t* profile, const wchar_t* presentation = nullptr) noexcept;
HRESULT CreateBrowserWindow(const RECT& bounds, HWND* output) noexcept {
    try {
        Initialize();
        Window window;
        Controls::Canvas root;
        window.Content(root);
        const HWND hwnd = GetWindowFromWindowId(window.AppWindow().Id());
        // XAML's content window. Its class name is the only handle the Window gives to it.
        const HWND bridge = FindWindowExW(hwnd, nullptr, L"Microsoft.UI.Content.DesktopChildSiteBridge", nullptr);
        check_bool(hwnd && bridge);
        check_hresult(NativeWindowHost::Attach(hwnd, window, root, bridge));
        check_bool(SetWindowPos(hwnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                                bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE));
        *output = hwnd;
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
const bool browserWindowFactory = (NativeWindowHost::CreateShellWindow = &CreateBrowserWindow, true);
HRESULT ShowStartupSurfaceForProfile(HWND hwnd, const wchar_t* profile, const wchar_t* presentation = nullptr) noexcept {
    try {
        check_hresult(Check(hwnd));
        if (!NativeStartupSurface::Eligible(hwnd)) return E_INVALIDARG;
        NativeWindowHost::FocusTrace("start-xaml-init-begin");
        Initialize();
        NativeWindowHost::FocusTrace("start-xaml-initialized");
        wchar_t interactive[2]{};
        if(GetEnvironmentVariableW(L"FENN_NATIVE_INTERACTIVE_STARTUP",interactive,2)==1 && (interactive[0]==L'1' || interactive[0]==L'2') &&
            GetPropW(hwnd,L"Fenn.NativeStartupOrigin")==reinterpret_cast<HANDLE>(2))
            return ShowInteractiveStartup(hwnd,profile,presentation);
        return NativeStartupSurface::Show(hwnd);
    } catch (...) { return to_hresult(); }
}
HRESULT WINAPI ShowStartupSurface(HWND hwnd) noexcept { return ShowStartupSurfaceForProfile(hwnd,nullptr); }
HRESULT WINAPI DismissStartupSurface(HWND hwnd) noexcept {
    if (!hwnd) return E_INVALIDARG;
    try {
        check_hresult(Check(hwnd));
        return NativeStartupSurface::Close(hwnd);
    } catch (...) { return to_hresult(); }
}
HRESULT WINAPI UpdateTabStrip(HWND hwnd, const RECT* rect, BOOL visible) noexcept {
    WorkScope scope{EntryUpdateTabStrip};
    const char* operation = "validate";
    try {
        check_hresult(Check(hwnd));
        if (!hwnd || !rect || rect->right < rect->left || rect->bottom < rect->top) return E_INVALIDARG;
        auto& s = state();
        auto found = s.panes.find(hwnd);
        if (!visible && found == s.panes.end()) return S_OK;
        if (found == s.panes.end()) {
            Initialize();
            auto pane = std::make_unique<Pane>();
            try {
                WorkScope create{EntryPaneCreate};
                pane->Create(hwnd);
            } catch (...) {
                auto error = to_hresult();
                if (DiagnosticsEnabled()) {
                    try { throw; }
                    catch (hresult_error const& detail) { fwprintf(stderr, L"WinUIPane create hr=0x%08X message=%s\n", unsigned(error), detail.message().c_str()); }
                    catch (...) {}
                }
                pane->Close();
                return error;
            }
            found = s.panes.emplace(hwnd, std::move(pane)).first;
        }
        auto& pane = *found->second;
        auto site = pane.source.SiteBridge();
        HWND child = GetWindowFromWindowId(site.WindowId());
        auto& work = geometryWork();
        ++work.updates;
        const RECT requested{rect->left, rect->top, rect->right, rect->bottom};
        if (!pane.boundsApplied || !EqualRect(&pane.appliedBounds, &requested)) {
            operation = "move-island";
            site.MoveAndResize({rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top});
            // Caption and passthrough rectangles use owner-client coordinates.
            // A position-only island move need not cause any XAML layout event.
            pane.strip->captionDirty = true;
            operation = "clip-caption";
            pane.windowDrag.SetClip(requested, pane.appliedExclusions);
            pane.appliedBounds = requested;
            pane.boundsApplied = true;
            ++work.geometryApplied;
        }
        operation = "restack";
        // Firefox's compositor is a sibling HWND covering the client area: below an island, above
        // the Window's XAML. Ask the window where things actually are: a restack by anything else is undone.
        if (visible && pane.source.Restack()) ++work.restacks;
        NativeWindowHost::KeepOrder(hwnd);
        operation = "show-island";
        if (pane.appliedVisible != int(bool(visible))) {
            if (visible)
                site.Show();
            else
                site.Hide();
            pane.appliedVisible = int(bool(visible));
            ++work.visibilityApplied;
        }
        operation = "update-caption";
        pane.windowDrag.SetVisible(bool(visible));
        pane.ApplyPageRegion();
        scope.Stop();
        if (DiagnosticsEnabled()) {
            RECT actual{};
            GetClientRect(child, &actual);
            fprintf(stderr, "WinUIPane geometry requested=%ld,%ld,%ld,%ld actual=%ld,%ld visible=%d top=%d\n",
                    rect->left, rect->top, rect->right, rect->bottom, actual.right, actual.bottom,
                    IsWindowVisible(child), GetWindow(child, GW_HWNDPREV) == nullptr);
        }
        return S_OK;
    } catch (hresult_error const& error) {
        fwprintf(stderr, L"WinUIPane update hr=0x%08X operation=%hs iconic=%d rect=%ld,%ld,%ld,%ld message=%s\n", unsigned(error.code()), operation, IsIconic(hwnd), rect ? rect->left : 0, rect ? rect->top : 0, rect ? rect->right : 0, rect ? rect->bottom : 0, error.message().c_str());
        return error.code();
    } catch (...) {
        return to_hresult();
    }
}
HRESULT WINAPI Clip(HWND hwnd, const RECT* exclusions, uint32_t count) noexcept {
    WorkScope scope{EntryClip};
    try {
        check_hresult(Check(hwnd));
        if (!hwnd || (count && !exclusions) || count > 1024) return E_INVALIDARG;
        for (uint32_t i = 0; i < count; ++i) {
            if (exclusions[i].right < exclusions[i].left || exclusions[i].bottom < exclusions[i].top)
                return E_INVALIDARG;
        }
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return count ? E_INVALIDARG : S_OK;
        auto& pane = *found->second;
        RECT bounds = pane.appliedBounds;
        // Exclusions are strip-relative: only the strip's size can change what they cut. A move is
        // followed by UpdateTabStrip, which places the caption and page region again.
        const RECT size{0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top};
        auto& work = geometryWork();
        ++work.clips;
        if (pane.clipApplied && EqualRect(&pane.appliedClipClient, &size) &&
            pane.appliedExclusions.size() == count &&
            std::equal(pane.appliedExclusions.begin(), pane.appliedExclusions.end(), exclusions,
                       [](RECT const& a, RECT const& b) { return EqualRect(&a, &b) != 0; }))
            return S_OK;
        pane.appliedExclusions.assign(exclusions, exclusions + count);
        pane.appliedClipClient = size;
        pane.clipApplied = true;
        pane.source.Clip(pane.appliedExclusions);
        pane.windowDrag.SetClip(bounds, pane.appliedExclusions);
        pane.ApplyPageRegion();
        ++work.clipsApplied;
        scope.Stop();
        if (DiagnosticsEnabled())
            fprintf(stderr, "WinUIPane clip exclusions=%u visible=%d\n", count, int(pane.source.SiteBridge().Visible()));
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
// Publication only, since v13: the address surface's intents and its row
// visibility are queued and drained through the request path instead.
HRESULT WINAPI SyncAddress(HWND hwnd, const wchar_t* input) noexcept {
    WorkScope scope{EntrySyncAddress};
    try {
        check_hresult(Check(hwnd));
        if (!input || wcsnlen(input, 1048577) > 1048576) return E_INVALIDARG;
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto snapshot = NativeAddress::JObject::Parse(input);
        if (!found->second->address) return snapshot.GetNamedBoolean(L"enabled", false) ? E_NOTIMPL : S_OK;
        auto& pane = *found->second;
        if(pane.startupRender) {
            if(!pane.startupAddressApply || !pane.startupAddressApply(snapshot))return S_OK;
        } else {
            pane.address->observeFocus();pane.address->Sync(snapshot);
        }
        if (pane.addressSource) {
            const bool backdrop = snapshot.GetNamedBoolean(L"enabled", false) &&
                                  snapshot.GetNamedBoolean(L"navigationToolbar", false);
            if (backdrop != pane.navigationBackdrop) {
                if (backdrop) Pane::SetBackdrop(pane.addressSource, pane.strip->backdrop);
                else pane.addressSource.SystemBackdrop(nullptr);
                pane.navigationBackdrop = backdrop;
            }
        }
        return S_OK;
    } catch (winrt::hresult_error const& error) {
        if (DiagnosticsEnabled())
            fprintf(stderr, "WinUIPane syncAddress failed hr=%08x: %ls\n", unsigned(error.code().value), error.message().c_str());
        return error.code();
    } catch (...) {
        return to_hresult();
    }
}
HRESULT WINAPI UpdateAddress(HWND hwnd, const RECT* rect, BOOL visible) noexcept {
    try {
        check_hresult(Check(hwnd));
        if (!rect || rect->right < rect->left || rect->bottom < rect->top) return E_INVALIDARG;
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return visible ? E_NOT_VALID_STATE : S_OK;
        auto& pane = *found->second;
        if (!visible) {
            // Firefox initializes its retained toolbar before it binds the
            // early presenter. Its provisional hide must not replace the live
            // local editor. Explicit post-binding hide and teardown still close it.
            if(pane.startupPresenter && !pane.startupContext && GetPropW(hwnd,L"Fenn.InteractiveStartup"))return S_OK;
            pane.CloseAddressHost(); return S_OK;
        }
        if (!pane.strip) return E_NOTIMPL;
        if (!pane.addressSource) {
            pane.addressSource = XamlSlot::New();
            pane.addressSource.Initialize(GetWindowIdFromWindow(hwnd));
            pane.address = std::make_unique<NativeAddress>(pane.shell);
            pane.address->navigation->productMenu.updates->owner = hwnd;
            pane.WireAddress();
            pane.addressTakeFocus = pane.addressSource.TakeFocusRequested([&pane](auto const&, auto const&) {
                SetFocus(NativeWindowHost::PageWindow(pane.parent));
                pane.address->LeaveForTraversal((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            });
            pane.addressSource.Content(pane.address->toolbarRoot);
            pane.addressWindow = GetWindowFromWindowId(pane.addressSource.SiteBridge().WindowId());
            check_bool(SetWindowSubclass(pane.addressWindow, Pane::FocusProc, reinterpret_cast<UINT_PTR>(&pane),
                                         reinterpret_cast<DWORD_PTR>(&pane)));
        }
        if (!pane.addressIsland && pane.address->root.XamlRoot())
            pane.addressIsland = pane.address->root.XamlRoot().ContentIsland();
        auto site = pane.addressSource.SiteBridge();
        if (!pane.addressBoundsApplied || !EqualRect(&pane.addressBounds, rect)) {
            site.MoveAndResize({rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top});
            pane.addressBounds = *rect;
            pane.addressBoundsApplied = true;
        }
        pane.addressSource.BringToTop();
        NativeWindowHost::KeepOrder(hwnd);
        site.Show();
        pane.ApplyPageRegion();
        pane.address->PlacePopup();
        return S_OK;
    } catch (...) {
        const auto error = to_hresult();
        auto found = state().panes.find(hwnd);
        if (found != state().panes.end()) found->second->CloseAddressHost();
        return error;
    }
}
HRESULT WINAPI MeasureChromeLayout(HWND hwnd, uint32_t stripHeight, BOOL navigation,
                                  uint32_t contentInset, WinUIChromeLayout* output) noexcept {
    if (!output) return E_POINTER;
    ShellChromeLayout plan{};
    const auto result = MeasureNativeChromeLayout(hwnd, stripHeight, !!navigation, contentInset, &plan);
    if (FAILED(result)) return result;
    auto rect = [](ShellChromeRect r) -> RECT { return {r.x, r.y, r.x + r.width, r.y + r.height}; };
    *output = {plan.fits, rect(plan.strip), rect(plan.navigation), rect(plan.content)};
    return S_OK;
}
HRESULT WINAPI FocusAddress(HWND hwnd, BOOL selectAll) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->addressSource) return E_NOT_VALID_STATE;
        auto& pane = *found->second;
        pane.address->interaction.Interrupt(false);
        pane.address->firefoxFocusRequest = !pane.address->EditorOwned();
        pane.addressSource.NavigateFocus(XamlSourceFocusNavigationRequest(XamlSourceFocusNavigationReason::First));
        if (!pane.address->field.Focus(FocusState::Programmatic)) return E_FAIL;
        if (selectAll) pane.address->field.SelectAll();
        return S_OK;
    } catch (...) { return to_hresult(); }
}
// Publication only, since v12: nothing native travels back through this call.
// Every native intent -- menu transitions, selection, and the strip's own
// gestures -- is queued and drained through the request path instead.
void EndPageHold(HWND hwnd);
HRESULT WINAPI SyncTabMenu(HWND hwnd, const wchar_t* input) noexcept {
    WorkScope scope{EntrySyncTabMenu};
    try {
        check_hresult(Check(hwnd));
        if (!input || wcsnlen(input, 1048577) > 1048576) return E_INVALIDARG;
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto snapshot = NativeTabMenu::Object::Parse(input);
        auto& pane = *found->second;
        if (snapshot.HasKey(L"startupPagePainted")) { EndPageHold(hwnd); return S_OK; }
        if (snapshot.HasKey(L"strip")) {
            if(pane.startupRender)return S_OK;
            // Strip publication is a separate transaction: validate the envelope
            // before touching XAML, and never drain any queued input here.
            if (!pane.strip || snapshot.Size() != 3 || snapshot.GetNamedNumber(L"version") != 2 ||
                !snapshot.GetNamedBoolean(L"publishOnly", false))
                return E_INVALIDARG;
            pane.strip->Publish(snapshot.GetNamedObject(L"strip"));
            return S_OK;
        }
        if (snapshot.HasKey(L"groupCreation")) {
            if(pane.startupRender)return S_OK;
            if(!pane.strip || snapshot.Size()!=3 || snapshot.GetNamedNumber(L"version")!=2 || !snapshot.GetNamedBoolean(L"publishOnly",false))return E_INVALIDARG;
            pane.strip->RequestGroupCreation(snapshot.GetNamedObject(L"groupCreation"));return S_OK;
        }
        if (snapshot.HasKey(L"allTabs")) {
            if(pane.startupRender)return S_OK;
            if(!pane.strip || snapshot.Size()!=3 || snapshot.GetNamedNumber(L"version")!=2 || !snapshot.GetNamedBoolean(L"publishOnly",false))return E_INVALIDARG;
            pane.strip->allTabsPanel.Apply(snapshot.GetNamedObject(L"allTabs"));return S_OK;
        }
        if (snapshot.HasKey(L"groupHeader")) {
            if(pane.startupRender)return S_OK;
            if (!pane.strip || snapshot.Size()!=3 || snapshot.GetNamedNumber(L"version")!=2 || !snapshot.GetNamedBoolean(L"publishOnly",false)) return E_INVALIDARG;
            pane.strip->groupHeader.core=pane.shell.core;
            pane.strip->groupHeader.Apply(snapshot.GetNamedObject(L"groupHeader"));return S_OK;
        }
        if (snapshot.HasKey(L"tabPreview")) {
            if(pane.startupRender)return S_OK;
            // Firefox's answer to a hover-card request: also its own envelope,
            // validated in full before the card changes.
            if (!pane.strip || snapshot.Size() != 3 || snapshot.GetNamedNumber(L"version") != 2 ||
                !snapshot.GetNamedBoolean(L"publishOnly", false))
                return E_INVALIDARG;
            pane.strip->ApplyPreview(snapshot.GetNamedObject(L"tabPreview"));
            return S_OK;
        }
        // A publication is the only thing that moves the published selection
        // now, so apply it here rather than waiting for a snapshot. The
        // previous set is what makes the apply a diff instead of a rescan.
        auto previousSelection = pane.tabMenu.selectedTabs;
        auto previousRevision = pane.tabMenu.StateRevision();
        pane.tabMenu.Sync(snapshot);
        if (pane.tabMenu.StateRevision() != previousRevision) pane.ApplyPublishedSelection(previousSelection);
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
// Publication only, since v14: close and new-tab joined the request path, and
// the reserved context/command pair went with them, so nothing is drained here.
bool ValidTabPublication(const WinUITab* tabs, uint32_t count, uint32_t selected, uint32_t revision) {
    if (!revision || (count && !tabs) || count > 10000) return false;
    std::set<uint32_t> ids;
    for (uint32_t i = 0; i < count; ++i) {
        if (!tabs[i].id || !tabs[i].title || !ids.insert(tabs[i].id).second ||
            wcsnlen(tabs[i].title, 32769) > 32768 || (tabs[i].commands & ~31u) || (tabs[i].commands & 12u) == 12u)
            return false;
    }
    return !selected || ids.count(selected);
}
HRESULT WINAPI SyncTabs(HWND hwnd, const WinUITab* tabs, uint32_t count, uint32_t selected,
                        uint32_t revision, uint32_t* requested) noexcept {
    WorkScope scope{EntrySyncTabs};
    try {
        check_hresult(Check(hwnd));
        if (!requested) return E_POINTER;
        *requested = 0;
        if (!hwnd || !ValidTabPublication(tabs, count, selected, revision)) return E_INVALIDARG;
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto& pane = *found->second;
        check_hresult(pane.Sync(tabs, count, selected, revision));
        // Acknowledge Firefox's publication stamp for the next delta's base.
        *requested = pane.ModelRevision();
        pane.ArmPresentation();
        return S_OK;
    } catch (hresult_error const& error) {
        fwprintf(stderr, L"WinUIPane tabs hr=0x%08X message=%s\n", unsigned(error.code()), error.message().c_str());
        return error.code();
    } catch (...) {
        return to_hresult();
    }
}
// --- ABI v11: the event-driven request path -----------------------------
//
// One endpoint, one wakeup. The pane calls the sink only when its queue goes
// from "nothing outstanding" to "work waiting"; the host answers with exactly
// one drainRequests/completeRequests pair, and completeRequests decides whether
// another wakeup is owed. Nothing here polls, and nothing here calls the host
// back except through the sink.
HRESULT WINAPI SetNotifySink(HWND hwnd, WinUIPaneNotifyFn notify, void* context) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto& pane = *found->second;
        pane.notifySink = notify;
        pane.notifyContext = context;
        NotifyStartup(pane);
        // A sink arriving after work was already queued must not strand it: the
        // queue reports whether a wakeup is still owed.
        if (notify && pane.shell.NotificationFailed()) pane.ScheduleDrain();
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
HRESULT WINAPI DrainRequests(HWND hwnd, wchar_t* output, uint32_t capacity) noexcept {
    WorkScope scope{EntryDrainRequests};
    try {
        check_hresult(Check(hwnd));
        if (!output || capacity < 1 || capacity > 262144) return E_INVALIDARG;
        output[0] = 0;
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto& pane = *found->second;
        if (pane.drainOpen) {
            // The host opened a drain and never closed it. Close it rather than
            // leave the endpoint busy forever, and count it: a nonzero count is
            // a host bug, not normal operation.
            ++pane.abandonedDrains;
            if (pane.shell.EndDrain()) pane.ScheduleDrain();
            pane.drainOpen = false;
        }
        if (!pane.shell.BeginDrain()) return S_FALSE;  // already draining, or blocked
        pane.drainOpen = true;
        // Bounded batch: entries and bytes, so one drain cannot monopolise the
        // browser's main thread however much input accumulated.
        constexpr uint32_t kMaxBatchEntries = 16;
        constexpr size_t kMaxBatchBytes = 64 * 1024;
        auto batch = pane.shell.TakeBatch(kMaxBatchEntries, kMaxBatchBytes);
        if (!batch.size()) {
            // Nothing to do; close the drain here so the host need not answer.
            if (pane.shell.EndDrain()) pane.ScheduleDrain();
            pane.drainOpen = false;
            return S_FALSE;
        }
        if (WorkAccountingEnabled()) ++pane.drainSequence;
        NativeTabMenu::Array requests;
        for (size_t i = 0; i < batch.size(); ++i) {
            auto const& request = batch[i];
            NativeTabMenu::Object entry;
            // The id is a double on the wire, matching every other counter that
            // crosses this boundary; the queue keeps ids well inside 2^53.
            entry.SetNamedValue(L"id", NativeTabMenu::Value::CreateNumberValue(double(request.id)));
            if (WorkAccountingEnabled()) {
                const double now = NowMs();
                auto queued = pane.queuedAt.find(request.id);
                if (queued != pane.queuedAt.end())
                    RecordStage(StageWakeup, now - queued->second.at, Classify(pane, queued->second),
                                pumpStallTotal() - queued->second.pumpStall);
                while (pane.drainedAt.size() >= 256) pane.drainedAt.erase(pane.drainedAt.begin());
                pane.drainedAt[request.id] = now;
            }
            entry.SetNamedValue(L"kind", NativeTabMenu::Value::CreateNumberValue(request.kind));
            entry.SetNamedValue(L"tab", NativeTabMenu::Value::CreateNumberValue(request.tab));
            entry.SetNamedValue(L"revision", NativeTabMenu::Value::CreateNumberValue(request.context_revision));
            auto payload = batch.payload(i);
            entry.SetNamedValue(L"action", NativeTabMenu::Object::Parse(hstring(payload)));
            requests.Append(entry);
        }
        NativeTabMenu::Object envelope;
        envelope.SetNamedValue(L"version", NativeTabMenu::Value::CreateNumberValue(1));
        envelope.SetNamedValue(L"generation",
                               NativeTabMenu::Value::CreateNumberValue(double(pane.shell.Stats().generation)));
        envelope.SetNamedValue(L"requests", requests);
        auto text = envelope.Stringify();
        if (text.size() >= capacity) {
            // The host cannot receive this batch. Close the drain and report
            // failure rather than silently dropping accepted requests; they
            // stay in flight and are cancelled by the endpoint's teardown.
            if (pane.shell.EndDrain()) pane.ScheduleDrain();
            pane.drainOpen = false;
            return E_NOT_SUFFICIENT_BUFFER;
        }
        wcscpy_s(output, capacity, text.c_str());
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
// Opens a drain ahead of Firefox's read, unless the last batch is still unanswered.
HRESULT PrepareDrain(HWND hwnd, wchar_t* output, uint32_t capacity) noexcept {
    auto found = state().panes.find(hwnd);
    if (found == state().panes.end() || found->second->drainOpen) return S_FALSE;
    return DrainRequests(hwnd, output, capacity);
}
// A posted publication has no caller left to fail to, so a refusal returns as a request.
// Firefox answers it with its usual recovery: one complete publication after a refused
// delta, or the surface's fallback after a refused complete publication.
void ReportRejectedPublication(HWND hwnd, const wchar_t* channel, bool delta, hstring const& request={}) {
    auto found=state().panes.find(hwnd);
    if(found==state().panes.end())return;
    NativeTabMenu::Object action;
    action.Insert(L"kind",winrt::Windows::Data::Json::JsonValue::CreateStringValue(L"publication-rejected"));
    action.Insert(L"channel",winrt::Windows::Data::Json::JsonValue::CreateStringValue(channel));
    action.Insert(L"delta",winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(delta));
    if(!request.empty())action.Insert(L"request",winrt::Windows::Data::Json::JsonValue::CreateStringValue(request));
    auto& pane=*found->second;
    pane.Enqueue(NativeShellCore::StripAction,0,pane.strip?pane.strip->presentationRevision:0,action.Stringify());
}
HRESULT WINAPI PostedSyncAddress(HWND hwnd, const wchar_t* input) noexcept {
    const auto result=SyncAddress(hwnd,input);
    if(FAILED(result))try{ReportRejectedPublication(hwnd,L"address",false);}catch(...){}
    return result;
}
HRESULT WINAPI PostedSyncTabMenu(HWND hwnd, const wchar_t* input) noexcept {
    const auto result=SyncTabMenu(hwnd,input);
    if(FAILED(result))try{
        const wchar_t* channel=L"other";bool delta=false;hstring request;
        NativeTabMenu::Object snapshot{nullptr};
        if(NativeTabMenu::Object::TryParse(input,snapshot)) {
            if(snapshot.HasKey(L"strip")){channel=L"strip";delta=snapshot.GetNamedObject(L"strip").GetNamedNumber(L"base",0)!=0;}
            else if(snapshot.HasKey(L"groupCreation")){channel=L"groupCreation";request=winrt::to_hstring(snapshot.GetNamedObject(L"groupCreation").GetNamedNumber(L"request",0));}
            else if(snapshot.HasKey(L"state")){channel=L"context";delta=snapshot.GetNamedObject(L"state").GetNamedNumber(L"base",0)!=0;}
        }
        ReportRejectedPublication(hwnd,channel,delta,request);
    }catch(...){}
    return result;
}
HRESULT WINAPI ReportedUpdateTabStrip(HWND hwnd, const RECT* rect, BOOL visible) noexcept {
    const auto result=UpdateTabStrip(hwnd,rect,visible);
    if(FAILED(result))try{ReportRejectedPublication(hwnd,L"stripHost",false);}catch(...){}
    return result;
}
HRESULT WINAPI ReportedClip(HWND hwnd, const RECT* rects, uint32_t count) noexcept {
    const auto result=Clip(hwnd,rects,count);
    if(FAILED(result))try{ReportRejectedPublication(hwnd,L"stripHost",false);}catch(...){}
    return result;
}
HRESULT WINAPI ReportedUpdateAddress(HWND hwnd, const RECT* rect, BOOL visible) noexcept {
    const auto result=UpdateAddress(hwnd,rect,visible);
    if(FAILED(result))try{ReportRejectedPublication(hwnd,L"addressHost",false);}catch(...){}
    return result;
}
HRESULT WINAPI CompleteRequests(HWND hwnd, const wchar_t* input) noexcept {
    WorkScope scope{EntryCompleteRequests};
    try {
        check_hresult(Check(hwnd));
        if (!input || wcsnlen(input, 1048577) > 1048576) return E_INVALIDARG;
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto& pane = *found->second;
        auto message = NativeTabMenu::Object::Parse(input);
        if (NativeJson::RequireId(message, L"version") != 1) return E_INVALIDARG;
        auto generation = message.GetNamedNumber(L"generation");
        auto outcomes = message.GetNamedArray(L"outcomes");
        if (outcomes.Size() > 64) return E_INVALIDARG;
        // An endpoint whose generation has moved on (a replaced session) must
        // not accept outcomes formed against the old one. An HWND is not a
        // lifetime identity; the generation is. A message carrying no outcomes
        // asserts nothing about identity -- it is the pure block/unblock
        // signal, which the host may send before it has ever drained.
        if (outcomes.Size() && generation != double(pane.shell.Stats().generation)) return E_NOT_VALID_STATE;
        for (auto value : outcomes) {
            auto entry = value.GetObject();
            auto id = entry.GetNamedNumber(L"id");
            auto outcome = NativeJson::RequireId(entry, L"outcome");
            if (!std::isfinite(id) || id < 1 || id > NativeJson::kMaxSafeInteger || std::floor(id) != id ||
                outcome > 4)
                return E_INVALIDARG;
            if (WorkAccountingEnabled()) {
                const double now = NowMs();
                auto drained = pane.drainedAt.find(uint64_t(id));
                if (drained != pane.drainedAt.end()) {
                    RecordStage(StageExecute, now - drained->second);
                    pane.drainedAt.erase(drained);
                }
                auto queued = pane.queuedAt.find(uint64_t(id));
                if (queued != pane.queuedAt.end()) {
                    RecordStage(StageTotal, now - queued->second.at, Classify(pane, queued->second),
                                pumpStallTotal() - queued->second.pumpStall);
                    pane.queuedAt.erase(queued);
                }
            }
            pane.shell.Complete(uint64_t(id), NativeShellCore::Outcome(outcome));
        }
        const bool blocked = message.GetNamedBoolean(L"blocked", false);
        bool notify = false;
        // A block signal is not an answer: a drain open here can be a batch Firefox has yet to read.
        if (pane.drainOpen && !message.GetNamedBoolean(L"signal", false)) {
            notify = pane.shell.EndDrain();
            pane.drainOpen = false;
            if (blocked) {
                pane.shell.SetBlocked(true);
                pane.SetHostBlocked(true);
            }
        } else if (blocked) {
            pane.shell.SetBlocked(true);
            pane.SetHostBlocked(true);
        } else {
            // Unblocking outside a drain: the queue says whether requests are
            // waiting, so the host is woken once instead of being re-polled.
            notify = pane.shell.SetBlocked(false);
            pane.SetHostBlocked(false);
        }
        if (!blocked) {
            if (notify) pane.ScheduleDrain();
        }
        pane.ReconcileOutcomes();
        if (!blocked && pane.address) pane.address->RetryEdit();
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
HRESULT WINAPI ApplyTabDelta(HWND hwnd, const wchar_t* input, uint32_t* revision) noexcept {
    WorkScope scope{EntryApplyTabDelta};
    try {
        check_hresult(Check(hwnd));
        if (!revision) return E_POINTER;
        *revision = 0;
        if (!input || wcsnlen(input, 1048577) > 1048576) return E_INVALIDARG;
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto hr = found->second->ApplyDelta(NativeTabMenu::Object::Parse(input));
        if (SUCCEEDED(hr)) {
            *revision = found->second->ModelRevision();
            found->second->ArmPresentation();
        }
        return hr;
    } catch (...) {
        return to_hresult();
    }
}

HRESULT WINAPI Focus(HWND hwnd, BOOL backwards) noexcept {
    WorkScope scope{EntryFocus};
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        found->second->source.NavigateFocus(XamlSourceFocusNavigationRequest(
            backwards ? XamlSourceFocusNavigationReason::Last : XamlSourceFocusNavigationReason::First));
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
HRESULT WINAPI ReportedFocus(HWND hwnd, BOOL backwards) noexcept {
    const auto result=Focus(hwnd,backwards);
    if(FAILED(result))try{ReportRejectedPublication(hwnd,L"stripHost",false);}catch(...){}
    return result;
}
HRESULT WINAPI ReportedFocusAddress(HWND hwnd, BOOL selectAll) noexcept {
    const auto result=FocusAddress(hwnd,selectAll);
    if(FAILED(result))try{ReportRejectedPublication(hwnd,L"addressFocus",false);}catch(...){}
    return result;
}
HRESULT WINAPI ReportedSyncTabs(HWND hwnd, const WinUITab* tabs, uint32_t count, uint32_t selected, uint32_t revision) noexcept {
    uint32_t acknowledged=0;
    const auto result=SyncTabs(hwnd,tabs,count,selected,revision,&acknowledged);
    if(FAILED(result))try{ReportRejectedPublication(hwnd,L"tabs",false);}catch(...){}
    return result;
}
// S_FALSE applied the delta but asks for a snapshot, so it is reported like a refusal.
HRESULT WINAPI PostedApplyTabDelta(HWND hwnd, const wchar_t* input) noexcept {
    uint32_t acknowledged=0;
    const auto result=ApplyTabDelta(hwnd,input,&acknowledged);
    if(result!=S_OK)try{
        NativeTabMenu::Object delta{nullptr};
        hstring request;
        if(NativeTabMenu::Object::TryParse(input,delta))request=winrt::to_hstring(uint32_t(delta.GetNamedNumber(L"revision",0)));
        ReportRejectedPublication(hwnd,L"tabs",true,request);
    }catch(...){}
    return result;
}
HRESULT WINAPI Destroy(HWND hwnd) noexcept {
    WorkScope scope{EntryDestroy};
    try {
        check_hresult(Check());
        auto& panes = state().panes;
        auto found = panes.find(hwnd);
        if (found != panes.end()) {
            found->second->Close();
            panes.erase(found);
        }
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
BOOL WINAPI Translate(MSG* message) noexcept {
    WorkScope scope{EntryTranslate};
    if (WorkAccountingEnabled()) RecordPumpGap();
    try {
        if (!message || FAILED(Check())) return FALSE;
        // Popup focus and Win32 drag capture have different owners. Consume
        // the actual queued Escape; a short press can end between timer polls.
        if(message->message==WM_KEYDOWN && message->wParam==VK_ESCAPE && dragSession().sourceHwnd &&
           (dragSession().listSource || dragSession().hoverPanel || (dragSession().sourceStrip && dragSession().sourceStrip->allTabsPanel.opening))){AbortDragSession();return TRUE;}

        // Message-agnostic: this is the ground-truth tick a process-wide
        // drag session's correctness depends on (Pane.cpp's TabDragSession/
        // TickDragSession), driven by every message this hook sees rather
        // than by parsing any one of them.
        TickDragSession();
        for (auto const& entry : state().panes) {
            auto& pane = *entry.second;
            if (pane.startupKey && (message->hwnd==pane.parent || IsChild(pane.parent,message->hwnd)) &&
                pane.startupKey(*message)) return TRUE;
            if (pane.address && pane.address->navigation && message->message == WM_KEYDOWN &&
                message->wParam == VK_ESCAPE && GetForegroundWindow() == pane.parent &&
                pane.address->navigation->customization.DismissOnEscape()) return TRUE;
            if (pane.strip && pane.strip->inlineGroups.hover.open &&
                message->message == WM_KEYDOWN && message->wParam == VK_ESCAPE &&
                GetForegroundWindow() == pane.parent) {
                pane.strip->inlineGroups.hover.Dismiss();
                return TRUE;
            }
            if (pane.strip && !pane.strip->inlineGroups.runs.empty() && pane.strip->root.IsLoaded() &&
                GetForegroundWindow() == pane.parent) {
                POINT point{};
                if (GetCursorPos(&point)) pane.strip->inlineGroups.PointerAt(pane.ScreenToStrip(point),pane.strip->view);
            }
            // Over the SDK caption buttons the pointer is non-client, yet the
            // toolbar island still hovers the control at the same x. Take the
            // island out of hit testing while the cursor is on a caption button.
            if (pane.address && pane.address->toolbarRoot) {
                POINT cursor{};
                const bool overCaption = GetCursorPos(&cursor) && pane.windowDrag.SystemButtonAt(cursor) != HTNOWHERE;
                if (overCaption == pane.toolbarHitTestVisible) {
                    pane.toolbarHitTestVisible = !overCaption;
                    pane.address->toolbarRoot.IsHitTestVisible(!overCaption);
                }
            }
            if (!pane.childWindow) {
                pane.childWindow = GetWindowFromWindowId(pane.source.SiteBridge().WindowId());
                check_bool(SetWindowSubclass(pane.childWindow, Pane::FocusProc, reinterpret_cast<UINT_PTR>(&pane),
                                             reinterpret_cast<DWORD_PTR>(&pane)));
            }
            if (!pane.contentIsland && pane.source.Content().XamlRoot())
                pane.contentIsland = pane.source.Content().XamlRoot().ContentIsland();
            switch (message->message) {
                case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
                case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: case WM_NCMBUTTONDOWN: case WM_NCXBUTTONDOWN:
                case WM_POINTERDOWN: case WM_NCPOINTERDOWN:
                    pane.OutsidePress(message->hwnd);
                    break;
                case WM_SETFOCUS: case WM_KILLFOCUS:
                    if (pane.address) pane.address->ScheduleFocusObservation();
                    break;
            }
            if (pane.windowDrag.TranslateContextMenu(*message)) return TRUE;
            HWND child = pane.childWindow;
            bool ownsMessage = message->hwnd == child || IsChild(child, message->hwnd);
            if (!ownsMessage && pane.addressWindow)
                ownsMessage = message->hwnd == pane.addressWindow || IsChild(pane.addressWindow, message->hwnd);
            if (!ownsMessage && pane.addressIsland) {
                for (auto popup : pane.addressIsland.Popups()) {
                    HWND popupWindow = GetWindowFromWindowId(popup.WindowId());
                    if (message->hwnd == popupWindow || IsChild(popupWindow, message->hwnd)) {
                        ownsMessage = true;
                        break;
                    }
                }
            }
            // MenuFlyout can create a top-level popup outside the child HWND.
            // Use WinUI's explicit ownership graph, never all browser/thread HWNDs.
            if (!ownsMessage && (pane.tabMenu.active || pane.address || pane.strip)) {
                // A Window has no XamlRoot until it is first shown, and so no popups yet either.
                if (!pane.contentIsland)
                    if (auto root = pane.source.Content().XamlRoot()) pane.contentIsland = root.ContentIsland();
                if (pane.contentIsland) for (auto popup : pane.contentIsland.Popups()) {
                    HWND popupWindow = GetWindowFromWindowId(popup.WindowId());
                    if (message->hwnd == popupWindow || IsChild(popupWindow, message->hwnd)) {
                        ownsMessage = true;
                        break;
                    }
                }
            }
            if (!ownsMessage) continue;
            if ((message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN) &&
                !(pane.address && pane.address->composing) && !NativeAltGrTextKey(UINT(message->wParam))) {
                const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool addressShortcut = pane.addressSource &&
                    (message->wParam == VK_F6 || message->wParam == VK_F5 || message->wParam == VK_F11 ||
                     (control && (message->wParam == 'K' || message->wParam == 'R')) ||
                     ((GetKeyState(VK_MENU) & 0x8000) &&
                      (message->wParam == 'D' || message->wParam == VK_LEFT || message->wParam == VK_RIGHT)));
                if (addressShortcut || (control && (message->wParam == 'L' || message->wParam == 'T' || message->wParam == 'W' ||
                                (pane.strip && (message->wParam == VK_TAB || message->wParam == VK_PRIOR ||
                                                message->wParam == VK_NEXT))))) {
                    const HWND page = NativeWindowHost::PageWindow(pane.parent);
                    SetFocus(page);
                    PostMessageW(page, message->message, message->wParam, message->lParam);
                    return TRUE;
                }
            }
            return ContentPreTranslateMessage(message);
        }
    } catch (...) {}
    return FALSE;
}
HRESULT WINAPI Shutdown() noexcept {
    try {
        check_hresult(Check());
        if (WorkAccountingEnabled()) {
            for (auto* entry = nativeWork(); entry->name; ++entry) {
                if (!entry->calls) continue;
                fwprintf(stderr, L"WinUIPane native-work entry=%s calls=%llu ms=%.3f maxMs=%.3f meanMs=%.4f\n",
                         entry->name, entry->calls, entry->totalMs, entry->maxMs,
                         entry->totalMs / double(entry->calls));
            }
            fwprintf(stderr, L"WinUIPane pump-gap maxMs=%.3f\n", pumpGapMax());
            for (auto* stage = requestLatency(); stage->work.name; ++stage) {
                if (!stage->work.calls && !stage->blocked && !stage->deferred) continue;
                if (!stage->work.calls) {
                    fwprintf(stderr, L"WinUIPane request-latency stage=%s n=0 blocked=%llu deferred=%llu (no "
                                     L"sample was this stage's own to answer for)\n",
                             stage->work.name, stage->blocked, stage->deferred);
                    continue;
                }
                fwprintf(stderr, L"WinUIPane request-latency stage=%s n=%llu blocked=%llu deferred=%llu stalled=%llu ms=%.3f"
                                 L" maxMs=%.3f meanMs=%.4f le1=%llu le4=%llu le16=%llu le64=%llu le256=%llu"
                                 L" over=%llu\n",
                         stage->work.name, stage->work.calls, stage->blocked, stage->deferred, stage->stalled,
                         stage->work.totalMs,
                         stage->work.maxMs, stage->work.totalMs / double(stage->work.calls), stage->buckets[0],
                         stage->buckets[1], stage->buckets[2], stage->buckets[3], stage->buckets[4],
                         stage->buckets[5]);
            }
            auto& work = geometryWork();
            fprintf(stderr,
                    "WinUIPane geometry-work updates=%llu geometryApplied=%llu restacks=%llu "
                    "visibilityApplied=%llu clips=%llu clipsApplied=%llu\n",
                    work.updates, work.geometryApplied, work.restacks, work.visibilityApplied, work.clips,
                    work.clipsApplied);
        }
        auto& s = state();
        s.stopped = true;
        NativeStartupSurface::Close(nullptr);
        for (auto& entry : s.panes) entry.second->Close();
        s.panes.clear();
        if (s.manager) {
            s.manager.Close();
            s.manager = nullptr;
        }
        if (s.dispatcher) {
            s.dispatcher.ShutdownQueue();
            s.dispatcher = nullptr;
        }
        s.app = nullptr;
        clear_factory_cache();
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
}  // namespace

HRESULT BeginStartupPresenter(HWND hwnd) noexcept;
bool FinishStartupPresenter(HWND hwnd);
HRESULT PublishStartupPresentation(HWND hwnd, Windows::Data::Json::JsonObject const& message);
#include "StartupTransfer.h"
#include "IndependentHost.h"
#include "LocalStartup.h"
HRESULT BeginStartupPresenter(HWND hwnd) noexcept {
    try {
        auto found=state().panes.find(hwnd);
        if(found==state().panes.end())return E_INVALIDARG;
        auto& pane=*found->second;
        if(pane.startupPresenter)return S_OK;
        if(!pane.address || !pane.strip || !pane.addressSource)return E_NOT_VALID_STATE;
        if(pane.address->composing)return E_PENDING;
        if(pane.strip->IsDragging() || pane.strip->currentGroups.Size())return E_NOTIMPL;
        for(auto const& [id,meta]:pane.strip->presentation)
            if((meta.flags&13) || !meta.containerName.empty() || meta.containerColor)return E_NOTIMPL;
        auto presenter=std::make_shared<LocalStartup::Presenter>(pane,true);
        presenter->Initialize();
        pane.startupPresenter=std::move(presenter);
        return S_OK;
    } catch (...) {return to_hresult();}
}

bool FinishStartupPresenter(HWND hwnd) {
    auto& pane=*state().panes.at(hwnd);
    auto presenter=std::static_pointer_cast<LocalStartup::Presenter>(pane.startupPresenter);
    if(!presenter)return true;
    if(!presenter->Finish())return false;
    pane.startupPresenter.reset();return true;
}

void ApplyStartupDirection(Pane& pane, bool rtl) {
    const auto flow=rtl?FlowDirection::RightToLeft:FlowDirection::LeftToRight;
    if(pane.strip->root.FlowDirection()!=flow)pane.strip->root.FlowDirection(flow);
    if(pane.address->root.FlowDirection()!=flow)pane.address->root.FlowDirection(flow);
    if(pane.address->toolbarRoot && pane.address->toolbarRoot.FlowDirection()!=flow)pane.address->toolbarRoot.FlowDirection(flow);
    if(pane.address->popupContent && pane.address->popupContent.FlowDirection()!=flow)pane.address->popupContent.FlowDirection(flow);
    // Preserve URL/editor character order and avoid resetting live composition.
    if(pane.address->field.TextReadingOrder()!=TextReadingOrder::DetectFromContent)
        pane.address->field.TextReadingOrder(TextReadingOrder::DetectFromContent);
    if(pane.address->field.TextAlignment()!=TextAlignment::Left)pane.address->field.TextAlignment(TextAlignment::Left);
}
HRESULT PublishStartupPresentation(HWND hwnd, Windows::Data::Json::JsonObject const& message) {
    auto& pane=*state().panes.at(hwnd);
    auto presenter=std::static_pointer_cast<LocalStartup::Presenter>(pane.startupPresenter);
    if(!presenter)return E_NOT_VALID_STATE;
    auto text=[&](wchar_t const* key){auto value=message.GetNamedString(key);
        if(value.empty())throw_hresult(E_INVALIDARG);
        NativeJson::RequireLength(value,NativeJson::kMaxShortLabel);return value;};
    LocalStartup::Presenter::Labels labels{text(L"newTab"),text(L"pending"),text(L"full"),text(L"composition")};
    const auto address=text(L"address"),unavailable=text(L"unavailable");
    const bool blocked=message.GetNamedBoolean(L"blocked");
    const auto recovery=message.HasKey(L"recovery")?text(L"recovery"):hstring{L"Startup needs attention"};
    const auto recoveryAction=message.HasKey(L"recoveryAction")?text(L"recoveryAction"):hstring{L"Use native layout"};
    const bool recoverable=message.GetNamedBoolean(L"recoverable",false);
    const bool rtl=message.GetNamedBoolean(L"rtl",false);
    auto id=[&](wchar_t const* key){auto value=message.GetNamedString(key);
        if(value.empty() || value.size()>20 || (value.size()>1 && value[0]==L'0'))throw_hresult(E_INVALIDARG);
        uint64_t result{};for(auto c:value){if(c<L'0'||c>L'9'||result>(UINT64_MAX-(c-L'0'))/10)throw_hresult(E_INVALIDARG);result=result*10+c-L'0';}return result;};
    const auto json=message.Stringify();bool apply{};
    if(shell_core_startup_presentation(pane.shell.core,id(L"session"),id(L"revision"),
        reinterpret_cast<const uint16_t*>(json.c_str()),json.size(),&apply)!=SHELL_STATUS_OK)return E_INVALIDARG;
    if(!apply)return S_OK;
    ApplyStartupDirection(pane,rtl);
    presenter->labels=labels;presenter->blocked=blocked?unavailable:hstring{};
    presenter->Recovery(recovery,recoveryAction,blocked && recoverable,
        message.GetNamedString(L"session"),message.GetNamedString(L"revision"));
    if(pane.address->field.PlaceholderText()!=address)pane.address->field.PlaceholderText(address);
    Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(pane.address->field,address);
    auto stripLabels=pane.strip->labels;stripLabels.newTab=labels.newTab;stripLabels.newTabTip=labels.newTab;
    pane.strip->ApplyLabels(stripLabels);
    presenter->Refresh(false);presenter->Feedback(SHELL_STATUS_OK);
    return S_OK;
}

namespace {
inline constexpr wchar_t InteractiveStartupProperty[]=L"Fenn.InteractiveStartup";
inline constexpr wchar_t PageHoldProperty[]=L"Fenn.NativeStartupPageHold";
inline constexpr UINT_PTR PageHoldTimer=0x46454E;
inline constexpr UINT PageHoldLimitMs=10000;
// The compositor window is white until Gecko first presents. While this property is set it
// joins the host hidden (FennCompositorParent.h), leaving the themed host showing, until the
// selected tab has painted.
void EndPageHold(HWND hwnd) {
    if(!RemovePropW(hwnd,PageHoldProperty))return;
    KillTimer(hwnd,PageHoldTimer);
    for(HWND child=FindWindowExW(hwnd,nullptr,L"MozillaCompositorWindowClass",nullptr);child;
        child=FindWindowExW(hwnd,child,L"MozillaCompositorWindowClass",nullptr))
        SetWindowPos(child,nullptr,0,0,0,0,SWP_SHOWWINDOW|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS);
    NativeWindowHost::FocusTrace("start-page-revealed");
}
void BeginPageHold(HWND hwnd) {
    SetPropW(hwnd,PageHoldProperty,reinterpret_cast<HANDLE>(1));
    SetTimer(hwnd,PageHoldTimer,PageHoldLimitMs,nullptr);
}
HRESULT LayoutInteractiveStartup(HWND hwnd) noexcept {
    WinUIChromeLayout layout{};
    auto result=MeasureChromeLayout(hwnd,40,TRUE,0,&layout);
    if(FAILED(result))return result;
    if(!layout.fits)return S_FALSE;
    result=UpdateTabStrip(hwnd,&layout.strip,TRUE);
    return FAILED(result)?result:UpdateAddress(hwnd,&layout.navigation,TRUE);
}
LRESULT CALLBACK InteractiveStartupProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    if(msg==WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd,InteractiveStartupProc,id);
        RemovePropW(hwnd,InteractiveStartupProperty);
        RemovePropW(hwnd,PageHoldProperty);
        Destroy(hwnd);
    } else if(msg==WM_TIMER && w==PageHoldTimer) {
        EndPageHold(hwnd);
        return 0;
    } else if(msg==WM_SIZE || msg==WM_DPICHANGED) {
        // Gecko owns subsequent bounds publications. Before attachment, the
        // native thread lays out these same islands without a browser callback.
        const auto* host=NativeWindowHost::Get(hwnd);
        if(host && host->state.phase==6) {
            if(msg==WM_DPICHANGED) {
                const auto& bounds=*reinterpret_cast<const RECT*>(l);
                SetWindowPos(hwnd,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
                    SWP_NOZORDER|SWP_NOACTIVATE);
            }
            LayoutInteractiveStartup(hwnd);
        }
    }
    return DefSubclassProc(hwnd,msg,w,l);
}
HRESULT ShowInteractiveStartup(HWND hwnd, const wchar_t* profile, const wchar_t* presentation) noexcept {
    try {
        if(GetPropW(hwnd,InteractiveStartupProperty))return S_FALSE;
        check_hresult(LayoutInteractiveStartup(hwnd));
        NativeWindowHost::FocusTrace("start-layout");
        check_hresult(BeginStartupPresenter(hwnd));
        NativeWindowHost::FocusTrace("start-presenter");
        // An island without a backdrop is white in either theme. Firefox's first
        // publication confirms this or takes it away if the toolbar is opted out.
        {
            auto& ready=*state().panes.at(hwnd);
            Pane::SetBackdrop(ready.addressSource,ready.strip->backdrop);
            ready.navigationBackdrop=true;
        }
        bool initialRtl{};
        auto initialPresentation=NativeStartupLabels::Resolve(presentation,profile,initialRtl);
        ApplyStartupDirection(*state().panes.at(hwnd),initialRtl);
        if(initialPresentation) {
            auto labels=initialPresentation.GetNamedObject(L"labels");
            auto& pane=*state().panes.at(hwnd);
            auto presenter=std::static_pointer_cast<LocalStartup::Presenter>(pane.startupPresenter);
            presenter->labels={labels.GetNamedString(L"newTab"),labels.GetNamedString(L"pending"),
                labels.GetNamedString(L"full"),labels.GetNamedString(L"composition")};
            const auto address=labels.GetNamedString(L"address");
            pane.address->field.PlaceholderText(address);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(pane.address->field,address);
            auto stripLabels=pane.strip->labels;stripLabels.newTab=presenter->labels.newTab;stripLabels.newTabTip=presenter->labels.newTab;
            pane.strip->ApplyLabels(stripLabels);presenter->Refresh(false);
        }
        NativeWindowHost::FocusTrace("start-presentation");
        check_bool(SetWindowSubclass(hwnd,InteractiveStartupProc,1,0));
        check_bool(SetPropW(hwnd,InteractiveStartupProperty,reinterpret_cast<HANDLE>(1)));
#ifdef WINUI_PANE_TESTING
        auto& initial=*state().panes.at(hwnd);
        initial.startupInitialLabel=Microsoft::UI::Xaml::Automation::AutomationProperties::GetName(initial.address->field);
        initial.startupBeforeGecko=GetModuleHandleW(L"xul.dll")==nullptr;
        initial.startupInitialRtl=initial.strip->root.FlowDirection()==FlowDirection::RightToLeft &&
            initial.address->root.FlowDirection()==FlowDirection::RightToLeft;
        // Seed through the real editor callback before Gecko can bind. Never
        // available in the production DLL or parsed as browser navigation here.
        wchar_t queued[4097]{};
        const auto length=GetEnvironmentVariableW(L"FENN_TEST_STARTUP_TEXT",queued,4097);
        if(length && length<4097) {
            auto& pane=*state().panes.at(hwnd);
            pane.address->field.Text(queued);
            pane.address->Action(L"activate");
        }
#endif
        // Measurement switch: skip DWM's window-open transition for the first show only, so its cost
        // can be separated from construction. Later minimize/maximize transitions are untouched.
        wchar_t noAnimation[2]{};
        const bool skipOpenTransition=GetEnvironmentVariableW(L"FENN_NATIVE_NO_OPEN_ANIMATION",noAnimation,2)==1 && noAnimation[0]==L'1';
        // Activate the first visible window like Firefox's own first window does (nsWindow::CanTakeFocus allows
        // it when no other window of this process is visible); Windows' foreground lock still protects other
        // applications. Without activation the editor has no keyboard focus and typed keys are lost until Gecko
        // asks for focus about 1.6 s later. FENN_NATIVE_STARTUP_NOACTIVATE=1 restores the old behaviour.
        wchar_t noActivate[2]{};
        const bool keepInactive=GetEnvironmentVariableW(L"FENN_NATIVE_STARTUP_NOACTIVATE",noActivate,2)==1 && noActivate[0]==L'1';
        BeginPageHold(hwnd);
        // An island's window is white until XAML first composes, in either theme. Clip it away until then
        // rather than hide it: XAML keeps rendering and the OS animation still plays. Never leave it clipped.
        {
            auto& pane=*state().panes.at(hwnd);
            const HWND strip=pane.childWindow,address=pane.addressWindow;
            SetWindowRgn(strip,CreateRectRgn(0,0,0,0),FALSE);
            SetWindowRgn(address,CreateRectRgn(0,0,0,0),FALSE);
            auto token=std::make_shared<winrt::event_token>();auto frames=std::make_shared<int>(0);auto done=std::make_shared<bool>(false);
            auto reveal=[strip,address,token,done]{
                if(*done)return;*done=true;
                Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(*token);
                if(IsWindow(strip))SetWindowRgn(strip,nullptr,TRUE);
                if(IsWindow(address))SetWindowRgn(address,nullptr,TRUE);
                NativeWindowHost::FocusTrace("start-chrome-revealed");
            };
            *token=Microsoft::UI::Xaml::Media::CompositionTarget::Rendering([frames,reveal](auto const&,auto const&){if(++*frames>=3)reveal();});
            auto timer=Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
            timer.Interval(std::chrono::milliseconds(1000));timer.IsRepeating(false);
            timer.Tick([timer,reveal](auto const&,auto const&){timer.Stop();reveal();});
            timer.Start();
        }
        if(skipOpenTransition){BOOL disable=TRUE;DwmSetWindowAttribute(hwnd,DWMWA_TRANSITIONS_FORCEDISABLED,&disable,sizeof(disable));}
        ShowWindow(hwnd,GetPropW(hwnd,L"Fenn.NativeStartupMaximized")?SW_SHOWMAXIMIZED:(keepInactive?SW_SHOWNOACTIVATE:SW_SHOWNORMAL));
        if(skipOpenTransition){BOOL disable=FALSE;DwmSetWindowAttribute(hwnd,DWMWA_TRANSITIONS_FORCEDISABLED,&disable,sizeof(disable));}
        NativeWindowHost::FocusTrace("start-shown");
        return S_OK;
    } catch (...) {Destroy(hwnd);return to_hresult();}
}
}

extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneGetApi(uint32_t version, uint32_t size,
                                                                WinUIPaneApi* output) noexcept {
    if (!output) return E_POINTER;
    if (version != WINUI_PANE_ABI_VERSION || size != sizeof(WinUIPaneApi)) return E_NOINTERFACE;
    *output = {sizeof(WinUIPaneApi),
               WINUI_PANE_ABI_VERSION,
               Focus,
               Destroy,
               Translate,
               Shutdown,
               SyncTabs,
               SyncAddress,
               Clip,
               SyncTabMenu,
               UpdateTabStrip,
               SetNotifySink,
               DrainRequests,
               CompleteRequests,
               ApplyTabDelta,
               UpdateAddress,
               FocusAddress,
               MeasureChromeLayout,
               // A native host exists only on the independent thread, which Route installs.
               [](DWORD, const wchar_t*, DWORD, const RECT*, HWND, HINSTANCE, HWND* output) noexcept {
                   if (output) *output=nullptr; return E_NOTIMPL; },
               NativeWindowHost::Destroy,
               ShowStartupSurface,
               DismissStartupSurface,
               IndependentHost::Start,
               IndependentHost::Bind,
               IndependentHost::Resolve,
               NativeWindowHost::SetAccessibility,
               NativePageInput::Publish, NativePageInput::Drain, NativePageInput::Commit, NativePageInput::Geometry,
               IndependentHost::DirectStartup,
               IndependentHost::SetTitle,
               NativeWindowHost::SetPageCursor,
               NativeWindowHost::RegisterPageTouch,
               IndependentHost::PostWindowRequest,
               PaneLink::SetStartupSink};
    IndependentHost::Route(*output);
    return S_OK;
}

#ifdef WINUI_PANE_TESTING
static HRESULT WINAPI TestStartupDraft(HWND hwnd,uint32_t operation,const wchar_t* text,uint32_t* pending) {
    auto require=[](bool value){if(!value)throw_hresult(E_FAIL);};
    try {
        auto found=state().panes.find(hwnd);
        if(found==state().panes.end() || !text || !pending || operation>30)return E_INVALIDARG;
        if(operation==30) {
            auto& pane=*found->second;
            const bool rtl=std::wstring_view(text)==L"rtl";
            require(pane.startupInitialRtl==rtl);
            require((pane.strip->root.FlowDirection()==FlowDirection::RightToLeft)==rtl);
            require((pane.address->root.FlowDirection()==FlowDirection::RightToLeft)==rtl);
            require(pane.address->field.TextAlignment()==TextAlignment::Left);
            *pending=rtl?1u:0u;return S_OK;
        }
        if(operation==29) {
            require(found->second->startupInitialLabel==text);
            *pending=found->second->startupBeforeGecko?1u:0u;return S_OK;
        }
        if(operation==28) {
            auto& pane=*found->second;
            require(pane.address!=nullptr);
            *pending=pane.address->HasActiveComposition()?1u:0u;return S_OK;
        }
        if(operation==24 || operation==25) {
            auto& pane=*found->second;
            fwprintf(stderr,L"Normal startup check: presenter=%d identity=%d text=%s range=%d/%d composing=%d\n",
                bool(pane.startupPresenter),pane.startupNormalIdentity && pane.startupNormalIdentity(),pane.address->field.Text().c_str(),
                pane.address->field.SelectionStart(),pane.address->field.SelectionLength(),pane.address->composing);
            require(!pane.startupPresenter && pane.startupNormalIdentity && pane.startupNormalIdentity());
            require(pane.address->field.Text()==text && !pane.address->composing);
            if(operation==25)require(pane.address->field.SelectionStart()==2 && pane.address->field.SelectionLength()==3);
            *pending=0;return S_OK;
        }
        if(operation>=6) {
            auto& pane=*found->second;
            auto local=std::static_pointer_cast<LocalStartup::Presenter>(pane.startupPresenter);
            if(!local || !local->sameEditor || !local->sameEditor() ||
                (operation<12 && (!local->sameControls || !local->sameControls())))return E_NOT_VALID_STATE;
            if(operation==26) {
                require(!local->browserEditor && !pane.address->composing);
                pane.strip->add();
                pane.address->field.Text(text);pane.address->field.Select(2,3);pane.address->Edit();
            } else if(operation==27) {
                const auto draft=std::stoull(text);
                const auto row=std::find_if(local->drafts.begin(),local->drafts.end(),[&](auto const& entry){return entry.second==draft;});
                require(row!=local->drafts.end());pane.strip->close(row->first);
            } else if(operation==21) {
                require(GetPropW(hwnd,InteractiveStartupProperty)!=nullptr);
                require(!NativeStartupSurface::Active());
            } else if(operation==22 || operation==23) {
                require(pane.address->field.Text().empty());
                pane.address->composing=operation==22;pane.address->Edit();
            } else if(operation==7) {
                require(SetForegroundWindow(hwnd)!=FALSE);
                local->FocusEditor();
                pane.address->field.Text(text);pane.address->field.Select(2,3);
                pane.address->composing=true;pane.address->Edit();
            } else if(operation==8) {
                require(pane.address->field.Text()==text && pane.address->field.SelectionStart()==2 &&
                    pane.address->field.SelectionLength()==3 && pane.address->composing);
                pane.address->composing=false;pane.address->Edit();
            } else if(operation==9) {
                const auto browser=std::stoull(text);
                auto row=std::find_if(local->browsers.begin(),local->browsers.end(),[&](auto const& entry){return entry.second==browser;});
                require(row!=local->browsers.end());
                require(Automation::AutomationProperties::GetName(pane.strip->rows.at(row->first))==pane.tabTitles.at(uint32_t(browser)));
                pane.strip->close(row->first);
            } else if(operation==10) {
                pane.address->field.Text(text);pane.address->field.Select(int32_t(wcslen(text)),0);
                pane.address->Action(L"activate");
            } else if(operation==11) {
                const auto draft=local->view.selected;
                require(local->view.tabs==1 && !local->browsers.empty());
                const auto row=std::find_if(local->drafts.begin(),local->drafts.end(),[&](auto const& entry){return entry.second==draft;});
                require(row!=local->drafts.end());pane.strip->close(row->first);
                require(local->Run(0) && !local->view.tabs && pane.address->field.IsReadOnly());
            } else if(operation==12) {
                require(local->browserEditor && !pane.address->field.IsReadOnly() &&
                    !pane.address->startupEdit && !pane.address->startupSubmit && pane.address->field.Text()==text);
            } else if(operation==13) {
                const auto browser=std::stoull(text);
                const auto row=std::find_if(local->browsers.begin(),local->browsers.end(),[&](auto const& entry){return entry.second==browser;});
                require(row!=local->browsers.end());pane.strip->select(row->first);
            } else if(operation==14) {
                require(local->browserEditor);local->FocusEditor();
                pane.address->field.Text(text);pane.address->field.Select(int32_t(wcslen(text)),0);
                pane.address->Edit();pane.address->Action(L"activate");
            } else if(operation==15) {
                require(local->browserEditor);pane.strip->add();
            } else if(operation==16) {
                require(local->Switching() && pane.address->field.IsReadOnly() && !pane.address->field.IsEnabled() && !pane.address->toolbarRoot.IsHitTestVisible());
            } else if(operation==17) {
                const auto draft=std::stoull(text);
                const auto row=std::find_if(local->drafts.begin(),local->drafts.end(),[&](auto const& entry){return entry.second==draft;});
                require(row!=local->drafts.end());pane.strip->select(row->first);
            } else if(operation==18) {
                require(local->browserEditor);local->FocusEditor();
                pane.address->field.Text(text);pane.address->field.Select(2,3);pane.address->Edit();
            } else if(operation==19) {
                require(!local->browserEditor && local->sameControls() && !pane.address->field.IsReadOnly() &&
                    pane.address->field.Text()==text && pane.address->field.SelectionStart()==2 && pane.address->field.SelectionLength()==3);
            } else if(operation==20) {
                const auto state=pane.shell.AddressState();
                *pending=!state.needs_dispatch && state.edit_acknowledged && !state.staged_kind && !state.mode_pending;return S_OK;
            } else require(pane.address->field.Text()==text);
            require(local->Run(0));*pending=uint32_t(local->view.pending);return S_OK;
        }
        auto core=found->second->shell.core;
        ShellStartupView view{};
        auto call=[&](uint32_t op){return shell_core_startup(core,op,1,0,
            reinterpret_cast<const uint16_t*>(text),wcslen(text),&view);};
        if(operation==0) {
            require(call(1)==SHELL_STATUS_OK);
            require(call(2)==SHELL_STATUS_OK);
            require(call(5)==SHELL_STATUS_OK);
        } else if(operation==1)require(call(2)==SHELL_STATUS_OK);
        else if(operation==2)require(call(4)==SHELL_STATUS_OK);
        else if(operation==4) {
            require(call(2)==SHELL_STATUS_OK);
            require(call(5)==SHELL_STATUS_OK);
        } else if(operation==5)require(call(1)==SHELL_STATUS_OK);
        else {
            require(call(0)==SHELL_STATUS_OK);
            require(hstring(reinterpret_cast<const wchar_t*>(view.text),uint32_t(view.length))==text);
        }
        *pending=uint32_t(view.pending);
        if(operation!=3)NotifyStartup(*found->second);
        return S_OK;
    } catch (...) {return to_hresult();}
}
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestStartupDraft(HWND hwnd,uint32_t operation,const wchar_t* text,uint32_t* pending) noexcept {
    return IndependentHost::Call<TestStartupDraft>(hwnd,operation,text,pending);
}
// Times the posted lifetime calls for the native stall suite. A null window does nothing when its job runs.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestPostedLifetime() noexcept {
    struct : IUnknown {
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void**) noexcept override {return E_NOINTERFACE;}
        ULONG STDMETHODCALLTYPE AddRef() noexcept override {return 2;}
        ULONG STDMETHODCALLTYPE Release() noexcept override {return 1;}
    } static provider;
    // A framed call needs a window the shell knows; generation 0 resolves no close.
    for(const auto result:{IndependentHost::PostResolve(IndependentHost::State().shell,0,0),IndependentHost::PostAccessibility(nullptr,&provider),
        IndependentHost::PostDismissSurface(nullptr)})if(FAILED(result))return result;
    return S_OK;
}
// Overfills the posted queue. Answers how many posts were refused; PostedChars shows the accounting afterward.
extern "C" __declspec(dllexport) uint32_t WINAPI WinUIPaneTestOverfillPosted() noexcept {
    uint32_t refused=0;
    for(uint32_t i=0;i<IndependentHost::MaxPosted+64;++i)if(FAILED(IndependentHost::PostJob(16,[]{return S_OK;})))++refused;
    return refused;
}
extern "C" __declspec(dllexport) uint32_t WINAPI WinUIPaneTestPostedChars() noexcept {
    return uint32_t(IndependentHost::State().postedChars.load());
}
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestLocalStartup(void(WINAPI* inspect)(HWND)) noexcept {
    HWND hwnd{};
    auto require=[](bool value){if(!value)throw_hresult(E_FAIL);};
    try {
        RECT bounds{80,80,1080,780};
        check_hresult(NativeWindowHost::CreateStartup(0,WS_OVERLAPPEDWINDOW,bounds,&hwnd));
        check_hresult(ShowInteractiveStartup(hwnd,nullptr));
        auto& pane=*state().panes.at(hwnd);
        {
            auto& local=*std::static_pointer_cast<LocalStartup::Presenter>(pane.startupPresenter);
            auto field=pane.address->field;
            auto source=pane.addressSource;
            field.Text(L"first search");
            pane.address->Action(L"activate");
            field.Select(2,3);
            ShellStartupView submitted{};
            require(shell_core_startup_read(pane.shell.core,true,0,&submitted)==SHELL_STATUS_OK);
            require(hstring(reinterpret_cast<const wchar_t*>(submitted.text),uint32_t(submitted.length))==L"first search");
            pane.strip->add();
            field.Text(L"second search");
            pane.address->Action(L"activate");
            require(local.Run(0) && local.view.tabs==2 && local.view.pending==2);
            pane.strip->select(1);
            require(field.Text()==L"first search");
            require(field.SelectionStart()==2 && field.SelectionLength()==3);
            pane.address->composing=true;pane.address->Edit();
            pane.strip->add();pane.strip->select(2);pane.address->Action(L"activate");
            require(local.Run(0) && local.view.tabs==2 && local.view.pending==2 && local.view.selected==1);
            require(field.Text()==L"first search" && pane.address->composing);
            pane.address->composing=false;pane.address->Edit();
            require(field==pane.address->field && source==pane.addressSource);
            // No browser snapshot or endpoint has been installed at any point.
            require(pane.address->session.empty());
            ShowWindow(hwnd,SW_SHOWNOACTIVATE);
            if(inspect)inspect(hwnd);
            require(local.Run(0) && local.view.tabs==3 && local.view.pending==3);
            require(field.Text()==L"keyboard search");
            require(shell_core_startup_read(pane.shell.core,true,2,&submitted)==SHELL_STATUS_OK);
            require(hstring(reinterpret_cast<const wchar_t*>(submitted.text),uint32_t(submitted.length))==L"keyboard search");
            pane.strip->close(3);
            pane.strip->close(2);
            require(local.view.tabs==1 && local.view.pending==1);
            // Simulated browser completion only: exercise the generated ABI
            // against live controls without claiming a real Gecko attachment.
            ShellStartupTransfer transfer{};
            auto send=[&](uint32_t operation,uint64_t request=0,uint64_t draft=0,
                          uint64_t browser=0,bool accepted=true,uint64_t generation=7) {
                return shell_core_startup_transfer(pane.shell.core,operation,42,transfer.session,generation,
                    request,draft,browser,accepted,&transfer);
            };
            auto transferOutput=std::make_unique<wchar_t[]>(262144);
            transferOutput[0]=L'X';
            require(StartupTransfer(hwnd,LR"({"version":1,"operation":1,"context":"42"})",
                transferOutput.get(),1)==E_INVALIDARG && transferOutput[0]==L'X');
            require(StartupTransfer(hwnd,LR"({"version":1,"operation":1,"context":"18446744073709551616"})",
                transferOutput.get(),262144)==E_INVALIDARG && transferOutput[0]==L'X');
            check_hresult(StartupTransfer(hwnd,LR"({"version":1,"operation":1,"context":"42"})",
                transferOutput.get(),262144));
            require(send(0)==SHELL_STATUS_OK && send(2)==SHELL_STATUS_OK);
            uint32_t startupNotifications=0;
            check_hresult(SetNotifySink(hwnd,[](void* value)->BOOL {
                return ++*static_cast<uint32_t*>(value)>1;
            },&startupNotifications));
            require(startupNotifications==1 && local.Observe() && startupNotifications==2);
            require(local.Observe() && startupNotifications==2);
            check_hresult(SetNotifySink(hwnd,nullptr,nullptr));
            require(send(3,0,1)==SHELL_STATUS_OK && transfer.kind==1);
            const auto createRequest=transfer.request;
            const auto draftRow=pane.strip->rows.at(1);
            local.browserTitle=[](uint64_t id){return id==90?hstring(L"Restored fixture tab"):hstring(L"Browser fixture tab");};
            std::vector<ShellStartupRow> mergedRows(SHELL_STARTUP_ROW_CAPACITY);
            uint32_t mergedCount{};
            auto publishRows=[&](uint64_t revision,std::vector<uint64_t> const& ids,uint64_t selected) {
                require(shell_core_startup_rows(pane.shell.core,transfer.session,7,revision,
                    ids.data(),ids.size(),selected,mergedRows.data(),mergedRows.size(),&mergedCount)==SHELL_STATUS_OK);
                local.Refresh(false);
            };
            publishRows(1,{90,123},90);
            require(mergedCount==3 && pane.strip->rows.at(1)==draftRow);
            const auto restoredId=mergedRows[0].presentation;
            const auto duplicateId=mergedRows[1].presentation;
            const auto restoredRow=pane.strip->rows.at(restoredId);
            require(restoredRow.IsEnabled());
            field.Text(L"edited during browser creation");field.Select(7,3);
            pane.address->composing=true;pane.address->Edit();
            require(send(6,createRequest,0,123,true,8)==SHELL_STATUS_REJECTED);
            require(send(6,createRequest,0,123)==SHELL_STATUS_OK);
            publishRows(2,{123,90,91},91);
            require(mergedCount==3 && pane.strip->rows.at(1)==draftRow &&
                pane.strip->rows.at(restoredId)==restoredRow && draftRow.IsEnabled());
            require(!pane.strip->rows.contains(duplicateId));
            require(mergedRows[mergedCount-1].draft==1 && mergedRows[mergedCount-1].browser==123 &&
                mergedRows[mergedCount-1].selected);
            require(field.Text()==L"edited during browser creation" &&
                field.SelectionStart()==7 && field.SelectionLength()==3 && pane.address->composing);
            require(field==pane.address->field && source==pane.addressSource && IsWindow(hwnd));
            require(send(4)==SHELL_STATUS_OK && transfer.kind==2 && transfer.browser==123);
            require(hstring(reinterpret_cast<const wchar_t*>(transfer.text),uint32_t(transfer.length))==L"first search");
            const auto submitRequest=transfer.request;
            require(send(6,submitRequest,0,123)==SHELL_STATUS_OK);
            require(send(6,submitRequest,0,123)==SHELL_STATUS_OK);
            require(local.Run(0) && local.view.pending==0);
            pane.address->composing=false;pane.address->Edit();
            for(size_t i=0;i<64;++i)require(local.Run(5,1));
            const auto retained=field.Text();
            require(!local.Run(5,1));
            require(field.Text()==retained &&
                Automation::AutomationProperties::GetItemStatus(field)==local.labels.full);
            pane.strip->close(1);
            require(local.view.tabs==0 && local.view.pending==0);
            require(send(5,0,1)==SHELL_STATUS_OK && transfer.kind==3);
            require(send(6,transfer.request,0,123)==SHELL_STATUS_OK && transfer.mappings==0);
        }
        check_hresult(Destroy(hwnd));
        check_hresult(NativeWindowHost::Destroy(hwnd));
        return Shutdown();
    } catch (...) {
        auto result=to_hresult();
        if(hwnd){Destroy(hwnd);NativeWindowHost::Destroy(hwnd);}
        Shutdown();return result;
    }
}
// Test-only on-owner query: a miss returns immediately and lets Gecko process
// the queued request between Marionette calls. No synchronous geometry lookup.
static HRESULT WINAPI TestPageRange(HWND hwnd,uint32_t start,uint32_t end,int32_t* output) {
    auto store=NativePageInput::Get(NativeWindowHost::PageWindow(hwnd));if(!store || !output || store->lock || start<store->textBase || end<store->textBase)return E_INVALIDARG;
    RECT bounds{};BOOL clipped{};store->lock=TS_LF_READ;
    HRESULT result=store->GetTextExt(0,start-store->textBase,end-store->textBase,&bounds,&clipped);
    store->lock=0;
    output[0]=bounds.left;output[1]=bounds.top;output[2]=bounds.right;output[3]=bounds.bottom;output[4]=clipped;
    output[5]=store->textBase;output[6]=store->Read().length;output[7]=store->Read().edit;
    return result;
}
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestPageRange(HWND hwnd,uint32_t start,uint32_t end,int32_t* output) noexcept {
    return IndependentHost::Call<TestPageRange>(hwnd,start,end,output);
}
#include "../../tests/native/PageInputTests.h"
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestPageInput() noexcept {
    try { PageInputContract::Run(); return S_OK; } catch (...) { return to_hresult(); }
}
#include "../../tests/native/ThreadedStartupTests.h"
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestThreadedStartup(void(WINAPI* inspect)(HWND), BOOL cancel) noexcept {
    if (!inspect) return E_POINTER;
    try { ThreadedStartupContract::Run(inspect,!!cancel); return S_OK; }
    catch (...) { return to_hresult(); }
}
#include "../../tests/native/StartupWindowHostTests.h"
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestStartupSurface(void(WINAPI* inspect)(HWND)) noexcept {
    if (!inspect) return E_POINTER;
    try {
        auto require=[](bool value) { if (!value) throw_hresult(E_FAIL); };
        for (bool cancel : {false, true}) {
            HWND hwnd{};
            RECT bounds{40,40,840,640};
            check_hresult(NativeWindowHost::CreateStartup(0, WS_OVERLAPPEDWINDOW, bounds, &hwnd));
            struct Cleanup { HWND hwnd; ~Cleanup() { if (IsWindow(hwnd)) NativeWindowHost::Destroy(hwnd); } } cleanup{hwnd};
            require(ShowStartupSurface(hwnd)==S_OK);
            require(ShowStartupSurface(hwnd)==S_FALSE);
            require(DismissStartupSurface(nullptr)==E_INVALIDARG);
            require(IsWindowVisible(hwnd) && GetPropW(hwnd, NativeStartupSurface::Property));
            auto root=NativeStartupSurface::Active()->source.Content().as<FrameworkElement>();
            for (auto theme : {ElementTheme::Light, ElementTheme::Dark}) {
                root.RequestedTheme(theme);
                inspect(hwnd);
            }
            SetWindowPos(hwnd, nullptr, 70,70,640,480, SWP_NOACTIVATE|SWP_NOZORDER);
            inspect(hwnd);
            if (cancel) {
                SendMessageW(hwnd, WM_CLOSE, 0, 0);
                require(!IsWindow(hwnd) && !NativeStartupSurface::Active());
            } else {
                require(DismissStartupSurface(hwnd)==S_OK);
                require(DismissStartupSurface(hwnd)==S_FALSE);
                require(IsWindow(hwnd) && !GetPropW(hwnd, NativeStartupSurface::Property));
                ShellWindowBinding binding{}; binding.shell=reinterpret_cast<uintptr_t>(hwnd); binding.content=42;
                require(NativeWindowHost::Get(hwnd)->Call(SHELL_WINDOW_HOST_OPERATION_BIND,binding)==SHELL_STATUS_OK);
                require(ShowStartupSurface(hwnd)==E_INVALIDARG);
            }
        }
        return S_OK;
    } catch (...) { return to_hresult(); }
}
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestStartupWindowHost() noexcept {
    try { StartupWindowHostContract::Run(); return S_OK; }
    catch (...) { return winrt::to_hresult(); }
}
#include "../../tests/native/WindowHostTests.h"
#include "../../tests/native/CompositionProbe.h"
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestWindowHost(const WinUIPaneApi* api, void (WINAPI* inspect)(HWND)) noexcept {
    if (!api || !inspect) return E_POINTER;
    try { WindowHostContract::Run(*api, inspect); return S_OK; }
    catch (...) { return winrt::to_hresult(); }
}

extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestMeasureChromeLayout(
    HWND hwnd, uint32_t stripHeight, BOOL navigation, uint32_t contentInset,
    ShellChromeLayout* output) noexcept {
    return MeasureNativeChromeLayout(hwnd, stripHeight, !!navigation, contentInset, output);
}

// Borrow the servicing host only while this address host is alive. The test
// retains shared_from_this() before replacing OS operations. The menu item is
// returned as an ordinary AddRef'd WinRT control. Neither export ships.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestGetPackageUpdates(
    HWND hwnd, void** service, void** item) noexcept {
    try {
        check_hresult(Check(hwnd));
        if (!service || !item) return E_POINTER;
        auto& pane = *state().panes.at(hwnd);
        if (!pane.address) return E_NOT_VALID_STATE;
        auto& menu = pane.address->navigation->productMenu;
        *service = menu.updates.get();
        auto control = menu.updateItem;
        *item = detach_abi(control);
        return S_OK;
    } catch (...) { return to_hresult(); }
}
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestGetAddressContent(HWND hwnd, void** output) noexcept {
    try {
        check_hresult(Check(hwnd));
        if (!output) return E_POINTER;
        auto& pane = *state().panes.at(hwnd);
        if (!pane.addressSource) return E_NOT_VALID_STATE;
        *output = detach_abi(pane.addressSource.Content());
        return S_OK;
    } catch (...) { return to_hresult(); }
}
// Test-only: make the Rust shell core panic, so the containment path can be
// exercised from a real call rather than argued for. The core catches it,
// marks itself poisoned and reports a status; nothing unwinds into C++ and the
// process survives. Not exported by the shipped DLL.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestCorePanic(HWND hwnd) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto& pane = *found->second;
        if (!pane.shell) return E_NOT_VALID_STATE;
        auto status = shell_core_test_panic(pane.shell.core);
        // A poisoned core must not keep driving the pane: drop it and fall back
        // to the snapshot path, which is the explicitly isolated legacy route.
        if (status == SHELL_STATUS_PANIC) {
            pane.notifySink = nullptr;
            pane.notifyContext = nullptr;
            pane.drainOpen = false;
            if (pane.strip) { pane.strip->core = nullptr; pane.strip->hoverCard.core = nullptr; }
            pane.shell.Close();
            return E_FAIL;
        }
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
// Test-only: one ABI entry point's call count, looked up by the same name the
// shutdown report prints, so nothing is coupled to the order of an array.
// Timing is deliberately not exposed: it is only taken when diagnostics are on,
// and a test that asserted on it would be asserting on the clock.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestGetEntryCalls(const wchar_t* name,
                                                                           uint64_t* calls) noexcept {
    if (!name || !calls) return E_POINTER;
    for (auto* entry = nativeWork(); entry->name; ++entry) {
        if (wcscmp(entry->name, name) == 0) {
            *calls = entry->calls;
            return S_OK;
        }
    }
    for (auto* stage = requestLatency(); stage->work.name; ++stage) {
        if (wcscmp(stage->work.name, name) == 0) {
            *calls = stage->work.calls;
            return S_OK;
        }
    }
    return E_INVALIDARG;
}
// Test-only: one counter of one latency stage, both named the way the shutdown
// report names them. A single lookup rather than an export per counter, so
// adding a way to classify a sample does not add ABI surface to the test DLL.
// Counters: "n" (samples this stage answers for), "blocked", "deferred",
// "stalled".
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestGetStageCounter(const wchar_t* name,
                                                                             const wchar_t* counter,
                                                                             uint64_t* value) noexcept {
    if (!name || !counter || !value) return E_POINTER;
    for (auto* stage = requestLatency(); stage->work.name; ++stage) {
        if (wcscmp(stage->work.name, name) != 0) continue;
        if (wcscmp(counter, L"n") == 0) *value = stage->work.calls;
        else if (wcscmp(counter, L"blocked") == 0) *value = stage->blocked;
        else if (wcscmp(counter, L"deferred") == 0) *value = stage->deferred;
        else if (wcscmp(counter, L"stalled") == 0) *value = stage->stalled;
        else return E_INVALIDARG;
        return S_OK;
    }
    return E_INVALIDARG;
}
// Test-only: the geometry work counters, so the contract test can pin that a
// repeated update or clip never reaches the island. Values, in order: updates,
// geometryApplied, restacks, visibilityApplied, clips, clipsApplied.
// Process-wide and monotonic; callers compare deltas around the call they are
// pinning. Not exported by the shipped DLL.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestGetGeometryWork(uint64_t* values,
                                                                             uint32_t count) noexcept {
    if (!values) return E_POINTER;
    if (count < 6) return E_INVALIDARG;
    auto& work = geometryWork();
    values[0] = work.updates;
    values[1] = work.geometryApplied;
    values[2] = work.restacks;
    values[3] = work.visibilityApplied;
    values[4] = work.clips;
    values[5] = work.clipsApplied;
    return S_OK;
}
// Test-only scaling and content inspection. Not exported by the shipped DLL.
// Exercise real island rasterization without changing desktop display settings.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestSetIslandScale(HWND hwnd, float scale) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !std::isfinite(scale) || scale < 1 || scale > 3) return E_INVALIDARG;
        found->second->source.SiteBridge().OverrideScale(scale);
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestGetContent(HWND hwnd, void** output) noexcept {
    try {
        if (!output) return E_POINTER;
        *output = nullptr;
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        *output = detach_abi(found->second->source.Content());
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestGetBackdrop(HWND hwnd, void** output) noexcept {
    try {
        if (!output) return E_POINTER;
        *output = nullptr;
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        *output = detach_abi(found->second->source.SystemBackdrop());
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestGetAddressBackdrop(HWND hwnd, void** output) noexcept {
    try {
        if (!output) return E_POINTER;
        *output = nullptr;
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        if (found->second->addressSource)
            *output = detach_abi(found->second->addressSource.SystemBackdrop());
        return S_OK;
    } catch (...) { return to_hresult(); }
}
// Test-only: drive the strip's TabItems()-order-derived reorder logic
// directly, as if a completed drag had already left TabItems() in the given
// state. Kept for the case a caller wants "the neighbor IS TabItems()'s
// current order" semantics without a real pointer gesture.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestCompleteReorder(HWND hwnd, uint32_t tabId) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        auto& strip = *found->second->strip;
        auto row = strip.rows.find(tabId);
        if (row == strip.rows.end()) return E_INVALIDARG;
        strip.CompleteReorder(row->second);
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
// Test-only: drive the explicit-target-index overload the hand-rolled
// pointer-driven drag actually uses (TabStrip.h), where TabItems()'s real
// order never changes during the gesture and the neighbor is resolved from
// an explicit index instead. A real OS drag gesture can't be triggered from
// outside actual pointer input, so this is the fastest way to exercise it.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestCompleteReorderAt(HWND hwnd, uint32_t tabId,
                                                                               uint32_t targetIndex) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        auto& strip = *found->second->strip;
        auto row = strip.rows.find(tabId);
        if (row == strip.rows.end()) return E_INVALIDARG;
        strip.CompleteReorder(row->second, targetIndex);
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
// Test-only: a new press arriving before the previous gesture was cleaned up.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestRearmDrag(HWND hwnd, uint32_t tab) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->strip || !found->second->strip->rows.count(tab)) return E_INVALIDARG;
        auto& pane = *found->second;
        ArmDragSession(pane, tab);
        POINT point{}; GetCursorPos(&point);
        pane.strip->ArmDrag(tab, pane.ScreenToStrip(point));
        return S_OK;
    } catch (...) { return to_hresult(); }
}
// Test-only: inspect visual lifetimes and simulate reduced motion without
// changing the user's Windows animation setting.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestGroupMotion(HWND hwnd, uint32_t operation, uint32_t* ghosts) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (!ghosts || found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        auto& motion = found->second->strip->groupSlide;
        if (operation == 1) motion.Cancel();
        else if (operation == 2) { motion.Cancel(); motion.disableForTest = true; }
        else if (operation == 3) motion.disableForTest = false;
        else if (operation == 7 || operation == 8) {
            // New-tab entry: 7 starts the reveal on the second tab and reports
            // whether it is clipped and tracked; 8 stops it and reports
            // whether the row is back to a clean, unclipped rest state.
            auto& strip = *found->second->strip;
            if (strip.view.TabItems().Size() < 2) return E_INVALIDARG;
            auto row = strip.view.TabItems().GetAt(1).as<Controls::TabViewItem>();
            const auto id = unbox_value<uint32_t>(row.Tag());
            if (operation == 7) {
                strip.RevealNewRow(row, id, std::numeric_limits<double>::quiet_NaN());
                *ghosts = strip.rowAnimations.count(id) && row.Clip() && strip.RowTransform(row).X() == 0 ? 1 : 0;            } else {
                strip.ResetRowOffset(row, id);
                *ghosts = !strip.rowAnimations.count(id) && !row.Clip() && strip.RowTransform(row).X() == 0 ? 1 : 0;
            }
            return S_OK;
        }
        else if (operation == 4) found->second->strip->CollapseDraggingGroup(7);
        else if (operation == 5) found->second->strip->RestoreDraggingGroup();
        else if (operation == 6) found->second->strip->UpdateTitle(41,L"Updated during drag");
        else if (operation == 9) { *ghosts = motion.resizing ? 1 : 0; return S_OK; }
        else if (operation == 10 || operation == 11 || operation == 12) {
            // 10 pauses the slide and its width motion halfway (75 of 150 ms)
            // so geometry can be read without depending on frame cadence; 11
            // resumes it. Reports whether the motion animates any width.
            *ghosts = motion.resizing ? 1 : 0;
            if (!motion.storyboard) return S_OK;
            if (operation == 11) motion.storyboard.Resume();
            else {
                motion.storyboard.Pause();
                motion.storyboard.SeekAlignedToLastTick(Windows::Foundation::TimeSpan{operation == 12 ? std::chrono::milliseconds(0) : InlineTabGroups::MotionDuration / 2});
            }
            return S_OK;
        }
        else if (operation != 0) return E_INVALIDARG;
        *ghosts = uint32_t(motion.departing.size());
        return S_OK;
    } catch (...) { return to_hresult(); }
}
// Test-only: observe the tab add/close motion without a screenshot. Operation
// 0 fills out[0..9]: tracked row animations, rows with a reveal clip, reveal
// clips scaled about local x = 0, 1 when the New Tab button carries a nonzero
// offset (out[3] is its sign: 1 positive, 2 negative), stand-in count, rows
// with a composition scale or translation in flight, and the first stand-in's
// Canvas.Left in tenths, then the first row's scale in thousandths and its translation in tenths plus 5000. Operations 1 and 2 set left-to-right or right-to-left.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestEntryMotion(HWND hwnd, uint32_t operation, uint32_t* out) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (!out || found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        auto& strip = *found->second->strip;
        if (operation == 1 || operation == 2) {
            strip.root.FlowDirection(operation == 2 ? FlowDirection::RightToLeft : FlowDirection::LeftToRight);
            strip.root.UpdateLayout();
            return S_OK;
        }
        if (operation == 3) {
            out[0] = 0;
            for (auto const& held : strip.settling) if (held.content) ++out[0];
            return S_OK;
        }
        if (operation != 0) return E_INVALIDARG;
        std::fill(out, out + 10, 0u);
        out[0] = uint32_t(strip.rowAnimations.size());
        for (auto const& [id, row] : strip.rows) {
            if (auto clip = row.Clip().try_as<Media::RectangleGeometry>()) {
                ++out[1];
                if (auto scale = clip.Transform().try_as<Media::ScaleTransform>(); scale && scale.CenterX() == 0) ++out[2];
            }
            auto visual = winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(row);
            winrt::Windows::Foundation::Numerics::float3 shift{};
            visual.Properties().TryGetVector3(L"Translation", shift);
            if (std::abs(visual.Scale().x - 1.f) > .001f || std::abs(shift.x) > .001f) ++out[6];
            if (!out[8] && &row == &strip.rows.begin()->second) {
                out[8] = uint32_t(std::max(0.f, visual.Scale().x * 1000));
                out[9] = uint32_t(std::max(0.f, shift.x * 10 + 5000));
            }
        }
        if (auto add = strip.FindAddButton(strip.view))
            if (auto transform = add.RenderTransform().try_as<Media::TranslateTransform>(); transform && std::abs(transform.X()) > .5)
                out[3] = transform.X() > 0 ? 1 : 2;
        if (strip.closeOverlay) {
            out[4] = strip.closeOverlay.Children().Size();
            if (out[4]) out[7] = uint32_t(std::max(0.0, Controls::Canvas::GetLeft(strip.closeOverlay.Children().GetAt(0)) * 10));
        }
        out[5] = uint32_t(strip.closeStoryboards.size());
        return S_OK;
    } catch (...) { return to_hresult(); }
}
// Test-only counts of actual width/ambient/separator/caption work, not events.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestLayoutPasses(HWND hwnd, uint64_t* counts) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (!counts || found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        auto& strip = *found->second->strip;
        counts[0] = strip.inlineGroups.capPasses;
        counts[1] = strip.ambientPasses;
        counts[2] = strip.separatorPasses;
        counts[3] = strip.captionPasses;
        return S_OK;
    } catch (...) { return to_hresult(); }
}
// Test-only: compare incremental decoration geometry with the full reference.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestSelectionReentry(HWND hwnd, uint64_t* counts, BOOL reset) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (!counts || found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        auto& strip = *found->second->strip;
        for (size_t i = 0; i < 8; ++i) {
            counts[i] = strip.selectionReentry[i];
            if (reset) strip.selectionReentry[i] = 0;
        }
        return S_OK;
    } catch (...) { return to_hresult(); }
}
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestSelectionDecorations(HWND hwnd, uint64_t* counts, BOOL verify) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (!counts || found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        auto& strip = *found->second->strip;
        counts[0] = strip.selectionDecorationPasses;
        counts[1] = strip.ambientPasses;
        counts[2] = strip.separatorPasses;
        counts[3] = (!strip.selectionGeometryReady ? 1u : 0u) | (strip.ambient.queued ? 2u : 0u) |
            (strip.ambient.observing ? 4u : 0u) | (strip.currentGroups.Size() ? 8u : 0u) |
            (strip.HasDecorationGroups() ? 16u : 0u) | (strip.surfaceMotion.following ? 32u : 0u) |
            (!strip.settling.empty() ? 64u : 0u) | (!strip.rowAnimations.empty() ? 128u : 0u);
        if (verify) {
            const auto faces = strip.ambient.faceMask.previous;
            const auto active = strip.ambient.clippedActive;
            const auto rimBounds = strip.ambient.upperRim.bounds;
            const auto rimColor = strip.ambient.upperRim.color;
            std::map<uint32_t, std::pair<bool, uint32_t>> tints;
            for (auto const& [id, tint] : strip.separatorTints) tints[id] = {tint.tinted, tint.color};
            strip.ambient.Update();
            if (faces != strip.ambient.faceMask.previous || active != strip.ambient.clippedActive ||
                rimBounds != strip.ambient.upperRim.bounds || rimColor != strip.ambient.upperRim.color)
                return E_FAIL;
            for (auto const& [id, tint] : strip.separatorTints)
                if (tints[id] != std::pair{tint.tinted, tint.color}) return E_FAIL;
        }
        return S_OK;
    } catch (...) { return to_hresult(); }
}
// Test-only: refresh decorative geometry. S_FALSE indicates a frame observer
// following an existing native tab animation or drag.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestRefreshAmbient(HWND hwnd) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        auto& ambient = found->second->strip->ambient;
        ambient.Update();
        return ambient.observing ? S_FALSE : S_OK;
    } catch (...) { return to_hresult(); }
}
// Test-only: inspect opt-in group glow motion. 0: is `value` lighting up;
// 1: visuals still dimming; 2: force system animations off (0), on (1) or
// back to the system setting (2); 3: edge re-catches so far; 4: total width
// of the tab faces standing in for empty drag slots in group surfaces;
// 5: group `value`'s drawn decoration, 1 for its outline and 2 for its glow.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestGlowMotion(HWND hwnd, uint32_t operation, uint32_t value, uint32_t* result) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->strip || !result) return E_INVALIDARG;
        auto& motion = found->second->strip->ambient.motion;
        switch (operation) {
        case 0: *result = motion.Active(value) ? 1 : 0; break;
        case 1: *result = uint32_t(motion.dimming.size()); break;
        case 2:
            if (value > 2) return E_INVALIDARG;
            if (value == 2) motion.animationsForTest.reset(); else motion.animationsForTest = value == 1;
            *result = motion.Enabled() ? 1 : 0;
            break;
        case 3: *result = uint32_t(motion.recatches); break;
        case 4: *result = uint32_t(found->second->strip->slotFaceWidth + .5f); break;
        case 5: {
            auto& strip = *found->second->strip;
            *result = (strip.groupSurface.surfaces.count(value) ? 1u : 0u) | (strip.ambient.groups.count(value) ? 2u : 0u);
            break;
        }
        default: return E_INVALIDARG;
        }
        return S_OK;
    } catch (...) { return to_hresult(); }
}
// Test-only: exercise a queued grouping dwell against a real loaded storyboard.
// No global drag is armed, so the harness can advance animation time separately.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestGroupSlide(HWND hwnd, uint32_t operation) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        auto& strip = *found->second->strip;
        if (strip.view.TabItems().Size() != 2) return E_INVALIDARG;
        auto source = strip.view.TabItems().GetAt(0).as<Controls::TabViewItem>();
        auto target = strip.view.TabItems().GetAt(1).as<Controls::TabViewItem>();
        const auto sourceId = unbox_value<uint32_t>(source.Tag()), targetId = unbox_value<uint32_t>(target.Tag());
        if (operation == 0) {
            strip.CancelDrag();
            strip.presentation[sourceId].groupSource = true;
            strip.presentation[targetId].groupTarget = true;
            strip.groupingDelay = 1000;
            const auto start = source.TransformToVisual(strip.view).TransformPoint({float(source.ActualWidth()/2), 16});
            strip.ArmDrag(sourceId, start);
            if (!strip.TryPromote(sourceId, {start.X + 4, start.Y})) return E_FAIL;
            const Windows::Foundation::Point center{float(strip.drag.originalLeftEdge[1] + target.ActualWidth()/2), 16};
            if (!strip.UpdateGroupHover(center) || strip.armedGroupTarget) return E_FAIL;
            // ShellCore deliberately keeps an exact row midpoint before that
            // row. Cross it explicitly so this fixture always opens the second
            // insertion gap instead of depending on fractional XAML geometry.
            shell_core_tab_drag_update(strip.core, center.X + 1, center.Y, strip.view.ActualWidth(), strip.view.ActualHeight(), true);
            strip.ShiftSibling(1);
            return strip.rowAnimations.count(targetId) ? S_OK : E_FAIL;
        }
        const Windows::Foundation::Point oldCenter{float(strip.drag.originalLeftEdge[1] + target.ActualWidth()/2), 16};
        if (operation == 1) {
            // A zero delay represents a timer whose deadline has already passed.
            strip.groupingDelay = 0;
            const auto storyboard = strip.rowAnimations.at(targetId).storyboard;
            const auto offset = strip.RowTransform(target).X();
            if (strip.UpdateGroupHover(oldCenter) || strip.groupCandidate || strip.armedGroupTarget ||
                strip.Drag().projected != 2 || !strip.rowAnimations.count(targetId) ||
                strip.rowAnimations.at(targetId).storyboard != storyboard || strip.RowTransform(target).X() != offset) return E_FAIL;
            return S_OK;
        }
        if (operation == 2) {
            if (!strip.rowAnimations.empty()) return E_PENDING;
            const auto offset = strip.RowTransform(target).X();
            if (offset >= -1 || strip.UpdateGroupHover(oldCenter)) return E_FAIL;
            auto actualCenter = oldCenter;
            actualCenter.X += float(offset);
            if (!strip.UpdateGroupHover(actualCenter) || strip.armedGroupTarget != targetId ||
                strip.Drag().projected != 2 || strip.RowTransform(target).X() != offset) return E_FAIL;
            strip.CancelDrag();
            strip.presentation[sourceId].groupSource = false;
            strip.presentation[targetId].groupTarget = false;
            return S_OK;
        }
        return E_INVALIDARG;
    } catch (...) { return to_hresult(); }
}
// Test-only: copy the actual popup bitmap for pixel regression checks.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestCopyDragBitmap(HBITMAP* result) noexcept {
    if (!result) return E_POINTER;
    *result = nullptr;
    try {
        check_hresult(Check());
        if (!dragSession().tearoffBitmap) return S_FALSE;
        *result = HBITMAP(CopyImage(dragSession().tearoffBitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
        return *result ? S_OK : E_FAIL;
    } catch (...) { return to_hresult(); }
}
// Test-only: force the same drag-in-progress state a real gesture sets, for
// the same reason as above -- confirms Sync() ignores Firefox snapshots that
// land mid-gesture without needing a real drag to hold one open.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestSetDragging(HWND hwnd, BOOL value) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->strip) return E_INVALIDARG;
        found->second->strip->testDragging = bool(value);
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
// Test-only: hover a row (or leave it) through the same entry points as the
// pointer, and return that row's hover-card Flyout for inspection.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestHoverCard(HWND hwnd, uint32_t tab, BOOL hover,
                                                                       void** flyout) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->strip || !flyout) return E_INVALIDARG;
        auto& strip = *found->second->strip;
        auto& card = strip.hoverCard;
        if (!strip.rows.count(tab) || !card.cards.count(tab)) return E_INVALIDARG;
        if (hover) card.Enter(tab, strip.rows.at(tab));
        else card.Hide(tab);
        // Hand out a new reference; detaching the card's own would empty it.
        *flyout = detach_abi(Controls::Flyout(card.cards.at(tab).flyout));
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
// Test-only: retain a rejected old departure while a newer one hides the same
// real row. Cleanup must match the source token, not merely its tab/lifetime.
extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneTestTransferOverlap(HWND sourceHwnd, HWND destinationHwnd, uint32_t tab) noexcept {
    try {
        check_hresult(Check(sourceHwnd)); check_hresult(Check(destinationHwnd));
        auto& source = *state().panes.at(sourceHwnd)->strip;
        auto& destination = *state().panes.at(destinationHwnd)->strip;
        if (!source.rows.count(tab)) return E_INVALIDARG;
        const auto old = destination.ReserveTransfer(source, tab);
        if (!old) return E_FAIL;
        source.BeginDeparture(tab);
        if (shell_core_transfer_bind(destination.core, old, 0, false) != SHELL_STATUS_OK) return E_FAIL;
        const auto newer = destination.ReserveTransfer(source, tab);
        if (!newer) return E_FAIL;
        source.BeginDeparture(tab);
        destination.ReconcileTransfers();
        if (source.rows.at(tab).Opacity() != 0 || destination.transferPeers.count(old)) return E_FAIL;
        if (shell_core_transfer_bind(destination.core, newer, 0, false) != SHELL_STATUS_OK) return E_FAIL;
        destination.ReconcileTransfers();
        return source.rows.at(tab).Opacity() == 1 && !destination.transferPeers.count(newer) ? S_OK : E_FAIL;
    } catch (...) { return to_hresult(); }
}
#endif
