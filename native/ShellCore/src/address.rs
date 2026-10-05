//! Address edit/request coordination. WinUI supplies observations; Firefox
//! supplies acknowledgments. Completing transport is not acknowledging a query.
use crate::intent::{self, Conditions, Decision, IntentKind};

pub const MAX_EDIT_ID: u64 = 9_007_199_254_740_991;

#[derive(Default)]
pub struct AddressState {
    edit_id: u64,
    acknowledged: u64,
    needs_dispatch: bool,
    query_settled: bool,
    staged: Option<(u64, IntentKind)>,
    mode_request: Option<u64>,
    deferred: bool,
    submit: Option<u32>,
}

#[repr(C)]
#[derive(Default, Clone, Copy, Debug)]
pub struct ShellAddressState {
    pub edit_id: u64,
    pub staged_kind: u32,
    pub needs_dispatch: u32,
    pub edit_acknowledged: u32,
    pub query_settled: u32,
    pub mode_pending: u32,
}

#[repr(C)]
#[derive(Default, Clone, Copy, Debug)]
pub struct ShellAddressAction {
    /// An intent::Decision value.
    pub decision: u32,
    /// Ctrl=1, Alt=2, Shift=4, Meta=8. Captured on the first Enter.
    pub modifiers: u32,
}

impl AddressState {
    pub fn state(&self) -> ShellAddressState {
        ShellAddressState {
            edit_id: self.edit_id,
            staged_kind: self.staged.map_or(0, |(_, kind)| kind as u32),
            needs_dispatch: self.needs_dispatch as u32,
            edit_acknowledged: (!self.needs_dispatch && self.acknowledged == self.edit_id) as u32,
            query_settled: self.query_settled as u32,
            mode_pending: self.mode_request.is_some() as u32,
        }
    }

    pub fn edit(&mut self) -> bool {
        if self.edit_id == MAX_EDIT_ID { return false; }
        self.edit_id += 1;
        self.needs_dispatch = true;
        self.cancel_submit();
        true
    }

    // Called only after queue admission. Refusal must leave the old admitted
    // request and the new unadmitted edit independent of one another.
    pub fn stage(&mut self, request: u64, kind: IntentKind) {
        self.staged = Some((request, kind));
        if matches!(kind, IntentKind::Edit | IntentKind::Clipboard | IntentKind::ContextMenu) { self.needs_dispatch = false; }
    }

    // Queue admission owns mode lifetime: refused actions never cancel Enter.
    pub fn stage_mode(&mut self, request: u64) {
        self.mode_request = Some(request);
        self.cancel_submit();
    }

    pub fn reset_mode(&mut self) { self.mode_request = None; }

    pub fn complete(&mut self, request: u64) {
        if self.mode_request == Some(request) { self.mode_request = None; }
        if self.staged.is_some_and(|(id, _)| id == request) { self.staged = None; }
    }

    pub fn publish(&mut self, acknowledged: u64, settled: bool) {
        self.acknowledged = acknowledged;
        self.query_settled = settled;
    }

    pub fn cancel_submit(&mut self) {
        self.deferred = false;
        self.submit = None;
    }

    pub fn action(&mut self, incoming: IntentKind, mut observed: Conditions, modifiers: u32) -> ShellAddressAction {
        let state = self.state();
        observed.edit_acknowledged = state.edit_acknowledged != 0;
        observed.query_settled = self.query_settled;
        observed.mode_intent_staged = self.mode_request.is_some();
        // Match the surface guard before capturing an Enter or cancelling work.
        if intent::decide(incoming, None, observed) == Decision::Drop {
            return ShellAddressAction { decision: Decision::Drop as u32, modifiers: 0 };
        }
        if incoming == IntentKind::Activate { self.submit.get_or_insert(modifiers); }
        let decision = intent::decide(incoming, self.staged.map(|(_, kind)| kind), observed);
        let mut result = ShellAddressAction { decision: decision as u32, modifiers: 0 };
        match decision {
            Decision::Defer => self.deferred = true,
            Decision::Drop => {},
            _ => {
                if matches!(incoming, IntentKind::Dismiss | IntentKind::Blur) {
                    self.needs_dispatch = false;
                    self.cancel_submit();
                }
                if matches!(incoming, IntentKind::Clipboard | IntentKind::ContextMenu) { self.cancel_submit(); }
                if incoming == IntentKind::Activate {
                    result.modifiers = self.submit.take().unwrap_or(modifiers);
                    self.deferred = false;
                }
            }
        }
        result
    }

    pub fn take_deferred(&mut self) -> bool {
        if !self.deferred || self.state().edit_acknowledged == 0 || !self.query_settled { return false; }
        self.deferred = false;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn observed() -> Conditions {
        Conditions { applying: false, composing: false, has_session: true,
            mode_intent_staged: false, edit_acknowledged: false, query_settled: false }
    }
    fn enter(s: &mut AddressState, modifiers: u32) -> ShellAddressAction {
        s.action(IntentKind::Activate, observed(), modifiers)
    }

    #[test]
    fn enter_waits_for_ack_and_results_not_just_request_completion() {
        let mut s = AddressState::default();
        assert!(s.edit());
        s.stage(11, IntentKind::Edit);
        assert_eq!(enter(&mut s, 5).decision, Decision::Defer as u32);
        s.complete(11);
        assert!(!s.take_deferred());
        s.publish(0, true);
        assert!(!s.take_deferred());
        s.publish(1, false);
        assert!(!s.take_deferred());
        s.publish(1, true);
        assert!(s.take_deferred());
        assert!(!s.take_deferred());
        let commit = enter(&mut s, 0);
        assert_eq!(commit.decision, Decision::Stage as u32);
        assert_eq!(commit.modifiers, 5);
    }

    #[test]
    fn refusal_and_old_completion_preserve_the_new_edit_and_early_enter() {
        let mut s = AddressState::default();
        s.edit(); s.stage(10, IntentKind::Edit);
        s.edit(); // admission refused; no stage call
        enter(&mut s, 2);
        s.complete(10);
        assert_eq!(s.state().needs_dispatch, 1);
        s.publish(2, true);
        assert!(!s.take_deferred());
        s.stage(12, IntentKind::Edit);
        s.complete(12);
        assert!(s.take_deferred());
        assert_eq!(enter(&mut s, 0).modifiers, 2);
        assert_eq!(s.state().edit_id, 2); // retries did not create another edit
    }

    #[test]
    fn newer_edit_cancels_enter_and_repeated_enter_keeps_first_modifiers() {
        let mut s = AddressState::default();
        s.edit(); enter(&mut s, 1); enter(&mut s, 8);
        s.edit();
        s.stage(2, IntentKind::Edit); s.complete(2); s.publish(2, true);
        assert!(!s.take_deferred());
        assert_eq!(enter(&mut s, 4).modifiers, 4);
        s.edit(); enter(&mut s, 1); enter(&mut s, 8);
        s.stage(3, IntentKind::Edit); s.complete(3); s.publish(3, true);
        assert!(s.take_deferred());
        assert_eq!(enter(&mut s, 0).modifiers, 1);
    }

    #[test]
    fn stale_completion_cannot_release_a_newer_request_even_after_session_reset() {
        let mut s = AddressState::default();
        s.edit(); s.stage(10, IntentKind::Edit);
        s.stage(11, IntentKind::Edit); s.complete(10);
        assert_eq!(s.state().staged_kind, IntentKind::Edit as u32);
        s = AddressState::default();
        s.edit(); s.stage(12, IntentKind::Edit); s.complete(11);
        assert_eq!(s.state().staged_kind, IntentKind::Edit as u32);
        s.complete(12);
        assert_eq!(s.state().staged_kind, 0);
    }

    #[test]
    fn cancellation_and_session_reset_revoke_retry_and_submit() {
        for kind in [IntentKind::Dismiss, IntentKind::Blur] {
            let mut s = AddressState::default();
            s.edit(); enter(&mut s, 15);
            s.action(kind, observed(), 0);
            assert_eq!(s.state().needs_dispatch, 0);
            s.publish(1, true);
            assert!(!s.take_deferred());
            assert_eq!(enter(&mut s, 0).modifiers, 0);
        }
        let mut s = AddressState::default();
        s.edit(); enter(&mut s, 15);
        s = AddressState::default();
        s.publish(0, true);
        assert!(!s.take_deferred());
        assert_eq!(s.state().edit_id, 0);
        assert_eq!(enter(&mut s, 0).modifiers, 0);
    }

    #[test]
    fn ime_and_programmatic_echoes_do_not_capture_submit() {
        let mut s = AddressState::default();
        s.publish(0, true);
        for c in [Conditions { composing: true, ..observed() }, Conditions { applying: true, ..observed() }] {
            assert_eq!(s.action(IntentKind::Activate, c, 15).decision, Decision::Drop as u32);
        }
        assert_eq!(enter(&mut s, 0).modifiers, 0);
    }

    #[test]
    fn exhausted_edit_identity_is_refused_without_losing_pending_state() {
        let mut s = AddressState { edit_id: MAX_EDIT_ID, ..Default::default() };
        enter(&mut s, 8);
        assert!(!s.edit());
        assert_eq!(s.state().edit_id, MAX_EDIT_ID);
        s.publish(MAX_EDIT_ID, true);
        assert!(s.take_deferred());
        assert_eq!(enter(&mut s, 0).modifiers, 8);
    }
}
