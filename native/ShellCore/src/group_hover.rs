//! One bounded dwell candidate. Time comes from the host's monotonic clock.
#[derive(Default)]
pub struct GroupHover {
    source: u32,
    target: u32,
    since: u64,
    delay: u32,
}

impl GroupHover {
    pub fn observe(&mut self, source: u32, target: u32, now: u64, delay: u32) -> bool {
        if source == 0 || target == 0 || source == target {
            *self = Self::default();
            return false;
        }
        if (self.source, self.target, self.delay) != (source, target, delay) || now < self.since {
            *self = Self { source, target, since: now, delay };
        }
        now.saturating_sub(self.since) >= u64::from(delay)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn dwell_and_target_change() {
        let mut h = GroupHover::default();
        assert!(!h.observe(1, 2, 100, 240));
        assert!(!h.observe(1, 2, 339, 240));
        assert!(h.observe(1, 2, 340, 240));
        assert!(!h.observe(1, 3, 341, 240));
        assert!(!h.observe(4, 3, 600, 240));
        assert!(h.observe(4, 3, 840, 240));
    }
    #[test]
    fn exit_cancel_clock_and_policy_reset() {
        let mut h = GroupHover::default();
        assert!(h.observe(1, 2, 100, 0));
        assert!(!h.observe(1, 0, 200, 0));
        assert!(!h.observe(1, 2, 300, 240));
        assert!(!h.observe(1, 2, 200, 240));
        assert!(!h.observe(1, 2, 440, 500));
        assert!(!h.observe(1, 1, 1000, 0));
        assert!(!h.observe(0, 2, 1000, 0));
    }
}
