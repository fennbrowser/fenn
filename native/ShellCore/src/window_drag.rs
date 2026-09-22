//! Bounded coordination around an OS-owned move loop. No HWNDs or browser policy.
use std::sync::atomic::{AtomicU64, Ordering};

static NEXT_ENDPOINT: AtomicU64 = AtomicU64::new(1);

#[repr(C)]
#[derive(Default, Clone, Copy, Debug, PartialEq, Eq)]
pub struct ShellWindowDrag {
    pub endpoint: u64,
    pub gesture: u64,
    pub target: u64,
    pub target_endpoint: u64,
    pub tab: u32,
    pub target_revision: u32,
    /// 0 idle, 1 moving, 2 pending adoption, 3 consumed, 4 awaiting result, 5 continuation ready.
    pub phase: u32,
}

pub struct WindowDrag {
    state: ShellWindowDrag,
    start: (i32, i32),
    threshold: (u32, u32),
    moved: bool,
    cancelled: bool,
}

impl Default for WindowDrag {
    fn default() -> Self {
        Self {
            state: ShellWindowDrag { endpoint: NEXT_ENDPOINT.fetch_add(1, Ordering::Relaxed), ..Default::default() },
            start: (0, 0), threshold: (1, 1), moved: false, cancelled: false,
        }
    }
}

impl WindowDrag {
    pub fn state(&self) -> ShellWindowDrag { self.state }
    pub fn begin(&mut self, tab: u32, x: i32, y: i32, dx: u32, dy: u32) {
        self.state = ShellWindowDrag {
            endpoint: self.state.endpoint,
            gesture: self.state.gesture.saturating_add(1),
            tab, phase: 1, ..Default::default()
        };
        self.start = (x, y);
        self.threshold = (dx.max(1), dy.max(1));
        self.moved = false;
        self.cancelled = self.state.gesture == u64::MAX;
    }
    pub fn observe(&mut self, eligible: bool, x: i32, y: i32, target: u64, endpoint: u64, revision: u32) {
        if self.state.phase != 1 { return; }
        self.cancelled |= !eligible;
        self.moved |= (i64::from(x) - i64::from(self.start.0)).unsigned_abs() >= u64::from(self.threshold.0)
            || (i64::from(y) - i64::from(self.start.1)).unsigned_abs() >= u64::from(self.threshold.1);
        let valid = !self.cancelled && self.state.tab != 0 && endpoint != 0 && endpoint != self.state.endpoint && revision != 0;
        self.state.target = if valid { target } else { 0 };
        self.state.target_endpoint = if valid { endpoint } else { 0 };
        self.state.target_revision = if valid { revision } else { 0 };
    }
    pub fn await_adoption(&mut self, request: u64) {
        self.cancel();
        if request == 0 { return; }
        self.state.target = request;
        self.state.tab = 0;
        self.state.phase = 4;
    }
    pub fn complete_adoption(&mut self, request: u64, tab: u32) {
        if self.state.phase != 4 || self.state.target != request { return; }
        self.state.tab = tab;
        self.state.phase = if tab != 0 { 5 } else { 0 };
    }
    pub fn take_continuation(&mut self) -> bool {
        if self.state.phase != 5 { return false; }
        self.state.phase = 3;
        true
    }
    pub fn cancel(&mut self) {
        self.cancelled = true;
        self.state.target = 0;
        self.state.phase = 0;
    }
    pub fn finish(&mut self, released: bool) {
        if self.state.phase != 1 { return; }
        self.state.phase = if released && !self.cancelled && self.moved && self.state.tab != 0 && self.state.target != 0 { 2 } else { 0 };
    }
    pub fn take(&mut self, gesture: u64) -> bool {
        if self.state.phase != 2 || self.state.gesture != gesture { return false; }
        self.state.phase = 3;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn moving() -> WindowDrag {
        let mut d = WindowDrag::default();
        d.begin(7, 100, 100, 4, 4);
        d.observe(true, 110, 100, 99, u64::MAX - 1, 3);
        d
    }
    #[test] fn continuation_requires_exact_result_and_is_revoked_by_release() {
        let mut d = WindowDrag::default();
        d.await_adoption(12);
        d.complete_adoption(11, 7); assert!(!d.take_continuation());
        d.complete_adoption(12, 7); assert!(d.take_continuation());
        assert_eq!(d.state.tab, 7); assert!(!d.take_continuation());
        d.await_adoption(13); d.cancel(); d.complete_adoption(13, 8);
        assert!(!d.take_continuation());
        d.await_adoption(14); d.complete_adoption(14, 0); assert!(!d.take_continuation());
        d.await_adoption(15); d.await_adoption(16); d.complete_adoption(15, 9);
        assert!(!d.take_continuation());
    }
    #[test] fn once_only_and_late_completion() {
        let mut d = moving(); let token = d.state.gesture;
        d.finish(true); assert!(d.take(token)); assert!(!d.take(token));
        d = moving(); let old = d.state.gesture; d.finish(true);
        d.begin(7, 0, 0, 4, 4); assert!(!d.take(old));
    }
    #[test] fn cancellation_and_eligibility_loss_are_sticky() {
        let mut d = moving(); d.cancel(); d.finish(true); assert_eq!(d.state.phase, 0);
        let mut d = moving(); d.observe(false, 120, 100, 99, 999, 3);
        d.observe(true, 130, 100, 99, 999, 3); d.finish(true); assert_eq!(d.state.phase, 0);
    }
    #[test] fn requires_motion_release_and_tab_identity() {
        let mut d = moving(); d.finish(false); assert_eq!(d.state.phase, 0);
        let mut d = WindowDrag::default(); d.begin(7, 100, 100, 4, 4);
        d.observe(true, 101, 100, 99, 999, 3); d.finish(true); assert_eq!(d.state.phase, 0);
        d.begin(0, 100, 100, 4, 4); d.observe(true, 120, 100, 99, 999, 3);
        d.finish(true); assert_eq!(d.state.phase, 0);
    }
    #[test] fn leaving_target_and_endpoint_replacement() {
        let mut d = moving(); d.observe(true, 120, 100, 0, 0, 0);
        d.finish(true); assert_eq!(d.state.phase, 0);
        let mut d = moving(); d.observe(true, 120, 100, 99, 12345, 5);
        d.finish(true); assert_eq!(d.state.target_endpoint, 12345); assert_eq!(d.state.target_revision, 5);
        assert_ne!(WindowDrag::default().state.endpoint, WindowDrag::default().state.endpoint);
    }
}
