//! Group surface motion during a tab drag: the destination group opening
//! toward a tail insertion gap, a lifted member's group holding its pick-up
//! extent, the joined group's surface held after release until Firefox
//! publishes the join, and the reach every surface extends past its outer
//! members. The host measures realized XAML geometry and applies the result;
//! the timing and bookkeeping live here. Time is the host's monotonic clock in
//! milliseconds.
use crate::strip::ShellStripSlot;

/// Ease-out duration shared with the sliding tabs.
pub const EASE_MS: f64 = 135.0;
/// Bound on surfaces and layout slots per frame.
pub const MAX_ENTRIES: usize = 10_000;

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct ShellGroupSurface {
    pub group: u32,
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
}

/// One refresh's inputs. `frame_ms` is sampled once per rendered frame, so
/// layout passes inside one frame see the same extension; `wall_ms` is
/// current time, so a held join still expires when no frames render.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct ShellGroupSurfaceFrame {
    pub frame_ms: f64,
    pub wall_ms: f64,
    /// Group whose last visible member the drag gap lies past, and the gap's
    /// width; zero otherwise.
    pub tail_group: u32,
    pub tail_width: f32,
    /// The dragged tab's own expanded group while it is a member; zero otherwise.
    pub source_group: u32,
    /// The dragged tab's held slot, in surface coordinates.
    pub drag_slot_x: f32,
    pub drag_slot_width: f32,
    /// The pointer, in surface coordinates.
    pub pointer_x: f32,
    /// The drag is detaching or targeting a join, so it cannot be returning
    /// into its source group.
    pub source_released: bool,
    pub dragging: bool,
    pub join_group: u32,
    pub animate: bool,
    pub right_to_left: bool,
    /// How far surfaces reach past their outer members, and the separation
    /// kept between neighboring groups.
    pub reach: f32,
    pub separation: f32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct ShellGroupSurfaceResult {
    /// A held or easing surface still moves on its own.
    pub following: bool,
    /// A surface holds an empty drag slot that needs a face.
    pub holding: bool,
}

#[derive(Clone, Copy, Default)]
struct Rect { x: f32, y: f32, width: f32, height: f32 }

impl Rect {
    fn of(s: &ShellGroupSurface) -> Self { Self { x: s.x, y: s.y, width: s.width, height: s.height } }
    fn store(self, s: &mut ShellGroupSurface) { s.x = self.x; s.y = self.y; s.width = self.width; s.height = self.height; }
}

fn eased(start: f64, now: f64) -> f32 {
    let t = ((now - start) / EASE_MS).clamp(0.0, 1.0) as f32;
    1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t)
}

#[derive(Clone, Copy, Default)]
struct Reach { group: u32, from: f32, target: f32, start: f64 }

impl Reach {
    fn value(&self, now: f64) -> f32 { self.from + (self.target - self.from) * eased(self.start, now) }
    fn settled(&self, now: f64) -> bool { now - self.start >= EASE_MS }
}

#[derive(Clone, Copy, Default)]
struct SourceHold { group: u32, rect: Rect, from: Rect, released: bool, at: f64 }

impl SourceHold {
    fn shown(&self, base: Rect, now: f64) -> Rect {
        let target = if self.released { base } else { self.rect };
        let e = eased(self.at, now);
        let left = self.from.x + (target.x - self.from.x) * e;
        let right = self.from.x + self.from.width + (target.x + target.width - self.from.x - self.from.width) * e;
        Rect { x: left, y: target.y, width: right - left, height: target.height }
    }
}

#[derive(Clone, Copy, Default)]
struct JoinHold { group: u32, tab: u32, rect: Rect, until: f64 }

#[derive(Default)]
pub struct GroupSurfaceMotion {
    reach: Reach,
    source: SourceHold,
    last: JoinHold,
    hold: JoinHold,
}

impl GroupSurfaceMotion {
    /// Adjust `surfaces` (one per group, unordered) for this refresh.
    pub fn frame(&mut self, f: &ShellGroupSurfaceFrame, layout: &[ShellStripSlot],
                 surfaces: &mut [ShellGroupSurface]) -> ShellGroupSurfaceResult {
        let now = f.frame_ms;
        // Open the destination group toward the insertion gap. A different
        // destination starts closed; the old one snaps shut.
        if f.tail_group != 0 && f.tail_group != self.reach.group {
            self.reach = Reach { group: f.tail_group, from: 0.0, target: 0.0, start: now };
        }
        let desired = if f.tail_group != 0 { f.tail_width } else { 0.0 };
        if self.reach.group != 0 && desired != self.reach.target {
            self.reach.from = if f.animate { self.reach.value(now) } else { desired };
            self.reach.target = desired;
            self.reach.start = now;
        }
        let extra = if self.reach.group != 0 { self.reach.value(now) } else { 0.0 };
        if extra > 0.01 {
            if let Some(s) = surfaces.iter_mut().find(|s| s.group == self.reach.group) {
                if f.right_to_left { s.x -= extra; }
                s.width += extra;
            }
        }
        if self.reach.group != 0 && f.tail_group == 0 && self.reach.target == 0.0 && self.reach.settled(now) {
            self.reach = Reach::default();
        }

        // Hold a lifted member's group at its pick-up extent while the pointer
        // stays within it; ease to the remaining members once it leaves, and
        // back again if it returns.
        if self.source.group != f.source_group { self.source = SourceHold::default(); }
        if f.source_group != 0 {
            if let Some(s) = surfaces.iter_mut().find(|s| s.group == f.source_group) {
                let base = Rect::of(s);
                let hold = &mut self.source;
                if hold.group == 0 {
                    let right = (base.x + base.width).max(f.drag_slot_x + f.drag_slot_width);
                    let x = base.x.min(f.drag_slot_x);
                    hold.group = f.source_group;
                    hold.rect = Rect { x, width: right - x, ..base };
                    hold.from = hold.rect;
                    hold.released = false;
                    hold.at = now - EASE_MS - 1.0;
                }
                let inside = !f.source_released && f.pointer_x >= hold.rect.x && f.pointer_x <= hold.rect.x + hold.rect.width;
                if inside == hold.released {
                    hold.from = hold.shown(base, now);
                    hold.released = !inside;
                    hold.at = now;
                }
                hold.shown(base, now).store(s);
            }
        }

        extend(surfaces, f.reach, f.separation);

        if f.dragging && f.join_group != 0 {
            if let Some(s) = surfaces.iter().find(|s| s.group == f.join_group) {
                self.last = JoinHold { group: f.join_group, tab: 0, rect: Rect::of(s), until: 0.0 };
            }
        }
        if self.hold.group != 0 {
            let joined = layout.iter().any(|s| s.group == self.hold.group && s.tab == self.hold.tab);
            if joined || f.dragging || f.wall_ms > self.hold.until {
                self.hold = JoinHold::default();
                self.reach = Reach::default();
            } else if let Some(s) = surfaces.iter_mut().find(|s| s.group == self.hold.group) {
                self.hold.rect.store(s);
            }
        }
        let holding = self.hold.group != 0 || self.source.group != 0;
        ShellGroupSurfaceResult {
            following: holding || (self.reach.group != 0 && !self.reach.settled(now)),
            holding,
        }
    }

    /// A drop asked Firefox to join `tab` to `group`: hold that group's last
    /// dragged surface until the join is published or `until` passes.
    pub fn join(&mut self, group: u32, tab: u32, until: f64) {
        if group != 0 && self.last.group == group {
            self.hold = JoinHold { group, tab, rect: self.last.rect, until };
        }
    }
}

/// Extend every surface past its first and last member so the outline, wash
/// and halo enclose an active tab at either end. Neighbors share the gap
/// between them, keep `separation` apart and never overlap.
pub fn extend(surfaces: &mut [ShellGroupSurface], reach: f32, separation: f32) {
    let mut order: Vec<usize> = (0..surfaces.len()).collect();
    order.sort_by(|&a, &b| surfaces[a].x.total_cmp(&surfaces[b].x));
    let mut left = vec![reach; order.len()];
    let mut right = vec![reach; order.len()];
    for i in 1..order.len() {
        let (prev, next) = (&surfaces[order[i - 1]], &surfaces[order[i]]);
        let share = ((next.x - (prev.x + prev.width) - separation) / 2.0).clamp(0.0, reach.max(0.0));
        right[i - 1] = right[i - 1].min(share);
        left[i] = left[i].min(share);
    }
    for (i, &at) in order.iter().enumerate() {
        surfaces[at].x -= left[i];
        surfaces[at].width += left[i] + right[i];
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn surface(group: u32, x: f32, width: f32) -> ShellGroupSurface {
        ShellGroupSurface { group, x, y: 0.0, width, height: 32.0 }
    }
    fn frame(now: f64) -> ShellGroupSurfaceFrame {
        ShellGroupSurfaceFrame { frame_ms: now, wall_ms: now, animate: true, ..Default::default() }
    }

    #[test]
    fn neighbors_share_the_gap_and_never_overlap() {
        let mut s = [surface(2, 300.0, 100.0), surface(1, 0.0, 100.0)];
        extend(&mut s, 5.0, 4.0);
        // Far apart: full reach on both sides.
        assert_eq!((s[1].x, s[1].width), (-5.0, 110.0));
        let mut s = [surface(1, 0.0, 100.0), surface(2, 108.0, 100.0)];
        extend(&mut s, 5.0, 4.0);
        // An 8-DIP gap less 4 separation leaves 2 per side.
        assert_eq!(s[0].x + s[0].width, 102.0);
        assert_eq!(s[1].x, 106.0);
        let mut s = [surface(1, 0.0, 100.0), surface(2, 101.0, 100.0)];
        extend(&mut s, 5.0, 4.0);
        assert_eq!((s[0].width, s[1].x), (105.0, 101.0));
        let mut s = [surface(1, 0.0, 100.0)];
        extend(&mut s, 0.0, 4.0);
        assert_eq!((s[0].x, s[0].width), (0.0, 100.0));
    }

    #[test]
    fn tail_reach_eases_open_and_closed_then_settles() {
        let mut m = GroupSurfaceMotion::default();
        let open = |now| ShellGroupSurfaceFrame { tail_group: 7, tail_width: 90.0, ..frame(now) };
        let mut s = [surface(7, 0.0, 100.0)];
        let r = m.frame(&open(1000.0), &[], &mut s);
        assert_eq!(s[0].width, 100.0);
        assert!(r.following && !r.holding);
        let mut s = [surface(7, 0.0, 100.0)];
        m.frame(&open(1000.0 + EASE_MS / 2.0), &[], &mut s);
        assert!(s[0].width > 100.0 + 45.0 && s[0].width < 190.0);
        let mut s = [surface(7, 0.0, 100.0)];
        let r = m.frame(&open(1000.0 + EASE_MS), &[], &mut s);
        assert_eq!(s[0].width, 190.0);
        assert!(!r.following);
        // Leaving closes it from where it is, then forgets the group.
        let mut s = [surface(7, 0.0, 100.0)];
        m.frame(&frame(1200.0), &[], &mut s);
        assert_eq!(s[0].width, 190.0);
        let mut s = [surface(7, 0.0, 100.0)];
        let r = m.frame(&frame(1200.0 + EASE_MS), &[], &mut s);
        assert_eq!(s[0].width, 100.0);
        assert!(!r.following && m.reach.group == 0);
    }

    #[test]
    fn reach_snaps_without_animation_and_extends_left_in_rtl() {
        let mut m = GroupSurfaceMotion::default();
        let f = ShellGroupSurfaceFrame { tail_group: 7, tail_width: 40.0, animate: false, right_to_left: true, ..frame(5.0) };
        let mut s = [surface(7, 100.0, 100.0)];
        m.frame(&f, &[], &mut s);
        assert_eq!((s[0].x, s[0].width), (60.0, 140.0));
        // A different destination starts closed; the old one snaps shut.
        let f = ShellGroupSurfaceFrame { tail_group: 8, ..f };
        let mut s = [surface(7, 100.0, 100.0), surface(8, 300.0, 100.0)];
        m.frame(&f, &[], &mut s);
        assert_eq!((s[0].width, s[1].width), (100.0, 140.0));
    }

    #[test]
    fn source_group_holds_its_pick_up_extent_until_the_pointer_leaves() {
        let mut m = GroupSurfaceMotion::default();
        let lifted = |now, pointer_x| ShellGroupSurfaceFrame {
            source_group: 3, drag_slot_x: 200.0, drag_slot_width: 100.0, pointer_x, dragging: true, ..frame(now) };
        // Remaining members span 0..200; the lifted tab's slot was 200..300.
        let mut s = [surface(3, 0.0, 200.0)];
        let r = m.frame(&lifted(0.0, 250.0), &[], &mut s);
        assert_eq!((s[0].x, s[0].width), (0.0, 300.0));
        assert!(r.holding && r.following);
        let mut s = [surface(3, 0.0, 200.0)];
        m.frame(&lifted(10.0, 400.0), &[], &mut s);
        assert_eq!(s[0].width, 300.0);
        let mut s = [surface(3, 0.0, 200.0)];
        m.frame(&lifted(10.0 + EASE_MS, 400.0), &[], &mut s);
        assert_eq!(s[0].width, 200.0);
        // Returning eases back out.
        let mut s = [surface(3, 0.0, 200.0)];
        m.frame(&lifted(200.0 + EASE_MS, 50.0), &[], &mut s);
        assert_eq!(s[0].width, 200.0);
        let mut s = [surface(3, 0.0, 200.0)];
        m.frame(&lifted(200.0 + 2.0 * EASE_MS, 50.0), &[], &mut s);
        assert_eq!(s[0].width, 300.0);
        // A join target or detaching drag releases it even inside.
        let mut s = [surface(3, 0.0, 200.0)];
        let f = ShellGroupSurfaceFrame { source_released: true, ..lifted(1000.0, 50.0) };
        m.frame(&f, &[], &mut s);
        let mut s = [surface(3, 0.0, 200.0)];
        m.frame(&ShellGroupSurfaceFrame { frame_ms: 1000.0 + EASE_MS, ..f }, &[], &mut s);
        assert_eq!(s[0].width, 200.0);
        // Ending the drag forgets the hold.
        let mut s = [surface(3, 0.0, 200.0)];
        let r = m.frame(&frame(2000.0), &[], &mut s);
        assert_eq!(s[0].width, 200.0);
        assert!(!r.holding);
    }

    #[test]
    fn joined_surface_is_held_until_published_dragging_or_expiry() {
        let layout = [ShellStripSlot { group: 5, tab: 0 }, ShellStripSlot { group: 5, tab: 1 }];
        let dragged = ShellGroupSurfaceFrame { dragging: true, join_group: 5, ..frame(0.0) };
        let setup = || {
            let mut m = GroupSurfaceMotion::default();
            let mut s = [surface(5, 0.0, 300.0)];
            m.frame(&dragged, &[], &mut s);
            m.join(5, 9, 1500.0);
            m
        };
        // Released: Firefox has not yet published, so the surface stays.
        let mut m = setup();
        let mut s = [surface(5, 0.0, 200.0)];
        let r = m.frame(&frame(100.0), &layout, &mut s);
        assert_eq!(s[0].width, 300.0);
        assert!(r.holding && r.following);
        // Published.
        let joined = [layout[0], layout[1], ShellStripSlot { group: 5, tab: 9 }];
        let mut s = [surface(5, 0.0, 300.0)];
        assert!(!m.frame(&frame(200.0), &joined, &mut s).holding);
        // Expired, measured on the wall clock.
        let mut m = setup();
        let mut s = [surface(5, 0.0, 200.0)];
        m.frame(&ShellGroupSurfaceFrame { wall_ms: 1501.0, ..frame(100.0) }, &layout, &mut s);
        assert_eq!(s[0].width, 200.0);
        // A new drag.
        let mut m = setup();
        let mut s = [surface(5, 0.0, 200.0)];
        m.frame(&ShellGroupSurfaceFrame { dragging: true, ..frame(100.0) }, &layout, &mut s);
        assert_eq!(s[0].width, 200.0);
        // Joining a group the drag never measured holds nothing.
        let mut m = setup();
        m.hold = JoinHold::default();
        m.join(6, 9, 1500.0);
        assert_eq!(m.hold.group, 0);
    }
}
