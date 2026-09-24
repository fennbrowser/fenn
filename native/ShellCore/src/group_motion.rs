//! Visibility-only group animation planning. Firefox supplies both layouts;
//! surviving tabs (including a retained active tab) never become departures.
use crate::strip::ShellStripSlot;
use std::collections::{HashMap, HashSet};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct ShellGroupMotionRow {
    pub tab: u32,
    pub group: u32,
    pub entering: bool,
}

pub fn plan(old: &[ShellStripSlot], new: &[ShellStripSlot], toggled: &[u32]) -> Vec<ShellGroupMotionRow> {
    let groups: HashSet<_> = toggled.iter().copied().collect();
    let before: HashMap<_, _> = old.iter().filter(|s| s.tab != 0).map(|s| (s.tab, s.group)).collect();
    let after: HashMap<_, _> = new.iter().filter(|s| s.tab != 0).map(|s| (s.tab, s.group)).collect();
    let mut result = Vec::new();
    for (slots, other, entering) in [(old, &after, false), (new, &before, true)] {
        for slot in slots {
            if slot.tab != 0 && groups.contains(&slot.group) && !other.contains_key(&slot.tab) {
                result.push(ShellGroupMotionRow { tab: slot.tab, group: slot.group, entering });
            }
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    fn slots(tabs: &[u32]) -> Vec<ShellStripSlot> {
        std::iter::once(ShellStripSlot { group: 7, tab: 0 })
            .chain(tabs.iter().map(|&tab| ShellStripSlot { group: 7, tab })).collect()
    }
    #[test]
    fn retained_active_at_every_position_never_departs() {
        for active in 1..=3 {
            let old = slots(&[1, 2, 3]);
            let new = slots(&[active]);
            let leaving = plan(&old, &new, &[7]);
            assert_eq!(leaving.len(), 2);
            assert!(leaving.iter().all(|r| !r.entering && r.tab != active));
            let entering = plan(&new, &old, &[7]);
            assert_eq!(entering.len(), 2);
            assert!(entering.iter().all(|r| r.entering && r.tab != active));
        }
    }
    #[test]
    fn unrelated_close_and_reorder_do_not_animate_as_group_departures() {
        assert!(plan(&slots(&[1, 2]), &slots(&[2, 1]), &[7]).is_empty());
        assert!(plan(&slots(&[1, 2]), &slots(&[1]), &[]).is_empty());
        assert_eq!(plan(&slots(&[1, 2]), &slots(&[]), &[7]).len(), 2);
    }
    #[test]
    fn simultaneous_groups_and_direction_changes_use_authoritative_membership() {
        let old = [ShellStripSlot { group: 7, tab: 1 }, ShellStripSlot { group: 7, tab: 2 },
                   ShellStripSlot { group: 8, tab: 3 }, ShellStripSlot { group: 8, tab: 4 }];
        let new = [old[1], old[2]];
        assert_eq!(plan(&old, &new, &[7, 8]), vec![
            ShellGroupMotionRow { tab: 1, group: 7, entering: false },
            ShellGroupMotionRow { tab: 4, group: 8, entering: false }]);
        assert_eq!(plan(&new, &old, &[7, 8]).len(), 2);
        // Moving a surviving tab into another group is not a visibility change.
        assert!(plan(&[old[0]], &[ShellStripSlot { group: 8, tab: 1 }], &[7, 8]).is_empty());
    }
}
