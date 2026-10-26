//! Shared structural tab-strip motion. Decorative lighting has its own timing.
pub const TAB_MOTION_MS: u32 = 135;
pub const GROUP_MOTION_MS: u32 = 150;

/// Cubic ease-out, matching WinUI CubicEase and the composition Bezier
/// (1/3, 1), (2/3, 1). Keep sampling and presentation on the same curve.
pub fn ease(t: f64) -> f64 {
    let rest = 1.0 - t.clamp(0.0, 1.0);
    1.0 - rest * rest * rest
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn structural_curve_is_bounded_monotonic_and_matches_cubic_out() {
        assert_eq!(ease(-1.0), 0.0);
        assert_eq!(ease(2.0), 1.0);
        assert_eq!(ease(0.5), 0.875);
        for step in 0..100 {
            assert!(ease(step as f64 / 100.0) <= ease((step + 1) as f64 / 100.0));
        }
    }
}
