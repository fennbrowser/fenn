//! Rows and group headers easing back to rest after a tab opens or closes.
//! The host holds each one at its old geometry with a composition-only
//! translation and left-anchored scale, then animates both to rest on the
//! compositor, where layout bounds cannot see them. This records each hold and
//! samples the same curve on the UI thread, so lighting that is placed from
//! layout bounds can follow the elements instead of jumping to their final
//! geometry. Time is the host's monotonic clock in milliseconds.
use std::collections::BTreeMap;

/// Matches the host's neighbor animation: 135 ms, cubic-bezier(.215, .61, .355, 1).
pub const SETTLE_MS: f64 = 135.0;
const CURVE: (f64, f64, f64, f64) = (0.215, 0.61, 0.355, 1.0);
/// Bound on simultaneously settling elements.
pub const MAX_SETTLING: usize = 4096;

pub const SHELL_SETTLE_TAB: u32 = 0;
pub const SHELL_SETTLE_HEADER: u32 = 1;

/// One element's current composition offset and horizontal scale.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct ShellSettleSample {
    pub active: bool,
    pub dx: f32,
    pub sx: f32,
}

fn bezier(a: f64, b: f64, u: f64) -> f64 {
    let v = 1.0 - u;
    3.0 * v * v * u * a + 3.0 * v * u * u * b + u * u * u
}

/// Progress of the easing curve at linear time `t` in [0, 1].
pub fn ease(t: f64) -> f64 {
    if t <= 0.0 { return 0.0; }
    if t >= 1.0 { return 1.0; }
    // x(u) is monotonic for these control points: bisect for x(u) = t.
    let (mut lo, mut hi) = (0.0, 1.0);
    for _ in 0..40 {
        let mid = (lo + hi) / 2.0;
        if bezier(CURVE.0, CURVE.2, mid) < t { lo = mid; } else { hi = mid; }
    }
    bezier(CURVE.1, CURVE.3, (lo + hi) / 2.0)
}

#[derive(Clone, Copy)]
struct Hold { dx: f32, sx: f32, start: f64 }

#[derive(Default)]
pub struct Settle {
    holds: BTreeMap<(u32, u32), Hold>,
}

impl Settle {
    /// Record an element held at offset `dx` and scale `sx`, easing to rest
    /// from `start`. Returns false when the bound is reached.
    pub fn hold(&mut self, kind: u32, id: u32, dx: f32, sx: f32, start: f64) -> bool {
        if self.holds.len() >= MAX_SETTLING && !self.holds.contains_key(&(kind, id)) { return false; }
        self.holds.insert((kind, id), Hold { dx, sx, start });
        true
    }

    /// Where the element is drawn at `now`, relative to its layout bounds.
    pub fn sample(&self, kind: u32, id: u32, now: f64) -> ShellSettleSample {
        let Some(h) = self.holds.get(&(kind, id)) else { return ShellSettleSample { active: false, dx: 0.0, sx: 1.0 }; };
        if now - h.start >= SETTLE_MS { return ShellSettleSample { active: false, dx: 0.0, sx: 1.0 }; }
        let rest = (1.0 - ease((now - h.start) / SETTLE_MS)) as f32;
        ShellSettleSample { active: true, dx: h.dx * rest, sx: 1.0 + (h.sx - 1.0) * rest }
    }

    /// Forget finished holds; whether any element is still settling.
    pub fn advance(&mut self, now: f64) -> bool {
        self.holds.retain(|_, h| now - h.start < SETTLE_MS);
        !self.holds.is_empty()
    }

    pub fn clear(&mut self) { self.holds.clear(); }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn curve_matches_its_control_points() {
        assert_eq!(ease(0.0), 0.0);
        assert!((ease(1.0) - 1.0).abs() < 1e-9);
        // Ease-out: well ahead of linear early, monotonic throughout.
        assert!(ease(0.25) > 0.5);
        let mut last = 0.0;
        for step in 1..=100 {
            let value = ease(step as f64 / 100.0);
            assert!(value >= last);
            last = value;
        }
        assert!((ease(0.5) - 0.875).abs() < 0.01);
    }

    #[test]
    fn samples_ease_from_the_hold_to_rest_and_then_expire() {
        let mut settle = Settle::default();
        assert!(settle.hold(SHELL_SETTLE_TAB, 7, -100.0, 0.5, 1000.0));
        let start = settle.sample(SHELL_SETTLE_TAB, 7, 1000.0);
        assert_eq!(start, ShellSettleSample { active: true, dx: -100.0, sx: 0.5 });
        let mid = settle.sample(SHELL_SETTLE_TAB, 7, 1000.0 + SETTLE_MS / 2.0);
        assert!(mid.active && mid.dx > -50.0 && mid.dx < 0.0 && mid.sx > 0.75 && mid.sx < 1.0);
        assert!(settle.advance(1000.0 + SETTLE_MS - 1.0));
        let done = settle.sample(SHELL_SETTLE_TAB, 7, 1000.0 + SETTLE_MS);
        assert_eq!(done, ShellSettleSample { active: false, dx: 0.0, sx: 1.0 });
        assert!(!settle.advance(1000.0 + SETTLE_MS));
        assert!(!settle.sample(SHELL_SETTLE_TAB, 7, 1000.0).active);
    }

    #[test]
    fn tabs_and_headers_are_separate_and_a_new_hold_restarts() {
        let mut settle = Settle::default();
        settle.hold(SHELL_SETTLE_TAB, 3, 10.0, 1.0, 0.0);
        settle.hold(SHELL_SETTLE_HEADER, 3, -20.0, 1.0, 0.0);
        assert_eq!(settle.sample(SHELL_SETTLE_HEADER, 3, 0.0).dx, -20.0);
        assert_eq!(settle.sample(SHELL_SETTLE_TAB, 3, 0.0).dx, 10.0);
        settle.hold(SHELL_SETTLE_TAB, 3, 40.0, 1.0, 100.0);
        assert_eq!(settle.sample(SHELL_SETTLE_TAB, 3, 100.0).dx, 40.0);
        assert!(settle.advance(SETTLE_MS + 1.0));
        assert!(!settle.sample(SHELL_SETTLE_HEADER, 3, SETTLE_MS + 1.0).active);
        settle.clear();
        assert!(!settle.advance(0.0));
    }

    #[test]
    fn holds_are_bounded() {
        let mut settle = Settle::default();
        for id in 0..MAX_SETTLING as u32 { assert!(settle.hold(SHELL_SETTLE_TAB, id, 1.0, 1.0, 0.0)); }
        assert!(!settle.hold(SHELL_SETTLE_TAB, MAX_SETTLING as u32, 1.0, 1.0, 0.0));
        assert!(settle.hold(SHELL_SETTLE_TAB, 0, 2.0, 1.0, 0.0));
    }
}
