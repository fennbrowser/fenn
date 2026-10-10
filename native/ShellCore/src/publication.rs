/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Shared acceptance semantics for Firefox presentation publications.
//!
//! This owns the *envelope* rules every native surface had been restating on
//! its own: which revision a publication claims, whether it repeats, conflicts
//! with, lags or skips the accepted one, and what each answer means. It holds
//! no surface content: callers validate their own bodies and keep their own
//! projections. There is deliberately no store shared between streams, so this
//! cannot grow into a second copy of Firefox's model.
//!
//! See the presentation protocol.

/// How a stream is published. The kind fixes the policy for a late revision;
/// classification is the same for all three.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum StreamKind {
    /// Base/revision deltas with occasional complete publications. A late
    /// revision means the producer has diverged, so it is refused and answered
    /// with one complete publication. The default, as the strictest policy.
    #[default]
    Delta,
    /// Each publication is complete and the newest wins. A late one is
    /// harmless and ignored.
    Snapshot,
    /// Complete publications scoped to one opening (a menu, a flyout). Late
    /// answers to a superseded revision are expected and ignored; the caller
    /// checks the opening itself before classifying.
    Session,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Classification {
    /// A newer revision, applicable to what this stream holds.
    Accepted,
    /// The accepted revision again, byte for byte. Nothing to apply.
    Unchanged,
    /// Older than the accepted revision.
    Stale,
    /// The accepted revision with different content: a producer error.
    Conflict,
    /// A delta computed against a base this stream does not hold.
    Gap,
    /// Not a publication at all (revision zero, or a base on a stream that
    /// takes none).
    Invalid,
}

impl Classification {
    /// The shared answer: `Ok(true)` apply, `Ok(false)` nothing to do,
    /// `Err(())` refuse without mutating. A refused delta stream is recovered
    /// by the producer with exactly one complete publication.
    pub fn outcome(self, kind: StreamKind) -> Result<bool, ()> {
        match self {
            Classification::Accepted => Ok(true),
            Classification::Unchanged => Ok(false),
            Classification::Stale if kind != StreamKind::Delta => Ok(false),
            Classification::Stale
            | Classification::Conflict
            | Classification::Gap
            | Classification::Invalid => Err(()),
        }
    }
}

/// One publication stream's accepted identity. Revision zero means nothing has
/// been accepted; a complete publication carries base zero.
#[derive(Debug, Default)]
pub struct Stream {
    kind: StreamKind,
    revision: u32,
    /// Exact serialized form of the accepted revision: a replay must match it.
    fingerprint: Vec<u16>,
}

impl Stream {
    pub const fn new(kind: StreamKind) -> Self {
        Self { kind, revision: 0, fingerprint: Vec::new() }
    }

    pub fn kind(&self) -> StreamKind {
        self.kind
    }

    pub fn revision(&self) -> u32 {
        self.revision
    }

    /// Classify a publication without mutating anything. `base` is zero for a
    /// complete publication and must be zero outside delta streams.
    pub fn classify(&self, base: u32, revision: u32, fingerprint: &[u16]) -> Classification {
        if revision == 0 || (base != 0 && self.kind != StreamKind::Delta) {
            return Classification::Invalid;
        }
        if revision < self.revision {
            return Classification::Stale;
        }
        if revision == self.revision {
            return if fingerprint == self.fingerprint.as_slice() {
                Classification::Unchanged
            } else {
                Classification::Conflict
            };
        }
        if base != 0 && base != self.revision {
            return Classification::Gap;
        }
        Classification::Accepted
    }

    /// Classify and apply the kind's policy in one step.
    pub fn check(&self, base: u32, revision: u32, fingerprint: &[u16]) -> Result<bool, ()> {
        self.classify(base, revision, fingerprint).outcome(self.kind)
    }

    /// Record an accepted publication. Call only after the caller has applied
    /// (or is certain to apply) its body, so a refused body leaves the stream
    /// exactly where it was.
    pub fn commit(&mut self, revision: u32, fingerprint: &[u16]) {
        self.revision = revision;
        self.fingerprint.clear();
        self.fingerprint.extend_from_slice(fingerprint);
    }

    /// Forget everything accepted: a new session or endpoint generation. The
    /// next publication must be complete.
    pub fn reset(&mut self) {
        self.revision = 0;
        self.fingerprint.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use Classification::*;

    const KINDS: [StreamKind; 3] = [StreamKind::Delta, StreamKind::Snapshot, StreamKind::Session];

    fn at(kind: StreamKind, revision: u32, fingerprint: &[u16]) -> Stream {
        let mut s = Stream::new(kind);
        s.commit(revision, fingerprint);
        s
    }

    #[test]
    fn classification_is_shared_by_every_kind() {
        for kind in KINDS {
            let s = at(kind, 5, &[1, 2]);
            assert_eq!(s.classify(0, 0, &[]), Invalid, "{kind:?}");
            assert_eq!(s.classify(0, 4, &[1, 2]), Stale, "{kind:?}");
            assert_eq!(s.classify(0, 5, &[1, 2]), Unchanged, "{kind:?}");
            assert_eq!(s.classify(0, 5, &[1]), Conflict, "{kind:?}");
            assert_eq!(s.classify(0, 6, &[9]), Accepted, "{kind:?}");
            assert_eq!(s.classify(0, u32::MAX, &[]), Accepted, "{kind:?}");
        }
    }

    #[test]
    fn only_delta_streams_take_a_base_and_a_skipped_base_is_a_gap() {
        let delta = at(StreamKind::Delta, 5, &[]);
        assert_eq!(delta.classify(5, 6, &[]), Accepted);
        assert_eq!(delta.classify(4, 6, &[]), Gap);
        assert_eq!(delta.classify(6, 7, &[]), Gap);
        for kind in [StreamKind::Snapshot, StreamKind::Session] {
            assert_eq!(at(kind, 5, &[]).classify(5, 6, &[]), Invalid, "{kind:?}");
        }
    }

    #[test]
    fn replay_and_staleness_are_judged_before_the_base() {
        // A replay is harmless whatever base it names, and a late delta is
        // stale rather than a gap: both match the strip's established rules.
        let s = at(StreamKind::Delta, 5, &[3]);
        assert_eq!(s.classify(4, 5, &[3]), Unchanged);
        assert_eq!(s.classify(9, 3, &[3]), Stale);
    }

    #[test]
    fn late_revisions_are_refused_only_on_delta_streams() {
        assert_eq!(Stale.outcome(StreamKind::Delta), Err(()));
        assert_eq!(Stale.outcome(StreamKind::Snapshot), Ok(false));
        assert_eq!(Stale.outcome(StreamKind::Session), Ok(false));
        for kind in KINDS {
            assert_eq!(Accepted.outcome(kind), Ok(true));
            assert_eq!(Unchanged.outcome(kind), Ok(false));
            for refused in [Conflict, Gap, Invalid] {
                assert_eq!(refused.outcome(kind), Err(()), "{refused:?} on {kind:?}");
            }
        }
    }

    #[test]
    fn a_fresh_or_reset_stream_needs_a_complete_publication() {
        let mut s = at(StreamKind::Delta, 5, &[1]);
        s.reset();
        assert_eq!(s.revision(), 0);
        assert_eq!(s.classify(5, 6, &[]), Gap);
        assert_eq!(s.classify(0, 1, &[]), Accepted);
        assert_eq!(Stream::new(StreamKind::Delta).classify(1, 2, &[]), Gap);
    }

    #[test]
    fn classifying_never_mutates() {
        let s = at(StreamKind::Delta, 5, &[1]);
        for (base, revision) in [(0, 0), (0, 4), (0, 5), (5, 6), (4, 6)] {
            let _ = s.check(base, revision, &[2]);
        }
        assert_eq!(s.revision(), 5);
        assert_eq!(s.classify(0, 5, &[1]), Unchanged);
    }

    #[test]
    fn exhaustive_small_range_agrees_with_the_written_rules() {
        for kind in KINDS {
            for held in 0..6u32 {
                let s = at(kind, held, &[held as u16]);
                for base in 0..7u32 {
                    for revision in 0..8u32 {
                        for same in [false, true] {
                            let fp = [if same { held as u16 } else { 99 }];
                            let expected = if revision == 0 || (base != 0 && kind != StreamKind::Delta) {
                                Invalid
                            } else if revision < held {
                                Stale
                            } else if revision == held {
                                if same { Unchanged } else { Conflict }
                            } else if base != 0 && base != held {
                                Gap
                            } else {
                                Accepted
                            };
                            assert_eq!(s.classify(base, revision, &fp), expected,
                                "{kind:?} held {held} base {base} revision {revision} same {same}");
                        }
                    }
                }
            }
        }
    }
}
