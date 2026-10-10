//! Customization publication identity only; Firefox owns placement and policy.
use crate::publication::{Classification, ShellPublicationCounts, Stream, StreamKind};
use crate::address::MAX_EDIT_ID;

pub const MAX_TOOLBAR_FINGERPRINT: usize = 6 * 1024 * 1024;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShellToolbarPublication {
    Ignored = 0,
    /// Revisioned content is identical; independent labels may refresh.
    Unchanged = 1,
    Accepted = 2,
}

pub struct ToolbarPublication { stream: Stream<u64> }
impl Default for ToolbarPublication {
    fn default() -> Self { Self { stream: Stream::new(StreamKind::Snapshot) } }
}
impl ToolbarPublication {
    pub fn reset(&mut self) { self.stream.reset(); }
    pub fn counts(&self) -> ShellPublicationCounts { self.stream.counts() }
    /// The fingerprint excludes labels, whose changes do not advance the wire
    /// revision. Resolve before host mutation; commit only accepted content.
    pub fn resolve(&mut self, revision: u64, fingerprint: &[u16], commit: bool) -> Result<ShellToolbarPublication, ()> {
        let class = if revision > MAX_EDIT_ID || fingerprint.len() > MAX_TOOLBAR_FINGERPRINT {
            Classification::Invalid
        } else { self.stream.classify(0, revision, fingerprint) };
        if commit || class != Classification::Accepted { self.stream.record(class); }
        let apply = class.outcome(StreamKind::Snapshot)?;
        if apply {
            if commit { self.stream.commit(revision, fingerprint); }
            Ok(ShellToolbarPublication::Accepted)
        } else if class == Classification::Unchanged { Ok(ShellToolbarPublication::Unchanged) }
        else { Ok(ShellToolbarPublication::Ignored) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ShellToolbarPublication::*;
    #[test] fn replay_stale_conflict_and_gaps_preserve_identity() {
        let mut p=ToolbarPublication::default();
        assert_eq!(p.resolve(2,&[7],true),Ok(Accepted));
        assert_eq!(p.resolve(1,&[8],false),Ok(Ignored));
        assert_eq!(p.resolve(2,&[7],false),Ok(Unchanged));
        assert_eq!(p.resolve(2,&[8],false),Err(()));
        assert_eq!(p.resolve(2,&[7],false),Ok(Unchanged));
        assert_eq!(p.resolve(9,&[8],false),Ok(Accepted));
        assert_eq!(p.resolve(2,&[7],false),Ok(Unchanged));
        assert_eq!(p.resolve(9,&[8],true),Ok(Accepted));
        assert_eq!(p.counts(),ShellPublicationCounts{accepted:2,unchanged:3,ignored:1,refused:1});
    }
    #[test] fn safe_integer_bounds_and_reset_preserve_counts() {
        let mut p=ToolbarPublication::default();let wide=u32::MAX as u64+1;
        assert_eq!(p.resolve(wide,&[1],true),Ok(Accepted));
        assert_eq!(p.resolve(MAX_EDIT_ID,&[2],true),Ok(Accepted));
        for revision in [0,MAX_EDIT_ID+1] {assert_eq!(p.resolve(revision,&[],false),Err(()));}
        assert_eq!(p.resolve(MAX_EDIT_ID,&vec![0;MAX_TOOLBAR_FINGERPRINT+1],false),Err(()));
        assert_eq!(p.resolve(MAX_EDIT_ID,&[2],false),Ok(Unchanged));
        p.reset();assert_eq!(p.resolve(1,&[3],true),Ok(Accepted));
        assert_eq!(p.counts().accepted,3);assert_eq!(p.counts().refused,3);
    }
}
