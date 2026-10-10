//! Identity-only table for C++-validated address subsections.
use crate::address::MAX_EDIT_ID;
use crate::publication::{Classification, ShellPublicationCounts, Stream, StreamKind};
pub const MAX_ADDRESS_SECTION_FINGERPRINT: usize = 6 * 1024 * 1024;
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShellAddressSectionDecision {
    Ignored = 0,
    Accepted = 1,
    Unchanged = 2,
}
struct Section {
    session: Vec<u16>,
    stream: Stream<u64>,
}
impl Section {
    fn new(kind: StreamKind) -> Self {
        Self {
            session: Vec::new(),
            stream: Stream::new(kind),
        }
    }
    fn reset(&mut self) {
        self.session.clear();
        self.stream.reset();
    }
    fn resolve(
        &mut self,
        session: &[u16],
        generation: u64,
        fingerprint: &[u16],
        eligible: bool,
        commit: bool,
    ) -> Result<ShellAddressSectionDecision, ()> {
        // Zero remains a valid wire generation. The offset is internal only and
        // fits u64 even for the largest JavaScript safe integer.
        let revision = generation.saturating_add(1);
        let class = if session.is_empty()
            || session.len() > 256
            || generation > MAX_EDIT_ID
            || fingerprint.len() > MAX_ADDRESS_SECTION_FINGERPRINT
        {
            Classification::Invalid
        } else if !eligible {
            Classification::Stale
        } else if session != self.session {
            Classification::Accepted
        } else {
            self.stream.classify(0, revision, fingerprint)
        };
        if commit || class != Classification::Accepted {
            self.stream.record(class);
        }
        class.outcome(self.stream.kind())?;
        if class == Classification::Accepted && commit {
            if session != self.session {
                self.stream.reset();
                self.session = session.to_vec();
            }
            self.stream.commit(revision, fingerprint);
        }
        Ok(match class {
            Classification::Accepted => ShellAddressSectionDecision::Accepted,
            Classification::Unchanged => ShellAddressSectionDecision::Unchanged,
            _ => ShellAddressSectionDecision::Ignored,
        })
    }
}
pub struct AddressSections {
    sections: [Section; 3],
}
impl Default for AddressSections {
    fn default() -> Self {
        Self {
            sections: [
                Section::new(StreamKind::Snapshot),
                Section::new(StreamKind::Session),
                Section::new(StreamKind::Session),
            ],
        }
    }
}
impl AddressSections {
    pub fn reset(&mut self) {
        for section in &mut self.sections {
            section.reset();
        }
    }
    pub fn counts(&self, index: usize) -> Option<ShellPublicationCounts> {
        self.sections.get(index).map(|s| s.stream.counts())
    }
    pub fn resolve(
        &mut self,
        index: usize,
        session: &[u16],
        generation: u64,
        fingerprint: &[u16],
        eligible: bool,
        commit: bool,
    ) -> Result<ShellAddressSectionDecision, ()> {
        self.sections.get_mut(index).ok_or(())?.resolve(
            session,
            generation,
            fingerprint,
            eligible,
            commit,
        )
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use ShellAddressSectionDecision::*;
    #[test]
    fn each_channel_orders_replays_and_conflicts_without_shared_content() {
        let mut p = AddressSections::default();
        for i in 0..3 {
            assert_eq!(p.resolve(i, &[1], 0, &[1], true, false), Ok(Accepted));
            assert_eq!(p.counts(i).unwrap().accepted, 0);
            assert_eq!(p.resolve(i, &[1], 0, &[1], true, true), Ok(Accepted));
            assert_eq!(p.resolve(i, &[1], 0, &[1], true, false), Ok(Unchanged));
            assert_eq!(p.resolve(i, &[1], 0, &[2], true, false), Err(()));
            assert_eq!(p.resolve(i, &[1], 3, &[3], true, true), Ok(Accepted));
            assert_eq!(p.resolve(i, &[1], 1, &[1], true, false), Ok(Ignored));
            assert_eq!(p.resolve(i, &[1], 4, &[4], false, false), Ok(Ignored));
            assert_eq!(p.resolve(i, &[1], 4, &[4], true, true), Ok(Accepted));
            assert_eq!(
                p.counts(i).unwrap(),
                ShellPublicationCounts {
                    accepted: 3,
                    unchanged: 1,
                    ignored: 2,
                    refused: 1
                }
            );
        }
    }
    #[test]
    fn session_reset_and_boundary_limits_preserve_diagnostics() {
        let mut p = AddressSections::default();
        p.resolve(0, &[1], MAX_EDIT_ID, &[1], true, true).unwrap();
        assert_eq!(p.resolve(0, &[2], 0, &[2], true, true), Ok(Accepted));
        for (scope, generation) in [(&[][..], 0), (&[1][..], MAX_EDIT_ID + 1)] {
            assert_eq!(p.resolve(1, scope, generation, &[], false, false), Err(()));
        }
        assert_eq!(p.resolve(1, &[1], u64::MAX, &[], false, false), Err(()));
        assert_eq!(p.counts(1).unwrap().refused, 3);
        assert_eq!(p.resolve(3, &[1], 0, &[], true, false), Err(()));
        assert_eq!(
            p.resolve(
                0,
                &[1],
                0,
                &vec![0; MAX_ADDRESS_SECTION_FINGERPRINT + 1],
                false,
                false
            ),
            Err(())
        );
        let counts = p.counts(0);
        p.reset();
        assert_eq!(p.counts(0), counts);
        assert_eq!(p.resolve(0, &[1], 0, &[1], true, true), Ok(Accepted));
    }
}
