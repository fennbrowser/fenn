/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Native command feedback, distinct from Firefox's authoritative tab state.
//! Only admitted requests are retained. The queue releases them when recording
//! an outcome, before its bounded outcome-report history can discard that record.
use std::collections::HashMap;

#[derive(Default)]
pub struct PendingCommands {
    closes: HashMap<u32, u64>,
    selection: Option<(u64, u32)>,
}

impl PendingCommands {
    pub fn closing(&self, tab: u32) -> bool { self.closes.contains_key(&tab) }

    pub fn close(&mut self, tab: u32, request: u64) {
        self.closes.insert(tab, request);
    }

    pub fn select(&mut self, request: u64, revision: u32) {
        self.selection = (request != 0).then_some((request, revision));
    }

    pub fn clear_selection(&mut self) { self.selection = None; }

    /// A context change permanently revokes optimistic feedback, even if an
    /// older revision is subsequently observed. It does not cancel execution.
    pub fn selection_pending(&mut self, revision: u32) -> bool {
        if self.selection.is_some_and(|(_, context)| context != revision) {
            self.clear_selection();
        }
        self.selection.is_some()
    }

    pub fn complete(&mut self, request: u64) {
        if self.selection.is_some_and(|(id, _)| id == request) {
            self.clear_selection();
        }
        self.closes.retain(|_, id| *id != request);
    }

    pub fn retain_tabs(&mut self, contains: impl Fn(u32) -> bool) {
        self.closes.retain(|tab, _| contains(*tab));
    }
}

#[cfg(test)]
mod tests {
    use crate::queue::{EnqueueStatus, Limits, Outcome, RequestKind::*, RequestQueue};

    fn drain(q: &mut RequestQueue) {
        q.begin_drain().unwrap();
        q.take_batch(256, 1_048_576);
        q.end_drain().unwrap();
    }

    #[test]
    fn closes_stay_suppressed_through_drain_until_each_outcome() {
        for outcome in [Outcome::Completed, Outcome::Rejected, Outcome::Cancelled, Outcome::Superseded] {
            let mut q = RequestQueue::new(Limits::default());
            let first = q.enqueue(CloseTab, 7, 1, &[]);
            let second = q.enqueue(CloseTab, 8, 1, &[]);
            assert_eq!(q.enqueue(CloseTab, 7, 1, &[]).status, EnqueueStatus::RejectedPendingClose);
            drain(&mut q);
            assert_eq!(q.enqueue(CloseTab, 7, 2, &[]).status, EnqueueStatus::RejectedPendingClose);
            q.complete(first.id, outcome).unwrap();
            assert_eq!(q.enqueue(CloseTab, 7, 2, &[]).status, EnqueueStatus::Accepted);
            assert_eq!(q.enqueue(CloseTab, 8, 2, &[]).status, EnqueueStatus::RejectedPendingClose);
            q.complete(second.id, outcome).unwrap();
        }
    }

    #[test]
    fn older_outcomes_cannot_release_newer_selection() {
        let mut q = RequestQueue::new(Limits::default());
        let old = q.enqueue(SelectTab, 1, 10, &[]);
        drain(&mut q);
        let replaced = q.enqueue(SelectTab, 2, 10, &[]);
        let newest = q.enqueue(SelectTab, 3, 10, &[]);
        assert_eq!(newest.superseded, replaced.id);
        q.complete(old.id, Outcome::Rejected).unwrap();
        assert!(q.selection_pending(10));
        drain(&mut q);
        assert!(q.selection_pending(10));
        q.complete(newest.id, Outcome::Rejected).unwrap();
        assert!(!q.selection_pending(10));
    }

    #[test]
    fn context_revocation_is_permanent_and_does_not_cancel_execution() {
        let mut q = RequestQueue::new(Limits::default());
        let request = q.enqueue(SelectTab, 1, 10, &[]);
        assert!(q.selection_pending(10));
        assert!(!q.selection_pending(11));
        assert!(!q.selection_pending(10));
        drain(&mut q);
        assert!(q.is_in_flight(request.id));
    }

    #[test]
    fn rejected_admissions_do_not_latch_and_restore_browser_selection() {
        let mut q = RequestQueue::new(Limits { max_entries: 1, max_request_bytes: 2, ..Limits::default() });
        q.enqueue(SelectTab, 1, 10, &[]);
        assert_eq!(q.enqueue(SelectTab, 2, 10, &[1, 2]).status, EnqueueStatus::RejectedTooLarge);
        assert!(!q.selection_pending(10));
        assert_eq!(q.enqueue(CloseTab, 7, 10, &[]).status, EnqueueStatus::RejectedQueueFull);
        drain(&mut q);
        assert_eq!(q.enqueue(CloseTab, 7, 10, &[]).status, EnqueueStatus::Accepted);
    }

    #[test]
    fn close_and_new_tab_release_selection_but_duplicate_close_does_not() {
        let mut q = RequestQueue::new(Limits::default());
        q.enqueue(SelectTab, 1, 10, &[]);
        q.enqueue(CloseTab, 7, 10, &[]);
        assert!(!q.selection_pending(10));
        q.enqueue(SelectTab, 2, 10, &[]);
        q.enqueue(CloseTab, 7, 10, &[]);
        assert!(q.selection_pending(10));
        q.enqueue(NewTab, 0, 10, &[]);
        assert!(!q.selection_pending(10));
    }

    #[test]
    fn removed_tabs_and_late_outcomes_do_not_release_replacement_closes() {
        let mut q = RequestQueue::new(Limits::default());
        let old = q.enqueue(CloseTab, 7, 10, &[]);
        drain(&mut q);
        q.retain_tabs(|tab| tab != 7);
        assert_eq!(q.enqueue(CloseTab, 7, 11, &[]).status, EnqueueStatus::Accepted);
        q.complete(old.id, Outcome::Rejected).unwrap();
        assert_eq!(q.enqueue(CloseTab, 7, 11, &[]).status, EnqueueStatus::RejectedPendingClose);
    }

    #[test]
    fn outcome_overflow_and_session_cancellation_cannot_leave_latches() {
        let mut q = RequestQueue::new(Limits { max_pending_outcomes: 1, ..Limits::default() });
        let close = q.enqueue(CloseTab, 7, 10, &[]);
        let select = q.enqueue(SelectTab, 1, 10, &[]);
        drain(&mut q);
        q.complete(close.id, Outcome::Rejected).unwrap();
        q.complete(select.id, Outcome::Completed).unwrap();
        assert_eq!(q.dropped_outcomes(), 1);
        assert!(!q.selection_pending(10));
        assert_eq!(q.enqueue(CloseTab, 7, 10, &[]).status, EnqueueStatus::Accepted);
        q.enqueue(SelectTab, 1, 10, &[]);
        q.cancel_all();
        assert!(!q.selection_pending(10));
        assert_eq!(q.enqueue(CloseTab, 7, 10, &[]).status, EnqueueStatus::Accepted);
        q.close();
        assert_eq!(q.enqueue(CloseTab, 7, 10, &[]).status, EnqueueStatus::RejectedClosed);
    }
}

