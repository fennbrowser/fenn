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

// Fluent structural recipes, not private Explorer constants. Legacy group
// and drag motion above remains separate.
pub const SHELL_MOTION_REPOSITION_MS: u32 = 167;
pub const SHELL_MOTION_ENTER_MS: u32 = 250;
pub const SHELL_MOTION_EXIT_MS: u32 = 125;
pub const SHELL_MOTION_FADE_MS: u32 = 83;
pub const SHELL_MOTION_TRAVEL_DIP: f64 = 6.0;
pub const SHELL_MOTION_MOVE_X1: f64 = 0.55;
pub const SHELL_MOTION_MOVE_Y1: f64 = 0.55;
pub const SHELL_MOTION_MOVE_X2: f64 = 0.0;
pub const SHELL_MOTION_MOVE_Y2: f64 = 1.0;

/// Invert the Bezier time axis so lighting follows compositor geometry.
pub fn reposition(t: f64) -> f64 {
    let t = t.clamp(0.0, 1.0);
    if t == 0.0 || t == 1.0 { return t; }
    fn axis(u: f64, a: f64, b: f64) -> f64 {
        let v = 1.0 - u;
        3.0 * v * v * u * a + 3.0 * v * u * u * b + u * u * u
    }
    let (mut low, mut high) = (0.0, 1.0);
    for _ in 0..32 {
        let u = (low + high) * 0.5;
        if axis(u, SHELL_MOTION_MOVE_X1, SHELL_MOTION_MOVE_X2) < t { low = u; }
        else { high = u; }
    }
    axis((low + high) * 0.5, SHELL_MOTION_MOVE_Y1, SHELL_MOTION_MOVE_Y2)
}

#[cfg(test)]
mod structural_tests {
    use super::*;
    #[test]
    fn reposition_matches_parametric_bezier_and_is_bounded() {
        assert_eq!(reposition(-1.0), 0.0);
        assert_eq!(reposition(2.0), 1.0);
        // At Bezier parameter u=0.5: x=.33125, y=.70625.
        assert!((reposition(0.33125) - 0.70625).abs() < 1e-8);
        for i in 0..1000 {
            let a = reposition(i as f64 / 1000.0);
            let b = reposition((i + 1) as f64 / 1000.0);
            assert!(a <= b && a >= 0.0 && b <= 1.0);
        }
    }
}
