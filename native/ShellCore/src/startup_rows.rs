//! Bounded presentation identities while local drafts and browser rows coexist.
//! This projects live state; it never adopts, closes or rewrites a browser tab.
use crate::{
    publication::{Stream, StreamKind},
    startup::Startup,
    startup_handoff::Handoff,
};
use std::collections::{BTreeMap, BTreeSet};
pub const STARTUP_BROWSER_ROWS: usize = 4096;
pub const STARTUP_PRESENTATION_ROWS: usize =
    STARTUP_BROWSER_ROWS + crate::startup::STARTUP_MAX_TABS;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ShellStartupRow {
    pub presentation: u32,
    pub draft: u64,
    pub browser: u64,
    pub selected: bool,
}
pub struct Rows {
    stream: Stream<u64>,
    browsers: Vec<u64>,
    selected: u64,
    drafts: BTreeMap<u64, u32>,
    aliases: BTreeMap<u64, u32>,
    next: u32,
    startup_order: Vec<u32>,
}
impl Default for Rows {
    fn default() -> Self {
        Self {
            stream: Stream::new(StreamKind::Snapshot),
            browsers: vec![],
            selected: 0,
            drafts: BTreeMap::new(),
            aliases: BTreeMap::new(),
            next: 0,
            startup_order: vec![],
        }
    }
}
impl Rows {
    pub fn selected_browser(&self) -> u64 {
        self.selected
    }
    pub fn contains_browser(&self, browser: u64) -> bool {
        self.browsers.contains(&browser)
    }
    /// Revision zero reprojects local changes over the last accepted browser
    /// snapshot. A nonzero revision publishes only authoritative visible IDs.
    pub fn update(
        &mut self,
        s: &Startup,
        h: &Handoff,
        revision: u64,
        ids: &[u64],
        selected: u64,
    ) -> Result<Vec<ShellStartupRow>, ()> {
        if ids.len() > STARTUP_BROWSER_ROWS
            || ids.iter().any(|id| *id == 0)
            || ids.iter().copied().collect::<BTreeSet<_>>().len() != ids.len()
            || (ids.is_empty() && selected != 0)
            || (!ids.is_empty() && !ids.contains(&selected))
            || (revision == 0 && (!ids.is_empty() || selected != 0))
        {
            return Err(());
        }
        let fingerprint: Vec<u16> = std::iter::once(selected)
            .chain(ids.iter().copied())
            .flat_map(|id| (0..4).map(move |i| (id >> (i * 16)) as u16))
            .collect();
        let apply = revision != 0 && self.stream.check(0, revision, &fingerprint)?;
        let browsers = if apply { ids } else { &self.browsers };
        let browser_selected = if apply { selected } else { self.selected };
        let mut drafts = self.drafts.clone();
        let mut aliases = self.aliases.clone();
        let mut next = self.next;
        let mut allocate = |map: &mut BTreeMap<u64, u32>, id: u64| -> Result<u32, ()> {
            if let Some(value) = map.get(&id) {
                return Ok(*value);
            }
            next = next.checked_add(1).ok_or(())?;
            map.insert(id, next);
            Ok(next)
        };
        drafts.retain(|id, _| s.tabs.iter().any(|tab| tab.id == *id));
        let mut local = Vec::new();
        let mut represented = BTreeSet::new();
        for tab in &s.tabs {
            let presentation = allocate(&mut drafts, tab.id)?;
            let browser = h.mappings.get(&tab.id).copied().unwrap_or(0);
            if browser != 0 {
                represented.insert(browser);
                aliases.insert(browser, presentation);
            }
            local.push(ShellStartupRow {
                presentation,
                draft: tab.id,
                browser,
                selected: s.selected == tab.id,
            });
        }
        // An adopted background row is still the same visible startup slot.
        // Do not move it ahead of the remaining local drafts merely because its
        // editor became browser-owned. Orphans awaiting Close are not adopted.
        let mut retired = Vec::new();
        let mut rows = Vec::new();
        // An untouched automatic placeholder is not a second tab. Keep the
        // accepted browser snapshot private until its explicit retirement;
        // editing/creating a draft immediately restores the normal mixed view.
        // This is presentation only: no browser ID is guessed or adopted here.
        let placeholder_only = s.selected == 1
            && h.editor_browser == 0
            && s.tabs.len() == 1
            && s.tabs[0].id == 1
            && !s.tabs[0].touched
            && s.tabs[0].revision == 0
            && s.tabs[0].text.is_empty()
            && s.submissions.is_empty()
            && h.mappings.is_empty();
        for browser in browsers {
            if placeholder_only || represented.contains(browser) {
                continue;
            }
            let row = ShellStartupRow {
                presentation: allocate(&mut aliases, *browser)?,
                browser: *browser,
                draft: 0,
                selected: s.selected == 0 && *browser == browser_selected,
            };
            if self.startup_order.contains(&row.presentation)
                && !h.mappings.values().any(|id| id == browser)
            {
                retired.push(row);
            } else {
                rows.push(row);
            }
        }
        let live_ids = local
            .iter()
            .map(|row| row.presentation)
            .collect::<BTreeSet<_>>();
        let retained_ids = retired
            .iter()
            .map(|row| row.presentation)
            .collect::<BTreeSet<_>>();
        let mut order = self
            .startup_order
            .iter()
            .copied()
            .filter(|id| live_ids.contains(id) || retained_ids.contains(id))
            .collect::<Vec<_>>();
        for row in &local {
            if !order.contains(&row.presentation) {
                order.push(row.presentation);
            }
        }
        // Respect a deliberate local reorder without moving already adopted slots.
        let mut live = local.iter();
        for id in &mut order {
            if live_ids.contains(id) {
                *id = live.next().ok_or(())?.presentation;
            }
        }
        let projected = local
            .into_iter()
            .chain(retired)
            .map(|row| (row.presentation, row))
            .collect::<BTreeMap<_, _>>();
        for id in &order {
            rows.push(*projected.get(id).ok_or(())?);
        }

        aliases.retain(|id, _| browsers.contains(id) || represented.contains(id));
        // Commit only after validation/allocation succeeded. A mapping that
        // arrives after its browser row always keeps the draft's native row.
        self.drafts = drafts;
        self.aliases = aliases;
        self.next = next;
        self.startup_order = order;
        if apply {
            self.browsers = ids.to_vec();
            self.selected = selected;
            self.stream.commit(revision, &fingerprint);
        }
        Ok(rows)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> (Startup, Handoff, Rows) {
        let mut s = Startup::default();
        s.add();
        s.edit(1, &[65]);
        let mut h = Handoff::default();
        h.prepare(42);
        h.bind(42, 7);
        (s, h, Rows::default())
    }
    #[test]
    fn background_adoption_keeps_row_order_until_normal_presentation() {
        let (mut s, mut h, mut r) = fixture();
        s.add();
        s.edit(2, &[66]);
        s.add();
        s.edit(3, &[67]);
        s.select(1);
        for id in 1..=3 {
            h.create(&s, id);
            h.complete(&mut s, 7, id, 90 + id, true);
        }
        let original = r.update(&s, &h, 1, &[90, 91, 92, 93], 90).unwrap();
        let ids = original
            .iter()
            .map(|row| row.presentation)
            .collect::<Vec<_>>();
        h.enable_adoption();
        h.next(&s);
        h.complete(&mut s, 7, 4, 92, true);
        let partial = r.update(&s, &h, 0, &[], 0).unwrap();
        assert_eq!(
            partial
                .iter()
                .map(|row| row.presentation)
                .collect::<Vec<_>>(),
            ids
        );
        assert_eq!(partial[2].draft, 0);
        assert_eq!(partial[1].draft, 1);
        assert!(partial[1].selected);
        h.next(&s);
        h.complete(&mut s, 7, 5, 93, true);
        assert_eq!(
            r.update(&s, &h, 0, &[], 0)
                .unwrap()
                .iter()
                .map(|row| row.presentation)
                .collect::<Vec<_>>(),
            ids
        );
        h.next(&s);
        h.complete(&mut s, 7, 6, 91, true);
        h.commit_adoption(&mut s, 91);
        let complete = r.update(&s, &h, 2, &[90, 91, 92, 93], 91).unwrap();
        assert_eq!(
            complete
                .iter()
                .map(|row| row.presentation)
                .collect::<Vec<_>>(),
            ids
        );
        assert!(complete.iter().all(|row| row.draft == 0));
        assert!(complete[1].selected);
    }
    #[test]
    fn mapping_keeps_draft_row_and_removes_duplicate_browser_row() {
        let (mut s, mut h, mut r) = fixture();
        let local = r.update(&s, &h, 0, &[], 0).unwrap()[0].presentation;
        let restored = r.update(&s, &h, 1, &[90, 91], 90).unwrap();
        let original = restored[0].presentation;
        h.create(&s, 1);
        s.editor(1, &[66, 67], 1, 1, true);
        h.complete(&mut s, 7, 1, 91, true);
        let merged = r.update(&s, &h, 0, &[], 0).unwrap();
        assert_eq!(merged.len(), 2);
        assert_eq!(merged[0].presentation, original);
        assert_eq!(
            merged[1],
            ShellStartupRow {
                presentation: local,
                draft: 1,
                browser: 91,
                selected: true
            }
        );
        assert_eq!(s.tabs[0].text, [66, 67]);
        assert!(s.tabs[0].composing);
        assert_eq!(s.tabs[0].selection_length, 1);
        let moved = r.update(&s, &h, 2, &[92, 91, 90], 92).unwrap();
        assert_eq!(moved.last().unwrap().presentation, local);
        assert_eq!(moved[1].presentation, original);
    }
    #[test]
    fn invalid_conflicting_and_stale_snapshots_cannot_rewrite_rows() {
        let (s, h, mut r) = fixture();
        let rows = r.update(&s, &h, 5, &[90], 90).unwrap();
        assert!(r.update(&s, &h, 5, &[91], 91).is_err());
        assert!(r.update(&s, &h, 6, &[90, 90], 90).is_err());
        assert!(r.update(&s, &h, 6, &[91], 90).is_err());
        assert_eq!(r.update(&s, &h, 4, &[91], 91).unwrap(), rows);
        assert_eq!(r.update(&s, &h, 5, &[90], 90).unwrap(), rows);
    }
    #[test]
    fn orphan_remains_browser_owned_and_removed_identity_is_never_reused() {
        let (mut s, mut h, mut r) = fixture();
        h.create(&s, 1);
        h.complete(&mut s, 7, 1, 90, true);
        let before = r.update(&s, &h, 1, &[90], 90).unwrap()[0].presentation;
        s.close(1);
        let orphan = r.update(&s, &h, 0, &[], 0).unwrap();
        assert_eq!(orphan[0].presentation, before);
        assert_eq!(orphan[0].draft, 0);
        assert!(orphan[0].selected);
        assert!(r.update(&s, &h, 2, &[], 0).unwrap().is_empty());
        assert_ne!(
            r.update(&s, &h, 3, &[90], 90).unwrap()[0].presentation,
            before
        );
    }
    #[test]
    fn untouched_initial_draft_is_not_adopted_and_capacity_rejection_is_atomic() {
        let (_, mut h, mut r) = fixture();
        let mut s = Startup::default();
        s.add();
        let before = r.update(&s, &h, 1, &[90], 90).unwrap();
        assert_eq!(before.len(), 1);
        assert!(before[0].selected);
        assert_eq!(before[0].browser, 0);
        let huge: Vec<_> = (1..=STARTUP_BROWSER_ROWS as u64 + 1).collect();
        assert!(r.update(&s, &h, 2, &huge, 1).is_err());
        assert_eq!(r.update(&s, &h, 0, &[], 0).unwrap(), before);
        assert!(h.adopt_placeholder(&mut s, 90));
        let after = r.update(&s, &h, 0, &[], 0).unwrap();
        assert_eq!(after.len(), 1);
        assert_eq!(after[0].browser, 90);
        assert!(after[0].selected);
    }
    #[test]
    fn waiting_placeholder_preserves_snapshot_and_never_hides_user_drafts() {
        let (_, h, mut r) = fixture();
        let mut s = Startup::default();
        s.add();
        let loading = r.update(&s, &h, 1, &[90, 91], 91).unwrap();
        assert_eq!(loading.len(), 1);
        assert!(s.park_editor());
        let browser_selected = r.update(&s, &h, 0, &[], 0).unwrap();
        assert_eq!(browser_selected.len(), 3);
        assert!(browser_selected[1].selected);
        // Set the model back to its untouched fixture, without simulating an
        // explicit user return (which intentionally marks the draft touched).
        s.selected = 1;
        s.editor(1, &[], 0, 0, true);
        assert_eq!(r.update(&s, &h, 0, &[], 0).unwrap(), loading);
        s.editor(1, &[65], 1, 0, true);
        let edited = r.update(&s, &h, 0, &[], 0).unwrap();
        assert_eq!(edited.len(), 3);
        assert_eq!(edited[0].browser, 90);
        assert_eq!(edited[1].browser, 91);
        assert_eq!(edited[2].presentation, loading[0].presentation);
        assert!(edited[2].selected);
        assert!(s.tabs[0].composing);
        assert_eq!(s.tabs[0].text, [65]);
        s.editor(1, &[], 0, 0, false);
        assert_eq!(r.update(&s, &h, 0, &[], 0).unwrap().len(), 3); // touched stays intentional
        let mut s = Startup::default();
        s.add();
        s.add();
        assert_eq!(r.update(&s, &h, 0, &[], 0).unwrap().len(), 4);
    }
}
