//! Address-section identity only. Firefox owns queries, policy and content.
use crate::publication::{Classification, Stream, StreamKind, ShellPublicationCounts};
use crate::address::MAX_EDIT_ID;

pub const MAX_FINGERPRINT: usize = 6 * 1024 * 1024;

pub struct AddressPublication {
    session: Vec<u16>,
    structure: u64,
    stream: Stream<u64>,
}
impl Default for AddressPublication {
    fn default() -> Self {
        Self { session: Vec::new(), structure: 0, stream: Stream::new(StreamKind::Snapshot) }
    }
}
impl AddressPublication {
    pub fn reset(&mut self) { self.session.clear(); self.structure = 0; self.stream.reset(); }
    pub fn counts(&self) -> ShellPublicationCounts { self.stream.counts() }
    pub fn resolve(&mut self, session: &[u16], sequence: u64, structure: u64, fingerprint: &[u16], commit: bool) -> Result<bool, ()> {
        let same_session = session == self.session;
        let class = if session.is_empty() || session.len() > 256 || sequence == 0 ||
            sequence > MAX_EDIT_ID || structure > MAX_EDIT_ID || fingerprint.len() > MAX_FINGERPRINT {
            Classification::Invalid
        } else {
            let class = if same_session { self.stream.classify(0, sequence, fingerprint) }
                else { Classification::Accepted };
            if same_session && class == Classification::Accepted && structure < self.structure {
                Classification::Invalid
            } else { class }
        };
        // Resolution counts no accepted work: edits may defer native application.
        if commit || class != Classification::Accepted { self.stream.record(class); }
        let apply = class.outcome(StreamKind::Snapshot)?;
        if apply && commit {
            if !same_session { self.stream.reset(); }
            self.session = session.to_vec();
            self.structure = structure;
            self.stream.commit(sequence, fingerprint);
        }
        Ok(apply)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ordering_replay_conflict_and_structure_are_atomic() {
        let mut p = AddressPublication::default();
        assert_eq!(p.resolve(&[1], 1, 0, &[1], true), Ok(true));
        assert_eq!(p.resolve(&[1], 2, 4, &[2], true), Ok(true));
        assert_eq!(p.resolve(&[1], 1, 0, &[9], true), Ok(false));
        assert_eq!(p.resolve(&[1], 2, 4, &[2], true), Ok(false));
        assert_eq!(p.resolve(&[1], 2, 4, &[9], true), Err(()));
        assert_eq!(p.resolve(&[1], 3, 3, &[3], true), Err(()));
        assert_eq!(p.resolve(&[1], 3, 4, &[3], true), Ok(true));
    }
    #[test]
    fn deferred_application_does_not_consume_a_publication_and_resets_preserve_counts() {
        let mut p = AddressPublication::default();
        assert_eq!(p.resolve(&[1], 9, 8, &[1], false), Ok(true));
        assert_eq!(p.counts().accepted, 0);
        assert_eq!(p.resolve(&[1], 9, 8, &[1], true), Ok(true));
        assert_eq!(p.resolve(&[2], 1, 0, &[2], true), Ok(true));
        p.reset();
        assert_eq!(p.resolve(&[2], 1, 0, &[2], true), Ok(true));
        assert_eq!(p.counts().accepted, 3);
    }
    #[test]
    fn safe_integer_revisions_are_not_truncated() {
        let mut p = AddressPublication::default();
        let wide = u32::MAX as u64 + 1;
        assert_eq!(p.resolve(&[1], wide, wide, &[1], true), Ok(true));
        assert_eq!(p.resolve(&[1], MAX_EDIT_ID, MAX_EDIT_ID, &[2], true), Ok(true));
        for (session, sequence, structure) in [(&[][..], 1, 0), (&[1][..], 0, 0),
            (&[1][..], MAX_EDIT_ID + 1, 0), (&[1][..], MAX_EDIT_ID, MAX_EDIT_ID + 1)] {
            assert_eq!(p.resolve(session, sequence, structure, &[2], true), Err(()));
        }
        assert_eq!(p.resolve(&[1], MAX_EDIT_ID, MAX_EDIT_ID, &[2], true), Ok(false));
    }
}
