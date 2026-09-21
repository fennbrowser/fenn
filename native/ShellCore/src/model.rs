/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Incremental native presentation state for the tab list.
//!
//! This is *presentation* state, not a browser model: opaque window-local tab
//! ids, their displayed titles, and which one Firefox says is active. Firefox
//! stays authoritative for everything else.
//!
//! The point of the type is what it refuses to do. A single tab's title or the
//! active tab changing is O(1) and yields exactly one change record, with no
//! scan of the other tabs and no rebuilt snapshot. Broad work happens only for
//! initialization, recovery, and genuine structural change, and even then the
//! caller receives a diff rather than "everything again".
//!
//! Ordering between snapshots and deltas is explicit. Every mutation advances
//! `revision`; a delta names the revision it was computed against and is
//! refused with [`ApplyError::Gap`] if that is not the current one. The caller
//! answers a gap with one snapshot, not with a permanent polling loop.

use std::collections::HashMap;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u32)]
pub enum ChangeKind {
    /// `tab`'s title changed. `from`/`to` are its current index.
    Title = 1,
    /// The active tab changed. `tab` is the new active tab (0 for none),
    /// `from` is the previous active tab id, `to` is the new one.
    Selected = 2,
    Inserted = 3,
    Removed = 4,
    Moved = 5,
}

pub const SHELL_NO_INDEX: u32 = u32::MAX;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Change {
    pub kind: ChangeKind,
    pub tab: u32,
    pub from: u32,
    pub to: u32,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ApplyError {
    /// The delta was computed against a different revision than this model
    /// holds. The producer must send a snapshot.
    Gap,
    /// The delta or snapshot is over a documented bound.
    TooLarge,
    /// A delta named a tab this model does not have.
    UnknownTab,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ModelLimits {
    pub max_tabs: usize,
    pub max_title_units: usize,
    /// Bound on accumulated change records. Reaching it means the consumer is
    /// not keeping up; the model then reports [`TabModel::needs_snapshot`] so
    /// recovery is one snapshot rather than an unbounded replay.
    pub max_changes: usize,
}

impl Default for ModelLimits {
    fn default() -> Self {
        Self { max_tabs: 10_000, max_title_units: 32_768, max_changes: 4096 }
    }
}

#[derive(Clone, PartialEq, Eq, Debug)]
pub struct TabEntry {
    pub id: u32,
    pub title: Vec<u16>,
}

pub struct TabModel {
    limits: ModelLimits,
    order: Vec<u32>,
    index: HashMap<u32, usize>,
    titles: HashMap<u32, Vec<u16>>,
    selected: u32,
    /// Advances on every accepted mutation. Deltas carry the revision they were
    /// computed against, which is how a gap is detected.
    revision: u32,
    /// Replaced wholesale on a session change (a new endpoint generation), so
    /// ids from a previous session can never be mistaken for current ones.
    session: u64,
    changes: Vec<Change>,
    needs_snapshot: bool,
}

impl TabModel {
    pub fn new(limits: ModelLimits) -> Self {
        Self {
            limits,
            order: Vec::new(),
            index: HashMap::new(),
            titles: HashMap::new(),
            selected: 0,
            revision: 0,
            session: 1,
            changes: Vec::new(),
            needs_snapshot: false,
        }
    }

    pub fn revision(&self) -> u32 {
        self.revision
    }

    pub fn session(&self) -> u64 {
        self.session
    }

    pub fn len(&self) -> usize {
        self.order.len()
    }

    pub fn is_empty(&self) -> bool {
        self.order.is_empty()
    }

    pub fn selected(&self) -> u32 {
        self.selected
    }

    pub fn contains(&self, tab: u32) -> bool {
        self.index.contains_key(&tab)
    }

    pub fn index_of(&self, tab: u32) -> Option<usize> {
        self.index.get(&tab).copied()
    }

    pub fn title(&self, tab: u32) -> Option<&[u16]> {
        self.titles.get(&tab).map(|value| value.as_slice())
    }

    pub fn order(&self) -> &[u32] {
        &self.order
    }

    /// True once the consumer has fallen too far behind; the producer must
    /// answer with a snapshot. It is not a polling flag: nothing sets it except
    /// an explicit bound being reached.
    pub fn needs_snapshot(&self) -> bool {
        self.needs_snapshot
    }

    pub fn pending_changes(&self) -> &[Change] {
        &self.changes
    }

    pub fn take_changes(&mut self) -> Vec<Change> {
        self.needs_snapshot = false;
        std::mem::take(&mut self.changes)
    }

    /// Apply one tab's new title. O(1): no other tab is visited, and an
    /// unchanged title produces no change record at all.
    pub fn set_title(&mut self, base_revision: u32, tab: u32, title: &[u16]) -> Result<bool, ApplyError> {
        self.check_base(base_revision)?;
        if title.len() > self.limits.max_title_units {
            return Err(ApplyError::TooLarge);
        }
        let Some(index) = self.index.get(&tab).copied() else {
            return Err(ApplyError::UnknownTab);
        };
        let current = self.titles.get_mut(&tab).expect("index and titles are kept together");
        if current.as_slice() == title {
            // Still an accepted delta, so the producer's revision stays in
            // step; it simply changed nothing.
            self.revision = self.revision.wrapping_add(1);
            return Ok(false);
        }
        current.clear();
        current.extend_from_slice(title);
        self.push(Change {
            kind: ChangeKind::Title,
            tab,
            from: index as u32,
            to: index as u32,
        });
        self.revision = self.revision.wrapping_add(1);
        Ok(true)
    }

    /// Apply the active tab. O(1). A tab that is not in the model is refused
    /// rather than silently ignored, so the producer notices a real gap.
    pub fn set_selected(&mut self, base_revision: u32, tab: u32) -> Result<bool, ApplyError> {
        self.check_base(base_revision)?;
        if tab != 0 && !self.index.contains_key(&tab) {
            return Err(ApplyError::UnknownTab);
        }
        if self.selected == tab {
            self.revision = self.revision.wrapping_add(1);
            return Ok(false);
        }
        let previous = self.selected;
        self.selected = tab;
        self.push(Change { kind: ChangeKind::Selected, tab, from: previous, to: tab });
        self.revision = self.revision.wrapping_add(1);
        Ok(true)
    }

    /// Replace the whole structure from an authoritative snapshot and report
    /// the diff. Used for initialization, recovery after a gap, and genuine
    /// structural change (open/close/move/hide). The work is bounded by the
    /// snapshot size and is reported so it can be measured.
    pub fn apply_snapshot(
        &mut self,
        session: u64,
        entries: &[TabEntry],
        selected: u32,
    ) -> Result<usize, ApplyError> {
        if entries.len() > self.limits.max_tabs {
            return Err(ApplyError::TooLarge);
        }
        if entries.iter().any(|entry| entry.title.len() > self.limits.max_title_units) {
            return Err(ApplyError::TooLarge);
        }
        let mut seen = HashMap::with_capacity(entries.len());
        for (position, entry) in entries.iter().enumerate() {
            if entry.id == 0 || seen.insert(entry.id, position).is_some() {
                return Err(ApplyError::UnknownTab);
            }
        }
        if selected != 0 && !seen.contains_key(&selected) {
            return Err(ApplyError::UnknownTab);
        }

        if session != self.session {
            // A replaced session shares no identities with the old one.
            self.order.clear();
            self.index.clear();
            self.titles.clear();
            self.selected = 0;
            self.changes.clear();
            self.session = session;
        }

        let before = self.changes.len();

        // Removals first, so the indices reported for insertions and moves are
        // positions in the resulting order.
        let removed: Vec<u32> =
            self.order.iter().copied().filter(|id| !seen.contains_key(id)).collect();
        for id in removed {
            let at = self.index[&id];
            self.order.remove(at);
            self.titles.remove(&id);
            self.reindex_from(at);
            self.index.remove(&id);
            self.push(Change {
                kind: ChangeKind::Removed,
                tab: id,
                from: at as u32,
                to: SHELL_NO_INDEX,
            });
        }

        for (position, entry) in entries.iter().enumerate() {
            match self.index.get(&entry.id).copied() {
                None => {
                    self.order.insert(position, entry.id);
                    self.titles.insert(entry.id, entry.title.clone());
                    self.reindex_from(position);
                    self.push(Change {
                        kind: ChangeKind::Inserted,
                        tab: entry.id,
                        from: SHELL_NO_INDEX,
                        to: position as u32,
                    });
                }
                Some(at) if at != position => {
                    self.order.remove(at);
                    self.order.insert(position, entry.id);
                    self.reindex_from(at.min(position));
                    self.push(Change {
                        kind: ChangeKind::Moved,
                        tab: entry.id,
                        from: at as u32,
                        to: position as u32,
                    });
                    self.apply_snapshot_title(entry, position);
                }
                Some(position_in_place) => self.apply_snapshot_title(entry, position_in_place),
            }
        }

        if self.selected != selected {
            let previous = self.selected;
            self.selected = selected;
            self.push(Change {
                kind: ChangeKind::Selected,
                tab: selected,
                from: previous,
                to: selected,
            });
        }

        self.revision = self.revision.wrapping_add(1);
        Ok(self.changes.len() - before)
    }

    fn apply_snapshot_title(&mut self, entry: &TabEntry, position: usize) {
        let current = self.titles.get_mut(&entry.id).expect("present in index");
        if current.as_slice() == entry.title.as_slice() {
            return;
        }
        current.clear();
        current.extend_from_slice(&entry.title);
        self.push(Change {
            kind: ChangeKind::Title,
            tab: entry.id,
            from: position as u32,
            to: position as u32,
        });
    }

    fn reindex_from(&mut self, start: usize) {
        for position in start..self.order.len() {
            self.index.insert(self.order[position], position);
        }
    }

    fn check_base(&self, base_revision: u32) -> Result<(), ApplyError> {
        if base_revision == self.revision {
            Ok(())
        } else {
            Err(ApplyError::Gap)
        }
    }

    fn push(&mut self, change: Change) {
        if self.changes.len() >= self.limits.max_changes {
            // Do not grow without bound and do not drop silently: collapse to
            // "resynchronize", which the consumer answers with one snapshot.
            self.changes.clear();
            self.needs_snapshot = true;
            return;
        }
        if self.needs_snapshot {
            return;
        }
        self.changes.push(change);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn utf16(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }

    fn entry(id: u32, title: &str) -> TabEntry {
        TabEntry { id, title: utf16(title) }
    }

    fn seeded(count: u32) -> TabModel {
        let mut model = TabModel::new(ModelLimits::default());
        let entries: Vec<TabEntry> =
            (1..=count).map(|id| entry(id, &format!("tab {id}"))).collect();
        model.apply_snapshot(1, &entries, 1).unwrap();
        model.take_changes();
        model
    }

    #[test]
    fn a_title_change_touches_only_its_own_tab() {
        let mut model = seeded(300);
        let revision = model.revision();
        assert!(model.set_title(revision, 217, &utf16("changed")).unwrap());
        let changes = model.take_changes();
        assert_eq!(changes.len(), 1, "no work for the other 299 tabs");
        assert_eq!(changes[0].kind, ChangeKind::Title);
        assert_eq!(changes[0].tab, 217);
        assert_eq!(changes[0].to, 216, "reported at its current index");
        assert_eq!(model.title(217).unwrap(), utf16("changed").as_slice());
        assert_eq!(model.title(216).unwrap(), utf16("tab 216").as_slice());
    }

    #[test]
    fn an_unchanged_title_produces_no_change_at_all() {
        let mut model = seeded(10);
        let revision = model.revision();
        assert!(!model.set_title(revision, 4, &utf16("tab 4")).unwrap());
        assert!(model.take_changes().is_empty());
        assert_eq!(model.revision(), revision + 1, "the delta was still accepted");
    }

    #[test]
    fn selection_is_one_change_and_names_both_ends() {
        let mut model = seeded(100);
        let revision = model.revision();
        assert!(model.set_selected(revision, 42).unwrap());
        let changes = model.take_changes();
        assert_eq!(changes.len(), 1);
        assert_eq!(changes[0].kind, ChangeKind::Selected);
        assert_eq!((changes[0].from, changes[0].to), (1, 42));
        assert_eq!(model.selected(), 42);
        assert!(!model.set_selected(model.revision(), 42).unwrap(), "no-op selection");
        assert!(model.take_changes().is_empty());
    }

    #[test]
    fn a_delta_against_a_stale_revision_is_a_gap_not_a_guess() {
        let mut model = seeded(10);
        let revision = model.revision();
        model.set_title(revision, 1, &utf16("first")).unwrap();
        assert_eq!(model.set_title(revision, 2, &utf16("second")), Err(ApplyError::Gap));
        assert_eq!(model.title(2).unwrap(), utf16("tab 2").as_slice(), "nothing was applied");
    }

    #[test]
    fn a_delta_for_an_unknown_tab_is_refused() {
        let mut model = seeded(5);
        assert_eq!(
            model.set_title(model.revision(), 99, &utf16("x")),
            Err(ApplyError::UnknownTab)
        );
        assert_eq!(model.set_selected(model.revision(), 99), Err(ApplyError::UnknownTab));
    }

    #[test]
    fn a_snapshot_reports_only_what_actually_differs() {
        let mut model = seeded(4);
        let entries = vec![entry(1, "tab 1"), entry(2, "renamed"), entry(3, "tab 3"), entry(4, "tab 4")];
        let count = model.apply_snapshot(1, &entries, 1).unwrap();
        assert_eq!(count, 1);
        let changes = model.take_changes();
        assert_eq!(changes[0].kind, ChangeKind::Title);
        assert_eq!(changes[0].tab, 2);
    }

    #[test]
    fn structural_changes_are_reported_as_a_diff() {
        let mut model = seeded(3);
        // Close 2, open 9 at the front, move 3 ahead of 1.
        let entries = vec![entry(9, "new"), entry(3, "tab 3"), entry(1, "tab 1")];
        model.apply_snapshot(1, &entries, 3).unwrap();
        let changes = model.take_changes();
        let kinds: Vec<ChangeKind> = changes.iter().map(|change| change.kind).collect();
        assert!(kinds.contains(&ChangeKind::Removed));
        assert!(kinds.contains(&ChangeKind::Inserted));
        assert!(kinds.contains(&ChangeKind::Selected));
        assert_eq!(model.order(), &[9, 3, 1]);
        assert_eq!(model.index_of(1), Some(2));
        assert_eq!(model.selected(), 3);
        assert!(!model.contains(2));
        // The diff must leave the model exactly where a fresh one would be.
        let mut fresh = TabModel::new(ModelLimits::default());
        fresh.apply_snapshot(1, &entries, 3).unwrap();
        assert_eq!(fresh.order(), model.order());
        assert_eq!(fresh.title(3), model.title(3));
    }

    #[test]
    fn indices_stay_correct_after_a_move_so_later_deltas_are_right() {
        let mut model = seeded(5);
        let entries: Vec<TabEntry> =
            [5u32, 1, 2, 3, 4].iter().map(|id| entry(*id, &format!("tab {id}"))).collect();
        model.apply_snapshot(1, &entries, 1).unwrap();
        model.take_changes();
        assert!(model.set_title(model.revision(), 5, &utf16("moved")).unwrap());
        let changes = model.take_changes();
        assert_eq!(changes[0].to, 0);
    }

    #[test]
    fn a_replaced_session_drops_the_previous_identities() {
        let mut model = seeded(3);
        model.apply_snapshot(2, &[entry(7, "only")], 7).unwrap();
        assert_eq!(model.session(), 2);
        assert_eq!(model.order(), &[7]);
        assert!(!model.contains(1));
        let changes = model.take_changes();
        assert!(changes.iter().all(|change| change.kind != ChangeKind::Removed),
            "a session replacement is not a stream of removals");
    }

    #[test]
    fn snapshots_are_bounded_and_validated() {
        let mut model = TabModel::new(ModelLimits { max_tabs: 2, ..ModelLimits::default() });
        assert_eq!(
            model.apply_snapshot(1, &[entry(1, "a"), entry(2, "b"), entry(3, "c")], 0),
            Err(ApplyError::TooLarge)
        );
        assert_eq!(
            model.apply_snapshot(1, &[entry(1, "a"), entry(1, "b")], 0),
            Err(ApplyError::UnknownTab),
            "duplicate ids are not a valid snapshot"
        );
        assert_eq!(
            model.apply_snapshot(1, &[entry(1, "a")], 9),
            Err(ApplyError::UnknownTab),
            "the active tab must be in the snapshot"
        );
    }

    #[test]
    fn unread_changes_collapse_to_a_resynchronization_request() {
        let mut model = TabModel::new(ModelLimits { max_changes: 4, ..ModelLimits::default() });
        let entries: Vec<TabEntry> = (1..=8).map(|id| entry(id, "t")).collect();
        model.apply_snapshot(1, &entries, 1).unwrap();
        for id in 1..=8u32 {
            let revision = model.revision();
            let _ = model.set_title(revision, id, &utf16("changed"));
        }
        assert!(model.needs_snapshot(), "the consumer is behind; ask for one snapshot");
        assert!(model.pending_changes().is_empty(), "not an unbounded replay");
        model.take_changes();
        assert!(!model.needs_snapshot());
    }

    #[test]
    fn titles_over_the_bound_are_refused_rather_than_truncated_silently() {
        let mut model = TabModel::new(ModelLimits { max_title_units: 8, ..ModelLimits::default() });
        model.apply_snapshot(1, &[entry(1, "short")], 1).unwrap();
        assert_eq!(
            model.set_title(model.revision(), 1, &utf16("far too long")),
            Err(ApplyError::TooLarge)
        );
        assert_eq!(model.title(1).unwrap(), utf16("short").as_slice());
    }
}
