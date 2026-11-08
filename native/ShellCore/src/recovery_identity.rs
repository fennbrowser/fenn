/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use crate::publication::{Stream, StreamKind};

/// Firefox-published identity for one endpoint. Privacy: 0 unknown, 1 normal,
/// 2 private. A zero key means no confirmed normal session-store identity yet.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ShellRecoveryWindowIdentity {
    pub revision: u64,
    pub privacy: u32,
    pub key: [u8; 16],
}

pub struct Identity {
    stream: Stream<u64>,
    state: ShellRecoveryWindowIdentity,
}
impl Default for Identity {
    fn default() -> Self {
        Self { stream: Stream::new(StreamKind::Snapshot), state: Default::default() }
    }
}
impl Identity {
    pub fn state(&self) -> ShellRecoveryWindowIdentity { self.state }
    pub fn publish(&mut self, revision: u64, privacy: u32, key: &[u16]) -> Result<(), ()> {
        if revision > 9_007_199_254_740_991 || !matches!(privacy, 1 | 2) ||
            (privacy == 2 && !key.is_empty()) || (!key.is_empty() && key.len() != 32) { return Err(()); }
        let mut parsed = [0u8; 16];
        for (i, unit) in key.iter().enumerate() {
            let digit = match unit { 48..=57 => unit - 48, 97..=102 => unit - 87, _ => return Err(()) };
            parsed[i / 2] |= (digit as u8) << (if i % 2 == 0 { 4 } else { 0 });
        }
        if !key.is_empty() && parsed == [0; 16] { return Err(()); }
        let mut fingerprint = vec![privacy as u16];
        fingerprint.extend_from_slice(key);
        let class = self.stream.classify(0, revision, &fingerprint);
        self.stream.record(class);
        if !class.outcome(StreamKind::Snapshot)? { return Ok(()); }
        if self.state.privacy != 0 && self.state.privacy != privacy { return Err(()); }
        self.stream.commit(revision, &fingerprint);
        self.state = ShellRecoveryWindowIdentity { revision, privacy, key: parsed };
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ffi_rejects_stale_endpoint_and_closed_core_without_mutating_output() {
        use crate::ffi::*;
        unsafe {
            let core = shell_core_create(std::ptr::null());
            let mut binding = crate::window_host::ShellWindowBinding::default();
            let input = crate::window_host::ShellWindowBinding { shell: 10, ..Default::default() };
            assert_eq!(shell_core_window_host(core, 7, &input, &mut binding), ShellStatus::Ok);
            let mut output = ShellRecoveryWindowIdentity::default();
            assert_eq!(shell_core_recovery_identity(core, binding.generation, 1, 2, std::ptr::null(), 0, &mut output), ShellStatus::Ok);
            let saved = output;
            assert_eq!(shell_core_recovery_identity(core, binding.generation + 1, 2, 1, std::ptr::null(), 0, &mut output), ShellStatus::Rejected);
            assert_eq!(output, saved);
            assert_eq!(shell_core_window_host(core, 8, &binding, &mut binding.clone()), ShellStatus::Ok);
            assert_eq!(shell_core_recovery_identity(core, binding.generation, 2, 2, std::ptr::null(), 0, &mut output), ShellStatus::Rejected);
            assert_eq!(output, saved);
            shell_core_destroy(core);
        }
    }
    #[test]
    fn normal_identity_waits_for_restore_and_uses_snapshot_rules() {
        let mut identity = Identity::default();
        let key: Vec<_> = "1234567890abcdef1234567890abcdef".encode_utf16().collect();
        identity.publish(1, 1, &[]).unwrap();
        assert_eq!(identity.state().key, [0;16]);
        identity.publish(2, 1, &key).unwrap();
        let saved = identity.state();
        identity.publish(2, 1, &key).unwrap();
        identity.publish(1, 1, &[]).unwrap();
        assert_eq!(identity.state(), saved);
        assert!(identity.publish(2, 1, &[]).is_err());
        assert!(identity.publish(3, 2, &[]).is_err());
        assert_eq!(identity.state(), saved);
    }
    #[test]
    fn private_identity_cannot_hold_a_key_or_become_normal() {
        let mut identity = Identity::default();
        let key: Vec<_> = "1234567890abcdef1234567890abcdef".encode_utf16().collect();
        assert!(identity.publish(1, 2, &key).is_err());
        identity.publish(1, 2, &[]).unwrap();
        assert!(identity.publish(2, 1, &key).is_err());
        assert_eq!(identity.state().key, [0;16]);
    }
    #[test]
    fn malformed_identity_never_changes_state() {
        let mut identity = Identity::default();
        for key in ["0".repeat(32), "x".repeat(32), "A".repeat(32), "a".repeat(31), "a".repeat(33)] {
            assert!(identity.publish(1, 1, &key.encode_utf16().collect::<Vec<_>>()).is_err());
        }
        for (revision, privacy) in [(0, 1), (1, 0), (1, 3), (u64::MAX, 1)] {
            assert!(identity.publish(revision, privacy, &[]).is_err());
        }
        assert_eq!(identity.state(), ShellRecoveryWindowIdentity::default());
    }
}
