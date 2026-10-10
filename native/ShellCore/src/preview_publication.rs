//! Hover-preview identity only. Firefox owns domain, thumbnail policy and pixels.
use crate::address::MAX_EDIT_ID;
use crate::publication::{Classification, ShellPublicationCounts, Stream, StreamKind};

use std::sync::atomic::{AtomicU64, Ordering};

// Identity allocation only, not shared presentation state. A capture can
// outlive a destroyed/recreated island, so endpoint-local IDs are insufficient.
static NEXT_REQUEST: AtomicU64 = AtomicU64::new(0);
fn next_request(counter: &AtomicU64) -> Result<u64, ()> {
    counter
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| {
            (value < MAX_EDIT_ID).then(|| value + 1)
        })
        .map(|previous| previous + 1)
        .map_err(|_| ())
}

pub const MAX_PREVIEW_FINGERPRINT: usize = 1024 * 1024;

pub struct PreviewPublication {
    request: u64,
    tab: u32,
    stream: Stream,
}
impl Default for PreviewPublication {
    fn default() -> Self {
        Self {
            request: 0,
            tab: 0,
            stream: Stream::new(StreamKind::Session),
        }
    }
}
impl PreviewPublication {
    pub fn begin(&mut self, tab: u32) -> Result<u64, ()> {
        if tab == 0 {
            return Err(());
        }
        self.request = next_request(&NEXT_REQUEST)?;
        self.tab = tab;
        self.stream.reset();
        Ok(self.request)
    }
    pub fn dismiss(&mut self, request: u64) {
        if request == self.request {
            self.reset();
        }
    }
    pub fn reset(&mut self) {
        self.request = 0;
        self.tab = 0;
        self.stream.reset();
    }
    pub fn counts(&self) -> ShellPublicationCounts {
        self.stream.counts()
    }
    /// Existing wire presence encodes the phase: absent image = domain (1),
    /// present image (including null on failed capture) = completion (2).
    /// Resolve after body validation, then commit after successful decoding.
    pub fn resolve(
        &mut self,
        tab: u32,
        request: u64,
        phase: u32,
        fingerprint: &[u16],
        commit: bool,
    ) -> Result<bool, ()> {
        let class = if tab == 0
            || request == 0
            || request > MAX_EDIT_ID
            || !(1..=2).contains(&phase)
            || fingerprint.len() > MAX_PREVIEW_FINGERPRINT
        {
            Classification::Invalid
        } else if tab != self.tab || request != self.request {
            Classification::Stale
        } else {
            self.stream.classify(0, phase, fingerprint)
        };
        if commit || class != Classification::Accepted {
            self.stream.record(class);
        }
        let apply = class.outcome(StreamKind::Session)?;
        if apply && commit {
            self.stream.commit(phase, fingerprint);
        }
        Ok(apply)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn phases_replay_conflict_and_reordered_answers_share_session_rules() {
        let mut p = PreviewPublication::default();
        let r = p.begin(7).unwrap();
        assert_eq!(p.resolve(7, r, 1, &[1], true), Ok(true));
        assert_eq!(p.resolve(7, r, 1, &[1], false), Ok(false));
        assert_eq!(p.resolve(7, r, 1, &[9], false), Err(()));
        assert_eq!(p.resolve(7, r, 2, &[2], false), Ok(true));
        assert_eq!(
            p.counts().accepted,
            1,
            "preflight does not consume completion"
        );
        assert_eq!(p.resolve(7, r, 2, &[2], true), Ok(true));
        assert_eq!(
            p.resolve(7, r, 1, &[9], false),
            Ok(false),
            "late domain cannot undo completion"
        );
        assert_eq!(p.resolve(7, r, 2, &[2], false), Ok(false));
        assert_eq!(p.resolve(7, r, 2, &[9], false), Err(()));
        assert_eq!(
            p.counts(),
            ShellPublicationCounts {
                accepted: 2,
                unchanged: 2,
                ignored: 1,
                refused: 2
            }
        );
    }
    #[test]
    fn replacement_dismissal_and_reset_revoke_identity_without_reusing_requests() {
        let mut p = PreviewPublication::default();
        let old = p.begin(7).unwrap();
        let new = p.begin(8).unwrap();
        p.dismiss(old);
        assert_eq!(p.resolve(7, old, 2, &[1], true), Ok(false));
        assert_eq!(p.resolve(7, new, 1, &[1], true), Ok(false));
        assert_eq!(p.resolve(8, new + 1, 1, &[1], true), Ok(false));
        assert_eq!(
            p.resolve(8, new, 2, &[2], true),
            Ok(true),
            "completion may arrive first"
        );
        p.dismiss(new);
        assert_eq!(p.resolve(8, new, 2, &[2], true), Ok(false));
        let counts = p.counts();
        p.reset();
        assert_eq!(p.counts(), counts);
        assert!(p.begin(7).unwrap() > new);
    }
    #[test]
    fn bounds_validation_precedes_staleness_and_exhaustion_never_wraps() {
        let mut p = PreviewPublication::default();
        let r = p.begin(7).unwrap();
        for (tab, request, phase) in [
            (0, r, 1),
            (7, 0, 1),
            (7, MAX_EDIT_ID + 1, 1),
            (7, r, 0),
            (7, r, 3),
        ] {
            assert_eq!(p.resolve(tab, request, phase, &[], true), Err(()));
        }
        assert_eq!(
            p.resolve(9, r, 1, &vec![0; MAX_PREVIEW_FINGERPRINT + 1], true),
            Err(())
        );
        let counter = AtomicU64::new(MAX_EDIT_ID - 1);
        assert_eq!(next_request(&counter), Ok(MAX_EDIT_ID));
        assert_eq!(next_request(&counter), Err(()));
        assert_eq!(counter.load(Ordering::Relaxed), MAX_EDIT_ID);
        assert_eq!(p.begin(0), Err(()));
    }
    #[test]
    fn replacement_endpoints_cannot_accept_an_old_capture_with_reused_tab_ids() {
        let mut old = PreviewPublication::default();
        let r = old.begin(7).unwrap();
        drop(old);
        let mut new = PreviewPublication::default();
        let fresh = new.begin(7).unwrap();
        assert!(fresh > r);
        assert_eq!(new.resolve(7, r, 2, &[1], true), Ok(false));
        assert_eq!(new.resolve(7, fresh, 1, &[2], true), Ok(true));
    }
}
