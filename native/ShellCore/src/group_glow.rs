//! Which groups' glow is lit, as the owner defines new lighting: a group
//! lights up when it first becomes eligible and shown, not when scrolling or
//! realization merely shows an already-lit group again. A full snapshot, or
//! light-up motion turning on, seeds everything eligible as already lit.
//! Also tracks each lit group's settled extent, so a settled group that grew
//! re-catches its moved edge. The host owns every Composition object and
//! animation; this decides only which groups each one applies to.
use std::collections::{BTreeMap, BTreeSet};

/// A settled group that grew at least this much re-catches its moved edge.
pub const RECATCH_GROWTH: f32 = 8.0;
pub const RECATCH_EDGE: f32 = 4.0;
/// Bound on groups per call.
pub const MAX_GROUPS: usize = 10_000;

/// One glow-eligible group (glow on and colored) this refresh.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct ShellGlowGroup {
    pub group: u32,
    /// Laid out and inside the viewport.
    pub shown: bool,
}

pub const SHELL_GLOW_LIT: u32 = 1;
pub const SHELL_GLOW_HALO_LIT: u32 = 2;
pub const SHELL_GLOW_GONE: u32 = 4;
pub const SHELL_GLOW_HALO_GONE: u32 = 8;

/// What happened to one group's light this refresh: `SHELL_GLOW_*` flags.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct ShellGlowChange {
    pub group: u32,
    pub flags: u32,
}

/// A shown group's horizontal extent in scroll-content space.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct ShellGlowExtent {
    pub group: u32,
    pub left: f32,
    pub right: f32,
}

/// A settled group grew: re-catch the edges that moved.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct ShellGlowRecatch {
    pub group: u32,
    pub left: bool,
    pub right: bool,
}

#[derive(Clone)]
pub struct GroupGlow {
    lit: BTreeSet<u32>,
    halo: BTreeSet<u32>,
    seed: bool,
    animated: bool,
    settled: BTreeMap<u32, (f32, f32)>,
}

impl Default for GroupGlow {
    fn default() -> Self {
        Self { lit: BTreeSet::new(), halo: BTreeSet::new(), seed: true, animated: false, settled: BTreeMap::new() }
    }
}

/// Drop ids no longer eligible into `gone`; light newly shown eligible ids
/// unless seeding, which accepts everything eligible as already lit.
fn advance_set(ids: &mut BTreeSet<u32>, eligible: &BTreeSet<u32>, shown: &BTreeSet<u32>,
               seed: bool, gone: &mut BTreeSet<u32>) -> Vec<u32> {
    ids.retain(|id| eligible.contains(id) || { gone.insert(*id); false });
    if seed {
        ids.extend(eligible.iter().copied());
        return Vec::new();
    }
    shown.iter().copied().filter(|id| eligible.contains(id) && ids.insert(*id)).collect()
}

impl GroupGlow {
    /// The next refresh shows its groups already lit (a full snapshot).
    pub fn seed(&mut self) { self.seed = true; }

    pub fn is_lit(&self, group: u32) -> bool { self.lit.contains(&group) }

    /// Advance one refresh. Returns whether light-up motion just turned off
    /// (the host cancels and detaches its motion) and each changed group.
    pub fn advance(&mut self, animate: bool, halo: bool, groups: &[ShellGlowGroup]) -> (bool, Vec<ShellGlowChange>) {
        let mut disabled = false;
        if animate != self.animated {
            self.animated = animate;
            if animate { self.seed = true; } else { disabled = true; self.settled.clear(); }
        }
        let seed = self.seed || !animate;
        self.seed = false;
        let eligible: BTreeSet<u32> = groups.iter().map(|g| g.group).collect();
        let shown: BTreeSet<u32> = groups.iter().filter(|g| g.shown).map(|g| g.group).collect();
        let none = BTreeSet::new();
        let (mut gone, mut halo_gone) = (BTreeSet::new(), BTreeSet::new());
        let lit = advance_set(&mut self.lit, &eligible, &shown, seed, &mut gone);
        let halo_lit = advance_set(&mut self.halo, if halo { &eligible } else { &none },
                                   if halo { &shown } else { &none }, seed, &mut halo_gone);
        let mut changes: BTreeMap<u32, u32> = BTreeMap::new();
        for (ids, flag) in [(lit.iter().collect::<Vec<_>>(), SHELL_GLOW_LIT), (halo_lit.iter().collect(), SHELL_GLOW_HALO_LIT),
                            (gone.iter().collect(), SHELL_GLOW_GONE | SHELL_GLOW_HALO_GONE),
                            (halo_gone.iter().collect(), SHELL_GLOW_HALO_GONE)] {
            for &id in ids { *changes.entry(id).or_default() |= flag; }
        }
        (disabled, changes.into_iter().map(|(group, flags)| ShellGlowChange { group, flags }).collect())
    }

    /// Forget extents of groups no longer lit; unless geometry is still
    /// moving, record each shown group's settled extent and report those that
    /// grew. Drags, slides and joins are followed by implicit motion, so
    /// extents are compared only once they settle.
    pub fn settle(&mut self, moving: bool, extents: &[ShellGlowExtent]) -> Vec<ShellGlowRecatch> {
        let lit = &self.lit;
        self.settled.retain(|id, _| lit.contains(id));
        if moving { return Vec::new(); }
        let mut recatch = Vec::new();
        for e in extents {
            let Some(old) = self.settled.insert(e.group, (e.left, e.right)) else { continue; };
            if (e.right - e.left) - (old.1 - old.0) < RECATCH_GROWTH { continue; }
            recatch.push(ShellGlowRecatch { group: e.group, left: old.0 - e.left >= RECATCH_EDGE,
                                            right: e.right - old.1 >= RECATCH_EDGE });
        }
        recatch
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn g(group: u32, shown: bool) -> ShellGlowGroup { ShellGlowGroup { group, shown } }
    fn flags(changes: &[ShellGlowChange], group: u32) -> u32 {
        changes.iter().find(|c| c.group == group).map_or(0, |c| c.flags)
    }

    #[test]
    fn first_refresh_and_motion_turning_on_seed_rather_than_light() {
        let mut glow = GroupGlow::default();
        let (disabled, changes) = glow.advance(true, false, &[g(1, true)]);
        assert!(!disabled && changes.is_empty() && glow.is_lit(1));
        // A group that becomes eligible later lights up once.
        let (_, changes) = glow.advance(true, false, &[g(1, true), g(2, true)]);
        assert_eq!(changes, vec![ShellGlowChange { group: 2, flags: SHELL_GLOW_LIT }]);
        assert!(glow.advance(true, false, &[g(1, true), g(2, true)]).1.is_empty());
        // Motion off: nothing lights, but light still goes out.
        let (disabled, changes) = glow.advance(false, false, &[g(1, true), g(3, true)]);
        assert!(disabled);
        assert_eq!(changes, vec![ShellGlowChange { group: 2, flags: SHELL_GLOW_GONE | SHELL_GLOW_HALO_GONE }]);
        assert!(glow.is_lit(3));
        // Motion back on seeds what is already eligible.
        assert!(glow.advance(true, false, &[g(1, true), g(3, true), g(4, true)]).1.is_empty());
    }

    #[test]
    fn scrolling_or_realization_never_relights() {
        let mut glow = GroupGlow::default();
        glow.advance(true, false, &[]);
        // Eligible but culled: not lit until shown.
        assert!(glow.advance(true, false, &[g(5, false)]).1.is_empty());
        assert!(!glow.is_lit(5));
        assert_eq!(flags(&glow.advance(true, false, &[g(5, true)]).1, 5), SHELL_GLOW_LIT);
        // Scrolled away and back: still lit, nothing changes.
        assert!(glow.advance(true, false, &[g(5, false)]).1.is_empty());
        assert!(glow.advance(true, false, &[g(5, true)]).1.is_empty());
    }

    #[test]
    fn halo_lights_and_goes_with_its_option_and_its_group() {
        let mut glow = GroupGlow::default();
        glow.advance(true, false, &[g(1, true)]);
        // Halo turning on alone ignites only the halo.
        assert_eq!(glow.advance(true, true, &[g(1, true)]).1, vec![ShellGlowChange { group: 1, flags: SHELL_GLOW_HALO_LIT }]);
        assert_eq!(glow.advance(true, false, &[g(1, true)]).1, vec![ShellGlowChange { group: 1, flags: SHELL_GLOW_HALO_GONE }]);
        let (_, changes) = glow.advance(true, true, &[g(1, true), g(2, true)]);
        assert_eq!(flags(&changes, 2), SHELL_GLOW_LIT | SHELL_GLOW_HALO_LIT);
        // Explicit seeding (a full snapshot) accepts a new group as lit.
        glow.seed();
        assert!(glow.advance(true, true, &[g(1, true), g(2, true), g(3, true)]).1.is_empty());
    }

    #[test]
    fn settled_growth_recatches_moved_edges_only_once_settled() {
        let mut glow = GroupGlow::default();
        glow.advance(true, false, &[g(1, true), g(2, true)]);
        let e = |group, left, right| ShellGlowExtent { group, left, right };
        assert!(glow.settle(false, &[e(1, 100.0, 200.0), e(2, 300.0, 400.0)]).is_empty());
        // Moving: no comparison and nothing recorded.
        assert!(glow.settle(true, &[e(1, 100.0, 300.0)]).is_empty());
        assert_eq!(glow.settle(false, &[e(1, 100.0, 210.0), e(2, 290.0, 405.0)]),
                   vec![ShellGlowRecatch { group: 1, left: false, right: true },
                        ShellGlowRecatch { group: 2, left: true, right: true }]);
        // Shrinking or small growth does not re-catch.
        assert!(glow.settle(false, &[e(1, 100.0, 150.0), e(2, 290.0, 412.0)]).is_empty());
        // Going out forgets the extent; turning motion off forgets all.
        glow.advance(true, false, &[g(2, true)]);
        glow.settle(true, &[]);
        assert!(glow.settle(false, &[e(1, 0.0, 500.0)]).is_empty());
        assert!(glow.settle(false, &[e(1, 0.0, 500.0)]).is_empty());
        glow.advance(false, false, &[g(2, true)]);
        assert!(glow.settled.is_empty());
    }
}
