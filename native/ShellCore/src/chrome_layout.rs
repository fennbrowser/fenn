/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Native-owned row allocation inside a host's client area. The host supplies
//! visibility decisions; this module knows no browser, DOM, HWND or controls.
//! A non-fitting allocation is a request for host fallback, not permission to
//! hide browser UI or overwrite its content bounds.

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ShellChromeLayoutInput {
    pub width: i32,
    pub height: i32,
    pub dpi: u32,
    /// Zero omits the strip; otherwise the supported 32/40/48 DIP row height.
    pub strip_height: u32,
    pub navigation: bool,
    /// Frame around navigation/content, in DIPs; ignored without navigation.
    pub content_inset: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ShellChromeRect {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ShellChromeLayout {
    pub fits: bool,
    pub strip: ShellChromeRect,
    pub navigation: ShellChromeRect,
    pub content: ShellChromeRect,
}

pub fn layout(input: ShellChromeLayoutInput) -> Option<ShellChromeLayout> {
    if !(0..=100_000).contains(&input.width) || !(0..=100_000).contains(&input.height)
        || !(48..=768).contains(&input.dpi) || ![0, 32, 40, 48].contains(&input.strip_height)
        || input.content_inset > 32 {
        return None;
    }
    // Round shared edges, not each row's height, so fractional scaling cannot
    // create a gap/overlap between rows. Bounded integer inputs cannot overflow.
    let edge = |dip: u32| ((dip * input.dpi + 48) / 96) as i32;
    let strip_end = edge(input.strip_height);
    let chrome_end = edge(input.strip_height + if input.navigation { 40 } else { 0 });
    let inset = if input.navigation { edge(input.content_inset) } else { 0 };
    let inner_width = input.width - 2 * inset;
    let minimum_width = if input.navigation { (528 * input.dpi).div_ceil(96) as i32 } else { 1 };
    let fits = inner_width >= minimum_width && input.height > chrome_end + inset;
    let mut result = ShellChromeLayout {
        fits,
        // Safe diagnostic fallback bounds. The caller decides whether/how to
        // restore Firefox UI; it must not apply an unsuccessful allocation.
        content: ShellChromeRect { width: input.width, height: input.height, ..Default::default() },
        ..Default::default()
    };
    if !fits { return Some(result); }
    if input.strip_height != 0 {
        result.strip = ShellChromeRect { width: input.width, height: strip_end, ..Default::default() };
    }
    if input.navigation {
        result.navigation = ShellChromeRect { x: inset, y: strip_end, width: inner_width, height: chrome_end - strip_end };
    }
    result.content = ShellChromeRect { x: inset, y: chrome_end, width: inner_width, height: input.height - chrome_end - inset };
    Some(result)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn input() -> ShellChromeLayoutInput {
        ShellChromeLayoutInput { width: 1000, height: 800, dpi: 96, strip_height: 40, navigation: true, content_inset: 4 }
    }
    #[test]
    fn default_rows_and_content_share_edges() {
        let plan = layout(input()).unwrap();
        assert!(plan.fits);
        assert_eq!(plan.strip, ShellChromeRect { x: 0, y: 0, width: 1000, height: 40 });
        assert_eq!(plan.navigation, ShellChromeRect { x: 4, y: 40, width: 992, height: 40 });
        assert_eq!(plan.content, ShellChromeRect { x: 4, y: 80, width: 992, height: 716 });
    }
    #[test]
    fn all_supported_rows_and_fractional_dpi_preserve_containment() {
        for dpi in [48, 96, 120, 144, 168, 192, 216, 240, 288, 768, 109] {
            for strip_height in [0, 32, 40, 48] {
                for navigation in [false, true] {
                    let plan = layout(ShellChromeLayoutInput { width: 8000, height: 4000, dpi, strip_height, navigation, ..input() }).unwrap();
                    assert!(plan.fits);
                    assert_eq!(plan.strip.height, plan.navigation.y.max(if navigation { 0 } else { plan.content.y }));
                    if navigation {
                        assert_eq!(plan.navigation.y + plan.navigation.height, plan.content.y);
                        assert_eq!(plan.navigation.x, plan.content.x);
                        assert_eq!(plan.navigation.width, plan.content.width);
                    }
                    for rect in [plan.strip, plan.navigation, plan.content] {
                        assert!(rect.x >= 0 && rect.y >= 0 && rect.width >= 0 && rect.height >= 0);
                        assert!(rect.x + rect.width <= 8000 && rect.y + rect.height <= 4000);
                    }
                }
            }
        }
    }
    #[test]
    fn insufficient_space_never_returns_partial_chrome() {
        for (width, height) in [(0, 0), (535, 800), (1000, 84), (1000, 0)] {
            let plan = layout(ShellChromeLayoutInput { width, height, ..input() }).unwrap();
            assert!(!plan.fits);
            assert_eq!(plan.strip, ShellChromeRect::default());
            assert_eq!(plan.navigation, ShellChromeRect::default());
            assert_eq!((plan.content.width, plan.content.height), (width, height));
        }
        assert!(layout(ShellChromeLayoutInput { width: 536, height: 85, ..input() }).unwrap().fits);
    }
    #[test]
    fn host_visibility_decisions_remove_reservations() {
        let plan = layout(ShellChromeLayoutInput { strip_height: 0, navigation: false, ..input() }).unwrap();
        assert!(plan.fits);
        assert_eq!(plan.content, ShellChromeRect { x: 0, y: 0, width: 1000, height: 800 });
        let strip = layout(ShellChromeLayoutInput { navigation: false, ..input() }).unwrap();
        assert_eq!(strip.content.x, 0);
        assert_eq!(strip.content.y, 40);
        assert_eq!(strip.content.height, 760);
    }
    #[test]
    fn bad_inputs_are_refused_and_resize_is_history_independent() {
        for bad in [ShellChromeLayoutInput { width: -1, ..input() }, ShellChromeLayoutInput { height: 100001, ..input() },
            ShellChromeLayoutInput { dpi: 0, ..input() }, ShellChromeLayoutInput { dpi: u32::MAX, ..input() },
            ShellChromeLayoutInput { strip_height: 41, ..input() }, ShellChromeLayoutInput { content_inset: 33, ..input() }] {
            assert_eq!(layout(bad), None);
        }
        let original = layout(input());
        for width in [800, 0, 300, 1200, 1000] { layout(ShellChromeLayoutInput { width, ..input() }); }
        assert_eq!(layout(input()), original);
    }
}
