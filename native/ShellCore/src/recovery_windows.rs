/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Engine-loss reconciliation policy, independent of session-store and HWND APIs.
//! Firefox supplies opaque persisted keys and a complete restored inventory.
//! This produces a plan, not browser actions: the adapter must revalidate the
//! current attempt and survivor lifetime before applying each effect. No draft
//! text, URLs, private session keys or navigation requests belong in this model.

use std::collections::BTreeSet;

/// Same upper bound as the link's default snapshot-key budget.
const MAX_RECOVERY_WINDOWS: usize = 1024;
pub type SessionKey = [u8; 16];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SurvivorKind {
    Normal {
        key: Option<SessionKey>,
    },
    Private,
    /// Privacy was never authoritatively published; do not assume normal.
    Unknown,
    /// Retain the normal key until reconciliation finishes to prevent revival.
    ClosedNormal {
        key: Option<SessionKey>,
    },
}

#[derive(Clone, Copy, Debug)]
pub struct Survivor {
    pub id: u64,
    pub kind: SurvivorKind,
}

/// Only normal restored windows may enter this inventory. Firefox filters
/// privacy before publication, rather than publishing private identifiers.
#[derive(Clone, Copy, Debug)]
pub struct RestoredWindow {
    pub id: u64,
    pub key: Option<SessionKey>,
    pub placeholder: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WindowEffect {
    BindRestored {
        survivor: u64,
        restored: u64,
    },
    /// Firefox creates an empty tab for this same surviving shell. The adapter
    /// may restore independently retained unsent editor text, never submit it.
    CreateEmpty {
        survivor: u64,
    },
    CloseAndDiscard {
        survivor: u64,
    },
    /// A checkpoint entry for a window closed during recovery cannot revive it.
    SuppressRestored {
        restored: u64,
    },
    /// Firefox restored a normal window with no surviving shell counterpart.
    CreateShell {
        restored: u64,
    },
}

#[derive(Debug, PartialEq, Eq)]
pub enum ReconcileError {
    Incomplete,
    StaleAttempt,
    Capacity,
    Identity,
}

#[derive(Debug, PartialEq, Eq)]
pub struct WindowPlan {
    pub attempt: u64,
    pub effects: Vec<WindowEffect>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ShellRecoveryWindow {
    pub id: u64,
    pub key: [u8; 16],
    pub kind: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ShellRecoveryWindowEffect {
    pub survivor: u64,
    pub restored: u64,
    pub kind: u32,
}
/// Kinds: 1 bind, 2 empty tab, 3 close/discard, 4 suppress restored, 5 new shell.
/// # Safety
/// Arrays are valid for their lengths, output has capacity slots, count is
/// writable; outputs are disjoint from inputs. Failure leaves outputs untouched.
#[no_mangle]
pub unsafe extern "C" fn shell_recovery_windows(
    attempt: u64,
    survivors: *const ShellRecoveryWindow,
    ns: usize,
    restored: *const ShellRecoveryWindow,
    nr: usize,
    output: *mut ShellRecoveryWindowEffect,
    capacity: usize,
    count: *mut usize,
) -> bool {
    if ns > MAX_RECOVERY_WINDOWS
        || nr > MAX_RECOVERY_WINDOWS
        || capacity < ns + nr
        || output.is_null()
        || count.is_null()
        || (ns != 0 && survivors.is_null())
        || (nr != 0 && restored.is_null())
    {
        return false;
    }
    let result = std::panic::catch_unwind(|| {
        let s = if ns == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(survivors, ns)
        };
        let r = if nr == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(restored, nr)
        };
        let key = |k: [u8; 16]| if k == [0; 16] { None } else { Some(k) };
        let mut windows = Vec::with_capacity(ns);
        for w in s {
            let kind = match w.kind {
                0 if w.key == [0; 16] => SurvivorKind::Unknown,
                1 => SurvivorKind::Normal { key: key(w.key) },
                2 if w.key == [0; 16] => SurvivorKind::Private,
                3 => SurvivorKind::ClosedNormal { key: key(w.key) },
                _ => return None,
            };
            windows.push(Survivor { id: w.id, kind });
        }
        if r.iter()
            .any(|w| w.kind != 1 && (w.kind != 4 || w.key != [0; 16]))
        {
            return None;
        }
        let restored: Vec<_> = r
            .iter()
            .map(|w| RestoredWindow {
                id: w.id,
                key: key(w.key),
                placeholder: w.kind == 4,
            })
            .collect();
        let plan = reconcile(attempt, attempt, true, &windows, &restored).ok()?;
        Some(
            plan.effects
                .into_iter()
                .map(|effect| match effect {
                    WindowEffect::BindRestored { survivor, restored } => {
                        ShellRecoveryWindowEffect {
                            survivor,
                            restored,
                            kind: 1,
                        }
                    }
                    WindowEffect::CreateEmpty { survivor } => ShellRecoveryWindowEffect {
                        survivor,
                        kind: 2,
                        ..Default::default()
                    },
                    WindowEffect::CloseAndDiscard { survivor } => ShellRecoveryWindowEffect {
                        survivor,
                        kind: 3,
                        ..Default::default()
                    },
                    WindowEffect::SuppressRestored { restored } => ShellRecoveryWindowEffect {
                        restored,
                        kind: 4,
                        ..Default::default()
                    },
                    WindowEffect::CreateShell { restored } => ShellRecoveryWindowEffect {
                        restored,
                        kind: 5,
                        ..Default::default()
                    },
                })
                .collect::<Vec<_>>(),
        )
    });
    if let Ok(Some(effects)) = result {
        std::ptr::copy_nonoverlapping(effects.as_ptr(), output, effects.len());
        *count = effects.len();
        true
    } else {
        false
    }
}

/// Atomically validate the full inventory before emitting any effects. A key
/// absent from an incomplete checkpoint is not evidence for an empty tab.
pub fn reconcile(
    current_attempt: u64,
    inventory_attempt: u64,
    complete: bool,
    survivors: &[Survivor],
    restored: &[RestoredWindow],
) -> Result<WindowPlan, ReconcileError> {
    if current_attempt == 0 || inventory_attempt != current_attempt {
        return Err(ReconcileError::StaleAttempt);
    }
    if !complete {
        return Err(ReconcileError::Incomplete);
    }
    if survivors.len() > MAX_RECOVERY_WINDOWS || restored.len() > MAX_RECOVERY_WINDOWS {
        return Err(ReconcileError::Capacity);
    }
    let mut survivor_ids = BTreeSet::new();
    let mut survivor_keys = BTreeSet::new();
    for survivor in survivors {
        if survivor.id == 0 || !survivor_ids.insert(survivor.id) {
            return Err(ReconcileError::Identity);
        }
        if let SurvivorKind::Normal { key: Some(key) }
        | SurvivorKind::ClosedNormal { key: Some(key) } = survivor.kind
        {
            if key == [0; 16] || !survivor_keys.insert(key) {
                return Err(ReconcileError::Identity);
            }
        }
    }
    if restored.iter().filter(|w| w.placeholder).count() > 1
        || restored.iter().any(|w| w.placeholder && w.key.is_some())
    {
        return Err(ReconcileError::Identity);
    }
    let mut restored_ids = BTreeSet::new();
    let mut restored_keys = BTreeSet::new();
    for window in restored {
        if window.id == 0 || !restored_ids.insert(window.id) {
            return Err(ReconcileError::Identity);
        }
        if let Some(key) = window.key {
            if key == [0; 16] || !restored_keys.insert(key) {
                return Err(ReconcileError::Identity);
            }
        }
    }
    let mut consumed = BTreeSet::new();
    let mut effects = Vec::new();
    for survivor in survivors {
        match survivor.kind {
            SurvivorKind::Private | SurvivorKind::Unknown => {
                effects.push(WindowEffect::CloseAndDiscard {
                    survivor: survivor.id,
                });
            }
            SurvivorKind::Normal { key } | SurvivorKind::ClosedNormal { key } => {
                let matched = key.and_then(|key| restored.iter().find(|w| w.key == Some(key)));
                if let Some(window) = matched {
                    consumed.insert(window.id);
                    effects.push(
                        if matches!(survivor.kind, SurvivorKind::ClosedNormal { .. }) {
                            WindowEffect::SuppressRestored {
                                restored: window.id,
                            }
                        } else {
                            WindowEffect::BindRestored {
                                survivor: survivor.id,
                                restored: window.id,
                            }
                        },
                    );
                } else if matches!(survivor.kind, SurvivorKind::Normal { .. }) {
                    effects.push(WindowEffect::CreateEmpty {
                        survivor: survivor.id,
                    });
                }
            }
        }
    }
    for window in restored {
        if !consumed.contains(&window.id) {
            effects.push(if window.placeholder {
                WindowEffect::SuppressRestored {
                    restored: window.id,
                }
            } else {
                WindowEffect::CreateShell {
                    restored: window.id,
                }
            });
        }
    }
    Ok(WindowPlan {
        attempt: current_attempt,
        effects,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn normal(id: u64, key: u8) -> Survivor {
        Survivor {
            id,
            kind: SurvivorKind::Normal {
                key: Some([key; 16]),
            },
        }
    }
    fn restored(id: u64, key: u8) -> RestoredWindow {
        RestoredWindow {
            id,
            key: Some([key; 16]),
            placeholder: false,
        }
    }
    #[test]
    fn matches_keys_not_order_and_keeps_missing_window_empty() {
        let plan = reconcile(
            2,
            2,
            true,
            &[normal(10, 1), normal(20, 2), normal(30, 3)],
            &[restored(200, 2), restored(100, 1)],
        )
        .unwrap();
        assert_eq!(
            plan.effects,
            vec![
                WindowEffect::BindRestored {
                    survivor: 10,
                    restored: 100
                },
                WindowEffect::BindRestored {
                    survivor: 20,
                    restored: 200
                },
                WindowEffect::CreateEmpty { survivor: 30 }
            ]
        );
    }
    #[test]
    fn incomplete_or_retired_inventory_never_creates_empty_tabs() {
        assert_eq!(
            reconcile(2, 2, false, &[normal(10, 1)], &[]),
            Err(ReconcileError::Incomplete)
        );
        for (current, received) in [(2, 1), (2, 3), (0, 0)] {
            assert_eq!(
                reconcile(current, received, true, &[], &[]),
                Err(ReconcileError::StaleAttempt)
            );
        }
    }
    #[test]
    fn closed_window_cannot_be_restored_into_a_new_shell() {
        let closed = Survivor {
            id: 10,
            kind: SurvivorKind::ClosedNormal { key: Some([1; 16]) },
        };
        assert_eq!(
            reconcile(2, 2, true, &[closed, normal(20, 2)], &[restored(100, 1)])
                .unwrap()
                .effects,
            vec![
                WindowEffect::SuppressRestored { restored: 100 },
                WindowEffect::CreateEmpty { survivor: 20 }
            ]
        );
        assert!(reconcile(2, 2, true, &[closed], &[])
            .unwrap()
            .effects
            .is_empty());
    }
    #[test]
    fn private_and_unclassified_windows_only_close_and_discard() {
        let survivors = [
            Survivor {
                id: 10,
                kind: SurvivorKind::Private,
            },
            Survivor {
                id: 20,
                kind: SurvivorKind::Unknown,
            },
        ];
        assert_eq!(
            reconcile(2, 2, true, &survivors, &[]).unwrap().effects,
            vec![
                WindowEffect::CloseAndDiscard { survivor: 10 },
                WindowEffect::CloseAndDiscard { survivor: 20 }
            ]
        );
    }
    #[test]
    fn missing_keys_are_not_matches_and_extra_restored_windows_stay_firefox_owned() {
        let survivor = Survivor {
            id: 10,
            kind: SurvivorKind::Normal { key: None },
        };
        let window = RestoredWindow {
            id: 100,
            key: None,
            placeholder: false,
        };
        assert_eq!(
            reconcile(2, 2, true, &[survivor], &[window])
                .unwrap()
                .effects,
            vec![
                WindowEffect::CreateEmpty { survivor: 10 },
                WindowEffect::CreateShell { restored: 100 }
            ]
        );
    }
    #[test]
    fn duplicate_or_zero_identity_rejects_whole_inventory() {
        for survivors in [
            vec![normal(10, 1), normal(10, 2)],
            vec![normal(10, 1), normal(20, 1)],
            vec![normal(0, 1)],
            vec![normal(10, 0)],
        ] {
            assert_eq!(
                reconcile(2, 2, true, &survivors, &[]),
                Err(ReconcileError::Identity)
            );
        }
        for windows in [
            vec![restored(100, 1), restored(100, 2)],
            vec![restored(100, 1), restored(200, 1)],
            vec![restored(0, 1)],
            vec![restored(100, 0)],
        ] {
            assert_eq!(
                reconcile(2, 2, true, &[normal(10, 1)], &windows),
                Err(ReconcileError::Identity)
            );
        }
    }
    #[test]
    fn empty_checkpoint_suppresses_only_explicit_startup_placeholder() {
        let placeholder = RestoredWindow {
            id: 100,
            key: None,
            placeholder: true,
        };
        assert_eq!(
            reconcile(2, 2, true, &[normal(10, 1), normal(20, 2)], &[placeholder])
                .unwrap()
                .effects,
            vec![
                WindowEffect::CreateEmpty { survivor: 10 },
                WindowEffect::CreateEmpty { survivor: 20 },
                WindowEffect::SuppressRestored { restored: 100 }
            ]
        );
        assert_eq!(
            reconcile(
                2,
                2,
                true,
                &[],
                &[
                    placeholder,
                    RestoredWindow {
                        id: 101,
                        ..placeholder
                    }
                ]
            ),
            Err(ReconcileError::Identity)
        );
        assert_eq!(
            reconcile(
                2,
                2,
                true,
                &[],
                &[RestoredWindow {
                    key: Some([1; 16]),
                    ..placeholder
                }]
            ),
            Err(ReconcileError::Identity)
        );
    }
    #[test]
    fn closed_missing_and_extra_windows_share_one_atomic_plan() {
        let closed = Survivor {
            id: 10,
            kind: SurvivorKind::ClosedNormal { key: Some([1; 16]) },
        };
        assert_eq!(
            reconcile(
                2,
                2,
                true,
                &[closed, normal(20, 2)],
                &[restored(100, 1), restored(300, 3)]
            )
            .unwrap()
            .effects,
            vec![
                WindowEffect::SuppressRestored { restored: 100 },
                WindowEffect::CreateEmpty { survivor: 20 },
                WindowEffect::CreateShell { restored: 300 }
            ]
        );
    }
    #[test]
    fn oversized_inventory_rejected_before_planning() {
        let windows = vec![restored(100, 1); MAX_RECOVERY_WINDOWS + 1];
        let survivors = vec![normal(10, 1); MAX_RECOVERY_WINDOWS + 1];
        assert_eq!(
            reconcile(2, 2, true, &[], &windows),
            Err(ReconcileError::Capacity)
        );
        assert_eq!(
            reconcile(2, 2, true, &survivors, &[]),
            Err(ReconcileError::Capacity)
        );
    }
}
