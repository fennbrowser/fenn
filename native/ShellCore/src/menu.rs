//! Native menu protocol identity. Firefox owns command meaning and eligibility;
//! WinUI supplies the enabled opaque IDs of the rendered description.
use std::collections::BTreeSet;
pub const MAX_TABS: usize = 10_000;
pub const MAX_COMMANDS: usize = 128;
pub const MAX_ID: usize = 64;
pub const MAX_FINGERPRINT: usize = 6 * 1024 * 1024;
#[repr(C)]
#[derive(Default, Clone, Copy, Debug, PartialEq, Eq)]
pub struct ShellMenuState {
    pub session: u32,
    pub tab: u32,
    pub revision: u32,
    pub state_revision: u32,
    pub awaiting: u32,
}
#[derive(Default)]
pub struct MenuState {
    state: ShellMenuState,
    next_session: u32,
    tabs: BTreeSet<u32>,
    fingerprint: Vec<u16>,
    enabled: BTreeSet<Vec<u16>>,
}
impl MenuState {
    pub fn state(&self) -> ShellMenuState { self.state }
    pub fn has_tab(&self, id: u32) -> bool { self.tabs.contains(&id) }
    pub fn publish(&mut self, revision: u32, tabs: &[u32], fingerprint: &[u16]) -> Result<bool, ()> {
        if revision == 0 || tabs.len() > MAX_TABS || fingerprint.len() > MAX_FINGERPRINT { return Err(()); }
        let members: BTreeSet<_> = tabs.iter().copied().collect();
        if members.contains(&0) || members.len() != tabs.len() { return Err(()); }
        if revision == self.state.state_revision && fingerprint != self.fingerprint { return Err(()); }
        if revision <= self.state.state_revision { return Ok(false); }
        self.tabs = members;
        self.fingerprint = fingerprint.to_vec();
        self.state.state_revision = revision;
        // The host emits the old session's dismiss before hiding its flyout.
        // Revision checks already prevent accepting/activating that old menu.
        Ok(true)
    }
    pub fn retain(&mut self, ids: &[u32]) {
        let keep: BTreeSet<_> = ids.iter().copied().collect();
        self.tabs.retain(|id| keep.contains(id));
    }
    pub fn open(&mut self, tab: u32) -> bool {
        self.dismiss();
        if !self.has_tab(tab) || self.next_session == u32::MAX { return false; }
        self.next_session += 1;
        self.state.session = self.next_session;
        self.state.tab = tab;
        self.state.revision = self.state.state_revision;
        self.state.awaiting = 1;
        true
    }
    pub fn dismiss(&mut self) {
        self.state = ShellMenuState { state_revision: self.state.state_revision, ..Default::default() };
        self.enabled.clear();
    }
    pub fn close(&mut self) {
        self.dismiss(); self.tabs.clear(); self.fingerprint.clear(); self.state.state_revision = 0;
        // Never reuse a session ID within this endpoint, even after cancellation.
    }
    pub fn matches(&self, session: u32, tab: u32, revision: u32) -> bool {
        session != 0 && session == self.state.session && tab == self.state.tab &&
            revision == self.state.revision && revision == self.state.state_revision &&
            self.state.awaiting != 0 && self.has_tab(tab)
    }
    pub fn accept(&mut self, session: u32, tab: u32, revision: u32, ids: &[Vec<u16>]) -> Result<bool, ()> {
        if ids.len() > MAX_COMMANDS || ids.iter().any(|id| id.is_empty() || id.len() > MAX_ID) { return Err(()); }
        let enabled: BTreeSet<_> = ids.iter().cloned().collect();
        if enabled.len() != ids.len() { return Err(()); }
        if !self.matches(session,tab,revision) { return Ok(false); }
        self.enabled = enabled; self.state.awaiting = 0;
        Ok(true)
    }
    pub fn activate(&mut self, session: u32, revision: u32, id: &[u16]) -> bool {
        if session == 0 || session != self.state.session || revision != self.state.revision ||
            revision != self.state.state_revision || self.state.awaiting != 0 ||
            !self.has_tab(self.state.tab) || !self.enabled.contains(id) { return false; }
        self.enabled.clear();
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn id(value: &str) -> Vec<u16> { value.encode_utf16().collect() }
    fn menu() -> MenuState {
        let mut m=MenuState::default(); assert_eq!(m.publish(1,&[7,8],&id("state1")),Ok(true));m
    }
    #[test]
    fn opening_requires_membership_and_exhaustion_never_wraps() {
        let mut m=menu(); assert!(!m.open(9)); assert!(m.open(7));
        assert_eq!(m.state().session,1); assert_eq!(m.state().awaiting,1);
        m.dismiss(); assert!(m.open(8)); assert_eq!(m.state().session,2);
        m.next_session=u32::MAX; assert!(!m.open(7)); assert_eq!(m.state().session,0);
    }
    #[test]
    fn description_requires_exact_live_opening_and_is_accepted_once() {
        let mut m=menu(); m.open(7); let s=m.state().session;
        for (session,tab,revision) in [(s+1,7,1),(s,8,1),(s,7,2)] {
            assert_eq!(m.accept(session,tab,revision,&[id("reload")]),Ok(false));
        }
        assert!(!m.activate(s,1,&id("reload")));
        assert_eq!(m.accept(s,7,1,&[id("reload")]),Ok(true));
        assert_eq!(m.accept(s,7,1,&[id("close")]),Ok(false));
        assert!(!m.activate(s,1,&id("close"))); assert!(m.activate(s,1,&id("reload")));
        assert!(!m.activate(s,1,&id("reload")));
    }
    #[test]
    fn dismissal_retarget_and_revision_revoke_old_answers_and_clicks() {
        let mut m=menu(); m.open(7); let s=m.state().session;
        m.dismiss(); assert_eq!(m.accept(s,7,1,&[id("a")]),Ok(false));
        m.open(8); let newer=m.state().session;
        assert_eq!(m.accept(newer,8,1,&[id("a")]),Ok(true));
        assert!(!m.activate(s,1,&id("a")));
        m.publish(2,&[7,8],&id("state2")).unwrap();
        assert!(!m.activate(newer,1,&id("a")));
        assert!(!m.matches(newer,8,1));
    }
    #[test]
    fn publication_validation_is_atomic_and_late_states_are_ignored() {
        let mut m=menu();m.open(7);let before=m.state();
        for (revision,tabs,fingerprint) in [(2,vec![7,7],id("duplicate")),(2,vec![0],id("zero")),(1,vec![7],id("conflict")),(0,vec![7],id("zero revision"))] {
            assert_eq!(m.publish(revision,&tabs,&fingerprint),Err(()));assert_eq!(m.state(),before);
            assert!(m.has_tab(8));
        }
        assert_eq!(m.publish(1,&[7,8],&id("state1")),Ok(false));
        m.publish(3,&[8],&id("state3")).unwrap();
        assert_eq!(m.publish(2,&[7],&id("state2")),Ok(false));assert!(!m.has_tab(7));
    }
    #[test]
    fn invalid_command_sets_do_not_consume_pending_description() {
        let mut m=menu();m.open(7);let s=m.state().session;
        for commands in [vec![id("")],vec![vec![65;65]],vec![id("a"),id("a")],vec![id("a");129]] {
            assert_eq!(m.accept(s,7,1,&commands),Err(()));assert_eq!(m.state().awaiting,1);
        }
        assert_eq!(m.accept(s,7,1,&[]),Ok(true));assert!(!m.activate(s,1,&id("disabled")));
    }
    #[test]
    fn pruning_and_close_revoke_actions_without_reusing_session_ids() {
        let mut m=menu();m.open(7);let s=m.state().session;
        m.accept(s,7,1,&[id("a")]).unwrap();m.retain(&[8]);
        assert!(!m.has_tab(7));assert!(!m.activate(s,1,&id("a")));
        m.close();assert!(!m.open(8));assert_eq!(m.state().state_revision,0);
        m.publish(1,&[7],&id("new state")).unwrap();m.open(7);
        assert!(m.state().session>s);assert_eq!(m.accept(s,7,1,&[id("a")]),Ok(false));
    }
}
