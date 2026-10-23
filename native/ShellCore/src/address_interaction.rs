//! Native address interaction lifetime. No browser policy or platform objects.
//! Tokens never repeat within an endpoint, including across session resets.
#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum ShellAddressInteractionKind {
    Inspect = 0, Observe = 1, PointerFocus = 2, Begin = 3,
    Move = 4, Release = 5, Cancel = 6, Interrupt = 7, ContentFocus = 8, PrepareSubmit = 9,
}
#[repr(u32)]
pub enum ShellAddressInteractionEffect { ClearSelection = 1, Focus = 2, Blur = 4, SelectAll = 8, ReleaseContentFocus = 16 }
#[repr(C)]
#[derive(Default, Clone, Copy, Debug)]
pub struct ShellAddressInteractionState {
    pub token: u64,
    pub effects: u32,
    pub focused: u32,
    pub holding: u32,
}
#[derive(Clone, Copy)]
struct Gesture { token: u64, pointer: u32, entry: bool, moved: bool }
#[derive(Default)]
pub struct AddressInteraction {
    serial: u64,
    focused: bool,
    pointer_entry: bool,
    gesture: Option<Gesture>,
}
impl AddressInteraction {
    fn revoke(&mut self) {
        self.serial = self.serial.saturating_add(1);
        self.gesture = None;
        self.pointer_entry = false;
    }
    pub fn reset(&mut self) {
        // Session replacement revokes completion, not observed editor focus.
        // A held entry must not become a fresh focus request in the new session.
        self.focused |= self.gesture.is_some();
        self.revoke();
    }
    pub fn state(&self, effects: u32) -> ShellAddressInteractionState {
        ShellAddressInteractionState { token: self.serial, effects,
            focused: self.focused as u32, holding: self.gesture.is_some() as u32 }
    }
    /// `value` is inside/entry/moved/editor-owned for the corresponding event.
    /// Returns None on malformed input, without mutation.
    pub fn event(&mut self, kind: u32, pointer: u32, token: u64, value: u32) -> Option<ShellAddressInteractionState> {
        use ShellAddressInteractionKind::*;
        if value > 1 { return None; }
        let kind = match kind {
            0 => Inspect, 1 => Observe, 2 => PointerFocus, 3 => Begin,
            4 => Move, 5 => Release, 6 => Cancel, 7 => Interrupt,
            8 => ContentFocus, 9 => PrepareSubmit, _ => return None,
        };
        use ShellAddressInteractionEffect as Effect;
        let mut effects = 0;
        match kind {
            Inspect => {},
            Observe if value == 0 => {
                self.revoke();
                effects = Effect::ClearSelection as u32 | if self.focused { Effect::Blur as u32 } else { 0 };
                self.focused = false;
            },
            Observe => {
                if self.gesture.is_none() && !self.focused {
                    self.focused = true;
                    effects = Effect::Focus as u32;
                }
            },
            PointerFocus => {
                self.pointer_entry = true;
                if let Some(g) = &mut self.gesture { g.entry = true; }
            },
            Begin => {
                // Fail closed on exhaustion; never reuse an old completion ID.
                self.gesture = None;
                if let Some(next) = self.serial.checked_add(1) {
                    self.serial = next;
                    self.gesture = Some(Gesture { token: next, pointer,
                        entry: self.pointer_entry || !self.focused || value != 0, moved: false });
                }
                self.pointer_entry = false;
            },
            Move | Release | Cancel => {
                if let Some(g) = self.gesture.filter(|g| g.pointer == pointer && g.token == token) {
                    if kind == Move { self.gesture.as_mut().unwrap().moved |= value != 0; }
                    else {
                        if kind == Cancel { self.revoke(); }
                        self.gesture = None; // consume before any host effect
                        self.pointer_entry = false;
                        if kind == Release && value != 0 {
                            self.focused = true;
                            effects = Effect::Focus as u32 | if g.entry && !g.moved { Effect::SelectAll as u32 } else { 0 };
                        }
                    }
                }
            },
            Interrupt | PrepareSubmit => {
                // Enter can arrive before first-entry release has adopted the
                // browser focus proxy. Request normal entry before submission.
                if kind == PrepareSubmit && value != 0 && !self.focused && self.gesture.is_some() {
                    effects = Effect::Focus as u32;
                }
                self.revoke();
                // Keyboard intent supersedes pointer completion. An Escape in
                // the editor must not synthesize a fresh entry on observation.
                if value != 0 { self.focused = true; }
            },
            // A retained content-focus snapshot cannot reclaim another native
            // control after the address interaction has already lost focus.
            ContentFocus => { if self.focused && self.gesture.is_none() { effects = Effect::ReleaseContentFocus as u32; } },
        }
        Some(self.state(effects))
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn ev(s: &mut AddressInteraction, k: u32, p: u32, t: u64, v: u32) -> ShellAddressInteractionState { s.event(k,p,t,v).unwrap() }
    #[test] fn entry_release_and_repeated_observation_are_single_transitions() {
        let mut s=AddressInteraction::default();
        let t=ev(&mut s,3,4,0,1).token;
        assert_eq!(ev(&mut s,1,0,0,1).effects,0);
        assert_eq!(ev(&mut s,5,4,t,1).effects,10);
        assert_eq!(ev(&mut s,5,4,t,1).effects,0);
        assert_eq!(ev(&mut s,1,0,0,1).effects,0);
        assert_eq!(ev(&mut s,1,0,0,0).effects,5);
        assert_eq!(ev(&mut s,1,0,0,0).effects,1);
    }
    #[test] fn movement_latches_and_caret_click_does_not_select_all() {
        let mut s=AddressInteraction::default();
        let t=ev(&mut s,3,1,0,1).token;
        ev(&mut s,4,1,t,1); ev(&mut s,4,1,t,0);
        assert_eq!(ev(&mut s,5,1,t,1).effects,2);
        let t=ev(&mut s,3,1,0,0).token;
        assert_eq!(ev(&mut s,5,1,t,1).effects,2);
    }
    #[test] fn interrupt_prevents_release_and_observation_from_undoing_escape() {
        let mut s=AddressInteraction::default();
        let t=ev(&mut s,3,1,0,1).token;
        ev(&mut s,7,0,0,1);
        assert_eq!(ev(&mut s,5,1,t,1).effects,0);
        assert_eq!(ev(&mut s,1,0,0,1).effects,0);
    }
    #[test] fn first_entry_submit_requests_focus_once_and_revokes_release() {
        let mut s=AddressInteraction::default();
        let old=ev(&mut s,3,1,0,1).token;
        assert_eq!(ev(&mut s,9,0,0,1).effects,2);
        assert_eq!(ev(&mut s,5,1,old,1).effects,0);
        assert_eq!(ev(&mut s,1,0,0,1).effects,0);
        assert_eq!(ev(&mut s,9,0,0,1).effects,0);
    }
    #[test] fn reset_rejects_old_release_and_delayed_cancel_even_with_reused_pointer() {
        let mut s=AddressInteraction::default();
        let old=ev(&mut s,3,1,0,1).token;
        s.reset();
        assert_eq!(ev(&mut s,5,1,old,1).effects,0);
        assert_eq!(ev(&mut s,1,0,0,1).effects,0);
        let new=ev(&mut s,3,1,0,1).token;
        assert_ne!(old,new);
        assert_eq!(ev(&mut s,6,1,old,0).holding,1);
        assert_eq!(ev(&mut s,5,1,new,1).effects,10);
    }
    #[test] fn cancellation_outside_and_wrong_pointer_never_complete_gesture() {
        for cancel in [1,6,7] {
            let mut s=AddressInteraction::default();
            let t=ev(&mut s,3,1,0,1).token;
            assert_eq!(ev(&mut s,5,2,t,1).holding,1);
            ev(&mut s,cancel,1,t,0);
            assert_eq!(ev(&mut s,5,1,t,1).effects,0);
        }
    }
    #[test] fn release_requires_editor_ownership_and_invalid_events_do_not_mutate() {
        let mut s=AddressInteraction::default();
        let t=ev(&mut s,3,1,0,1).token;
        assert!(s.event(99,1,t,0).is_none());
        assert!(s.event(1,1,t,2).is_none());
        assert_eq!(s.state(0).holding,1);
        assert_eq!(ev(&mut s,5,1,t,0).effects,0);
    }
}

#[cfg(test)]
mod effect_tests {
    use super::*;
    #[test] fn content_handoff_does_not_take_focus_from_other_native_controls() {
        let mut state=AddressInteraction::default();
        assert_eq!(state.event(8,0,0,0).unwrap().effects,0);
        state.event(1,0,0,1);
        assert_eq!(state.event(8,0,0,0).unwrap().effects,16);
        state.event(1,0,0,0);
        assert_eq!(state.event(8,0,0,0).unwrap().effects,0);
    }
    #[test] fn effects_are_revoked_by_interrupt_and_content_handoff_waits_for_gesture() {
        let mut state=AddressInteraction::default();
        let begin=state.event(3,1,0,1).unwrap();
        assert_eq!(state.event(8,0,0,0).unwrap().effects,0);
        let effect=state.event(5,1,begin.token,1).unwrap();
        state.event(7,0,0,1);
        assert_ne!(effect.token,state.state(0).token);
        assert_eq!(state.event(8,0,0,0).unwrap().effects,16);
    }
    #[test] fn capture_loss_after_release_does_not_cancel_subsequent_entry() {
        let mut s=AddressInteraction::default();
        let old=s.event(3,1,0,1).unwrap().token;
        s.event(5,1,old,1);
        let new=s.event(3,1,0,1).unwrap().token;
        s.event(6,1,old,0);
        assert_eq!(s.state(0).holding,1);
        assert_eq!(s.event(5,1,new,1).unwrap().effects,10);
    }
    #[test] fn programmatic_entry_cancels_pointer_without_suppressing_focus_transition() {
        let mut s=AddressInteraction::default();
        let old=s.event(3,1,0,1).unwrap().token;
        s.event(7,0,0,0);
        assert_eq!(s.event(1,0,0,1).unwrap().effects,2);
        assert_eq!(s.event(5,1,old,1).unwrap().effects,0);
    }
    #[test] fn exhaustion_fails_closed() {
        let mut s=AddressInteraction {serial:u64::MAX,..Default::default()};
        assert_eq!(s.event(3,1,0,1).unwrap().holding,0);
    }
}
