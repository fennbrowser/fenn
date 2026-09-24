/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! The C boundary. `bridge/v2/shell_core.h` is generated from this file.
//!
//! Rules this module keeps, in the order they matter:
//!
//! * **Nothing unwinds out of an `extern "C"` function.** Every entry point
//!   runs inside [`guard`], which catches a panic, marks the core poisoned and
//!   returns [`ShellStatus::Panic`]. That is containment, not recovery: the
//!   failed operation did not happen, a poisoned core refuses every later call,
//!   and the host is expected to fall back to the legacy path rather than
//!   pretend the request succeeded.
//! * **No Rust `String`/`Vec`/`HashMap` layout is exposed.** Anything the host
//!   reads is a `#[repr(C)]` struct or a raw `uint16_t` buffer whose lifetime is
//!   stated on the function that produced it.
//! * **Every allocation has a release function in this crate.** The host never
//!   frees a buffer this crate allocated, and this crate never frees one the
//!   host allocated. They do not share an allocator.
//! * **Thread affinity is checked, not assumed.** A core belongs to the thread
//!   that created it (the browser's owning STA); calls from anywhere else are
//!   refused with [`ShellStatus::WrongThread`]. There is no `unsafe impl Send`.
//! * **No borrow, iterator or lock is held across a call back into the host.**
//!   These functions never call the host at all: they mutate state and return.
//!   Wakeups are values in an out-parameter, so the host performs the effect
//!   after this crate has released its `&mut`.
//!
//! Booleans cross as `bool`, which is one byte with value 0 or 1 on both sides
//! for the pinned MSVC target; `size_t`/`uintptr_t` match `usize`.

use std::os::raw::c_void;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::thread::ThreadId;

use crate::intent::{self, IntentKind};
use crate::model::{ApplyError, Change, ModelLimits, TabEntry, TabModel};
use crate::queue::{
    CompleteError, DrainError, EnqueueStatus, Limits, Outcome, Request, RequestKind, RequestQueue,
};
use crate::SHELL_CORE_ABI_VERSION;
use crate::strip::{ShellStripChange, ShellStripSlot, StripState};

/// Result of every entry point. `Ok` is zero so the host can test it directly.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u32)]
pub enum ShellStatus {
    Ok = 0,
    /// A null pointer, an unknown enum value, or a length over a bound that has
    /// no more specific status.
    InvalidArgument = 1,
    /// Called from a thread other than the one that created this core.
    WrongThread = 2,
    /// A drain is already running, or the queue is blocked.
    Busy = 3,
    /// `end_drain` without `begin_drain`.
    NotDraining = 4,
    UnknownRequest = 5,
    /// This request already has an outcome: a replay inside the live session.
    DuplicateRequest = 6,
    /// A delta was computed against a revision this core no longer holds. The
    /// host answers with one snapshot.
    Gap = 7,
    TooLarge = 8,
    UnknownTab = 9,
    /// The endpoint is closed; nothing new is accepted.
    Closed = 10,
    /// A panic was contained. The operation did not happen and this core will
    /// refuse every later call.
    Panic = 11,
    /// The queue refused the request and said why in the result structure.
    Rejected = 12,
}

/// Bounds for one endpoint. Zero in any field means "use the built-in default
/// for that field", so the host can set only what it cares about.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct ShellCoreLimits {
    pub max_entries: u32,
    pub max_queued_bytes: u32,
    pub max_request_bytes: u32,
    pub max_in_flight: u32,
    pub completion_history: u32,
    pub max_pending_outcomes: u32,
    pub max_tabs: u32,
    pub max_title_units: u32,
    pub max_changes: u32,
    pub reserved: u32,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct ShellEnqueueResult {
    pub id: u64,
    /// The earlier queued request this one replaced, or 0. Its
    /// [`Outcome::Superseded`] is waiting in `shell_core_take_outcomes`.
    pub superseded: u64,
    /// One of [`crate::queue::EnqueueStatus`] as a number.
    pub status: u32,
    /// True only when the host must now schedule exactly one drain
    /// notification. Never true while one is already outstanding.
    pub notify: bool,
    pub reserved: [u8; 3],
}

/// One drained request. `payload` points into the batch and stays valid until
/// `shell_core_release_batch`; copy anything you keep.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct ShellRequest {
    pub id: u64,
    pub payload: *const u16,
    pub payload_len: usize,
    pub kind: u32,
    pub tab: u32,
    pub context_revision: u32,
    pub reserved: u32,
}

/// Owned by this crate. Release with `shell_core_release_batch`, exactly once.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct ShellRequestBatch {
    pub requests: *const ShellRequest,
    pub len: usize,
    /// Opaque owner. The host must not inspect, copy or free it.
    pub handle: *mut c_void,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct ShellOutcome {
    pub id: u64,
    pub outcome: u32,
    pub reserved: u32,
}

/// Owned by this crate. Release with `shell_core_release_outcomes`.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct ShellOutcomeBatch {
    pub outcomes: *const ShellOutcome,
    pub len: usize,
    pub handle: *mut c_void,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct ShellChange {
    pub kind: u32,
    pub tab: u32,
    pub from: u32,
    pub to: u32,
}

/// Owned by this crate. Release with `shell_core_release_changes`.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct ShellChangeBatch {
    pub changes: *const ShellChange,
    pub len: usize,
    pub handle: *mut c_void,
}

/// Low-overhead counters for the measurement harness. Reading them costs
/// nothing beyond the copy, so the host can sample them on a hot path.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct ShellCoreStats {
    pub generation: u64,
    pub enqueued: u64,
    pub drained: u64,
    pub notifications: u64,
    pub dropped_outcomes: u64,
    pub queued_entries: u32,
    pub queued_bytes: u32,
    pub in_flight: u32,
    pub drain_state: u32,
    pub model_revision: u32,
    pub model_tabs: u32,
    pub pending_changes: u32,
    pub reserved: u32,
}

/// Opaque per-endpoint state. One per native shell surface.
pub struct ShellCore {
    owner: ThreadId,
    poisoned: bool,
    queue: RequestQueue,
    model: TabModel,
    strip: StripState,
    group_hover: crate::group_hover::GroupHover,
    window_drag: crate::window_drag::WindowDrag,
    snapshot: Option<SnapshotBuilder>,
    stats: Counters,
}

#[derive(Default)]
struct Counters {
    enqueued: u64,
    drained: u64,
    notifications: u64,
}

struct SnapshotBuilder {
    session: u64,
    entries: Vec<TabEntry>,
}

struct BatchOwner {
    #[allow(dead_code)]
    requests: Vec<Request>,
    view: Vec<ShellRequest>,
}

struct OutcomeOwner {
    view: Vec<ShellOutcome>,
}

struct ChangeOwner {
    view: Vec<ShellChange>,
}

fn limits_from(raw: &ShellCoreLimits) -> (Limits, ModelLimits) {
    let default_queue = Limits::default();
    let default_model = ModelLimits::default();
    let pick = |value: u32, fallback: usize| {
        if value == 0 {
            fallback
        } else {
            value as usize
        }
    };
    (
        Limits {
            max_entries: pick(raw.max_entries, default_queue.max_entries),
            max_queued_bytes: pick(raw.max_queued_bytes, default_queue.max_queued_bytes),
            max_request_bytes: pick(raw.max_request_bytes, default_queue.max_request_bytes),
            max_in_flight: pick(raw.max_in_flight, default_queue.max_in_flight),
            completion_history: pick(raw.completion_history, default_queue.completion_history),
            max_pending_outcomes: pick(raw.max_pending_outcomes, default_queue.max_pending_outcomes),
        },
        ModelLimits {
            max_tabs: pick(raw.max_tabs, default_model.max_tabs),
            max_title_units: pick(raw.max_title_units, default_model.max_title_units),
            max_changes: pick(raw.max_changes, default_model.max_changes),
        },
    )
}

fn apply_error(error: ApplyError) -> ShellStatus {
    match error {
        ApplyError::Gap => ShellStatus::Gap,
        ApplyError::TooLarge => ShellStatus::TooLarge,
        ApplyError::UnknownTab => ShellStatus::UnknownTab,
    }
}

fn change_view(change: &Change) -> ShellChange {
    ShellChange { kind: change.kind as u32, tab: change.tab, from: change.from, to: change.to }
}

/// Runs `body` with the core, having checked the handle, the owning thread and
/// the poison flag, and containing any panic.
///
/// # Safety
/// `core` must be a pointer returned by `shell_core_create` and not yet passed
/// to `shell_core_destroy`.
unsafe fn guard<F>(core: *mut ShellCore, body: F) -> ShellStatus
where
    F: FnOnce(&mut ShellCore) -> ShellStatus,
{
    let Some(core) = core.as_mut() else {
        return ShellStatus::InvalidArgument;
    };
    if core.owner != std::thread::current().id() {
        return ShellStatus::WrongThread;
    }
    if core.poisoned {
        return ShellStatus::Panic;
    }
    match catch_unwind(AssertUnwindSafe(|| body(core))) {
        Ok(status) => status,
        Err(_) => {
            core.poisoned = true;
            ShellStatus::Panic
        }
    }
}

/// # Safety
/// `pointer` must be null, or valid for reads of `len` `u16` values.
unsafe fn slice<'a>(pointer: *const u16, len: usize) -> Option<&'a [u16]> {
    match (pointer.is_null(), len) {
        (true, 0) => Some(&[]),
        (true, _) => None,
        (false, _) => Some(std::slice::from_raw_parts(pointer, len)),
    }
}

/// Version of this boundary. The host refuses a core whose version differs from
/// the one its header was generated for.
#[no_mangle]
pub extern "C" fn shell_core_abi_version() -> u32 {
    SHELL_CORE_ABI_VERSION
}

/// What the caller observed about the address surface when an intent arrived.
/// Each field is `0` for false and non-zero for true; `bool` is deliberately
/// not used across the boundary, where its representation is the host
/// compiler's business.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ShellIntentConditions {
    pub applying: u32,
    pub composing: u32,
    pub has_session: u32,
    pub mode_intent_staged: u32,
    pub edit_acknowledged: u32,
    pub query_settled: u32,
}

/// Decide what to do with an arriving address-bar intent, given the one already
/// staged. `staged` is `0` when nothing is outstanding. `out_decision` receives
/// a [`crate::intent::Decision`] value.
///
/// This is a pure function of its arguments: no core handle, no thread
/// affinity, no allocation. It exists so the policy has one definition, tested
/// in `intent.rs`, rather than one in Rust and another in the caller.
///
/// # Safety
/// `conditions` and `out_decision` must be readable/writable pointers.
#[no_mangle]
pub unsafe extern "C" fn shell_core_decide_intent(
    incoming: u32,
    staged: u32,
    conditions: *const ShellIntentConditions,
    out_decision: *mut u32,
) -> ShellStatus {
    let Some(out) = out_decision.as_mut() else {
        return ShellStatus::InvalidArgument;
    };
    let Some(raw) = conditions.as_ref() else {
        return ShellStatus::InvalidArgument;
    };
    let Some(incoming) = IntentKind::from_raw(incoming) else {
        return ShellStatus::InvalidArgument;
    };
    // 0 means "nothing staged"; any other value must name a real kind.
    let staged = if staged == 0 {
        None
    } else {
        match IntentKind::from_raw(staged) {
            Some(kind) => Some(kind),
            None => return ShellStatus::InvalidArgument,
        }
    };
    let conditions = intent::Conditions {
        applying: raw.applying != 0,
        composing: raw.composing != 0,
        has_session: raw.has_session != 0,
        mode_intent_staged: raw.mode_intent_staged != 0,
        edit_acknowledged: raw.edit_acknowledged != 0,
        query_settled: raw.query_settled != 0,
    };
    match catch_unwind(AssertUnwindSafe(|| intent::decide(incoming, staged, conditions))) {
        Ok(decision) => {
            *out = decision as u32;
            ShellStatus::Ok
        }
        Err(_) => ShellStatus::Panic,
    }
}

/// Create one endpoint's core, owned by the calling thread. Returns null only
/// if `limits` is null. Release with `shell_core_destroy`.
///
/// # Safety
/// `limits` must be null or point to a readable [`ShellCoreLimits`].
#[no_mangle]
pub unsafe extern "C" fn shell_core_create(limits: *const ShellCoreLimits) -> *mut ShellCore {
    let raw = match limits.as_ref() {
        Some(value) => *value,
        None => ShellCoreLimits {
            max_entries: 0,
            max_queued_bytes: 0,
            max_request_bytes: 0,
            max_in_flight: 0,
            completion_history: 0,
            max_pending_outcomes: 0,
            max_tabs: 0,
            max_title_units: 0,
            max_changes: 0,
            reserved: 0,
        },
    };
    let (queue_limits, model_limits) = limits_from(&raw);
    let core = Box::new(ShellCore {
        owner: std::thread::current().id(),
        poisoned: false,
        queue: RequestQueue::new(queue_limits),
        model: TabModel::new(model_limits),
        strip: StripState::default(),
        group_hover: crate::group_hover::GroupHover::default(),
        window_drag: crate::window_drag::WindowDrag::default(),
        snapshot: None,
        stats: Counters::default(),
    });
    Box::into_raw(core)
}

/// Destroy the core. Outstanding requests are cancelled first, so nothing is
/// left believing it is still in flight. Safe to call with null. Must be called
/// on the owning thread; calling it twice is undefined, as for any `free`.
///
/// # Safety
/// `core` must be null, or a pointer from `shell_core_create` that has not been
/// destroyed.
#[no_mangle]
pub unsafe extern "C" fn shell_core_destroy(core: *mut ShellCore) {
    if core.is_null() {
        return;
    }
    let mut owned = Box::from_raw(core);
    // Containment again: a panic inside teardown must not unwind into C++.
    let _ = catch_unwind(AssertUnwindSafe(|| owned.queue.close()));
}

/// # Safety
/// See [`guard`]. `payload` must be valid for `payload_len` `u16` values, and
/// `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_enqueue(
    core: *mut ShellCore,
    kind: u32,
    tab: u32,
    context_revision: u32,
    payload: *const u16,
    payload_len: usize,
    out: *mut ShellEnqueueResult,
) -> ShellStatus {
    let Some(out) = out.as_mut() else {
        return ShellStatus::InvalidArgument;
    };
    *out = ShellEnqueueResult {
        id: 0,
        superseded: 0,
        status: EnqueueStatus::RejectedInvalid as u32,
        notify: false,
        reserved: [0; 3],
    };
    let Some(kind) = RequestKind::from_raw(kind) else {
        return ShellStatus::InvalidArgument;
    };
    let Some(payload) = slice(payload, payload_len) else {
        return ShellStatus::InvalidArgument;
    };
    guard(core, |core| {
        let result = core.queue.enqueue(kind, tab, context_revision, payload);
        out.id = result.id;
        out.superseded = result.superseded;
        out.status = result.status as u32;
        out.notify = result.notify;
        if result.notify {
            core.stats.notifications += 1;
        }
        match result.status {
            EnqueueStatus::Accepted => {
                core.stats.enqueued += 1;
                ShellStatus::Ok
            }
            EnqueueStatus::RejectedClosed => ShellStatus::Closed,
            _ => ShellStatus::Rejected,
        }
    })
}

/// Observe the browser context revision and report whether native selection
/// feedback is still pending. A mismatched revision permanently clears it.
///
/// # Safety
/// See [`guard`]. `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_selection_pending(
    core: *mut ShellCore, revision: u32, out: *mut bool,
) -> ShellStatus {
    guard(core, |core| {
        let Some(out) = out.as_mut() else { return ShellStatus::InvalidArgument; };
        *out = core.queue.selection_pending(revision);
        ShellStatus::Ok
    })
}

/// Report that the drain notification could not be scheduled. The core returns
/// to a state where the next enqueue notifies again; `still_pending` says
/// whether work is waiting, so the host can retry deliberately.
///
/// # Safety
/// See [`guard`].
#[no_mangle]
pub unsafe extern "C" fn shell_core_notification_failed(
    core: *mut ShellCore,
    still_pending: *mut bool,
) -> ShellStatus {
    guard(core, |core| {
        let pending = core.queue.notification_failed();
        if let Some(out) = still_pending.as_mut() {
            *out = pending;
        }
        ShellStatus::Ok
    })
}

/// # Safety
/// See [`guard`].
#[no_mangle]
pub unsafe extern "C" fn shell_core_begin_drain(core: *mut ShellCore) -> ShellStatus {
    guard(core, |core| match core.queue.begin_drain() {
        Ok(()) => ShellStatus::Ok,
        Err(DrainError::Busy) => ShellStatus::Busy,
        Err(DrainError::NotDraining) => ShellStatus::NotDraining,
    })
}

/// Take up to `max_entries` requests, or `max_bytes` of payload, whichever
/// comes first (always at least one entry, so a large payload cannot wedge the
/// queue). The batch and every `payload` in it stay valid until
/// `shell_core_release_batch`.
///
/// # Safety
/// See [`guard`]. `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_take_batch(
    core: *mut ShellCore,
    max_entries: u32,
    max_bytes: usize,
    out: *mut ShellRequestBatch,
) -> ShellStatus {
    let Some(out) = out.as_mut() else {
        return ShellStatus::InvalidArgument;
    };
    *out = ShellRequestBatch { requests: std::ptr::null(), len: 0, handle: std::ptr::null_mut() };
    if max_entries == 0 {
        return ShellStatus::InvalidArgument;
    }
    guard(core, |core| {
        let requests = core.queue.take_batch(max_entries as usize, max_bytes);
        core.stats.drained += requests.len() as u64;
        if requests.is_empty() {
            return ShellStatus::Ok;
        }
        let view: Vec<ShellRequest> = requests
            .iter()
            .map(|request| ShellRequest {
                id: request.id,
                payload: request.payload.as_ptr(),
                payload_len: request.payload.len(),
                kind: request.kind as u32,
                tab: request.tab,
                context_revision: request.context_revision,
                reserved: 0,
            })
            .collect();
        let owner = Box::new(BatchOwner { requests, view });
        out.requests = owner.view.as_ptr();
        out.len = owner.view.len();
        out.handle = Box::into_raw(owner) as *mut c_void;
        ShellStatus::Ok
    })
}

/// Release a batch from `shell_core_take_batch`. Safe with a zeroed batch.
///
/// # Safety
/// `batch` must be null or point to a batch this crate produced and that has
/// not already been released.
#[no_mangle]
pub unsafe extern "C" fn shell_core_release_batch(batch: *mut ShellRequestBatch) {
    let Some(batch) = batch.as_mut() else { return };
    if !batch.handle.is_null() {
        drop(Box::from_raw(batch.handle as *mut BatchOwner));
    }
    *batch = ShellRequestBatch { requests: std::ptr::null(), len: 0, handle: std::ptr::null_mut() };
}

/// End the drain. `notify` is set when the host must schedule exactly one more
/// drain notification because work arrived while this drain was running.
///
/// # Safety
/// See [`guard`].
#[no_mangle]
pub unsafe extern "C" fn shell_core_end_drain(
    core: *mut ShellCore,
    notify: *mut bool,
) -> ShellStatus {
    guard(core, |core| match core.queue.end_drain() {
        Ok(value) => {
            if let Some(out) = notify.as_mut() {
                *out = value;
            }
            if value {
                core.stats.notifications += 1;
            }
            ShellStatus::Ok
        }
        Err(DrainError::NotDraining) => ShellStatus::NotDraining,
        Err(DrainError::Busy) => ShellStatus::Busy,
    })
}

/// Block or unblock execution, for a host that cannot run browser commands
/// right now. Unblocking sets `notify` when work is waiting; nothing is posted
/// while blocked.
///
/// # Safety
/// See [`guard`].
#[no_mangle]
pub unsafe extern "C" fn shell_core_set_blocked(
    core: *mut ShellCore,
    blocked: bool,
    notify: *mut bool,
) -> ShellStatus {
    guard(core, |core| {
        let value = core.queue.set_blocked(blocked);
        if let Some(out) = notify.as_mut() {
            *out = value;
        }
        if value {
            core.stats.notifications += 1;
        }
        ShellStatus::Ok
    })
}

/// Record the authoritative outcome of one drained request.
///
/// # Safety
/// See [`guard`].
#[no_mangle]
pub unsafe extern "C" fn shell_core_complete(
    core: *mut ShellCore,
    id: u64,
    outcome: u32,
) -> ShellStatus {
    let Some(outcome) = Outcome::from_raw(outcome) else {
        return ShellStatus::InvalidArgument;
    };
    guard(core, |core| match core.queue.complete(id, outcome) {
        Ok(()) => ShellStatus::Ok,
        Err(CompleteError::Unknown) => ShellStatus::UnknownRequest,
        Err(CompleteError::Duplicate) => ShellStatus::DuplicateRequest,
    })
}

/// Whether this request may still be executed for the current generation.
///
/// # Safety
/// See [`guard`]. `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_is_in_flight(
    core: *mut ShellCore,
    id: u64,
    out: *mut bool,
) -> ShellStatus {
    guard(core, |core| {
        let Some(out) = out.as_mut() else {
            return ShellStatus::InvalidArgument;
        };
        *out = core.queue.is_in_flight(id);
        ShellStatus::Ok
    })
}

/// Take the outcomes recorded since the last call, so the host can reconcile
/// optimistic native feedback with what actually happened.
///
/// # Safety
/// See [`guard`]. `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_take_outcomes(
    core: *mut ShellCore,
    out: *mut ShellOutcomeBatch,
) -> ShellStatus {
    let Some(out) = out.as_mut() else {
        return ShellStatus::InvalidArgument;
    };
    *out = ShellOutcomeBatch { outcomes: std::ptr::null(), len: 0, handle: std::ptr::null_mut() };
    guard(core, |core| {
        let taken = core.queue.take_outcomes();
        if taken.is_empty() {
            return ShellStatus::Ok;
        }
        let view: Vec<ShellOutcome> = taken
            .into_iter()
            .map(|(id, outcome)| ShellOutcome { id, outcome: outcome as u32, reserved: 0 })
            .collect();
        let owner = Box::new(OutcomeOwner { view });
        out.outcomes = owner.view.as_ptr();
        out.len = owner.view.len();
        out.handle = Box::into_raw(owner) as *mut c_void;
        ShellStatus::Ok
    })
}

/// # Safety
/// `batch` must be null or a batch this crate produced, not already released.
#[no_mangle]
pub unsafe extern "C" fn shell_core_release_outcomes(batch: *mut ShellOutcomeBatch) {
    let Some(batch) = batch.as_mut() else { return };
    if !batch.handle.is_null() {
        drop(Box::from_raw(batch.handle as *mut OutcomeOwner));
    }
    *batch = ShellOutcomeBatch { outcomes: std::ptr::null(), len: 0, handle: std::ptr::null_mut() };
}

/// Cancel everything outstanding and advance the endpoint generation. Used when
/// the session is replaced; the core stays usable.
///
/// # Safety
/// See [`guard`].
#[no_mangle]
pub unsafe extern "C" fn shell_core_cancel_all(core: *mut ShellCore) -> ShellStatus {
    guard(core, |core| {
        core.queue.cancel_all();
        core.window_drag.cancel();
        core.group_hover = Default::default();
        core.snapshot = None;
        ShellStatus::Ok
    })
}

/// Stop accepting work and cancel what is outstanding. Idempotent.
///
/// # Safety
/// See [`guard`].
#[no_mangle]
pub unsafe extern "C" fn shell_core_close(core: *mut ShellCore) -> ShellStatus {
    guard(core, |core| {
        core.queue.close();
        core.window_drag.cancel();
        core.group_hover = Default::default();
        core.snapshot = None;
        ShellStatus::Ok
    })
}

/// # Safety
/// See [`guard`]. `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_stats(
    core: *mut ShellCore,
    out: *mut ShellCoreStats,
) -> ShellStatus {
    guard(core, |core| {
        let Some(out) = out.as_mut() else {
            return ShellStatus::InvalidArgument;
        };
        *out = ShellCoreStats {
            generation: core.queue.generation(),
            enqueued: core.stats.enqueued,
            drained: core.stats.drained,
            notifications: core.stats.notifications,
            dropped_outcomes: core.queue.dropped_outcomes(),
            queued_entries: core.queue.queued_len() as u32,
            queued_bytes: core.queue.queued_bytes() as u32,
            in_flight: core.queue.in_flight_len() as u32,
            drain_state: core.queue.state() as u32,
            model_revision: core.model.revision(),
            model_tabs: core.model.len() as u32,
            pending_changes: core.model.pending_changes().len() as u32,
            reserved: 0,
        };
        ShellStatus::Ok
    })
}

/// Atomically accept a strip transaction after native JSON field validation.
/// Base zero replaces the retained state; other bases must match exactly.
///
/// # Safety
/// See [`guard`]. `changes` must contain `count` readable entries.
#[no_mangle]
pub unsafe extern "C" fn shell_core_strip_apply(
    core: *mut ShellCore, base: u32, revision: u32,
    changes: *const ShellStripChange, count: usize,
) -> ShellStatus {
    guard(core, |core| {
        if count > 20_000 || (count != 0 && changes.is_null()) {
            return ShellStatus::InvalidArgument;
        }
        let changes = if count == 0 { &[] } else { std::slice::from_raw_parts(changes, count) };
        if core.strip.apply(base, revision, changes) { ShellStatus::Ok }
        else { ShellStatus::InvalidArgument }
    })
}

/// Atomically validate strip metadata and optional ordered group layout.
/// # Safety
/// See [`guard`]. Arrays must contain their stated number of readable entries.
#[no_mangle]
pub unsafe extern "C" fn shell_core_strip_apply_layout(
    core: *mut ShellCore, base: u32, revision: u32,
    changes: *const ShellStripChange, count: usize,
    layout: *const ShellStripSlot, layout_count: usize, has_layout: bool,
) -> ShellStatus {
    guard(core, |core| {
        if count > 20_000 || layout_count > 20_000 || (count != 0 && changes.is_null()) ||
           (layout_count != 0 && (layout.is_null() || !has_layout)) { return ShellStatus::InvalidArgument; }
        let changes = if count == 0 { &[] } else { std::slice::from_raw_parts(changes, count) };
        let layout = if !has_layout { None } else if layout_count == 0 { Some(&[][..]) }
                     else { Some(std::slice::from_raw_parts(layout, layout_count)) };
        if core.strip.apply_layout(base, revision, changes, layout) { ShellStatus::Ok }
        else { ShellStatus::InvalidArgument }
    })
}

/// Copy the retained ordered layout. Returns required count; no partial copy.
/// # Safety
/// See [`guard`]. `count` must be writable; `slots` has `capacity` writable entries.
#[no_mangle]
pub unsafe extern "C" fn shell_core_strip_layout(
    core: *mut ShellCore, slots: *mut ShellStripSlot, capacity: usize, count: *mut usize,
) -> ShellStatus {
    guard(core, |core| {
        if count.is_null() { return ShellStatus::InvalidArgument; }
        let layout = core.strip.layout();
        *count = layout.len();
        if capacity < layout.len() { return ShellStatus::InvalidArgument; }
        if !layout.is_empty() {
            if slots.is_null() { return ShellStatus::InvalidArgument; }
            std::ptr::copy_nonoverlapping(layout.as_ptr(), slots, layout.len());
        }
        ShellStatus::Ok
    })
}

/// The model revision a delta must be computed against.
///
/// # Safety
/// See [`guard`]. `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_model_revision(
    core: *mut ShellCore,
    out: *mut u32,
) -> ShellStatus {
    guard(core, |core| {
        let Some(out) = out.as_mut() else {
            return ShellStatus::InvalidArgument;
        };
        *out = core.model.revision();
        ShellStatus::Ok
    })
}

/// Apply one tab's title. O(1); no other tab is visited. `changed` reports
/// whether anything actually differs, so an unchanged title costs no native
/// property assignment.
///
/// # Safety
/// See [`guard`]. `title` must be valid for `title_len` `u16` values.
#[no_mangle]
pub unsafe extern "C" fn shell_core_model_set_title(
    core: *mut ShellCore,
    base_revision: u32,
    tab: u32,
    title: *const u16,
    title_len: usize,
    changed: *mut bool,
) -> ShellStatus {
    let Some(title) = slice(title, title_len) else {
        return ShellStatus::InvalidArgument;
    };
    guard(core, |core| match core.model.set_title(base_revision, tab, title) {
        Ok(value) => {
            if let Some(out) = changed.as_mut() {
                *out = value;
            }
            ShellStatus::Ok
        }
        Err(error) => apply_error(error),
    })
}

/// Apply the active tab. O(1).
///
/// # Safety
/// See [`guard`].
#[no_mangle]
pub unsafe extern "C" fn shell_core_model_set_selected(
    core: *mut ShellCore,
    base_revision: u32,
    tab: u32,
    changed: *mut bool,
) -> ShellStatus {
    guard(core, |core| match core.model.set_selected(base_revision, tab) {
        Ok(value) => {
            if value { core.queue.clear_selection(); }
            if let Some(out) = changed.as_mut() {
                *out = value;
            }
            ShellStatus::Ok
        }
        Err(error) => apply_error(error),
    })
}

/// Start a snapshot for `session`. Snapshots are built entry by entry so no
/// array of pointers crosses the boundary. A second `begin` discards an
/// unfinished one.
///
/// # Safety
/// See [`guard`].
#[no_mangle]
pub unsafe extern "C" fn shell_core_model_begin_snapshot(
    core: *mut ShellCore,
    session: u64,
    capacity: u32,
) -> ShellStatus {
    guard(core, |core| {
        core.snapshot = Some(SnapshotBuilder {
            session,
            entries: Vec::with_capacity(capacity.min(4096) as usize),
        });
        ShellStatus::Ok
    })
}

/// # Safety
/// See [`guard`]. `title` must be valid for `title_len` `u16` values.
#[no_mangle]
pub unsafe extern "C" fn shell_core_model_push_snapshot_tab(
    core: *mut ShellCore,
    tab: u32,
    title: *const u16,
    title_len: usize,
) -> ShellStatus {
    let Some(title) = slice(title, title_len) else {
        return ShellStatus::InvalidArgument;
    };
    guard(core, |core| {
        let Some(builder) = core.snapshot.as_mut() else {
            return ShellStatus::NotDraining;
        };
        builder.entries.push(TabEntry { id: tab, title: title.to_vec() });
        ShellStatus::Ok
    })
}

/// Commit the snapshot and report how many changes it produced, so a caller can
/// measure how much structural work a snapshot actually caused.
///
/// # Safety
/// See [`guard`].
#[no_mangle]
pub unsafe extern "C" fn shell_core_model_commit_snapshot(
    core: *mut ShellCore,
    selected: u32,
    change_count: *mut u32,
) -> ShellStatus {
    guard(core, |core| {
        let Some(builder) = core.snapshot.take() else {
            return ShellStatus::NotDraining;
        };
        match core.model.apply_snapshot(builder.session, &builder.entries, selected) {
            Ok(count) => {
                core.queue.retain_tabs(|tab| core.model.contains(tab));
                if let Some(out) = change_count.as_mut() {
                    *out = count as u32;
                }
                ShellStatus::Ok
            }
            Err(error) => apply_error(error),
        }
    })
}

/// Take the accumulated presentation changes. The host applies exactly these to
/// its controls; there is no full snapshot to diff.
///
/// # Safety
/// See [`guard`]. `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_model_take_changes(
    core: *mut ShellCore,
    out: *mut ShellChangeBatch,
) -> ShellStatus {
    let Some(out) = out.as_mut() else {
        return ShellStatus::InvalidArgument;
    };
    *out = ShellChangeBatch { changes: std::ptr::null(), len: 0, handle: std::ptr::null_mut() };
    guard(core, |core| {
        let taken = core.model.take_changes();
        if taken.is_empty() {
            return ShellStatus::Ok;
        }
        let view: Vec<ShellChange> = taken.iter().map(change_view).collect();
        let owner = Box::new(ChangeOwner { view });
        out.changes = owner.view.as_ptr();
        out.len = owner.view.len();
        out.handle = Box::into_raw(owner) as *mut c_void;
        ShellStatus::Ok
    })
}

/// # Safety
/// `batch` must be null or a batch this crate produced, not already released.
#[no_mangle]
pub unsafe extern "C" fn shell_core_release_changes(batch: *mut ShellChangeBatch) {
    let Some(batch) = batch.as_mut() else { return };
    if !batch.handle.is_null() {
        drop(Box::from_raw(batch.handle as *mut ChangeOwner));
    }
    *batch = ShellChangeBatch { changes: std::ptr::null(), len: 0, handle: std::ptr::null_mut() };
}

/// Whether the consumer has fallen behind and the producer should send one
/// snapshot. Never set by ordinary operation.
///
/// # Safety
/// See [`guard`]. `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_model_needs_snapshot(
    core: *mut ShellCore,
    out: *mut bool,
) -> ShellStatus {
    guard(core, |core| {
        let Some(out) = out.as_mut() else {
            return ShellStatus::InvalidArgument;
        };
        *out = core.model.needs_snapshot();
        ShellStatus::Ok
    })
}

/// Position of a tab in the current order, or `UINT32_MAX` if absent. O(1), so
/// a single-tab update never searches the collection.
///
/// # Safety
/// See [`guard`]. `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_model_index_of(
    core: *mut ShellCore,
    tab: u32,
    out: *mut u32,
) -> ShellStatus {
    guard(core, |core| {
        let Some(out) = out.as_mut() else {
            return ShellStatus::InvalidArgument;
        };
        *out = core.model.index_of(tab).map_or(u32::MAX, |value| value as u32);
        ShellStatus::Ok
    })
}

/// Deliberately panics, so the host's panic-containment path can be tested from
/// a real call. Present only in the test build of the pane.
///
/// # Safety
/// See [`guard`].
#[no_mangle]
pub unsafe extern "C" fn shell_core_test_panic(core: *mut ShellCore) -> ShellStatus {
    guard(core, |_| panic!("shell_core_test_panic"))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn core() -> *mut ShellCore {
        unsafe { shell_core_create(std::ptr::null()) }
    }

    fn utf16(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }

    #[test]
    fn group_dwell_is_revoked_by_endpoint_cancellation() {
        unsafe {
            let core = core();
            let mut armed = false;
            assert_eq!(shell_core_group_hover(core, 1, 2, 100, 240, &mut armed), ShellStatus::Ok);
            assert!(!armed);
            assert_eq!(shell_core_group_hover(core, 1, 2, 340, 240, &mut armed), ShellStatus::Ok);
            assert!(armed);
            assert_eq!(shell_core_cancel_all(core), ShellStatus::Ok);
            assert_eq!(shell_core_group_hover(core, 1, 2, 350, 240, &mut armed), ShellStatus::Ok);
            assert!(!armed);
            assert_eq!(shell_core_group_hover(core, 1, 2, 590, 240, std::ptr::null_mut()), ShellStatus::InvalidArgument);
            shell_core_destroy(core);
        }
    }

    #[test]
    fn create_destroy_and_version() {
        assert_eq!(shell_core_abi_version(), SHELL_CORE_ABI_VERSION);
        let core = core();
        assert!(!core.is_null());
        unsafe { shell_core_destroy(core) };
        unsafe { shell_core_destroy(std::ptr::null_mut()) };
    }

    #[test]
    fn pending_feedback_follows_validated_publications_across_the_abi() {
        unsafe {
            let core = core();
            let mut result = std::mem::zeroed::<ShellEnqueueResult>();
            let mut pending = false;
            let mut changed = false;
            let mut revision = 0;
            let enqueue = |kind, tab| {
                let mut result = std::mem::zeroed::<ShellEnqueueResult>();
                shell_core_enqueue(core, kind, tab, 9, std::ptr::null(), 0, &mut result)
            };
            assert_eq!(shell_core_model_begin_snapshot(core, 1, 2), ShellStatus::Ok);
            for tab in [1, 2] {
                assert_eq!(shell_core_model_push_snapshot_tab(core, tab, std::ptr::null(), 0), ShellStatus::Ok);
            }
            assert_eq!(shell_core_model_commit_snapshot(core, 1, std::ptr::null_mut()), ShellStatus::Ok);
            assert_eq!(enqueue(1, 2), ShellStatus::Ok);
            assert_eq!(shell_core_selection_pending(core, 9, &mut pending), ShellStatus::Ok);
            assert!(pending);
            assert_eq!(shell_core_selection_pending(core, 10, std::ptr::null_mut()), ShellStatus::InvalidArgument);
            shell_core_model_revision(core, &mut revision);
            // Rejected and unchanged publications must preserve feedback.
            assert_eq!(shell_core_model_set_selected(core, revision + 1, 2, &mut changed), ShellStatus::Gap);
            assert_eq!(shell_core_model_set_selected(core, revision, 1, &mut changed), ShellStatus::Ok);
            assert!(!changed);
            shell_core_selection_pending(core, 9, &mut pending);
            assert!(pending);
            shell_core_model_revision(core, &mut revision);
            assert_eq!(shell_core_model_set_selected(core, revision, 2, &mut changed), ShellStatus::Ok);
            assert!(changed);
            shell_core_selection_pending(core, 9, &mut pending);
            assert!(!pending);

            assert_eq!(enqueue(3, 2), ShellStatus::Ok);
            // Invalid snapshots cannot prune an admitted close.
            shell_core_model_begin_snapshot(core, 1, 0);
            assert_eq!(shell_core_model_commit_snapshot(core, 99, std::ptr::null_mut()), ShellStatus::UnknownTab);
            assert_eq!(shell_core_enqueue(core, 3, 2, 9, std::ptr::null(), 0, &mut result), ShellStatus::Rejected);
            assert_eq!(result.status, EnqueueStatus::RejectedPendingClose as u32);
            shell_core_model_begin_snapshot(core, 1, 0);
            assert_eq!(shell_core_model_commit_snapshot(core, 0, std::ptr::null_mut()), ShellStatus::Ok);
            assert_eq!(enqueue(3, 2), ShellStatus::Ok);
            assert_eq!(shell_core_selection_pending(std::ptr::null_mut(), 9, &mut pending), ShellStatus::InvalidArgument);
            shell_core_destroy(core);
        }
    }

    #[test]
    fn null_core_and_null_out_parameters_are_refused_not_dereferenced() {
        let mut result = ShellEnqueueResult {
            id: 0,
            superseded: 0,
            status: 0,
            notify: false,
            reserved: [0; 3],
        };
        unsafe {
            assert_eq!(
                shell_core_enqueue(std::ptr::null_mut(), 1, 1, 0, std::ptr::null(), 0, &mut result),
                ShellStatus::InvalidArgument
            );
            let core = core();
            assert_eq!(
                shell_core_enqueue(core, 1, 1, 0, std::ptr::null(), 0, std::ptr::null_mut()),
                ShellStatus::InvalidArgument
            );
            assert_eq!(shell_core_stats(core, std::ptr::null_mut()), ShellStatus::InvalidArgument);
            assert_eq!(shell_core_enqueue(core, 99, 1, 0, std::ptr::null(), 0, &mut result),
                ShellStatus::InvalidArgument, "unknown kind");
            // The C++ side mirrors these numbers by hand (NativeShellCore::Kind
            // in native/WinUIPane/ShellCore.h); cbindgen does not export the
            // enum because the boundary carries it as a u32. Pin the range so a
            // kind added on one side and not the other fails here.
            for kind in 1..=8 {
                assert_eq!(
                    shell_core_enqueue(core, kind, 1, 0, std::ptr::null(), 0, &mut result),
                    ShellStatus::Ok, "kind {kind} is part of the boundary"
                );
            }
            assert_eq!(shell_core_enqueue(core, 9, 1, 0, std::ptr::null(), 0, &mut result),
                ShellStatus::InvalidArgument, "one past the last kind");
            // The intent boundary refuses the same way, and refuses a staged
            // kind it does not know rather than treating it as "nothing".
            let conditions = ShellIntentConditions {
                applying: 0, composing: 0, has_session: 1,
                mode_intent_staged: 0, edit_acknowledged: 1, query_settled: 1,
            };
            let mut decision = 0u32;
            assert_eq!(
                shell_core_decide_intent(1, 0, &conditions, &mut decision),
                ShellStatus::Ok
            );
            assert_eq!(decision, crate::intent::Decision::Stage as u32);
            assert_eq!(
                shell_core_decide_intent(10, 0, &conditions, &mut decision),
                ShellStatus::InvalidArgument, "unknown incoming kind"
            );
            assert_eq!(
                shell_core_decide_intent(1, 10, &conditions, &mut decision),
                ShellStatus::InvalidArgument, "unknown staged kind"
            );
            assert_eq!(
                shell_core_decide_intent(1, 0, std::ptr::null(), &mut decision),
                ShellStatus::InvalidArgument
            );
            assert_eq!(
                shell_core_decide_intent(1, 0, &conditions, std::ptr::null_mut()),
                ShellStatus::InvalidArgument
            );
            // A non-null length with a null pointer must never be dereferenced.
            assert_eq!(
                shell_core_enqueue(core, 1, 1, 0, std::ptr::null(), 4, &mut result),
                ShellStatus::InvalidArgument
            );
            shell_core_destroy(core);
        }
    }

    #[test]
    fn a_core_is_refused_from_another_thread() {
        let core = core();
        let address = core as usize;
        let status = std::thread::spawn(move || unsafe {
            shell_core_begin_drain(address as *mut ShellCore)
        })
        .join()
        .unwrap();
        assert_eq!(status, ShellStatus::WrongThread);
        unsafe { shell_core_destroy(core) };
    }

    #[test]
    fn a_panic_is_contained_and_poisons_the_core() {
        let core = core();
        unsafe {
            assert_eq!(shell_core_test_panic(core), ShellStatus::Panic);
            // Poisoned: nothing later pretends to work.
            assert_eq!(shell_core_begin_drain(core), ShellStatus::Panic);
            let mut stats = std::mem::zeroed::<ShellCoreStats>();
            assert_eq!(shell_core_stats(core, &mut stats), ShellStatus::Panic);
            shell_core_destroy(core);
        }
    }

    #[test]
    fn a_full_request_round_trip_reports_every_stage() {
        let core = core();
        unsafe {
            let payload = utf16("{\"tabs\":[7]}");
            let mut result = std::mem::zeroed::<ShellEnqueueResult>();
            assert_eq!(
                shell_core_enqueue(core, RequestKind::SelectTab as u32, 7, 3,
                    payload.as_ptr(), payload.len(), &mut result),
                ShellStatus::Ok
            );
            assert!(result.notify);
            assert_eq!(result.id, 1);

            assert_eq!(shell_core_begin_drain(core), ShellStatus::Ok);
            assert_eq!(shell_core_begin_drain(core), ShellStatus::Busy);

            let mut batch = std::mem::zeroed::<ShellRequestBatch>();
            assert_eq!(shell_core_take_batch(core, 8, 1 << 20, &mut batch), ShellStatus::Ok);
            assert_eq!(batch.len, 1);
            let request = *batch.requests;
            assert_eq!(request.kind, RequestKind::SelectTab as u32);
            assert_eq!(request.tab, 7);
            assert_eq!(request.context_revision, 3);
            let delivered =
                std::slice::from_raw_parts(request.payload, request.payload_len).to_vec();
            assert_eq!(delivered, payload);
            shell_core_release_batch(&mut batch);
            assert!(batch.handle.is_null(), "release clears the batch");
            shell_core_release_batch(&mut batch);

            let mut in_flight = false;
            shell_core_is_in_flight(core, request.id, &mut in_flight);
            assert!(in_flight);
            assert_eq!(shell_core_complete(core, request.id, Outcome::Completed as u32), ShellStatus::Ok);
            assert_eq!(
                shell_core_complete(core, request.id, Outcome::Completed as u32),
                ShellStatus::DuplicateRequest
            );
            assert_eq!(shell_core_complete(core, 404, 1), ShellStatus::UnknownRequest);
            assert_eq!(shell_core_complete(core, request.id, 77), ShellStatus::InvalidArgument);

            let mut notify = true;
            assert_eq!(shell_core_end_drain(core, &mut notify), ShellStatus::Ok);
            assert!(!notify);
            assert_eq!(shell_core_end_drain(core, &mut notify), ShellStatus::NotDraining);

            let mut outcomes = std::mem::zeroed::<ShellOutcomeBatch>();
            assert_eq!(shell_core_take_outcomes(core, &mut outcomes), ShellStatus::Ok);
            assert_eq!(outcomes.len, 1);
            assert_eq!((*outcomes.outcomes).outcome, Outcome::Completed as u32);
            shell_core_release_outcomes(&mut outcomes);

            shell_core_destroy(core);
        }
    }

    #[test]
    fn rejection_is_reported_separately_from_a_boundary_error() {
        let limits = ShellCoreLimits {
            max_entries: 1,
            max_queued_bytes: 0,
            max_request_bytes: 0,
            max_in_flight: 0,
            completion_history: 0,
            max_pending_outcomes: 0,
            max_tabs: 0,
            max_title_units: 0,
            max_changes: 0,
            reserved: 0,
        };
        unsafe {
            let core = shell_core_create(&limits);
            let mut result = std::mem::zeroed::<ShellEnqueueResult>();
            assert_eq!(
                shell_core_enqueue(core, RequestKind::CloseTab as u32, 1, 0, std::ptr::null(), 0, &mut result),
                ShellStatus::Ok
            );
            assert_eq!(
                shell_core_enqueue(core, RequestKind::CloseTab as u32, 2, 0, std::ptr::null(), 0, &mut result),
                ShellStatus::Rejected
            );
            assert_eq!(result.status, EnqueueStatus::RejectedQueueFull as u32);
            assert_eq!(result.id, 0);
            assert_eq!(shell_core_close(core), ShellStatus::Ok);
            assert_eq!(
                shell_core_enqueue(core, RequestKind::CloseTab as u32, 3, 0, std::ptr::null(), 0, &mut result),
                ShellStatus::Closed
            );
            shell_core_destroy(core);
        }
    }

    #[test]
    fn the_model_round_trips_snapshots_and_deltas() {
        unsafe {
            let core = core();
            assert_eq!(shell_core_model_begin_snapshot(core, 1, 3), ShellStatus::Ok);
            for (id, title) in [(1u32, "one"), (2, "two"), (3, "three")] {
                let value = utf16(title);
                assert_eq!(
                    shell_core_model_push_snapshot_tab(core, id, value.as_ptr(), value.len()),
                    ShellStatus::Ok
                );
            }
            let mut count = 0;
            assert_eq!(shell_core_model_commit_snapshot(core, 2, &mut count), ShellStatus::Ok);
            assert_eq!(count, 4, "three insertions and the active tab");

            let mut changes = std::mem::zeroed::<ShellChangeBatch>();
            assert_eq!(shell_core_model_take_changes(core, &mut changes), ShellStatus::Ok);
            assert_eq!(changes.len, 4);
            shell_core_release_changes(&mut changes);

            let mut revision = 0;
            shell_core_model_revision(core, &mut revision);
            let title = utf16("renamed");
            let mut changed = false;
            assert_eq!(
                shell_core_model_set_title(core, revision, 3, title.as_ptr(), title.len(), &mut changed),
                ShellStatus::Ok
            );
            assert!(changed);
            // The same delta replayed names a revision that has moved on.
            assert_eq!(
                shell_core_model_set_title(core, revision, 3, title.as_ptr(), title.len(), &mut changed),
                ShellStatus::Gap
            );
            shell_core_model_revision(core, &mut revision);
            assert_eq!(shell_core_model_set_selected(core, revision, 99, &mut changed), ShellStatus::UnknownTab);
            assert_eq!(shell_core_model_set_selected(core, revision, 1, &mut changed), ShellStatus::Ok);
            assert!(changed);

            assert_eq!(shell_core_model_take_changes(core, &mut changes), ShellStatus::Ok);
            assert_eq!(changes.len, 2, "one title and one selection, nothing else");
            shell_core_release_changes(&mut changes);

            let mut index = 0;
            shell_core_model_index_of(core, 3, &mut index);
            assert_eq!(index, 2);
            shell_core_model_index_of(core, 99, &mut index);
            assert_eq!(index, u32::MAX);

            let mut stats = std::mem::zeroed::<ShellCoreStats>();
            shell_core_stats(core, &mut stats);
            assert_eq!(stats.model_tabs, 3);
            assert_eq!(stats.pending_changes, 0);

            shell_core_destroy(core);
        }
    }

    #[test]
    fn pushing_a_snapshot_tab_without_beginning_one_is_refused() {
        unsafe {
            let core = core();
            let title = utf16("x");
            assert_eq!(
                shell_core_model_push_snapshot_tab(core, 1, title.as_ptr(), title.len()),
                ShellStatus::NotDraining
            );
            assert_eq!(
                shell_core_model_commit_snapshot(core, 0, std::ptr::null_mut()),
                ShellStatus::NotDraining
            );
            shell_core_destroy(core);
        }
    }

    #[test]
    fn cancelling_advances_the_generation_and_discards_a_partial_snapshot() {
        unsafe {
            let core = core();
            let mut result = std::mem::zeroed::<ShellEnqueueResult>();
            shell_core_enqueue(core, RequestKind::CloseTab as u32, 1, 0, std::ptr::null(), 0, &mut result);
            shell_core_model_begin_snapshot(core, 1, 1);
            let mut before = std::mem::zeroed::<ShellCoreStats>();
            shell_core_stats(core, &mut before);
            assert_eq!(shell_core_cancel_all(core), ShellStatus::Ok);
            let mut after = std::mem::zeroed::<ShellCoreStats>();
            shell_core_stats(core, &mut after);
            assert!(after.generation > before.generation);
            assert_eq!(after.queued_entries, 0);
            assert_eq!(
                shell_core_model_commit_snapshot(core, 0, std::ptr::null_mut()),
                ShellStatus::NotDraining
            );
            let mut outcomes = std::mem::zeroed::<ShellOutcomeBatch>();
            shell_core_take_outcomes(core, &mut outcomes);
            assert_eq!(outcomes.len, 1);
            assert_eq!((*outcomes.outcomes).outcome, Outcome::Cancelled as u32);
            shell_core_release_outcomes(&mut outcomes);
            shell_core_destroy(core);
        }
    }

    #[test]
    fn destroying_a_core_with_work_in_flight_does_not_unwind_or_leak_state() {
        unsafe {
            let core = core();
            let mut result = std::mem::zeroed::<ShellEnqueueResult>();
            shell_core_enqueue(core, RequestKind::CloseTab as u32, 1, 0, std::ptr::null(), 0, &mut result);
            shell_core_begin_drain(core);
            let mut batch = std::mem::zeroed::<ShellRequestBatch>();
            shell_core_take_batch(core, 8, 1 << 20, &mut batch);
            // Release the batch first: it borrows nothing from the core, but the
            // host's own ordering rule is batch-before-core.
            shell_core_release_batch(&mut batch);
            shell_core_destroy(core);
        }
    }

    #[test]
    fn replacing_or_closing_an_endpoint_revokes_pending_window_adoption() {
        unsafe {
            let core = core();
            let mut state = crate::window_drag::ShellWindowDrag::default();
            for close in [false, true] {
                assert_eq!(shell_core_window_drag(core, 1, 7, 0, 0, 4, 0, 4,
                    false, 0, &mut state), ShellStatus::Ok);
                shell_core_window_drag(core, 2, 0, 10, 10, 123, u64::MAX - 1, 4,
                    true, 0, &mut state);
                shell_core_window_drag(core, 3, 0, 0, 0, 0, 0, 0,
                    true, 0, &mut state);
                assert_eq!(state.phase, 2);
                let gesture = state.gesture;
                if close { shell_core_close(core); } else { shell_core_cancel_all(core); }
                shell_core_window_drag(core, 5, 0, 0, 0, 0, 0, 0,
                    false, gesture, &mut state);
                assert_ne!(state.phase, 3);
            }
            assert_eq!(shell_core_window_drag(core, 0, 0, 0, 0, 0, 0, 0,
                false, 0, std::ptr::null_mut()), ShellStatus::InvalidArgument);
            assert_eq!(shell_core_window_drag(core, 99, 0, 0, 0, 0, 0, 0,
                false, 0, &mut state), ShellStatus::InvalidArgument);
            shell_core_destroy(core);
        }
    }
}

/// Observe an OS move. Operations: 0 query, 1 begin (target/revision are drag
/// thresholds), 2 motion (eligible and destination identity), 3 finish (eligible
/// means confirmed release or explicit hover handoff), 4 cancel, 5 take pending
/// adoption by gesture token, 6 await request (gesture), 7 complete request
/// (gesture/tab; zero tab rejects), 8 take the accepted continuation once.
/// # Safety
/// See [`guard`]. `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_window_drag(
    core: *mut ShellCore, operation: u32, tab: u32, x: i32, y: i32,
    target: u64, target_endpoint: u64, revision: u32, eligible: bool,
    gesture: u64, out: *mut crate::window_drag::ShellWindowDrag,
) -> ShellStatus {
    guard(core, |core| {
        let Some(out) = out.as_mut() else { return ShellStatus::InvalidArgument; };
        match operation {
            0 => {},
            1 => core.window_drag.begin(tab, x, y, (target.min(u64::from(u32::MAX))) as u32, revision),
            2 => core.window_drag.observe(eligible, x, y, target, target_endpoint, revision),
            3 => core.window_drag.finish(eligible),
            4 => core.window_drag.cancel(),
            5 => if !core.window_drag.take(gesture) { *out = Default::default(); return ShellStatus::Ok; },
            6 => core.window_drag.await_adoption(gesture),
            7 => core.window_drag.complete_adoption(gesture, tab),
            8 => if !core.window_drag.take_continuation() { *out = Default::default(); return ShellStatus::Ok; },
            _ => return ShellStatus::InvalidArgument,
        }
        *out = core.window_drag.state();
        ShellStatus::Ok
    })
}

/// Observe a dwell candidate, or pass target=0 to cancel. Returns armed state.
#[no_mangle]
pub unsafe extern "C" fn shell_core_group_hover(
    core: *mut ShellCore, source: u32, target: u32, now_ms: u64,
    delay_ms: u32, armed: *mut bool,
) -> ShellStatus {
    guard(core, |core| {
        let Some(armed) = armed.as_mut() else { return ShellStatus::InvalidArgument; };
        *armed = core.group_hover.observe(source, target, now_ms, delay_ms);
        ShellStatus::Ok
    })
}


// Address lifecycle operations share the endpoint's queue lifetime and guard.
unsafe fn address_guard(core: *mut ShellCore, body: impl FnOnce(&mut RequestQueue) -> ShellStatus) -> ShellStatus {
    guard(core, |core| {
        if core.queue.is_closed() { return ShellStatus::Closed; }
        body(&mut core.queue)
    })
}

/// Read address coordination state; no WinUI objects or borrowed data escape.
/// # Safety
/// `core` is a live handle and `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_address_state(core: *mut ShellCore, out: *mut crate::address::ShellAddressState) -> ShellStatus {
    let Some(out) = out.as_mut() else { return ShellStatus::InvalidArgument; };
    *out = Default::default();
    address_guard(core, |queue| { *out = queue.address.state(); ShellStatus::Ok })
}

/// Reset on address disable or session replacement. Old outcomes cannot clear
/// requests admitted after this reset because request IDs are endpoint-unique.
/// # Safety
/// `core` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn shell_core_address_reset(core: *mut ShellCore) -> ShellStatus {
    address_guard(core, |queue| { queue.address = Default::default(); ShellStatus::Ok })
}

/// Forget the mode request on switcher disable/reset. This revokes local
/// tracking, not browser work already admitted to the queue.
/// # Safety
/// `core` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn shell_core_address_reset_mode(core: *mut ShellCore) -> ShellStatus {
    address_guard(core, |queue| { queue.address.reset_mode(); ShellStatus::Ok })
}

/// Observe a new text/composition edit. Retry keeps this same edit identity.
/// # Safety
/// `core` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn shell_core_address_edit(core: *mut ShellCore) -> ShellStatus {
    address_guard(core, |queue| if queue.address.edit() { ShellStatus::Ok } else { ShellStatus::TooLarge })
}

/// Associate an admitted address request with its intent before draining it.
/// Zero, non-address, drained and completed request IDs are rejected.
/// # Safety
/// `core` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn shell_core_address_stage(core: *mut ShellCore, request: u64, kind: u32) -> ShellStatus {
    let Some(kind) = IntentKind::from_raw(kind) else { return ShellStatus::InvalidArgument; };
    address_guard(core, |queue| if queue.stage_address(request, kind) { ShellStatus::Ok } else { ShellStatus::UnknownRequest })
}

/// Record the acknowledgment/readiness of an applied Firefox publication.
/// # Safety
/// `core` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn shell_core_address_publish(core: *mut ShellCore, edit: u64, settled: bool) -> ShellStatus {
    if edit > crate::address::MAX_EDIT_ID { return ShellStatus::InvalidArgument; }
    address_guard(core, |queue| { queue.address.publish(edit, settled); ShellStatus::Ok })
}

/// Cancel retained Enter (mode changes and global actions).
/// # Safety
/// `core` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn shell_core_address_cancel_submit(core: *mut ShellCore) -> ShellStatus {
    address_guard(core, |queue| { queue.address.cancel_submit(); ShellStatus::Ok })
}

/// Negotiate an action and retain/consume deferred Enter modifiers. The core
/// supplies mode priority, edit acknowledgment and query readiness; those fields in the
/// caller's conditions are ignored here.
/// # Safety
/// `core` is live, `conditions` readable and `out` writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_address_action(core: *mut ShellCore, incoming: u32,
    conditions: *const ShellIntentConditions, modifiers: u32, out: *mut crate::address::ShellAddressAction) -> ShellStatus {
    let Some(out) = out.as_mut() else { return ShellStatus::InvalidArgument; };
    *out = crate::address::ShellAddressAction { decision: intent::Decision::Drop as u32, modifiers: 0 };
    let Some(raw) = conditions.as_ref() else { return ShellStatus::InvalidArgument; };
    let Some(incoming) = IntentKind::from_raw(incoming) else { return ShellStatus::InvalidArgument; };
    if modifiers & !15 != 0 { return ShellStatus::InvalidArgument; }
    let observed = intent::Conditions {
        applying: raw.applying != 0, composing: raw.composing != 0, has_session: raw.has_session != 0,
        mode_intent_staged: false, edit_acknowledged: false, query_settled: false,
    };
    address_guard(core, |queue| { *out = queue.address.action(incoming, observed, modifiers); ShellStatus::Ok })
}

/// Consume the readiness notification for a retained Enter once per publication.
/// # Safety
/// `core` is live and `out` writable.
#[no_mangle]
pub unsafe extern "C" fn shell_core_address_take_deferred(core: *mut ShellCore, out: *mut bool) -> ShellStatus {
    let Some(out) = out.as_mut() else { return ShellStatus::InvalidArgument; };
    *out = false;
    address_guard(core, |queue| { *out = queue.address.take_deferred(); ShellStatus::Ok })
}


#[cfg(test)]
mod address_ffi_tests {
    use super::*;
    use crate::address::{ShellAddressAction, ShellAddressState};

    #[test]
    fn lifecycle_boundary_checks_and_publication_gated_enter() {
        unsafe {
            let core = shell_core_create(&std::mem::zeroed());
            assert!(!core.is_null());
            let mut state = ShellAddressState::default();
            let mut action = ShellAddressAction::default();
            let conditions = ShellIntentConditions { applying: 0, composing: 0, has_session: 1,
                mode_intent_staged: 0, edit_acknowledged: 1, query_settled: 1 };
            assert_eq!(shell_core_address_state(core, std::ptr::null_mut()), ShellStatus::InvalidArgument);
            assert_eq!(shell_core_address_edit(std::ptr::null_mut()), ShellStatus::InvalidArgument);
            assert_eq!(shell_core_address_stage(core, 0, 1), ShellStatus::UnknownRequest);
            assert_eq!(shell_core_address_stage(core, 1, 99), ShellStatus::InvalidArgument);
            assert_eq!(shell_core_address_publish(core, u64::MAX, true), ShellStatus::InvalidArgument);
            assert_eq!(shell_core_address_action(core, 4, &conditions, 16, &mut action), ShellStatus::InvalidArgument);
            assert_eq!(shell_core_address_action(core, 4, std::ptr::null(), 0, &mut action), ShellStatus::InvalidArgument);
            assert_eq!(shell_core_address_edit(core), ShellStatus::Ok);
            let mut admitted: ShellEnqueueResult = std::mem::zeroed();
            assert_eq!(shell_core_enqueue(core, 6, 0, 1, std::ptr::null(), 0, &mut admitted), ShellStatus::Ok);
            assert_eq!(shell_core_address_stage(core, admitted.id, 1), ShellStatus::Ok);
            assert_eq!(shell_core_address_action(core, 4, &conditions, 5, &mut action), ShellStatus::Ok);
            assert_eq!(action.decision, intent::Decision::Defer as u32); // ignores caller's claimed readiness
            assert_eq!(shell_core_begin_drain(core), ShellStatus::Ok);
            let mut batch: ShellRequestBatch = std::mem::zeroed();
            assert_eq!(shell_core_take_batch(core, 16, 65536, &mut batch), ShellStatus::Ok);
            shell_core_release_batch(&mut batch);
            assert_eq!(shell_core_complete(core, admitted.id, 1), ShellStatus::Ok);
            let mut ready = true;
            assert_eq!(shell_core_address_take_deferred(core, &mut ready), ShellStatus::Ok);
            assert!(!ready);
            assert_eq!(shell_core_address_publish(core, 1, true), ShellStatus::Ok);
            assert_eq!(shell_core_address_take_deferred(core, &mut ready), ShellStatus::Ok);
            assert!(ready);
            shell_core_address_action(core, 4, &conditions, 0, &mut action);
            assert_eq!(action.modifiers, 5);
            assert_eq!(action.decision, intent::Decision::Stage as u32);
            assert_eq!(shell_core_enqueue(core, 7, 0, 1, std::ptr::null(), 0, &mut admitted), ShellStatus::Ok);
            shell_core_address_state(core, &mut state);
            assert_eq!(state.mode_pending, 1);
            shell_core_address_action(core, 4, &conditions, 0, &mut action);
            assert_eq!(action.decision, intent::Decision::Drop as u32);
            assert_eq!(shell_core_address_reset_mode(core), ShellStatus::Ok);
            shell_core_address_state(core, &mut state);
            assert_eq!(state.mode_pending, 0);
            assert_eq!(shell_core_address_reset_mode(std::ptr::null_mut()), ShellStatus::InvalidArgument);
            shell_core_address_reset(core);
            shell_core_address_state(core, &mut state);
            assert_eq!(state.edit_id, 0);
            let handle = core as usize;
            assert_eq!(std::thread::spawn(move || shell_core_address_edit(handle as *mut ShellCore)).join().unwrap(), ShellStatus::WrongThread);
            shell_core_close(core);
            assert_eq!(shell_core_address_edit(core), ShellStatus::Closed);
            assert_eq!(shell_core_address_reset_mode(core), ShellStatus::Closed);
            shell_core_destroy(core);
        }
    }
}
