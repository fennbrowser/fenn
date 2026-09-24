/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Bounded native-intent queue and its drain scheduling.
//!
//! One instance per endpoint (one native shell surface attached to one browser
//! window). The endpoint is identified by a generation counter, never by an
//! HWND: handles are reused, generations are not.
//!
//! Scheduling contract, which the tests below pin down:
//!
//! * At most one drain notification is outstanding at a time. `enqueue`
//!   returns `notify == true` only on the transition out of [`DrainState::Idle`].
//! * Work enqueued while a drain is running is picked up by
//!   [`RequestQueue::end_drain`], which arranges the next notification. No
//!   wakeup can be lost between "the drain took the last entry" and "new work
//!   arrived".
//! * A failed notification returns the queue to `Idle` so the next enqueue
//!   notifies again, rather than leaving a wakeup that will never fire.
//! * When the consumer cannot run (a nested modal loop, a command already in
//!   progress), it blocks the queue. Unblocking arranges one notification; the
//!   queue never reposts a notification in a loop while blocked.

use std::collections::VecDeque;

/// Kinds of native intent this queue carries. The numeric values are part of
/// the C boundary.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u32)]
pub enum RequestKind {
    /// Select a tab (optionally extending a multiselection). The native
    /// control's current selection is the intent, so a newer request replaces
    /// an older queued one and the older one completes as `Superseded`.
    SelectTab = 1,
    /// One tab-menu input transition: opening (which is also the request for
    /// that tab's authoritative command description), activating a command, or
    /// dismissing. Never superseded: these are ordered transitions, not a
    /// replaceable value, and dropping one would leave a session half-open.
    MenuEvent = 2,
    /// Close one tab. Never superseded: dropping a close silently would lose a
    /// user command.
    CloseTab = 3,
    /// Open a new tab. Never superseded.
    NewTab = 4,
    /// One TabView strip command the user completed with a gesture: toggling a
    /// group, reordering a tab, tearing one out to a new window, or dropping
    /// one in from another window. Never superseded -- each is a distinct
    /// completed gesture, and collapsing two reorders would silently discard a
    /// move the user already made. Carries no tab of its own for a group
    /// toggle or an incoming adopt, so it is not subject to the target check.
    StripAction = 5,
    /// One address-bar input transition: an edit, a selection, an activation,
    /// focus or blur. A newer one replaces an older queued one, because the
    /// native side has already decided it should: the address surface runs a
    /// pairwise policy over the action it last queued (an activate defers
    /// behind an unacknowledged edit, a select replaces a text-selection, and
    /// so on) and only queues an action that policy allows. At most one is
    /// therefore outstanding, which is what the single slot this replaced
    /// guaranteed. Carries no tab.
    AddressAction = 6,
    /// One search-mode intent: revealing, opening, picking or exiting the mode
    /// menu. A separate kind from [`Self::AddressAction`] precisely so the two
    /// supersede independently -- they are two slots on the native side, and
    /// collapsing them into one would let an edit evict a queued mode pick and
    /// vice versa. Carries no tab.
    AddressModeAction = 7,
    /// Which address rows are currently on screen. A report about native
    /// layout, not something the user did, so it has a kind of its own: it
    /// coalesces with itself -- only the newest answer matters -- but it must
    /// never displace an intent, and its outcome must not be mistaken for the
    /// answer to one. Carries no tab.
    AddressVisibility = 8,
}

impl RequestKind {
    pub fn from_raw(value: u32) -> Option<Self> {
        match value {
            1 => Some(Self::SelectTab),
            2 => Some(Self::MenuEvent),
            3 => Some(Self::CloseTab),
            4 => Some(Self::NewTab),
            5 => Some(Self::StripAction),
            6 => Some(Self::AddressAction),
            7 => Some(Self::AddressModeAction),
            8 => Some(Self::AddressVisibility),
            _ => None,
        }
    }

    /// Whether a newer queued request of this kind replaces an older one that
    /// has not been drained yet. Replacement is explicit: the replaced request
    /// gets an [`Outcome::Superseded`], it is not silently discarded, and the
    /// replacement takes the replaced request's *position* so ordering against
    /// other kinds survives the collapse.
    pub fn replaces_queued(self) -> bool {
        matches!(
            self,
            Self::SelectTab
                | Self::AddressAction
                | Self::AddressModeAction
                | Self::AddressVisibility
        )
    }
}

/// How an operation finished. Receipt is not one of these: acceptance into the
/// queue is reported separately by [`EnqueueStatus`].
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u32)]
pub enum Outcome {
    Completed = 1,
    /// Firefox revalidated the request and refused it (the target moved, the
    /// policy changed, a dialog owns the window).
    Rejected = 2,
    /// The endpoint or session went away with the request outstanding.
    Cancelled = 3,
    /// A newer request of the same kind replaced this one before it ran.
    Superseded = 4,
}

impl Outcome {
    pub fn from_raw(value: u32) -> Option<Self> {
        match value {
            1 => Some(Self::Completed),
            2 => Some(Self::Rejected),
            3 => Some(Self::Cancelled),
            4 => Some(Self::Superseded),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u32)]
pub enum EnqueueStatus {
    Accepted = 0,
    /// The queue is at its entry bound. Explicit backpressure: the caller must
    /// reconcile whatever optimistic feedback it already showed.
    RejectedQueueFull = 1,
    /// The queue is at its byte bound, or this one payload is over the
    /// per-request bound.
    RejectedTooLarge = 2,
    /// Unknown kind, or a request with no target where one is required.
    RejectedInvalid = 3,
    /// The endpoint is shutting down; nothing new is accepted.
    RejectedClosed = 4,
    /// This tab already has an admitted close awaiting its outcome.
    RejectedPendingClose = 5,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CompleteError {
    /// No such request is in flight for this endpoint generation.
    Unknown,
    /// This request already has an outcome. Prevents replay inside the live
    /// session; it says nothing about delivery across a process crash.
    Duplicate,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DrainError {
    /// A drain is already running, or the consumer has blocked the queue.
    Busy,
    /// `end_drain` without a matching `begin_drain`.
    NotDraining,
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Request {
    pub id: u64,
    pub kind: RequestKind,
    /// Primary target tab, or 0 when the kind does not name one.
    pub tab: u32,
    /// The published context revision the native side acted against. Firefox
    /// revalidates against its own current state; this only lets it recognise
    /// an intent formed against state that has since been replaced.
    pub context_revision: u32,
    /// Bounded owned UTF-16 detail (a small JSON object). Copied on enqueue;
    /// nothing is borrowed across the boundary.
    pub payload: Vec<u16>,
}

impl Request {
    fn bytes(&self) -> usize {
        self.payload.len() * 2
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Limits {
    pub max_entries: usize,
    /// Total queued payload, in bytes. Bounding by entries alone lets a few
    /// large payloads outweigh many small ones.
    pub max_queued_bytes: usize,
    pub max_request_bytes: usize,
    pub max_in_flight: usize,
    /// How many finished requests are remembered for replay rejection. Bounded,
    /// so this is replay protection within a live session, not a durable log.
    pub completion_history: usize,
    /// How many outcomes wait to be read back by the native side.
    pub max_pending_outcomes: usize,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            max_entries: 64,
            max_queued_bytes: 256 * 1024,
            max_request_bytes: 64 * 1024,
            max_in_flight: 64,
            completion_history: 256,
            max_pending_outcomes: 256,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DrainState {
    Idle,
    NotificationPending,
    Draining,
    Blocked,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct EnqueueResult {
    pub status: EnqueueStatus,
    pub id: u64,
    /// Set when this request replaced an earlier queued one, which now has an
    /// [`Outcome::Superseded`] waiting in [`RequestQueue::take_outcomes`].
    pub superseded: u64,
    /// True only on the transition out of `Idle`: exactly one notification per
    /// endpoint is outstanding at a time.
    pub notify: bool,
}

pub struct RequestQueue {
    limits: Limits,
    pending: crate::pending::PendingCommands,
    generation: u64,
    next_id: u64,
    queued: VecDeque<Request>,
    queued_bytes: usize,
    in_flight: Vec<u64>,
    history: VecDeque<(u64, Outcome)>,
    pending_outcomes: VecDeque<(u64, Outcome)>,
    dropped_outcomes: u64,
    state: DrainState,
    /// A block requested while a drain was running, applied at `end_drain`.
    block_after_drain: bool,
    closed: bool,
}

impl RequestQueue {
    pub fn new(limits: Limits) -> Self {
        Self {
            limits,
            pending: Default::default(),
            generation: 1,
            next_id: 1,
            queued: VecDeque::new(),
            queued_bytes: 0,
            in_flight: Vec::new(),
            history: VecDeque::new(),
            pending_outcomes: VecDeque::new(),
            dropped_outcomes: 0,
            state: DrainState::Idle,
            block_after_drain: false,
            closed: false,
        }
    }

    pub fn limits(&self) -> Limits {
        self.limits
    }

    pub fn generation(&self) -> u64 {
        self.generation
    }

    pub fn state(&self) -> DrainState {
        self.state
    }

    pub fn queued_len(&self) -> usize {
        self.queued.len()
    }

    pub fn queued_bytes(&self) -> usize {
        self.queued_bytes
    }

    pub fn in_flight_len(&self) -> usize {
        self.in_flight.len()
    }

    /// How many outcomes were dropped because the pending-outcome bound was
    /// reached. A nonzero count is reported rather than hidden: the native side
    /// must then reconcile from authoritative state instead of from outcomes.
    pub fn dropped_outcomes(&self) -> u64 {
        self.dropped_outcomes
    }

    pub fn selection_pending(&mut self, revision: u32) -> bool {
        self.pending.selection_pending(revision)
    }

    pub fn clear_selection(&mut self) { self.pending.clear_selection(); }

    pub fn retain_tabs(&mut self, contains: impl Fn(u32) -> bool) {
        self.pending.retain_tabs(contains);
    }

    pub fn enqueue(
        &mut self,
        kind: RequestKind,
        tab: u32,
        context_revision: u32,
        payload: &[u16],
    ) -> EnqueueResult {
        if !self.closed && kind == RequestKind::CloseTab && self.pending.closing(tab) {
            return EnqueueResult { status: EnqueueStatus::RejectedPendingClose,
                id: 0, superseded: 0, notify: false };
        }
        let result = self.enqueue_inner(kind, tab, context_revision, payload);
        // Preserve the native feedback rules even on backpressure: a refused
        // selection restores browser authority; close/new-tab attempts also
        // release selection feedback, except a duplicate close above.
        match kind {
            RequestKind::SelectTab => self.pending.select(result.id, context_revision),
            RequestKind::CloseTab if tab != 0 => {
                self.pending.clear_selection();
                if result.status == EnqueueStatus::Accepted {
                    self.pending.close(tab, result.id);
                }
            }
            RequestKind::NewTab => self.pending.clear_selection(),
            _ => {}
        }
        result
    }

    fn enqueue_inner(
        &mut self,
        kind: RequestKind,
        tab: u32,
        context_revision: u32,
        payload: &[u16],
    ) -> EnqueueResult {
        let reject = |status| EnqueueResult { status, id: 0, superseded: 0, notify: false };
        if self.closed {
            return reject(EnqueueStatus::RejectedClosed);
        }
        if matches!(kind, RequestKind::SelectTab | RequestKind::MenuEvent | RequestKind::CloseTab)
            && tab == 0
        {
            return reject(EnqueueStatus::RejectedInvalid);
        }
        let bytes = payload.len() * 2;
        if bytes > self.limits.max_request_bytes {
            return reject(EnqueueStatus::RejectedTooLarge);
        }

        // Decide replacement before testing the bounds, so a superseding kind is
        // never refused for room its own replacement would have freed -- but do
        // not record the outcome until the replacement is certain to happen.
        let replace_at = if kind.replaces_queued() {
            self.queued.iter().position(|entry| entry.kind == kind)
        } else {
            None
        };
        let freed = replace_at.map_or(0, |position| self.queued[position].bytes());

        if replace_at.is_none() && self.queued.len() >= self.limits.max_entries {
            return reject(EnqueueStatus::RejectedQueueFull);
        }
        if self.queued_bytes - freed + bytes > self.limits.max_queued_bytes {
            return reject(EnqueueStatus::RejectedTooLarge);
        }

        let id = self.next_id;
        self.next_id += 1;
        let request = Request { id, kind, tab, context_revision, payload: payload.to_vec() };
        let mut superseded = 0;
        match replace_at {
            // The newest value takes the replaced request's place in the
            // stream, so a selection enqueued before a menu opening still
            // reaches Firefox before it, however many times it was updated.
            Some(position) => {
                superseded = self.queued[position].id;
                self.queued[position] = request;
                self.queued_bytes -= freed;
                self.record_outcome(superseded, Outcome::Superseded);
            }
            None => self.queued.push_back(request),
        }
        self.queued_bytes += bytes;

        let notify = match self.state {
            DrainState::Idle => {
                self.state = DrainState::NotificationPending;
                true
            }
            // A notification is already outstanding, a drain is running and
            // will check the queue when it ends, or the consumer is blocked and
            // will be notified when it unblocks.
            DrainState::NotificationPending | DrainState::Draining | DrainState::Blocked => false,
        };
        EnqueueResult { status: EnqueueStatus::Accepted, id, superseded, notify }
    }

    /// The notification could not be scheduled. Return to `Idle` so a later
    /// enqueue schedules a fresh one, and tell the caller whether work is still
    /// waiting (so it can retry deliberately rather than dropping the work).
    pub fn notification_failed(&mut self) -> bool {
        if self.state == DrainState::NotificationPending {
            self.state = DrainState::Idle;
        }
        !self.queued.is_empty()
    }

    pub fn begin_drain(&mut self) -> Result<(), DrainError> {
        match self.state {
            DrainState::Idle | DrainState::NotificationPending => {
                self.state = DrainState::Draining;
                Ok(())
            }
            DrainState::Draining | DrainState::Blocked => Err(DrainError::Busy),
        }
    }

    /// Take a bounded batch. FIFO, so no kind can starve another.
    pub fn take_batch(&mut self, max_entries: usize, max_bytes: usize) -> Vec<Request> {
        let mut batch = Vec::new();
        let mut bytes = 0;
        while batch.len() < max_entries {
            let Some(front) = self.queued.front() else { break };
            let size = front.bytes();
            // Always take at least one entry, so a single large payload cannot
            // wedge the queue.
            if !batch.is_empty() && bytes + size > max_bytes {
                break;
            }
            if self.in_flight.len() >= self.limits.max_in_flight {
                break;
            }
            let request = self.queued.pop_front().expect("front was just checked");
            self.queued_bytes -= size;
            bytes += size;
            self.in_flight.push(request.id);
            batch.push(request);
        }
        batch
    }

    /// Finish the drain. Returns whether one notification must be scheduled,
    /// which is exactly "work is waiting and nobody is going to look at it".
    pub fn end_drain(&mut self) -> Result<bool, DrainError> {
        if self.state != DrainState::Draining {
            return Err(DrainError::NotDraining);
        }
        if self.block_after_drain {
            self.block_after_drain = false;
            self.state = DrainState::Blocked;
            return Ok(false);
        }
        if self.queued.is_empty() {
            self.state = DrainState::Idle;
            Ok(false)
        } else {
            self.state = DrainState::NotificationPending;
            Ok(true)
        }
    }

    /// Block or unblock execution. Blocking during a drain is deferred to
    /// `end_drain` so the running drain finishes coherently. Unblocking
    /// arranges one notification if work is waiting.
    pub fn set_blocked(&mut self, blocked: bool) -> bool {
        if blocked {
            match self.state {
                DrainState::Draining => self.block_after_drain = true,
                _ => self.state = DrainState::Blocked,
            }
            return false;
        }
        self.block_after_drain = false;
        if self.state != DrainState::Blocked {
            return false;
        }
        if self.queued.is_empty() {
            self.state = DrainState::Idle;
            false
        } else {
            self.state = DrainState::NotificationPending;
            true
        }
    }

    pub fn complete(&mut self, id: u64, outcome: Outcome) -> Result<(), CompleteError> {
        if let Some(position) = self.in_flight.iter().position(|value| *value == id) {
            self.in_flight.remove(position);
            self.record_outcome(id, outcome);
            return Ok(());
        }
        if self.history.iter().any(|(known, _)| *known == id) {
            return Err(CompleteError::Duplicate);
        }
        Err(CompleteError::Unknown)
    }

    /// Whether this id may still be executed: it must be in flight for this
    /// generation. A replayed drain batch fails here.
    pub fn is_in_flight(&self, id: u64) -> bool {
        self.in_flight.contains(&id)
    }

    pub fn take_outcomes(&mut self) -> Vec<(u64, Outcome)> {
        self.pending_outcomes.drain(..).collect()
    }

    /// Invalidate everything outstanding: queued and in-flight requests get an
    /// explicit `Cancelled`, and the endpoint generation advances so late
    /// completions from the old generation are rejected.
    pub fn cancel_all(&mut self) {
        let ids: Vec<u64> = self
            .queued
            .drain(..)
            .map(|request| request.id)
            .chain(self.in_flight.drain(..))
            .collect();
        for id in ids {
            self.record_outcome(id, Outcome::Cancelled);
        }
        self.pending = Default::default();
        self.queued_bytes = 0;
        self.generation += 1;
        self.block_after_drain = false;
        self.state = DrainState::Idle;
    }

    /// Stop accepting work and cancel what is outstanding. Idempotent, so
    /// teardown paths can call it more than once.
    pub fn close(&mut self) {
        self.cancel_all();
        self.closed = true;
    }

    pub fn is_closed(&self) -> bool {
        self.closed
    }

    fn record_outcome(&mut self, id: u64, outcome: Outcome) {
        self.pending.complete(id);
        while self.history.len() >= self.limits.completion_history {
            self.history.pop_front();
        }
        self.history.push_back((id, outcome));
        if self.pending_outcomes.len() >= self.limits.max_pending_outcomes {
            self.pending_outcomes.pop_front();
            self.dropped_outcomes += 1;
        }
        self.pending_outcomes.push_back((id, outcome));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn queue() -> RequestQueue {
        RequestQueue::new(Limits::default())
    }

    #[test]
    fn one_notification_per_endpoint_until_drained() {
        let mut q = queue();
        let first = q.enqueue(RequestKind::CloseTab, 7, 1, &[]);
        assert_eq!(first.status, EnqueueStatus::Accepted);
        assert!(first.notify, "the idle transition schedules the only wakeup");
        for tab in 8..12 {
            let more = q.enqueue(RequestKind::CloseTab, tab, 1, &[]);
            assert!(!more.notify, "additional work accumulates, it does not repost");
        }
        assert_eq!(q.state(), DrainState::NotificationPending);
        assert_eq!(q.queued_len(), 5);
    }

    #[test]
    fn enqueue_during_drain_is_picked_up_by_end_drain() {
        let mut q = queue();
        assert!(q.enqueue(RequestKind::CloseTab, 1, 0, &[]).notify);
        q.begin_drain().unwrap();
        let batch = q.take_batch(16, 1 << 20);
        assert_eq!(batch.len(), 1);
        // Work arriving after the batch was taken but before the drain ends.
        let late = q.enqueue(RequestKind::CloseTab, 2, 0, &[]);
        assert!(!late.notify, "a running drain must not also post a wakeup");
        q.complete(batch[0].id, Outcome::Completed).unwrap();
        assert!(q.end_drain().unwrap(), "the late request must get a wakeup");
        assert_eq!(q.state(), DrainState::NotificationPending);
    }

    #[test]
    fn empty_drain_returns_to_idle_and_rearms() {
        let mut q = queue();
        assert!(q.enqueue(RequestKind::NewTab, 0, 0, &[]).notify);
        q.begin_drain().unwrap();
        let batch = q.take_batch(16, 1 << 20);
        q.complete(batch[0].id, Outcome::Completed).unwrap();
        assert!(!q.end_drain().unwrap());
        assert_eq!(q.state(), DrainState::Idle);
        assert!(q.enqueue(RequestKind::NewTab, 0, 0, &[]).notify, "idle rearms");
    }

    #[test]
    fn failed_notification_rearms_instead_of_losing_the_wakeup() {
        let mut q = queue();
        assert!(q.enqueue(RequestKind::CloseTab, 3, 0, &[]).notify);
        assert!(q.notification_failed(), "work is still waiting");
        assert_eq!(q.state(), DrainState::Idle);
        assert!(
            q.enqueue(RequestKind::CloseTab, 4, 0, &[]).notify,
            "a later enqueue re-notifies"
        );
    }

    #[test]
    fn blocking_defers_work_and_unblocking_schedules_exactly_one_wakeup() {
        let mut q = queue();
        q.set_blocked(true);
        let queued = q.enqueue(RequestKind::CloseTab, 5, 0, &[]);
        assert_eq!(queued.status, EnqueueStatus::Accepted);
        assert!(!queued.notify, "blocked queues wait for the unblock, they do not post");
        assert!(!q.enqueue(RequestKind::CloseTab, 6, 0, &[]).notify);
        assert_eq!(q.begin_drain(), Err(DrainError::Busy));
        assert!(q.set_blocked(false), "unblocking arranges the wakeup");
        assert_eq!(q.state(), DrainState::NotificationPending);
        assert!(!q.set_blocked(false), "already unblocked");
    }

    #[test]
    fn blocking_during_a_drain_applies_after_it_ends() {
        let mut q = queue();
        q.enqueue(RequestKind::CloseTab, 1, 0, &[]);
        q.begin_drain().unwrap();
        q.take_batch(1, 1 << 20);
        q.set_blocked(true);
        q.enqueue(RequestKind::CloseTab, 2, 0, &[]);
        assert!(!q.end_drain().unwrap(), "blocked: no wakeup until unblocked");
        assert_eq!(q.state(), DrainState::Blocked);
        assert!(q.set_blocked(false));
    }

    #[test]
    fn newer_selection_supersedes_the_queued_one_explicitly() {
        let mut q = queue();
        let first = q.enqueue(RequestKind::SelectTab, 10, 4, &[]);
        let second = q.enqueue(RequestKind::SelectTab, 11, 4, &[]);
        assert_eq!(second.superseded, first.id);
        assert_eq!(q.queued_len(), 1);
        let outcomes = q.take_outcomes();
        assert_eq!(outcomes, vec![(first.id, Outcome::Superseded)]);
        q.begin_drain().unwrap();
        let batch = q.take_batch(8, 1 << 20);
        assert_eq!(batch.len(), 1);
        assert_eq!(batch[0].tab, 11);
    }

    #[test]
    fn a_superseding_request_keeps_its_predecessors_place_in_the_stream() {
        let mut q = queue();
        // The real ordering requirement: a pending selection must reach Firefox
        // before the menu opening that was formed against it, no matter how
        // many times the selection was updated in between.
        let first = q.enqueue(RequestKind::SelectTab, 10, 1, &[]);
        q.enqueue(RequestKind::MenuEvent, 10, 1, &[]);
        let newest = q.enqueue(RequestKind::SelectTab, 12, 1, &[]);
        assert_eq!(newest.superseded, first.id);
        q.begin_drain().unwrap();
        let batch = q.take_batch(8, 1 << 20);
        let kinds: Vec<RequestKind> = batch.iter().map(|request| request.kind).collect();
        assert_eq!(kinds, vec![RequestKind::SelectTab, RequestKind::MenuEvent]);
        assert_eq!(batch[0].tab, 12, "the newest selection, in the oldest one's place");
    }

    #[test]
    fn an_address_action_replaces_the_queued_one_in_place() {
        let mut q = queue();
        // The native side has already applied its own policy by the time an
        // action reaches here, so the queue's job is only to keep at most one
        // outstanding -- exactly what the single slot this replaced did --
        // and to report the replaced one rather than drop it silently.
        q.enqueue(RequestKind::StripAction, 10, 1, &[]);
        let first = q.enqueue(RequestKind::AddressAction, 0, 1, &[]);
        let second = q.enqueue(RequestKind::AddressAction, 0, 1, &[]);
        assert_eq!(second.superseded, first.id);
        assert_eq!(q.take_outcomes(), vec![(first.id, Outcome::Superseded)]);
        q.begin_drain().unwrap();
        let batch = q.take_batch(8, 1 << 20);
        let kinds: Vec<RequestKind> = batch.iter().map(|request| request.kind).collect();
        assert_eq!(kinds, vec![RequestKind::StripAction, RequestKind::AddressAction]);
        assert_eq!(batch[1].id, second.id);
    }

    #[test]
    fn a_visibility_report_never_displaces_an_intent() {
        // Reports and intents are different things. A scroll producing a fresh
        // view of which rows are on screen must not throw away the commit the
        // user just made, and answering the report must not be mistaken for
        // answering the commit.
        let mut q = queue();
        let commit = q.enqueue(RequestKind::AddressAction, 0, 1, &[]);
        let report = q.enqueue(RequestKind::AddressVisibility, 0, 1, &[]);
        assert_eq!(report.superseded, 0, "a report leaves an intent alone");
        let newer_report = q.enqueue(RequestKind::AddressVisibility, 0, 1, &[]);
        assert_eq!(newer_report.superseded, report.id, "but reports collapse into the newest");
        q.begin_drain().unwrap();
        let batch = q.take_batch(8, 1 << 20);
        assert_eq!(
            batch.iter().map(|r| r.id).collect::<Vec<_>>(),
            vec![commit.id, newer_report.id]
        );
    }

    #[test]
    fn a_mode_intent_and_an_address_intent_do_not_evict_each_other() {
        // They are two separate slots on the native side. Sharing one kind
        // would let an edit throw away a queued mode pick, which is a user
        // action silently lost.
        let mut q = queue();
        let mode = q.enqueue(RequestKind::AddressModeAction, 0, 1, &[]);
        let address = q.enqueue(RequestKind::AddressAction, 0, 1, &[]);
        assert_eq!(address.superseded, 0, "an address intent leaves a mode intent alone");
        let newer_mode = q.enqueue(RequestKind::AddressModeAction, 0, 1, &[]);
        assert_eq!(newer_mode.superseded, mode.id, "but a mode intent supersedes its own kind");
        q.begin_drain().unwrap();
        let batch = q.take_batch(8, 1 << 20);
        let kinds: Vec<RequestKind> = batch.iter().map(|request| request.kind).collect();
        assert_eq!(kinds, vec![RequestKind::AddressModeAction, RequestKind::AddressAction]);
        assert_eq!(batch[0].id, newer_mode.id);
    }

    #[test]
    fn an_address_action_does_not_disturb_a_queued_selection() {
        let mut q = queue();
        // Two superseding kinds must supersede independently: an address edit
        // must not consume the queued tab selection's place, or vice versa.
        let selection = q.enqueue(RequestKind::SelectTab, 10, 1, &[]);
        let address = q.enqueue(RequestKind::AddressAction, 0, 1, &[]);
        assert_eq!(address.superseded, 0);
        let newer_address = q.enqueue(RequestKind::AddressAction, 0, 1, &[]);
        assert_eq!(newer_address.superseded, address.id);
        let newer_selection = q.enqueue(RequestKind::SelectTab, 11, 1, &[]);
        assert_eq!(newer_selection.superseded, selection.id);
        q.begin_drain().unwrap();
        let batch = q.take_batch(8, 1 << 20);
        let kinds: Vec<RequestKind> = batch.iter().map(|request| request.kind).collect();
        assert_eq!(kinds, vec![RequestKind::SelectTab, RequestKind::AddressAction]);
        assert_eq!(batch[0].tab, 11);
        assert_eq!(batch[1].id, newer_address.id);
    }

    #[test]
    fn strip_gestures_are_ordered_and_never_collapsed() {
        let mut q = queue();
        // Two drags in a row are two moves the user already made. Collapsing
        // them would land the tab in the wrong place and report nothing wrong.
        let first = q.enqueue(RequestKind::StripAction, 10, 1, &[]);
        let second = q.enqueue(RequestKind::StripAction, 11, 1, &[]);
        assert_eq!(second.superseded, 0, "a strip gesture never supersedes another");
        assert_eq!(q.queued_len(), 2);
        q.begin_drain().unwrap();
        let batch = q.take_batch(8, 1 << 20);
        assert_eq!(batch.iter().map(|r| r.id).collect::<Vec<_>>(), vec![first.id, second.id]);
    }

    #[test]
    fn a_strip_gesture_without_a_tab_of_its_own_is_accepted() {
        let mut q = queue();
        // A group toggle names a group, and an incoming adopt names another
        // window's tab; neither has an id in this window's tab space. The
        // kinds that do address a tab still must not be targetless.
        assert_eq!(q.enqueue(RequestKind::StripAction, 0, 1, &[]).status, EnqueueStatus::Accepted);
        assert_eq!(
            q.enqueue(RequestKind::SelectTab, 0, 1, &[]).status,
            EnqueueStatus::RejectedInvalid
        );
    }

    #[test]
    fn a_strip_gesture_keeps_its_place_among_other_kinds() {
        let mut q = queue();
        // A drop that lands a tab and then selects it must reach Firefox in
        // that order; the selection's supersede rule must not reorder around
        // the gesture that preceded it.
        q.enqueue(RequestKind::StripAction, 10, 1, &[]);
        q.enqueue(RequestKind::SelectTab, 10, 1, &[]);
        q.enqueue(RequestKind::StripAction, 11, 1, &[]);
        q.enqueue(RequestKind::SelectTab, 12, 1, &[]);
        q.begin_drain().unwrap();
        let batch = q.take_batch(8, 1 << 20);
        let kinds: Vec<RequestKind> = batch.iter().map(|request| request.kind).collect();
        assert_eq!(
            kinds,
            vec![RequestKind::StripAction, RequestKind::SelectTab, RequestKind::StripAction]
        );
        assert_eq!(batch[1].tab, 12, "the newest selection, in the oldest one's place");
    }

    #[test]
    fn menu_transitions_are_ordered_and_never_collapsed() {
        let mut q = queue();
        for _ in 0..3 {
            q.enqueue(RequestKind::MenuEvent, 5, 1, &[]);
        }
        assert_eq!(q.queued_len(), 3, "open/activate/dismiss are transitions, not a value");
    }

    #[test]
    fn closes_are_never_superseded() {
        let mut q = queue();
        q.enqueue(RequestKind::CloseTab, 10, 0, &[]);
        let second = q.enqueue(RequestKind::CloseTab, 11, 0, &[]);
        assert_eq!(second.superseded, 0);
        assert_eq!(q.queued_len(), 2);
    }

    #[test]
    fn overflow_is_explicit_backpressure_not_a_silent_drop() {
        let mut q = RequestQueue::new(Limits { max_entries: 2, ..Limits::default() });
        assert_eq!(q.enqueue(RequestKind::CloseTab, 1, 0, &[]).status, EnqueueStatus::Accepted);
        assert_eq!(q.enqueue(RequestKind::CloseTab, 2, 0, &[]).status, EnqueueStatus::Accepted);
        let full = q.enqueue(RequestKind::CloseTab, 3, 0, &[]);
        assert_eq!(full.status, EnqueueStatus::RejectedQueueFull);
        assert_eq!(full.id, 0);
        assert_eq!(q.queued_len(), 2, "nothing already accepted was thrown away");
        // A superseding kind still fits, because it frees its own slot first.
        let mut q = RequestQueue::new(Limits { max_entries: 1, ..Limits::default() });
        q.enqueue(RequestKind::SelectTab, 1, 0, &[]);
        assert_eq!(q.enqueue(RequestKind::SelectTab, 2, 0, &[]).status, EnqueueStatus::Accepted);
    }

    #[test]
    fn payload_bounds_are_by_bytes_as_well_as_entries() {
        let mut q = RequestQueue::new(Limits {
            max_entries: 64,
            max_queued_bytes: 64,
            max_request_bytes: 40,
            ..Limits::default()
        });
        let big = vec![b'x' as u16; 32];
        assert_eq!(
            q.enqueue(RequestKind::CloseTab, 1, 0, &big).status,
            EnqueueStatus::RejectedTooLarge
        );
        let ok = vec![b'x' as u16; 16];
        assert_eq!(q.enqueue(RequestKind::CloseTab, 1, 0, &ok).status, EnqueueStatus::Accepted);
        assert_eq!(q.queued_bytes(), 32);
        assert_eq!(q.enqueue(RequestKind::CloseTab, 2, 0, &ok).status, EnqueueStatus::Accepted);
        assert_eq!(
            q.enqueue(RequestKind::CloseTab, 3, 0, &ok).status,
            EnqueueStatus::RejectedTooLarge
        );
    }

    #[test]
    fn a_single_oversized_payload_cannot_wedge_the_batch() {
        let mut q = queue();
        let payload = vec![0u16; 4096];
        q.enqueue(RequestKind::CloseTab, 1, 0, &payload);
        q.enqueue(RequestKind::CloseTab, 2, 0, &payload);
        q.begin_drain().unwrap();
        let batch = q.take_batch(8, 16);
        assert_eq!(batch.len(), 1, "always make progress on at least one entry");
    }

    #[test]
    fn duplicate_and_unknown_completions_are_rejected() {
        let mut q = queue();
        let request = q.enqueue(RequestKind::CloseTab, 1, 0, &[]);
        q.begin_drain().unwrap();
        q.take_batch(8, 1 << 20);
        assert!(q.is_in_flight(request.id));
        q.complete(request.id, Outcome::Completed).unwrap();
        assert!(!q.is_in_flight(request.id));
        assert_eq!(q.complete(request.id, Outcome::Completed), Err(CompleteError::Duplicate));
        assert_eq!(q.complete(9999, Outcome::Completed), Err(CompleteError::Unknown));
    }

    #[test]
    fn replay_of_a_drained_request_is_rejected() {
        let mut q = queue();
        let request = q.enqueue(RequestKind::SelectTab, 1, 0, &[]);
        q.begin_drain().unwrap();
        let batch = q.take_batch(8, 1 << 20);
        q.complete(batch[0].id, Outcome::Completed).unwrap();
        q.end_drain().unwrap();
        // The same batch delivered a second time names an id that is no longer
        // executable for this endpoint.
        assert!(!q.is_in_flight(request.id));
        assert_eq!(q.complete(request.id, Outcome::Completed), Err(CompleteError::Duplicate));
    }

    #[test]
    fn destruction_with_pending_work_cancels_explicitly_and_is_idempotent() {
        let mut q = queue();
        let queued = q.enqueue(RequestKind::CloseTab, 1, 0, &[]);
        let running = q.enqueue(RequestKind::CloseTab, 2, 0, &[]);
        q.begin_drain().unwrap();
        q.take_batch(1, 1 << 20);
        let generation = q.generation();
        q.close();
        let outcomes = q.take_outcomes();
        assert!(outcomes.contains(&(queued.id, Outcome::Cancelled)));
        assert!(outcomes.contains(&(running.id, Outcome::Cancelled)));
        assert!(q.generation() > generation, "late completions cannot match the old generation");
        assert_eq!(
            q.enqueue(RequestKind::CloseTab, 3, 0, &[]).status,
            EnqueueStatus::RejectedClosed
        );
        q.close();
        assert!(q.is_closed());
    }

    #[test]
    fn outcome_history_is_bounded_and_reports_what_it_dropped() {
        let mut q = RequestQueue::new(Limits {
            max_pending_outcomes: 2,
            completion_history: 2,
            ..Limits::default()
        });
        for tab in 1..=4 {
            let request = q.enqueue(RequestKind::CloseTab, tab, 0, &[]);
            q.begin_drain().unwrap();
            q.take_batch(1, 1 << 20);
            q.complete(request.id, Outcome::Completed).unwrap();
            q.end_drain().unwrap();
        }
        assert_eq!(q.dropped_outcomes(), 2);
        assert_eq!(q.take_outcomes().len(), 2);
    }

    #[test]
    fn nested_drain_is_refused_rather_than_reentered() {
        let mut q = queue();
        q.enqueue(RequestKind::CloseTab, 1, 0, &[]);
        q.begin_drain().unwrap();
        assert_eq!(q.begin_drain(), Err(DrainError::Busy));
        q.take_batch(8, 1 << 20);
        assert_eq!(q.end_drain(), Ok(false));
        assert_eq!(q.end_drain(), Err(DrainError::NotDraining));
    }

    #[test]
    fn requests_that_need_a_target_are_rejected_without_one() {
        let mut q = queue();
        assert_eq!(
            q.enqueue(RequestKind::SelectTab, 0, 0, &[]).status,
            EnqueueStatus::RejectedInvalid
        );
        assert_eq!(q.enqueue(RequestKind::NewTab, 0, 0, &[]).status, EnqueueStatus::Accepted);
    }
}
