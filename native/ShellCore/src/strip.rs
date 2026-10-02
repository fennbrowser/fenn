/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Strip publication identity and retained resource bounds. WinUI owns the
//! projection (hstrings, images and controls); this owns atomic delta acceptance
//! and the publication envelope: version, base/revision, exact-replay identity
//! and the strip options a delta keeps by omission.
use std::collections::{HashMap, HashSet};

/// A 1 MiB UTF-16 publication re-serialized with every unit escaped (`\uXXXX`).
pub(crate) const MAX_FINGERPRINT: usize = 6 * 1024 * 1024;
/// Matches `Number.MAX_SAFE_INTEGER`, the bound Firefox enforces on revisions.
const MAX_SAFE_INTEGER: f64 = 9_007_199_254_740_991.0;
const MAX_GROUPING_DELAY: f64 = 60_000.0;

#[derive(Default)]
pub struct StripState {
    revision: u32,
    rows: HashMap<u32, u32>,
    pixels: u64,
    layout: Vec<ShellStripSlot>,
    /// Exact serialized form of the accepted revision: a replay must match it.
    fingerprint: Vec<u16>,
    options: ShellStripOptions,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum ShellBackdrop {
    Mica = 0,
    MicaAlt = 1,
    Acrylic = 2,
}

/// The resolved publication envelope. A full snapshot (base zero) takes the
/// defaults for omitted options; a delta keeps the accepted values.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct ShellStripOptions {
    pub base: u32,
    pub revision: u32,
    /// Zero when Firefox has no window drag session for this strip.
    pub window_drag_session: u32,
    pub grouping_delay: u32,
    pub backdrop: ShellBackdrop,
    /// DIP that group surfaces reach past their outer members.
    pub group_reach: f32,
    pub window_drag_private: bool,
    pub group_glow: bool,
    pub container_glow: bool,
    pub container_dot: bool,
    pub group_hover: bool,
    pub group_halo: bool,
    pub group_glow_animations: bool,
    pub group_glow_fade_out: bool,
    pub rtl: bool,
}

pub(crate) const LARGE_GROUP_REACH: f32 = 5.0;

impl Default for ShellStripOptions {
    fn default() -> Self {
        ShellStripOptions {
            base: 0,
            revision: 0,
            window_drag_session: 0,
            grouping_delay: 240,
            backdrop: ShellBackdrop::MicaAlt,
            group_reach: LARGE_GROUP_REACH,
            window_drag_private: false,
            group_glow: false,
            container_glow: false,
            container_dot: false,
            group_hover: false,
            group_halo: false,
            group_glow_animations: false,
            group_glow_fade_out: false,
            rtl: false,
        }
    }
}

/// Envelope fields as read from the publication, before any validation.
/// Numbers are the raw JSON values; `None` means the key was absent.
#[derive(Clone, Copy, Default)]
pub struct Envelope<'a> {
    pub version: f64,
    pub base: Option<f64>,
    pub revision: f64,
    pub window_drag_session: Option<f64>,
    pub window_drag_private: Option<bool>,
    pub grouping_delay: Option<f64>,
    pub backdrop: Option<&'a [u16]>,
    pub group_reach: Option<&'a [u16]>,
    pub group_glow: Option<bool>,
    pub container_glow: Option<bool>,
    pub container_dot: Option<bool>,
    pub group_hover: Option<bool>,
    pub group_halo: Option<bool>,
    pub group_glow_animations: Option<bool>,
    pub group_glow_fade_out: Option<bool>,
    pub rtl: Option<bool>,
}

#[derive(Debug, PartialEq)]
pub enum Resolution {
    /// The accepted revision again, byte for byte: nothing to apply.
    Unchanged,
    Accepted(ShellStripOptions),
}

fn integer(value: f64, min: f64, max: f64) -> Option<f64> {
    (value.is_finite() && value >= min && value <= max && value.floor() == value).then_some(value)
}

fn id(value: f64) -> Option<u32> {
    integer(value, 1.0, f64::from(u32::MAX)).map(|value| value as u32)
}

fn named(value: &[u16], name: &str) -> bool {
    value.iter().copied().eq(name.encode_utf16())
}

fn backdrop(name: &[u16]) -> Option<ShellBackdrop> {
    if named(name, "mica") { Some(ShellBackdrop::Mica) }
    else if named(name, "mica-alt") { Some(ShellBackdrop::MicaAlt) }
    else if named(name, "acrylic") { Some(ShellBackdrop::Acrylic) }
    else { None }
}

fn group_reach(name: &[u16]) -> Option<f32> {
    if named(name, "none") { Some(0.0) }
    else if named(name, "small") { Some(2.0) }
    else if named(name, "medium") { Some(3.0) }
    else if named(name, "large") { Some(LARGE_GROUP_REACH) }
    else { None }
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

/// Ordered group anchors (tab == 0) and visible members. Group zero is ungrouped.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct ShellStripSlot {
    pub group: u32,
    pub tab: u32,
}

impl StripState {
    /// Validate the envelope and resolve its options against the accepted
    /// publication. Mutates nothing: [`StripState::publish`] commits.
    pub fn resolve(&self, envelope: &Envelope, fingerprint: &[u16]) -> Result<Resolution, ()> {
        let e = envelope;
        if fingerprint.len() > MAX_FINGERPRINT { return Err(()); }
        let base = if e.version == 1.0 { 0 } else if e.version == 2.0 {
            let base = integer(e.base.ok_or(())?, 0.0, MAX_SAFE_INTEGER).ok_or(())?;
            if base > f64::from(u32::MAX) { return Err(()); }
            base as u32
        } else { return Err(()); };
        let revision = id(e.revision).ok_or(())?;
        // A revision is accepted once; a replay must be that publication exactly.
        if revision < self.revision || (revision == self.revision && fingerprint != self.fingerprint.as_slice()) {
            return Err(());
        }
        if revision == self.revision { return Ok(Resolution::Unchanged); }
        if base != 0 && base != self.revision { return Err(()); }
        // A full snapshot must state its direction; everything else defaults.
        if base == 0 && e.rtl.is_none() { return Err(()); }
        let previous = if base == 0 { ShellStripOptions::default() } else { self.options };
        let delay = e.grouping_delay.unwrap_or(f64::from(previous.grouping_delay));
        let grouping_delay = integer(delay, 0.0, MAX_GROUPING_DELAY).ok_or(())? as u32;
        let window_drag_session = match e.window_drag_session {
            Some(value) => id(value).ok_or(())?,
            None => previous.window_drag_session,
        };
        let backdrop = match e.backdrop {
            Some(name) => backdrop(name).ok_or(())?,
            None => previous.backdrop,
        };
        let group_reach = match e.group_reach {
            Some(name) => group_reach(name).ok_or(())?,
            None => previous.group_reach,
        };
        Ok(Resolution::Accepted(ShellStripOptions {
            base,
            revision,
            window_drag_session,
            grouping_delay,
            backdrop,
            group_reach,
            window_drag_private: e.window_drag_private.unwrap_or(previous.window_drag_private),
            group_glow: e.group_glow.unwrap_or(previous.group_glow),
            container_glow: e.container_glow.unwrap_or(previous.container_glow),
            container_dot: e.container_dot.unwrap_or(previous.container_dot),
            group_hover: e.group_hover.unwrap_or(previous.group_hover),
            group_halo: e.group_halo.unwrap_or(previous.group_halo),
            group_glow_animations: e.group_glow_animations.unwrap_or(previous.group_glow_animations),
            group_glow_fade_out: e.group_glow_fade_out.unwrap_or(previous.group_glow_fade_out),
            rtl: e.rtl.unwrap_or(previous.rtl),
        }))
    }

    /// Accept a new publication atomically: envelope, rows and optional layout
    /// all validate before the revision, options or fingerprint change.
    pub fn publish(&mut self, envelope: &Envelope, fingerprint: &[u16], changes: &[ShellStripChange],
                   layout: Option<&[ShellStripSlot]>) -> Option<ShellStripOptions> {
        let Ok(Resolution::Accepted(options)) = self.resolve(envelope, fingerprint) else { return None; };
        if !self.apply_layout(options.base, options.revision, changes, layout) { return None; }
        self.options = options;
        self.fingerprint.clear();
        self.fingerprint.extend_from_slice(fingerprint);
        Some(options)
    }

    pub fn apply_layout(&mut self, base: u32, revision: u32, changes: &[ShellStripChange],
                        layout: Option<&[ShellStripSlot]>) -> bool {
        if let Some(slots) = layout {
            if slots.len() > 20_000 { return false; }
            let mut tabs: HashSet<u32> = if base == 0 { HashSet::new() } else { self.rows.keys().copied().collect() };
            for c in changes {
                if c.remove { tabs.remove(&c.id); } else { tabs.insert(c.id); }
            }
            let mut groups = HashSet::new();
            let mut current = 0;
            for slot in slots {
                if slot.tab == 0 {
                    if slot.group == 0 || !groups.insert(slot.group) { return false; }
                    current = slot.group;
                } else {
                    if !tabs.remove(&slot.tab) { return false; }
                    if slot.group == 0 { current = 0; }
                    else if slot.group != current { return false; }
                }
            }
            if !tabs.is_empty() { return false; }
        } else if base != 0 && !self.layout.is_empty() && changes.iter().any(|c| c.remove || !self.rows.contains_key(&c.id)) {
            // A membership change must carry the matching authoritative order.
            return false;
        }
        if !self.apply(base, revision, changes) { return false; }
        if let Some(slots) = layout { self.layout = slots.to_vec(); }
        else if base == 0 { self.layout.clear(); }
        true
    }

    pub fn layout(&self) -> &[ShellStripSlot] { &self.layout }

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
    fn text(value: &str) -> Vec<u16> { value.encode_utf16().collect() }
    fn full(revision: f64) -> Envelope<'static> {
        Envelope { version: 1.0, revision, rtl: Some(false), ..Default::default() }
    }
    fn delta(base: f64, revision: f64) -> Envelope<'static> {
        Envelope { version: 2.0, base: Some(base), revision, ..Default::default() }
    }

    #[test]
    fn full_snapshot_takes_defaults_and_must_state_direction() {
        let mut s = StripState::default();
        assert_eq!(s.resolve(&Envelope { rtl: None, ..full(1.0) }, &[]), Err(()));
        // Version 2 with base zero is a full snapshot too.
        assert_eq!(s.resolve(&Envelope { rtl: None, ..delta(0.0, 1.0) }, &[]), Err(()));
        let options = s.publish(&full(1.0), &text("a"), &[row(1, 0)], None).unwrap();
        assert_eq!(options, ShellStripOptions { revision: 1, ..Default::default() });
        assert_eq!(options.grouping_delay, 240);
        assert_eq!(options.backdrop, ShellBackdrop::MicaAlt);
        assert_eq!(options.group_reach, LARGE_GROUP_REACH);
    }

    #[test]
    fn delta_keeps_omitted_options_and_full_snapshot_resets_them() {
        let mut s = StripState::default();
        let acrylic = text("acrylic");
        let small = text("small");
        let first = Envelope {
            window_drag_session: Some(9.0), window_drag_private: Some(true), grouping_delay: Some(0.0),
            backdrop: Some(&acrylic), group_reach: Some(&small), group_glow: Some(true),
            container_glow: Some(true), container_dot: Some(true), group_hover: Some(true),
            group_halo: Some(true), group_glow_animations: Some(true), group_glow_fade_out: Some(true),
            rtl: Some(true), ..full(1.0)
        };
        let stated = s.publish(&first, &text("1"), &[row(1, 0)], None).unwrap();
        let kept = s.publish(&delta(1.0, 2.0), &text("2"), &[], None).unwrap();
        assert_eq!(kept, ShellStripOptions { base: 1, revision: 2, ..stated });
        assert_eq!((kept.backdrop, kept.group_reach, kept.grouping_delay), (ShellBackdrop::Acrylic, 2.0, 0));
        let changed = s.publish(&Envelope { group_glow: Some(false), ..delta(2.0, 3.0) }, &text("3"), &[], None).unwrap();
        assert!(!changed.group_glow && changed.group_halo && changed.rtl);
        let reset = s.publish(&full(4.0), &text("4"), &[row(1, 0)], None).unwrap();
        assert_eq!(reset, ShellStripOptions { revision: 4, ..Default::default() });
    }

    #[test]
    fn replay_must_match_exactly_and_revisions_only_advance() {
        let mut s = StripState::default();
        s.publish(&full(5.0), &text("five"), &[row(1, 0)], None).unwrap();
        assert_eq!(s.resolve(&full(5.0), &text("five")), Ok(Resolution::Unchanged));
        // A replay is identified before its envelope is reconsidered.
        assert_eq!(s.resolve(&Envelope { rtl: None, ..full(5.0) }, &text("five")), Ok(Resolution::Unchanged));
        assert_eq!(s.resolve(&full(5.0), &text("other")), Err(()));
        assert_eq!(s.resolve(&full(4.0), &text("four")), Err(()));
        assert_eq!(s.resolve(&delta(4.0, 6.0), &[]), Err(()));
        assert!(matches!(s.resolve(&delta(5.0, 6.0), &[]), Ok(Resolution::Accepted(_))));
        // Publishing a replay is not a new acceptance.
        assert!(s.publish(&full(5.0), &text("five"), &[row(1, 0)], None).is_none());
    }

    #[test]
    fn malformed_envelopes_are_rejected() {
        let s = StripState::default();
        let bad = [text("Mica"), text(""), text("mica ")];
        for version in [0.0, 3.0, 1.5, f64::NAN] {
            assert_eq!(s.resolve(&Envelope { version, ..full(1.0) }, &[]), Err(()));
        }
        for base in [None, Some(-1.0), Some(0.5), Some(4_294_967_296.0), Some(f64::INFINITY)] {
            assert_eq!(s.resolve(&Envelope { base, rtl: Some(false), ..delta(0.0, 1.0) }, &[]), Err(()));
        }
        for revision in [0.0, -1.0, 1.5, 4_294_967_296.0, f64::NAN] {
            assert_eq!(s.resolve(&full(revision), &[]), Err(()));
        }
        for delay in [-1.0, 0.5, 60_001.0, f64::NAN] {
            assert_eq!(s.resolve(&Envelope { grouping_delay: Some(delay), ..full(1.0) }, &[]), Err(()));
        }
        assert!(s.resolve(&Envelope { grouping_delay: Some(60_000.0), ..full(1.0) }, &[]).is_ok());
        for session in [0.0, 1.5, 4_294_967_296.0] {
            assert_eq!(s.resolve(&Envelope { window_drag_session: Some(session), ..full(1.0) }, &[]), Err(()));
        }
        for name in &bad {
            assert_eq!(s.resolve(&Envelope { backdrop: Some(name), ..full(1.0) }, &[]), Err(()));
            assert_eq!(s.resolve(&Envelope { group_reach: Some(name), ..full(1.0) }, &[]), Err(()));
        }
        // Version 1 ignores a stray base; it is always a full snapshot.
        assert!(matches!(s.resolve(&Envelope { base: Some(7.0), ..full(1.0) }, &[]),
                         Ok(Resolution::Accepted(ShellStripOptions { base: 0, .. }))));
        assert_eq!(s.resolve(&full(1.0), &vec![0; MAX_FINGERPRINT + 1]), Err(()));
    }

    #[test]
    fn rejected_rows_leave_envelope_state_untouched() {
        let mut s = StripState::default();
        let halo = s.publish(&Envelope { group_halo: Some(true), ..full(1.0) }, &text("1"), &[row(1, 0)], None).unwrap();
        // Unknown row patched without its required fields.
        let partial = ShellStripChange { complete: false, ..row(2, 0) };
        assert!(s.publish(&Envelope { group_halo: Some(false), ..delta(1.0, 2.0) }, &text("2"), &[partial], None).is_none());
        assert_eq!(s.resolve(&delta(1.0, 2.0), &text("2")), Ok(Resolution::Accepted(ShellStripOptions { base: 1, revision: 2, ..halo })));
        assert_eq!(s.resolve(&full(1.0), &text("1")), Ok(Resolution::Unchanged));
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
    fn group_layout_is_atomic_complete_and_retains_empty_anchors() {
        let mut s = StripState::default();
        let expanded = [ShellStripSlot { group: 0, tab: 1 }, ShellStripSlot { group: 7, tab: 0 },
                        ShellStripSlot { group: 7, tab: 2 }];
        assert!(s.apply_layout(0, 1, &[row(1, 0), row(2, 0)], Some(&expanded)));
        let remove = ShellStripChange { remove: true, ..row(2, 0) };
        assert!(!s.apply_layout(1, 2, &[remove], None));
        assert!(!s.apply_layout(1, 2, &[remove], Some(&expanded)));
        assert_eq!(s.layout(), &expanded);
        assert!(s.apply_layout(1, 2, &[remove], Some(&expanded[..2])));
        assert_eq!(s.layout()[1].tab, 0);
        assert!(s.apply_layout(2, 3, &[row(1, 4)], None));
        assert_eq!(s.layout(), &expanded[..2]);
        // A tab cannot refer to an absent anchor, repeat, or re-enter a group
        // after an ungrouped run. None may consume the pending revision.
        for bad in [vec![ShellStripSlot { group: 8, tab: 1 }],
                    vec![expanded[0], expanded[0]],
                    vec![expanded[1], expanded[0], expanded[2]]] {
            assert!(!s.apply_layout(3, 4, &[], Some(&bad)));
        }
        assert!(s.apply_layout(3, 4, &[row(2, 0)], Some(&expanded)));
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