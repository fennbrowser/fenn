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
#include "TabMenu.h"
#include "TabStrip.h"
#include "WindowDrag.h"

namespace {
bool DiagnosticsEnabled();
namespace SystemDrag = Microsoft::UI::Input::DragDrop;
// Reject external system drags before WinUI creates XAML DragEventArgs.
// AllowDrop=false only rejects them after that unsafe dispatch has happened.
// This target owns no pane, controls, drag data or deferred work: an in-flight
// leave/drop remains safe even after the island that requested it is destroyed.
struct RejectExternalDrop : implements<RejectExternalDrop, SystemDrag::IDropOperationTarget> {
    using Operation = Windows::ApplicationModel::DataTransfer::DataPackageOperation;
    Windows::Foundation::IAsyncOperation<Operation> EnterAsync(SystemDrag::DragInfo const&,
                                                              SystemDrag::DragUIOverride const&) {
        if (DiagnosticsEnabled()) fprintf(stderr, "WinUIPane external drag rejected\n");
        co_return Operation::None;
    }
    Windows::Foundation::IAsyncOperation<Operation> OverAsync(SystemDrag::DragInfo const&,
                                                             SystemDrag::DragUIOverride const&) {
        co_return Operation::None;
    }
    Windows::Foundation::IAsyncAction LeaveAsync(SystemDrag::DragInfo const&) { co_return; }
    Windows::Foundation::IAsyncOperation<Operation> DropAsync(SystemDrag::DragInfo const&) {
        co_return Operation::None;
    }
};
// How much of what update() and clip() are asked to do turns out to repeat
// what the island already has. Process-wide; read by the contract test through
// WinUIPaneTestGetGeometryWork and reported at shutdown under
// FIREFOX_WINUI_PANE_DIAGNOSTICS.
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
        {L"update", 0, 0, 0},       {L"updateTabStrip", 0, 0, 0}, {L"clip", 0, 0, 0},
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
// distorted by the second: with FIREFOX_WINUI_PANE_DIAGNOSTICS on, every
// update() writes a line to a redirected stderr, which cost 20.5ms across 105
// calls -- a fifth of what update() itself was reported to cost. The perf
// harness turns diagnostics off for exactly that reason, so it sets this.
inline bool WorkAccountingEnabled() {
    static const bool enabled =
        GetEnvironmentVariableW(L"FIREFOX_WINUI_PANE_WORK", nullptr, 0) != 0 || DiagnosticsEnabled();
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
    EntryUpdate,
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
    static const bool enabled = GetEnvironmentVariableW(L"FIREFOX_WINUI_PANE_DIAGNOSTICS", nullptr, 0) != 0;
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
    bool groupSource{};
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
    DesktopWindowXamlSource source{nullptr};
    DesktopWindowXamlSource addressSource{nullptr};
    HWND addressWindow{};
    RECT addressBounds{};
    bool addressBoundsApplied{};
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
        if (tabMenu.active) tabMenu.Dismiss();
        if (strip) strip->inlineGroups.hover.Dismiss();
        if (address && address->mode.flyout.IsOpen()) address->mode.flyout.Hide();
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
        if (!stripPopup && strip && (strip->inlineGroups.hover.open || strip->inlineGroups.hover.showPending))
            strip->inlineGroups.hover.Dismiss();
        const HWND addressHost = addressSource ? addressWindow : childWindow;
        if (!InWindow(addressHost, target) && !addressPopup) LeaveAddress();
        else if (!addressPopup && address && address->mode.flyout.IsOpen()) address->mode.flyout.Hide();
        if (!InWindow(childWindow, target) && !InWindow(addressWindow, target) &&
            !stripPopup && !addressPopup &&
            (InWindow(childWindow, GetFocus()) || InWindow(addressWindow, GetFocus()) ||
             InPopups(contentIsland, GetFocus()) || InPopups(addressIsland, GetFocus())) &&
            InWindow(parent, target)) SetFocus(parent);
        if (address) address->ScheduleFocusObservation();
    }
    void CloseAddressHost() {
        if (!addressSource) return;
        if (addressWindow) RemoveWindowSubclass(addressWindow, FocusProc, reinterpret_cast<UINT_PTR>(this));
        addressSource.TakeFocusRequested(addressTakeFocus);
        addressSource.SiteBridge().Hide();
        addressSource.Content(nullptr);
        address.reset();
        shell.ResetAddress();
        addressIsland = nullptr;
        addressWindow = nullptr;
        addressBoundsApplied = false;
        addressSource.Close();
        addressSource = nullptr;
    }
    void WireAddress() {
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
    Controls::ListView tabs{nullptr};
    Windows::Foundation::Collections::IObservableVector<Windows::Foundation::IInspectable> items{nullptr};
    std::vector<uint32_t> tabIds;
    std::map<uint32_t, hstring> tabTitles;
    std::map<uint32_t, uint32_t> tabCommands;
    NativeTabMenu tabMenu;
    std::unique_ptr<NativeTabStrip> strip;
    event_token selection{}, takeFocus{}, containers{};
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
        if (!strip) return {};
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
    ~Pane() { DisarmPresentation(); }
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
        if (strip && (strip.get() == dragSession().sourceStrip || strip.get() == dragSession().hoverStrip))
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
        if (shell.core) shell_core_close(shell.core);
        lifetime.reset();
        if (tabs) {
            tabs.SelectionChanged(selection);
            tabs.ContainerContentChanging(containers);
        }
        source.TakeFocusRequested(takeFocus);
        externalDropRevoker.revoke();
        source.SystemBackdrop(nullptr);
        source.Content(nullptr);
        address.reset();
        strip.reset();
        shell.Close();
        tabs = nullptr;
        items = nullptr;
        tabIds.clear();
        tabTitles.clear();
        source.Close();
        source = nullptr;
    }
    void UpdateRow(Controls::Primitives::SelectorItem const& row) {
        auto index = tabs.IndexFromContainer(row);
        if (index < 0 || size_t(index) >= tabIds.size()) return;
        auto found = tabTitles.find(tabIds[index]);
        if (found == tabTitles.end()) return;
        auto const& title = found->second;
        Automation::AutomationProperties::SetName(row, title);
        Automation::AutomationProperties::SetItemStatus(row, tabIds[index] == browserSelection ? L"Active tab" : L"");
        if (auto root = row.ContentTemplateRoot()) {
            auto grid = root.as<Controls::Grid>();
            auto label = grid.Children().GetAt(0).as<Controls::TextBlock>();
            label.Text(title);
            label.FontWeight(Windows::UI::Text::FontWeight{uint16_t(tabIds[index] == browserSelection ? 600 : 400)});
            Controls::ToolTipService::SetToolTip(label, box_value(title));
            Automation::AutomationProperties::SetName(grid.Children().GetAt(1), L"Close tab: " + title);
        }
    }
    void AttachMenu(Controls::Primitives::SelectorItem const& row, bool replace = false, uint32_t itemId = 0) {
        auto index = tabs.IndexFromContainer(row);
        auto id = itemId ? itemId : index >= 0 && size_t(index) < tabIds.size() ? tabIds[index] : 0;
        if (!id || !tabMenu.HasTab(id)) {
            row.ContextFlyout(nullptr);
            return;
        }
        if (!replace && row.ContextFlyout()) return;
        Controls::MenuFlyout menu;
        menu.Items().Append(NativeTabMenu::Placeholder());
        menu.Opening([this, alive = std::weak_ptr<int>(lifetime), weakRow = make_weak(row), weakMenu = make_weak(menu)](
                         auto const&, auto const&) {
            if (alive.expired()) return;
            auto row = weakRow.get();
            auto menu = weakMenu.get();
            if (!row || !menu) return;
            auto index = tabs.IndexFromContainer(row);
            auto id = index >= 0 && size_t(index) < tabIds.size() ? tabIds[index] : 0;
            tabMenu.Open(menu, id);
        });
        menu.Opened([this, alive = std::weak_ptr<int>(lifetime), weak = make_weak(menu)](auto const&, auto const&) {
            if (alive.expired()) return;
            // Focus the first enabled item after its presenter is loaded.
            if (weak.get() == tabMenu.active) tabMenu.FocusFirst();
        });
        menu.Closed([this, alive = std::weak_ptr<int>(lifetime), weakRow = make_weak(row), weak = make_weak(menu)](
                        auto const&, auto const&) {
            if (alive.expired()) return;
            if (weak.get() == tabMenu.active) tabMenu.Dismiss();
            // Keep fresh WinUI input state; the next opening synchronously
            // renders the current Firefox state without another browser exchange.
            if (auto row = weakRow.get(); row && row.ContextFlyout() == weak.get()) AttachMenu(row, true);
        });
        row.ContextFlyout(menu);
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
    void QueueTabSelection(uint32_t activate = 0) {
        NativeTabMenu::Array selected;
        uint32_t active = activate;
        std::set<uint32_t> members;
        for (auto item : tabs.SelectedItems()) {
            auto id = unbox_value<uint32_t>(item);
            if (!members.insert(id).second) continue;
            selected.Append(NativeTabMenu::Value::CreateNumberValue(id));
            if (!active || (!activate && id == browserSelection)) active = id;
        }
        if (!active) return;
        // In single-selection mode SelectedItems() can be empty, and Firefox
        // requires the active tab to be a member of the requested set.
        if (!members.count(active)) selected.Append(NativeTabMenu::Value::CreateNumberValue(active));
        NativeTabMenu::Object event;
        event.SetNamedValue(L"kind", NativeTabMenu::Value::CreateStringValue(L"selection"));
        event.SetNamedValue(L"revision", NativeTabMenu::Value::CreateNumberValue(tabMenu.StateRevision()));
        event.SetNamedValue(L"tab", NativeTabMenu::Value::CreateNumberValue(active));
        event.SetNamedValue(L"tabs", selected);
        event.SetNamedValue(L"control",
                            NativeTabMenu::Value::CreateBooleanValue((GetKeyState(VK_CONTROL) & 0x8000) != 0));
        event.SetNamedValue(L"shift", NativeTabMenu::Value::CreateBooleanValue((GetKeyState(VK_SHIFT) & 0x8000) != 0));
        Enqueue(NativeShellCore::SelectTab, active, tabMenu.StateRevision(), event.Stringify());
    }
    // ShellCore has already reconciled tab/address/mode bookkeeping when each
    // outcome was recorded. Deliver the remaining surface-specific effects.
    void ReconcileOutcomes() {
        shell.TakeOutcomes(); // Diagnostic reports do not own cleanup anymore.
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
    void SeedModel(const WinUITab* snapshot, uint32_t count, uint32_t selected) {
        if (!shell) return;
        shell.BeginSnapshot(shell.Stats().generation, count);
        for (uint32_t i = 0; i < count; ++i) shell.PushSnapshotTab(snapshot[i].id, snapshot[i].title);
        uint32_t changes = 0;
        shell.CommitSnapshot(selected, changes);
        // The snapshot is the authority: whatever diff it produced has just been
        // applied from the snapshot itself, so nothing is left pending.
        shell.TakeChanges();
    }
    void Sync(const WinUITab* snapshot, uint32_t count, uint32_t selected) {
        SeedModel(snapshot, count, selected);
        if (strip) {
            browserSelection = selected;
            shell.SelectionPending(tabMenu.StateRevision());
            strip->Sync(snapshot, count, selected);
            return;
        }
        syncing = true;
        browserSelection = selected;
        try {
            std::map<uint32_t, hstring> titles;
            std::map<uint32_t, uint32_t> commands;
            bool reordered = count != tabIds.size();
            for (uint32_t i = 0; i < count; ++i) {
                titles.emplace(snapshot[i].id, snapshot[i].title);
                commands.emplace(snapshot[i].id, snapshot[i].commands);
                if (i >= tabIds.size() || tabIds[i] != snapshot[i].id) reordered = true;
            }
            std::vector<uint32_t> retainedIds;
            for (auto const& [id, command] : commands) retainedIds.push_back(id);
            tabMenu.RetainTabs(retainedIds);
            // Revoke the open menu when its target, ordering or eligibility changes.
            if (tabMenu.State().tab &&
                (reordered || !commands.count(tabMenu.State().tab) || commands[tabMenu.State().tab] != tabCommands[tabMenu.State().tab]))
                tabMenu.Dismiss(true);
            tabCommands.swap(commands);
            tabTitles.swap(titles);
            // Let ListView generate and recycle its own containers. Keep unchanged
            // data items so ordinary title/selection updates preserve scroll and focus.
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t match = i;
                while (match < tabIds.size() && tabIds[match] != snapshot[i].id) ++match;
                if (match != i || match == tabIds.size()) {
                    if (match < tabIds.size()) {
                        items.RemoveAt(match);
                        tabIds.erase(tabIds.begin() + match);
                    }
                    tabIds.insert(tabIds.begin() + i, snapshot[i].id);
                    // Data-item equality must use stable IDs, not possibly identical
                    // titles. Boxed duplicate strings break UIA peer enumeration.
                    items.InsertAt(i, box_value(snapshot[i].id));
                }
            }
            while (tabIds.size() > count) {
                items.RemoveAtEnd();
                tabIds.pop_back();
            }
            for (uint32_t i = 0; i < count; ++i) {
                if (auto row = tabs.ContainerFromIndex(i).try_as<Controls::ListViewItem>()) {
                    AttachMenu(row);
                    UpdateRow(row);
                }
            }
            if (tabMenu.hasSelection) {
                tabs.SelectionMode(Controls::ListViewSelectionMode::Extended);
                if (!shell.SelectionPending(tabMenu.StateRevision())) {
                    for (uint32_t i = 0; i < count; ++i) {
                        auto item = items.GetAt(i);
                        uint32_t existing;
                        bool wanted = tabMenu.selectedTabs.count(snapshot[i].id) != 0;
                        bool present = tabs.SelectedItems().IndexOf(item, existing);
                        if (wanted && !present) tabs.SelectedItems().Append(item);
                        if (!wanted && present) tabs.SelectedItems().RemoveAt(existing);
                    }
                }
            } else {
                tabs.SelectionMode(Controls::ListViewSelectionMode::Single);
                int index = -1;
                for (uint32_t i = 0; i < count; ++i)
                    if (snapshot[i].id == selected) index = int(i);
                if (tabs.SelectedIndex() != index) {
                    tabs.SelectedIndex(index);
                    if (index >= 0) tabs.ScrollIntoView(items.GetAt(index));
                }
            }
        } catch (...) {
            syncing = false;
            throw;
        }
        syncing = false;
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
        if (base != shell.ModelRevision()) return E_NOT_VALID_STATE;
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
                if (shell.IndexOf(id) == SHELL_NO_INDEX) return E_NOT_VALID_STATE;
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
            if (nextSelected && shell.IndexOf(nextSelected) == SHELL_NO_INDEX) return E_NOT_VALID_STATE;
        }
        for (auto const& [id, title] : titles) {
            bool changed = false;
            auto status = shell.SetTitle(shell.ModelRevision(), id, std::wstring_view(title), changed);
            if (status != SHELL_STATUS_OK) return status == SHELL_STATUS_GAP ? E_NOT_VALID_STATE : E_INVALIDARG;
            if (changed) tabTitles[id] = title;
        }
        if (delta.HasKey(L"selected")) {
            bool changed = false;
            auto status = shell.SetSelected(shell.ModelRevision(), nextSelected, changed);
            if (status != SHELL_STATUS_OK) return status == SHELL_STATUS_GAP ? E_NOT_VALID_STATE : E_INVALIDARG;
            if (changed) {
                browserSelection = nextSelected;
            }
        }
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
        // Same guard as ApplyPublishedSelection: a pane that is not presenting
        // has nothing to apply to, and the model catches up at the next
        // snapshot.
        if (!changes.size() || (!strip && (!tabs || !items))) return;
        syncing = true;
        try {
            for (size_t i = 0; i < changes.size(); ++i) {
                auto const& change = changes[i];
                if (change.kind == NativeShellCore::TitleChanged) {
                    if (strip)
                        strip->UpdateTitle(change.tab, tabTitles.count(change.tab) ? tabTitles[change.tab] : L"");
                    else if (change.to != SHELL_NO_INDEX)
                        if (auto row = tabs.ContainerFromIndex(int(change.to)).try_as<Controls::ListViewItem>())
                            UpdateRow(row);
                } else if (change.kind == NativeShellCore::SelectionChanged) {
                    // `from`/`to` are the previous and new active tab ids.
                    if (strip)
                        strip->ApplySelection(change.from, change.to);
                    else
                        ApplySidebarSelection(change.from, change.to);
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
        // A native selection is still awaiting its authoritative result; its
        // optimistic feedback stands until the outcome or a newer publication.
        if (shell.SelectionPending(tabMenu.StateRevision())) return;
        auto const& current = tabMenu.selectedTabs;
        if (strip) {
            strip->ApplySelectedSet(previous, current);
            return;
        }
        if (!tabs || !items) return;
        syncing = true;
        try {
            tabs.SelectionMode(tabMenu.hasSelection ? Controls::ListViewSelectionMode::Extended
                                                    : Controls::ListViewSelectionMode::Single);
            if (tabMenu.hasSelection) {
                for (auto id : previous) {
                    if (current.count(id)) continue;
                    auto index = shell.IndexOf(id);
                    if (index == SHELL_NO_INDEX) continue;
                    uint32_t at{};
                    if (tabs.SelectedItems().IndexOf(items.GetAt(index), at)) tabs.SelectedItems().RemoveAt(at);
                }
                for (auto id : current) {
                    if (previous.count(id)) continue;
                    auto index = shell.IndexOf(id);
                    if (index == SHELL_NO_INDEX) continue;
                    uint32_t at{};
                    if (!tabs.SelectedItems().IndexOf(items.GetAt(index), at))
                        tabs.SelectedItems().Append(items.GetAt(index));
                }
            }
        } catch (...) {
            syncing = false;
            throw;
        }
        syncing = false;
    }
    void ApplySidebarSelection(uint32_t previous, uint32_t current) {
        auto index = shell.IndexOf(current);
        if (!tabMenu.hasSelection) {
            if (index != SHELL_NO_INDEX && tabs.SelectedIndex() != int(index)) {
                tabs.SelectedIndex(int(index));
                tabs.ScrollIntoView(items.GetAt(index));
            }
        }
        // The active-tab emphasis lives on the row itself, so refresh exactly
        // the two rows whose emphasis can have changed.
        for (auto id : {previous, current}) {
            auto at = shell.IndexOf(id);
            if (at == SHELL_NO_INDEX) continue;
            if (auto row = tabs.ContainerFromIndex(int(at)).try_as<Controls::ListViewItem>()) UpdateRow(row);
        }
    }
    void Create(HWND hwnd, bool tabStrip = false) {
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
        source = DesktopWindowXamlSource();
        source.Initialize(GetWindowIdFromWindow(hwnd));
        childWindow = GetWindowFromWindowId(source.SiteBridge().WindowId());
        check_bool(SetWindowSubclass(childWindow, FocusProc, reinterpret_cast<UINT_PTR>(this),
                                     reinterpret_cast<DWORD_PTR>(this)));
        if (tabStrip) {
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
            takeFocus = source.TakeFocusRequested([this](auto const&, auto const&) { SetFocus(parent); });
            source.Content(strip->root);
            // The island owns the backdrop and its theme/activation/fallback
            // handling. Keep it local to the strip, including the caption insets.
            // Mica Alt until Firefox's first publication names its preference.
            strip->applyBackdrop = [this](NativeTabStrip::Backdrop kind) {
                if (kind == NativeTabStrip::Backdrop::Acrylic) {
                    source.SystemBackdrop(Media::DesktopAcrylicBackdrop());
                    return;
                }
                Media::MicaBackdrop mica;
                mica.Kind(kind == NativeTabStrip::Backdrop::Mica ? Composition::SystemBackdrops::MicaKind::Base
                                                                  : Composition::SystemBackdrops::MicaKind::BaseAlt);
                source.SystemBackdrop(mica);
            };
            strip->applyBackdrop(strip->backdrop);
            windowDrag.Open(hwnd, strip.get());
            // Subscribe after XAML installs its default target, before showing
            // the island. SetTarget substitutes a rejecting target at the system
            // boundary, before XAML drag event creation (which can fail-fast in
            // embedded Firefox). Do not Close this shared, WinUI-owned manager
            // or replace Firefox's HWND/OLE registration. Native tab gestures
            // use our independent pointer session and never enter this path.
            auto dropManager = SystemDrag::DragDropManager::GetForIsland(strip->root.XamlRoot().ContentIsland());
            externalDropRevoker = dropManager.TargetRequested(auto_revoke,
                [](auto const&, SystemDrag::DropOperationTargetRequestedEventArgs const& args) {
                    args.SetTarget(make<RejectExternalDrop>());
                });
            strip->armDragSession = [this](uint32_t id) { ArmDragSession(*this, id); };
            strip->armGroupDragSession = [this](uint32_t id) { ArmDragSession(*this,id); dragSession().groupSource=true; };
            strip->tickDragSession = TickDragSession;
            return;
        }
        auto panel = Markup::XamlReader::Load(
                         LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                Background="{ThemeResource SolidBackgroundFillColorBaseBrush}"><Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="*"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions></Grid>)")
                         .as<Controls::Grid>();
        panel.Padding({16, 16, 16, 16});

        panel.Loaded([](auto const& sender, auto const&) {
            if (!DiagnosticsEnabled()) return;
            auto element = sender.template as<FrameworkElement>();
            fprintf(stderr, "WinUIPane loaded width=%.1f height=%.1f rootWidth=%.1f rootHeight=%.1f\n",
                    element.ActualWidth(), element.ActualHeight(), element.XamlRoot().Size().Width,
                    element.XamlRoot().Size().Height);
            auto panel = element.as<Controls::Grid>();
            auto background = panel.Background().as<Media::SolidColorBrush>().Color();
            auto foreground =
                panel.Children().GetAt(0).as<Controls::TextBlock>().Foreground().as<Media::SolidColorBrush>().Color();
            fprintf(stderr, "WinUIPane theme=%d background=%u,%u,%u,%u foreground=%u,%u,%u,%u\n",
                    int(panel.ActualTheme()), background.A, background.R, background.G, background.B, foreground.A,
                    foreground.R, foreground.G, foreground.B);
        });
        Controls::TextBlock title;
        title.Text(L"Tabs");
        title.Margin({0, 0, 0, 12});
        tabs = Controls::ListView();
        items = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
        tabs.ItemsSource(items);
        tabs.ItemTemplate(
            Markup::XamlReader::Load(
                LR"(<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"><Grid><Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions><TextBlock VerticalAlignment="Center" TextTrimming="CharacterEllipsis" /><Button Grid.Column="1" Width="32" Height="32" MinWidth="0" Padding="0" Margin="8,0,0,0" ToolTipService.ToolTip="Close tab" AutomationProperties.AutomationId="FirefoxWinUICloseTab"><SymbolIcon Symbol="Cancel" /></Button></Grid></DataTemplate>)")
                .as<DataTemplate>());
        tabs.ItemContainerStyle(
            Markup::XamlReader::Load(
                LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="ListViewItem"><Setter Property="HorizontalContentAlignment" Value="Stretch"/></Style>)")
                .as<Style>());
        containers =
            tabs.ContainerContentChanging([this](auto const&, Controls::ContainerContentChangingEventArgs const& args) {
                if (args.InRecycleQueue()) return;
                auto row = args.ItemContainer();
                // The container may not have an index yet during realization.
                // The data item already carries its stable Firefox ID.
                AttachMenu(row, false, unbox_value<uint32_t>(args.Item()));
                UpdateRow(row);
                args.RegisterUpdateCallback([this, alive = std::weak_ptr<int>(lifetime)](
                                                auto const&, Controls::ContainerContentChangingEventArgs const& ready) {
                    if (alive.expired() || ready.InRecycleQueue()) return;
                    auto row = ready.ItemContainer();
                    auto grid = row.ContentTemplateRoot().as<Controls::Grid>();
                    // Resolve deferred work from the current snapshot, including
                    // title changes that do not replace the stable ListView item.
                    UpdateRow(row);
                    auto button = grid.Children().GetAt(1).as<Controls::Button>();
                    if (button.Tag()) return;
                    button.Tag(box_value(true));
                    button.Click([this, alive = std::weak_ptr<int>(lifetime), weak = make_weak(row)](auto const&,
                                                                                                     auto const&) {
                        if (alive.expired() || syncing) return;
                        if (auto row = weak.get()) {
                            auto index = tabs.IndexFromContainer(row);
                            if (index >= 0 && size_t(index) < tabIds.size()) {
                                // Clicking a background close button must not select its row,
                                // which QueueClose also takes care of.
                                QueueClose(tabIds[index]);
                            }
                        }
                    });
                });
            });
        tabs.IsItemClickEnabled(true);
        tabs.ItemClick(
            [this, alive = std::weak_ptr<int>(lifetime)](auto const&, Controls::ItemClickEventArgs const& args) {
                if (alive.expired() || syncing || !tabMenu.hasSelection) return;
                try {
                    QueueTabSelection(unbox_value<uint32_t>(args.ClickedItem()));
                } catch (...) {}
            });
        tabs.SelectionMode(Controls::ListViewSelectionMode::Single);
        Automation::AutomationProperties::SetAutomationId(tabs, L"FirefoxWinUITabs");
        Automation::AutomationProperties::SetName(tabs, L"Tabs in this window");
        Controls::Grid::SetRow(tabs, 1);
        selection = tabs.SelectionChanged([this](auto const&, Controls::SelectionChangedEventArgs const& args) {
            if (syncing) return;
            try {
                if (tabMenu.hasSelection) {
                    uint32_t endpoint = 0;
                    if (GetKeyState(VK_SHIFT) & 0x8000) {
                        if (args.AddedItems().Size())
                            endpoint = unbox_value<uint32_t>(args.AddedItems().GetAt(args.AddedItems().Size() - 1));
                        else if (auto focused =
                                     Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(tabs.XamlRoot())
                                         .try_as<Controls::ListViewItem>()) {
                            auto index = tabs.IndexFromContainer(focused);
                            if (index >= 0 && size_t(index) < tabIds.size()) endpoint = tabIds[index];
                        }
                    }
                    QueueTabSelection(endpoint);
                } else {
                    // Single-selection mode: the same request path, with the
                    // clicked row as the only member of the set. Selection
                    // never travels through syncTabs any more, so no command
                    // can reach Firefox through two executors.
                    auto index = tabs.SelectedIndex();
                    if (index >= 0 && size_t(index) < tabIds.size()) QueueTabSelection(tabIds[index]);
                }
            } catch (...) {}
        });
        takeFocus = source.TakeFocusRequested([this](auto const&, auto const&) { SetFocus(parent); });
        panel.Children().Append(title);
        panel.Children().Append(tabs);
        auto newTab =
            Markup::XamlReader::Load(
                LR"x(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Width="32" Height="32" MinWidth="0" Padding="0" HorizontalAlignment="Left" Margin="0,12,0,0" ToolTipService.ToolTip="New tab (Ctrl+T)" AutomationProperties.Name="New tab" AutomationProperties.AutomationId="FirefoxWinUINewTab"><SymbolIcon Symbol="Add" /></Button>)x")
                .as<Controls::Button>();
        Controls::Grid::SetRow(newTab, 2);
        newTab.Click([this, alive = std::weak_ptr<int>(lifetime)](auto const&, auto const&) {
            if (alive.expired() || syncing) return;
            QueueNewTab();
        });
        panel.Children().Append(newTab);
        address = std::make_unique<NativeAddress>(shell);
        // Rust records the admitted request/intent and clears it on matching
        // completion, including when bounded outcome reports are evicted.
        address->dispatch = [this](hstring const& action) {
            return Enqueue(NativeShellCore::AddressAction, 0, address->revision, action);
        };
        // The island and the HWND live here, so the answer to "does this
        // surface hold focus" is assembled here and the address controls only
        // say when to ask.
        WireAddress();
        // Deliberately not tracked as an outstanding intent: a report is not
        // something the surface is waiting to hear back about.
        address->reportVisible = [this](hstring const& action) {
            Enqueue(NativeShellCore::AddressVisibility, 0, address->revision, action);
        };
        address->mode.dispatch = [this](hstring const& action) {
            Enqueue(NativeShellCore::AddressModeAction, 0, address->revision, action);
        };
        Controls::Grid::SetRow(address->root, 3);
        panel.Children().Append(address->root);
        source.Content(panel);
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
    HANDLE activation{INVALID_HANDLE_VALUE};
    ULONG_PTR activationCookie{};
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

constexpr wchar_t kTearoffClassName[] = L"FirefoxWinUITabTearoff";

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

void ShowOrMoveTearoff(TabDragSession& session, POINT screenPoint) {
    if (!session.tearoffWindow) {
        if (!CreateTearoffWindow(session)) return;
        if (session.sourceStrip && session.sourceStrip->rows.count(session.tabId))
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
// would resolve it. Skips the source window and its owned popups, and windows
// that are both layered and input-transparent: the drag's own preview and the
// shell's snap overlay are such windows and never take a drop.
HWND VisibleRootAt(POINT screenPoint, HWND source) {
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (!IsWindowVisible(window) || IsIconic(window) || GetAncestor(window, GA_ROOTOWNER) == source) continue;
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
    const auto visible = VisibleRootAt(screenPoint, GetAncestor(sourcePane.parent, GA_ROOT));
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
        const bool cancel=(GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0 || GetForegroundWindow()!=sourcePane.parent ||
            (session.sourceStrip->GroupDrag().phase==2 && GetCapture()!=sourcePane.parent) || !sourcePane.appliedVisible;
        auto* strip = session.sourceStrip;
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
        strip->TickGroupDrag(sourcePane.ScreenToStrip(point), down, cancel, hitPane != nullptr);
        const auto group = strip->GroupDrag();
        if (!group.phase) { AbortDragSession(); return; }
        if (hitPane) {
            if (!hitPane->strip->ForeignGroupOver(hitPane->ScreenToStrip(point), strip->GroupDragSlotWidth()))
                session.hoverStrip = nullptr;
        }
        UpdateGroupTearoff(session, point, group.phase == 2 && group.detaching,
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
            session.sourceStrip->FinishDrag(session.tabId);
            AbortDragSession();
        } else {
            session.sourceStrip->TryPromote(session.tabId, local);
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
        phase = "activation-context";
        HMODULE module{};
        check_bool(
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&Initialize), &module));
        ACTCTXW context{sizeof(context)};
        context.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        context.hModule = module;
        context.lpResourceName = MAKEINTRESOURCEW(2);
        s.activation = CreateActCtxW(&context);
        check_bool(s.activation != INVALID_HANDLE_VALUE);
        check_bool(ActivateActCtx(s.activation, &s.activationCookie));
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
        phase = "resources";
        s.app.Resources().MergedDictionaries().Append(Controls::XamlControlsResources());
        // Firefox caps a tab at 225 CSS px; the stock TabView default is 240.
        s.app.Resources().Insert(box_value(L"TabViewItemMaxWidth"), box_value(225.0));
        s.initialization = S_OK;
    } catch (...) {
        s.initialization = to_hresult();
        fprintf(stderr, "WinUIPane initialization phase=%s hr=0x%08X\n", phase, unsigned(s.initialization));
        throw;
    }
}

HRESULT UpdateImpl(HWND hwnd, const RECT* rect, BOOL visible, bool strip) noexcept {
    WorkScope scope{strip ? EntryUpdateTabStrip : EntryUpdate};
    try {
        check_hresult(Check(hwnd));
        if (!hwnd || !rect || rect->right < rect->left || rect->bottom < rect->top) return E_INVALIDARG;
        auto& s = state();
        auto found = s.panes.find(hwnd);
        if (!visible && found == s.panes.end()) return S_OK;
        if (found != s.panes.end() && bool(found->second->strip) != strip) {
            found->second->Close();
            s.panes.erase(found);
            found = s.panes.end();
            if (!visible) return S_OK;
        }
        if (found == s.panes.end()) {
            Initialize();
            auto pane = std::make_unique<Pane>();
            try {
                WorkScope create{EntryPaneCreate};
                pane->Create(hwnd, strip);
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
            site.MoveAndResize({rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top});
            pane.appliedBounds = requested;
            pane.boundsApplied = true;
            ++work.geometryApplied;
        }
        if (visible) {
            // Firefox's compositor is a sibling HWND covering the client area.
            // Ask the window where it actually is rather than remembering where
            // it was last put: a restack by anything else still has to be undone.
            if (pane.strip) {
                NativeWindowDrag::Restack(child);
            } else if (GetWindow(child, GW_HWNDPREV) != nullptr) {
                check_bool(SetWindowPos(child, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
                ++work.restacks;
            }
        }
        if (pane.appliedVisible != int(bool(visible))) {
            if (visible)
                site.Show();
            else
                site.Hide();
            pane.appliedVisible = int(bool(visible));
            ++work.visibilityApplied;
        }
        if (pane.strip) pane.windowDrag.SetVisible(bool(visible));
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
        fwprintf(stderr, L"WinUIPane update hr=0x%08X message=%s\n", unsigned(error.code()), error.message().c_str());
        return error.code();
    } catch (...) {
        return to_hresult();
    }
}
HRESULT WINAPI Update(HWND hwnd, const RECT* rect, BOOL visible) noexcept {
    return UpdateImpl(hwnd, rect, visible, false);
}
HRESULT WINAPI UpdateTabStrip(HWND hwnd, const RECT* rect, BOOL visible) noexcept {
    return UpdateImpl(hwnd, rect, visible, true);
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
        HWND child = GetWindowFromWindowId(pane.source.SiteBridge().WindowId());
        RECT bounds{};
        check_bool(GetClientRect(child, &bounds));
        auto& work = geometryWork();
        ++work.clips;
        // The same exclusions against the same client rect produce the same
        // region, and the window already holds it. Rebuilding it and handing it
        // back costs a redrawing SetWindowRgn for no change. The client rect is
        // the whole of what can invalidate a region: it is in the window's own
        // coordinates, so moving the island leaves it valid and only a resize
        // does not -- and GetClientRect above has already reported that.
        if (pane.clipApplied && EqualRect(&pane.appliedClipClient, &bounds) &&
            pane.appliedExclusions.size() == count &&
            std::equal(pane.appliedExclusions.begin(), pane.appliedExclusions.end(), exclusions,
                       [](RECT const& a, RECT const& b) { return EqualRect(&a, &b) != 0; }))
            return S_OK;
        HRGN region = CreateRectRgnIndirect(&bounds);
        if (!region) return E_OUTOFMEMORY;
        for (uint32_t i = 0; i < count; ++i) {
            RECT exclusion = exclusions[i];
            if (GetWindowLongPtrW(child, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) {
                exclusion.left = bounds.right - exclusions[i].right;
                exclusion.right = bounds.right - exclusions[i].left;
            }
            HRGN cut = CreateRectRgnIndirect(&exclusion);
            int result = cut ? CombineRgn(region, region, cut, RGN_DIFF) : ERROR;
            if (cut) DeleteObject(cut);
            if (result == ERROR) {
                DeleteObject(region);
                return E_FAIL;
            }
        }
        // Windows owns the region on success; it clips rendering and hit testing.
        if (!SetWindowRgn(child, region, TRUE)) {
            DeleteObject(region);
            return E_FAIL;
        }
        pane.appliedExclusions.assign(exclusions, exclusions + count);
        pane.appliedClipClient = bounds;
        pane.clipApplied = true;
        if (pane.strip) pane.windowDrag.SetClip(child, pane.appliedExclusions);
        ++work.clipsApplied;
        scope.Stop();
        if (DiagnosticsEnabled())
            fprintf(stderr, "WinUIPane clip exclusions=%u visible=%d\n", count, IsWindowVisible(child));
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
        found->second->address->observeFocus();
        found->second->address->Sync(snapshot);
        return S_OK;
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
        if (!visible) { pane.CloseAddressHost(); return S_OK; }
        if (!pane.strip) return E_NOTIMPL;
        if (!pane.addressSource) {
            pane.addressSource = DesktopWindowXamlSource();
            pane.addressSource.Initialize(GetWindowIdFromWindow(hwnd));
            pane.address = std::make_unique<NativeAddress>(pane.shell, true);
            pane.WireAddress();
            pane.addressTakeFocus = pane.addressSource.TakeFocusRequested([&pane](auto const&, auto const&) {
                pane.address->focusWithin = false;
                SetFocus(pane.parent);
                pane.address->Action(L"blur", (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
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
        if (GetWindow(pane.addressWindow, GW_HWNDPREV))
            check_bool(SetWindowPos(pane.addressWindow, HWND_TOP, 0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        site.Show();
        pane.address->PlacePopup();
        return S_OK;
    } catch (...) {
        const auto error = to_hresult();
        auto found = state().panes.find(hwnd);
        if (found != state().panes.end()) found->second->CloseAddressHost();
        return error;
    }
}
HRESULT WINAPI FocusAddress(HWND hwnd, BOOL selectAll) noexcept {
    try {
        check_hresult(Check(hwnd));
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end() || !found->second->addressSource) return E_NOT_VALID_STATE;
        auto& pane = *found->second;
        pane.addressSource.NavigateFocus(XamlSourceFocusNavigationRequest(XamlSourceFocusNavigationReason::First));
        if (!pane.address->field.Focus(FocusState::Programmatic)) return E_FAIL;
        if (selectAll) pane.address->field.SelectAll();
        return S_OK;
    } catch (...) { return to_hresult(); }
}
// Publication only, since v12: nothing native travels back through this call.
// Every native intent -- menu transitions, selection, and the strip's own
// gestures -- is queued and drained through the request path instead.
HRESULT WINAPI SyncTabMenu(HWND hwnd, const wchar_t* input) noexcept {
    WorkScope scope{EntrySyncTabMenu};
    try {
        check_hresult(Check(hwnd));
        if (!input || wcsnlen(input, 1048577) > 1048576) return E_INVALIDARG;
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto snapshot = NativeTabMenu::Object::Parse(input);
        auto& pane = *found->second;
        if (snapshot.HasKey(L"strip")) {
            // Strip publication is a separate transaction: validate the envelope
            // before touching XAML, and never drain any queued input here.
            if (!pane.strip || snapshot.Size() != 3 || snapshot.GetNamedNumber(L"version") != 2 ||
                !snapshot.GetNamedBoolean(L"publishOnly", false))
                return E_INVALIDARG;
            pane.strip->Publish(snapshot.GetNamedObject(L"strip"));
            return S_OK;
        }
        if (snapshot.HasKey(L"tabPreview")) {
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
HRESULT WINAPI SyncTabs(HWND hwnd, const WinUITab* tabs, uint32_t count, uint32_t selected,
                        uint32_t* requested) noexcept {
    WorkScope scope{EntrySyncTabs};
    try {
        check_hresult(Check(hwnd));
        if (!requested) return E_POINTER;
        *requested = 0;
        if (!hwnd || (count && !tabs) || count > 10000) return E_INVALIDARG;
        std::set<uint32_t> ids;
        for (uint32_t i = 0; i < count; ++i) {
            if (!tabs[i].id || !tabs[i].title || !ids.insert(tabs[i].id).second ||
                wcsnlen(tabs[i].title, 32769) > 32768 || (tabs[i].commands & ~31u) || (tabs[i].commands & 12u) == 12u)
                return E_INVALIDARG;
        }
        if (selected && !ids.count(selected)) return E_INVALIDARG;
        auto found = state().panes.find(hwnd);
        if (found == state().panes.end()) return E_INVALIDARG;
        auto& pane = *found->second;
        // Reconcile any outstanding optimistic selection feedback against the
        // authoritative outcomes recorded since the last snapshot.
        pane.ReconcileOutcomes();
        pane.Sync(tabs, count, selected);
        pane.browserSelection = selected;
        // Since v11 this carries the presentation-model revision this snapshot
        // produced; the next applyTabDelta names it as its base. Selection,
        // close and new-tab all travel on the request path instead.
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
        if (pane.drainOpen) {
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
        // Message-agnostic: this is the ground-truth tick a process-wide
        // drag session's correctness depends on (Pane.cpp's TabDragSession/
        // TickDragSession), driven by every message this hook sees rather
        // than by parsing any one of them.
        TickDragSession();
        for (auto const& entry : state().panes) {
            auto& pane = *entry.second;
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
                // Left to throw exactly as the direct expression did if the
                // island is not available yet: Translate's own catch handles it.
                if (!pane.contentIsland) pane.contentIsland = pane.source.Content().XamlRoot().ContentIsland();
                for (auto popup : pane.contentIsland.Popups()) {
                    HWND popupWindow = GetWindowFromWindowId(popup.WindowId());
                    if (message->hwnd == popupWindow || IsChild(popupWindow, message->hwnd)) {
                        ownsMessage = true;
                        break;
                    }
                }
            }
            if (!ownsMessage) continue;
            if (message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN) {
                const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool addressShortcut = pane.addressSource &&
                    (message->wParam == VK_F6 || message->wParam == VK_F5 ||
                     (control && (message->wParam == 'K' || message->wParam == 'R')) ||
                     ((GetKeyState(VK_MENU) & 0x8000) &&
                      (message->wParam == 'D' || message->wParam == VK_LEFT || message->wParam == VK_RIGHT)));
                if (addressShortcut || (control && (message->wParam == 'L' || message->wParam == 'T' || message->wParam == 'W' ||
                                (pane.strip && (message->wParam == VK_TAB || message->wParam == VK_PRIOR ||
                                                message->wParam == VK_NEXT))))) {
                    SetFocus(pane.parent);
                    PostMessageW(pane.parent, message->message, message->wParam, message->lParam);
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
        if (s.activationCookie) {
            check_bool(DeactivateActCtx(0, s.activationCookie));
            s.activationCookie = 0;
        }
        if (s.activation != INVALID_HANDLE_VALUE) {
            ReleaseActCtx(s.activation);
            s.activation = INVALID_HANDLE_VALUE;
        }
        return S_OK;
    } catch (...) {
        return to_hresult();
    }
}
}  // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI WinUIPaneGetApi(uint32_t version, uint32_t size,
                                                                WinUIPaneApi* output) noexcept {
    if (!output) return E_POINTER;
    if (version != WINUI_PANE_ABI_VERSION || size != sizeof(WinUIPaneApi)) return E_NOINTERFACE;
    *output = {sizeof(WinUIPaneApi),
               WINUI_PANE_ABI_VERSION,
               Update,
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
               FocusAddress};
    return S_OK;
}

#ifdef WINUI_PANE_TESTING
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
            if (pane.strip) pane.strip->core = nullptr;
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
        else if (operation == 10 || operation == 11) {
            // 10 pauses the slide and its width motion halfway (75 of 150 ms)
            // so geometry can be read without depending on frame cadence; 11
            // resumes it. Reports whether the motion animates any width.
            *ghosts = motion.resizing ? 1 : 0;
            if (!motion.storyboard) return S_OK;
            if (operation == 11) motion.storyboard.Resume();
            else {
                motion.storyboard.Pause();
                motion.storyboard.SeekAlignedToLastTick(Windows::Foundation::TimeSpan{InlineTabGroups::MotionDuration / 2});
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
