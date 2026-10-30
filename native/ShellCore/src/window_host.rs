/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! One shell/content binding per endpoint lifetime. Handles are opaque identities;
//! the Win32 adapter validates their thread, parent and lifetime before binding.
//! Close approval is supplied by Firefox, never inferred from a native click.

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ShellWindowBinding {
    pub generation: u64,
    pub shell: usize,
    pub content: usize,
    pub close_request: u64,
    /// 0 unbound, 1 attached, 2 awaiting Firefox, 3 approved, 4 content gone,
    /// 5 shell destroyed, 6 native startup awaiting content, 7 startup cancelled.
    /// An endpoint can prepare and bind only once; cancellation is terminal.
    pub phase: u32,
}

#[derive(Default)]
pub struct WindowHost {
    state: ShellWindowBinding,
    next_close: u64,
}

impl WindowHost {
    pub fn state(&self) -> ShellWindowBinding { self.state }
    pub fn prepare(&mut self, generation: u64, shell: usize) -> bool {
        if self.state.phase != 0 || generation == 0 || shell == 0 { return false; }
        self.state = ShellWindowBinding { generation, shell, phase: 6, ..Default::default() };
        true
    }
    pub fn cancel_startup(&mut self, generation: u64, shell: usize) -> bool {
        if self.state.phase != 6 || self.state.generation != generation || self.state.shell != shell { return false; }
        self.state.phase = 7;
        true
    }
    pub fn invalidate_close(&mut self) {
        if matches!(self.state.phase, 2 | 3) { self.state.phase = 1; }
        self.state.close_request = 0;
    }
    pub fn bind(&mut self, generation: u64, shell: usize, content: usize) -> bool {
        if generation == 0 || shell == 0 || content == 0 || shell == content { return false; }
        if self.state.phase != 0 && !(self.state.phase == 6 &&
            self.state.generation == generation && self.state.shell == shell) { return false; }
        self.state = ShellWindowBinding { generation, shell, content, phase: 1, close_request: 0 };
        true
    }
    /// Repeated Close while awaiting a decision is coalesced. The adapter posts
    /// the returned request once, outside the window-procedure callback stack.
    pub fn request_close(&mut self) -> bool {
        if self.state.phase != 1 { return false; }
        let Some(next) = self.next_close.checked_add(1) else { return false; };
        self.next_close = next;
        self.state.close_request = next;
        self.state.phase = 2;
        true
    }
    pub fn resolve_close(&mut self, generation: u64, request: u64, allow: bool) -> bool {
        if self.state.phase != 2 || generation != self.state.generation || request != self.state.close_request { return false; }
        self.state.phase = if allow { 3 } else { 1 };
        self.state.close_request = 0;
        true
    }
    /// Browser-initiated destruction and startup/renderer failure may remove
    /// content without a native close request. Invalidate the endpoint first;
    /// this is cleanup permission, not permission to close a live browser.
    pub fn content_destroyed(&mut self, generation: u64, content: usize) -> bool {
        if !(1..=3).contains(&self.state.phase) || generation != self.state.generation || content != self.state.content { return false; }
        self.state.content = 0;
        self.state.close_request = 0;
        self.state.phase = 4;
        true
    }
    pub fn shell_destroyed(&mut self, generation: u64, shell: usize) -> bool {
        if !matches!(self.state.phase, 4 | 7) || generation != self.state.generation || shell != self.state.shell { return false; }
        self.state.shell = 0;
        self.state.phase = 5;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn host() -> WindowHost { let mut h=WindowHost::default(); assert!(h.bind(7,10,11)); h }
    #[test]
    fn startup_keeps_identity_and_attaches_only_once() {
        let mut h=WindowHost::default();
        assert!(!h.prepare(0,10)); assert!(!h.prepare(7,0));
        assert!(h.prepare(7,10)); let ready=h.state();
        assert!(!h.prepare(7,10)); assert!(!h.bind(8,10,11)); assert!(!h.bind(7,12,11));
        assert!(!h.request_close()); assert_eq!(h.state(),ready);
        assert!(h.bind(7,10,11)); assert_eq!(h.state().shell,ready.shell);
        assert!(!h.bind(7,10,12)); assert!(!h.cancel_startup(7,10));
    }
    #[test]
    fn cancelled_startup_cannot_attach_or_resurrect() {
        let mut h=WindowHost::default(); assert!(h.prepare(7,10));
        assert!(!h.cancel_startup(8,10)); assert!(!h.shell_destroyed(7,10));
        assert!(h.cancel_startup(7,10)); assert!(!h.bind(7,10,11));
        assert!(h.shell_destroyed(7,10)); assert!(!h.prepare(8,10));
    }
    #[test]
    fn close_requires_current_browser_decision_and_content_teardown() {
        let mut h=host(); assert!(!h.shell_destroyed(7,10));
        assert!(h.request_close()); let first=h.state().close_request;
        assert!(!h.request_close()); assert!(!h.resolve_close(8,first,true));
        assert!(h.resolve_close(7,first,false)); assert_eq!(h.state().phase,1);
        assert!(h.request_close()); let second=h.state().close_request; assert_ne!(first,second);
        assert!(!h.resolve_close(7,first,true)); assert!(h.resolve_close(7,second,true));
        assert!(!h.resolve_close(7,second,true)); assert!(!h.shell_destroyed(7,10));
        assert!(h.content_destroyed(7,11)); assert!(h.shell_destroyed(7,10));
        assert!(!h.bind(8,10,11)); assert!(!h.request_close());
    }
    #[test]
    fn invalid_binding_and_stale_handles_do_not_mutate() {
        let mut h=WindowHost::default();
        for (g,s,c) in [(0,10,11),(7,0,11),(7,10,0),(7,10,10)] { assert!(!h.bind(g,s,c)); }
        assert!(h.bind(7,10,11)); let before=h.state();
        assert!(!h.bind(8,12,13)); assert!(!h.content_destroyed(8,11)); assert!(!h.content_destroyed(7,12));
        assert_eq!(before,h.state());
    }
    #[test]
    fn content_loss_invalidates_pending_approval_and_recycled_handles() {
        let mut old=host(); assert!(old.request_close()); let request=old.state().close_request;
        assert!(old.content_destroyed(7,11)); assert!(!old.resolve_close(7,request,true));
        let mut new=WindowHost::default(); assert!(new.bind(8,10,11)); assert!(new.request_close());
        assert!(!new.resolve_close(7,request,true)); assert!(!new.content_destroyed(7,11));
        assert!(old.shell_destroyed(7,10)); assert_eq!(new.state().phase,2);
    }
    #[test]
    fn exhausted_close_identity_never_wraps() {
        let mut h=host(); h.next_close=u64::MAX; let before=h.state();
        assert!(!h.request_close()); assert_eq!(h.state(),before);
    }
}
