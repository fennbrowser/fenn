/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Strip publication identity and retained resource bounds. WinUI owns the
//! projection (hstrings, images and controls); this owns atomic delta acceptance.
use std::collections::{HashMap, HashSet};

#[derive(Default)]
pub struct StripState {
    revision: u32,
    rows: HashMap<u32, u32>,
    pixels: u64,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct ShellStripChange {
    pub id: u32,
    /// Total pixels in the resulting row, including an image retained by omission.
    pub pixels: u32,
    pub remove: bool,
    /// New rows must supply every required field, not a patch of an unknown ID.
    pub complete: bool,
}

impl StripState {
    pub fn apply(&mut self, base: u32, revision: u32, changes: &[ShellStripChange]) -> bool {
        if revision <= self.revision || (base != 0 && base != self.revision) || changes.len() > 20_000 {
            return false;
        }
        let mut ids = HashSet::new();
        let mut count = if base == 0 { 0 } else { self.rows.len() };
        let mut pixels = if base == 0 { 0 } else { self.pixels };
        for change in changes {
            if change.id == 0 || !ids.insert(change.id) || change.pixels > 1024 { return false; }
            let old = if base == 0 { None } else { self.rows.get(&change.id) };
            if let Some(old) = old { pixels -= u64::from(*old); }
            if change.remove {
                if old.is_none() || change.pixels != 0 { return false; }
                count -= 1;
            } else {
                if old.is_none() {
                    if !change.complete { return false; }
                    count += 1;
                }
                pixels += u64::from(change.pixels);
            }
        }
        if count > 10_000 || pixels > 262_144 { return false; }
        // No mutation precedes the validation pass. Ordinary deltas touch only
        // named IDs; no clone/scan of the retained map or pixel payloads.
        if base == 0 { self.rows.clear(); }
        for change in changes {
            if change.remove { self.rows.remove(&change.id); }
            else { self.rows.insert(change.id, change.pixels); }
        }
        self.revision = revision;
        self.pixels = pixels;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn row(id: u32, pixels: u32) -> ShellStripChange {
        ShellStripChange { id, pixels, remove: false, complete: true }
    }
    #[test]
    fn gap_replay_and_invalid_transaction_leave_base_usable() {
        let mut s = StripState::default();
        assert!(s.apply(0, 1, &[row(1, 4)]));
        assert!(!s.apply(2, 3, &[row(2, 4)]));
        assert!(!s.apply(1, 1, &[]));
        assert!(!s.apply(1, 2, &[row(2, 4), row(2, 0)]));
        assert!(!s.apply(1, 2, &[ShellStripChange { complete: false, ..row(2, 4) }]));
        assert!(s.apply(1, 2, &[row(1, 0)]));
        assert_eq!(s.pixels, 0);
        assert_eq!(s.rows.len(), 1);
        assert!(s.apply(0, 8, &[row(3, 2)]));
        assert!(!s.rows.contains_key(&1));
    }
    #[test]
    fn cumulative_images_are_bounded_across_deltas() {
        let mut s = StripState::default();
        let rows: Vec<_> = (1..=256).map(|id| row(id, 1024)).collect();
        assert!(s.apply(0, 1, &rows));
        assert!(!s.apply(1, 2, &[row(257, 1)]));
        assert!(s.apply(1, 2, &[ShellStripChange { remove: true, ..row(1, 0) }, row(257, 1024)]));
        assert_eq!(s.pixels, 262_144);
        assert!(!s.apply(2, 3, &[ShellStripChange { remove: true, ..row(1, 0) }]));
    }
}