/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Acknowledged local-draft transfer. This is a command protocol, not a
//! publication channel or a replacement for Firefox session restoration.
use crate::startup::{Draft, Startup, Submission, STARTUP_MAX_TABS};
use std::collections::BTreeMap;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Action {
    Create {
        draft: u64,
    },
    Adopt {
        draft: Draft,
        browser: u64,
        selected: bool,
    },
    Submit {
        browser: u64,
        submission: Submission,
    },
    Close {
        draft: u64,
        browser: u64,
    },
    Select {
        draft: u64,
        browser: u64,
        local: bool,
    },
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Request {
    pub generation: u64,
    pub id: u64,
    pub action: Action,
}
#[derive(Clone, Debug, PartialEq, Eq)]
struct Receipt {
    request: u64,
    browser: u64,
    accepted: bool,
}

#[derive(Default)]
pub struct Handoff {
    context: u64,
    pub generation: u64,
    pub lost: bool,
    pub editor_browser: u64,
    next_request: u64,
    pub pending: Option<Request>,
    pub mappings: BTreeMap<u64, u64>,
    receipt: Option<Receipt>,
    wake_posted: bool,
    pub adopt_enabled: bool,
    pub ready: Option<(Draft, u64)>,
}
impl Handoff {
    pub fn enable_adoption(&mut self) -> bool {
        if self.lost || self.generation == 0 {
            return false;
        }
        self.adopt_enabled = true;
        true
    }
    pub fn ready_current(&self, s: &Startup) -> bool {
        self.ready.as_ref().is_some_and(|(saved, _)| {
            s.selected == saved.id && s.tabs.iter().any(|tab| tab == saved)
        })
    }
    fn adoptable(&self, s: &Startup) -> bool {
        self.adopt_enabled
            && s.submissions.is_empty()
            && !s.tabs.iter().any(|t| t.composing)
            && s.tabs.iter().any(|t| self.mappings.contains_key(&t.id))
            && (!self.ready_current(s) || s.tabs.len() > 1)
    }
    pub fn commit_adoption(&mut self, s: &mut Startup, browser: u64) -> bool {
        if self.lost
            || self.generation == 0
            || self.pending.is_some()
            || !s.submissions.is_empty()
            || !self.ready_current(s)
        {
            return false;
        }
        let (draft, target) = self.ready.as_ref().unwrap();
        if browser != *target || draft.composing || s.tabs.len() != 1 {
            return false;
        }
        let id = draft.id;
        if !s.close(id) {
            return false;
        }
        s.selected = 0;
        self.mappings.remove(&id);
        self.editor_browser = browser;
        self.ready = None;
        true
    }
    /// Explicit editor switch, serialized with startup side effects. Drafts
    /// retain their text/range; the selection changes only on terminal ack.
    pub fn select_editor(
        &mut self,
        startup: &Startup,
        draft: u64,
        browser: u64,
        local: bool,
    ) -> bool {
        if browser == 0
            || self.pending.is_some()
            || !startup.submissions.is_empty()
            || self
                .mappings
                .keys()
                .any(|id| !startup.tabs.iter().any(|tab| tab.id == *id))
            || startup
                .tabs
                .iter()
                .any(|tab| !self.mappings.contains_key(&tab.id))
        {
            return false;
        }
        let Some(tab) = startup.tabs.iter().find(|tab| tab.id == draft) else {
            return false;
        };
        if tab.composing || !self.mappings.contains_key(&draft) {
            return false;
        }
        if local {
            if startup.selected != 0
                || self.editor_browser == 0
                || self.mappings.get(&draft) != Some(&browser)
            {
                return false;
            }
        } else if startup.selected != draft
            || self.editor_browser != 0
            || self.mappings.values().any(|id| *id == browser)
        {
            return false;
        }
        self.issue(Action::Select {
            draft,
            browser,
            local,
        })
    }
    /// Retire only the untouched automatic placeholder. The selected browser
    /// is authoritative; no browser tab is created, closed or repurposed.
    pub fn adopt_placeholder(&mut self, startup: &mut Startup, browser: u64) -> bool {
        if browser == 0 || !self.placeholder_ready(startup) {
            return false;
        }
        let selected = startup.selected == 1;
        if !startup.close(1) {
            return false;
        }
        if selected {
            self.editor_browser = browser;
        }
        true
    }
    fn placeholder_ready(&self, startup: &Startup) -> bool {
        self.generation != 0
            && !self.lost
            && self.editor_browser == 0
            && self.pending.is_none()
            && !self.mappings.contains_key(&1)
            && startup.submissions.is_empty()
            && self
                .mappings
                .keys()
                .all(|id| startup.tabs.iter().any(|tab| tab.id == *id))
            && (startup.selected != 1 || startup.tabs.len() == 1)
            && startup.tabs.iter().any(|tab| {
                tab.id == 1
                    && !tab.touched
                    && !tab.composing
                    && tab.text.is_empty()
                    && tab.revision == 0
            })
    }
    /// Release local editing only after every draft and leased side effect has
    /// settled. The FFI additionally validates the selected published browser.
    pub fn finish_editor(&mut self, startup: &Startup, browser: u64) -> bool {
        if browser == 0
            || self.generation == 0
            || self.lost
            || !startup.is_closed()
            || self.pending.is_some()
            || !startup.tabs.is_empty()
            || !startup.submissions.is_empty()
            || !self.mappings.is_empty()
        {
            return false;
        }
        self.editor_browser = browser;
        true
    }
    fn has_work(&self, startup: &Startup) -> bool {
        self.pending.is_some()
            || !startup.submissions.is_empty()
            || self.placeholder_ready(startup)
            || self.adoptable(startup)
            || (startup.is_closed() && self.editor_browser == 0)
            || self
                .mappings
                .keys()
                .any(|id| !startup.tabs.iter().any(|tab| tab.id == *id))
            || startup
                .tabs
                .iter()
                .any(|tab| tab.touched && !self.mappings.contains_key(&tab.id))
    }
    /// Coalesced notification claim. The host posts only after Rust returns.
    pub fn notify(&mut self, startup: &Startup) -> bool {
        if self.lost || self.generation == 0 || self.wake_posted || !self.has_work(startup) {
            return false;
        }
        self.wake_posted = true;
        true
    }
    pub fn notification_failed(&mut self) -> bool {
        self.wake_posted = false;
        true
    }
    /// Select one command without consuming it. Inspect/retry returns the same
    /// lease. The adapter must pause on rejection, never spin on a denied close.
    pub fn next(&mut self, startup: &Startup) -> bool {
        if self.lost || self.generation == 0 {
            return false;
        }
        self.wake_posted = false;
        if self.pending.is_some() {
            return true;
        }
        if let Some(draft) = self
            .mappings
            .keys()
            .copied()
            .find(|id| !startup.tabs.iter().any(|tab| tab.id == *id))
        {
            return self.close(startup, draft);
        }
        if let Some(submission) = startup.submissions.front() {
            return if self.mappings.contains_key(&submission.tab) {
                self.submit(startup)
            } else {
                self.create(startup, submission.tab)
            };
        }
        // Keep the untouched automatic placeholder local until the presenter
        // can atomically replace it with Firefox's restored selection. A blank
        // draft is not authorization to create a duplicate browser tab.
        if let Some(tab) = startup
            .tabs
            .iter()
            .find(|tab| tab.touched && !self.mappings.contains_key(&tab.id))
        {
            return self.create(startup, tab.id);
        }
        if self.adoptable(startup) {
            self.ready = None;
            let tab = startup
                .tabs
                .iter()
                .filter(|t| self.mappings.contains_key(&t.id))
                .min_by_key(|t| t.id == startup.selected)
                .unwrap();
            return self.issue(Action::Adopt {
                draft: tab.clone(),
                browser: self.mappings[&tab.id],
                selected: tab.id == startup.selected,
            });
        }
        true // Idle is successful, not a retryable failure.
    }
    /// Set once, from the host's resolved profile/privacy context. No text or
    /// profile path crosses this API. A remote endpoint must echo this token.
    pub fn prepare(&mut self, context: u64) -> bool {
        if context == 0 || self.context != 0 || self.generation != 0 || self.lost {
            return false;
        }
        self.context = context;
        true
    }
    pub fn bind(&mut self, context: u64, generation: u64) -> bool {
        if generation == 0
            || context == 0
            || context != self.context
            || self.generation != 0
            || self.lost
        {
            return false;
        }
        self.generation = generation;
        true
    }
    fn issue(&mut self, action: Action) -> bool {
        if self.generation == 0 || self.lost || self.pending.is_some() {
            return false;
        }
        let Some(id) = self.next_request.checked_add(1) else {
            return false;
        };
        self.next_request = id;
        self.pending = Some(Request {
            generation: self.generation,
            id,
            action,
        });
        true
    }
    /// Create means a NEW empty Firefox tab. It must never repurpose a restored,
    /// policy or command-line tab. Adoption of an untouched initial draft is a
    /// separate future reconciliation operation, not inferred here.
    pub fn create(&mut self, startup: &Startup, draft: u64) -> bool {
        if !startup.tabs.iter().any(|t| t.id == draft)
            || self.mappings.contains_key(&draft)
            || self.mappings.len() >= STARTUP_MAX_TABS
        {
            return false;
        }
        self.issue(Action::Create { draft })
    }
    /// Lease the oldest local submission. A repeated inspect returns the same
    /// request; it does not remove text or claim execution. No later submission
    /// may jump ahead of an unmapped target.
    pub fn submit(&mut self, startup: &Startup) -> bool {
        if self.pending.is_some() || self.lost || self.generation == 0 {
            return false;
        }
        let Some(submission) = startup.submissions.front() else {
            return false;
        };
        let Some(browser) = self.mappings.get(&submission.tab).copied() else {
            return false;
        };
        self.issue(Action::Submit {
            browser,
            submission: submission.clone(),
        })
    }
    /// A draft disappearing after Create was issued still requires a matching
    /// Create acknowledgment and an explicit Firefox-owned close operation.
    pub fn close(&mut self, startup: &Startup, draft: u64) -> bool {
        if startup.tabs.iter().any(|t| t.id == draft) {
            return false;
        }
        let Some(browser) = self.mappings.get(&draft).copied() else {
            return false;
        };
        self.issue(Action::Close { draft, browser })
    }
    /// accepted means terminal completion, not merely receipt. A browser-side
    /// rejection is terminal for that command; the adapter must expose it.
    /// A denied Close retains the mapping so reconciliation cannot forget it.
    pub fn complete(
        &mut self,
        startup: &mut Startup,
        generation: u64,
        request: u64,
        browser: u64,
        accepted: bool,
    ) -> bool {
        if self.lost || generation == 0 || generation != self.generation {
            return false;
        }
        let receipt = Receipt {
            request,
            browser,
            accepted,
        };
        if self.receipt.as_ref() == Some(&receipt) {
            return true;
        }
        let Some(pending) = self.pending.as_ref() else {
            return false;
        };
        if request != pending.id {
            return false;
        }
        match &pending.action {
            Action::Adopt {
                draft,
                browser: target,
                selected,
            } => {
                if browser != *target {
                    return false;
                }
                if accepted
                    && startup.submissions.is_empty()
                    && startup.tabs.iter().any(|tab| tab == draft)
                    && (startup.selected == draft.id) == *selected
                {
                    if *selected {
                        self.ready = Some((draft.clone(), browser));
                    } else {
                        startup.close(draft.id);
                        self.mappings.remove(&draft.id);
                    }
                }
            }
            Action::Create { draft } => {
                if accepted {
                    if browser == 0 || self.mappings.values().any(|id| *id == browser) {
                        return false;
                    }
                    self.mappings.insert(*draft, browser);
                } else if browser != 0 {
                    return false;
                }
            }
            Action::Submit {
                browser: target,
                submission,
            } => {
                if browser != *target {
                    return false;
                }
                // A local close may already have cancelled this submission.
                // Never remove a newer request just because it is now first.
                startup.submissions.retain(|item| item.id != submission.id);
            }
            Action::Close {
                draft,
                browser: target,
            } => {
                if browser != *target {
                    return false;
                }
                if accepted {
                    self.mappings.remove(draft);
                }
            }
            Action::Select {
                draft,
                browser: target,
                local,
            } => {
                if browser != *target {
                    return false;
                }
                if accepted {
                    if *local {
                        if !startup.select(*draft) {
                            return false;
                        }
                        self.editor_browser = 0;
                    } else {
                        if startup.selected != *draft {
                            return false;
                        }
                        if !startup.park_editor() {
                            return false;
                        }
                        self.editor_browser = *target;
                    }
                }
            }
        }
        self.pending = None;
        self.receipt = Some(receipt);
        true
    }
    /// Terminal until explicit recovery is designed. Keep the in-flight request
    /// and editor state for diagnosis; never replay it into a new generation.
    pub fn disconnect(&mut self, generation: u64) -> bool {
        if generation == 0 || generation != self.generation || self.lost {
            return false;
        }
        self.lost = true;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn adoption() -> (Startup, Handoff) {
        let mut s = Startup::default();
        s.add();
        s.editor(1, &[65, 66, 67, 68], 1, 2, false);
        let mut h = Handoff::default();
        h.prepare(42);
        h.bind(42, 7);
        h.create(&s, 1);
        assert!(h.complete(&mut s, 7, 1, 90, true));
        h.enable_adoption();
        (s, h)
    }
    #[test]
    fn adoption_waits_for_composition_and_preserves_exact_range() {
        let (mut s, mut h) = adoption();
        s.editor(1, &[65, 66, 67, 68], 1, 2, true);
        assert!(h.next(&s));
        assert!(h.pending.is_none());
        s.editor(1, &[65, 66, 67, 68], 1, 2, false);
        assert!(h.notify(&s));
        assert!(h.next(&s));
        assert!(
            matches!(&h.pending.as_ref().unwrap().action,Action::Adopt {draft,selected:true,..} if draft.selection_start==1 && draft.selection_length==2)
        );
        assert!(h.complete(&mut s, 7, 2, 90, true));
        assert_eq!(s.tabs.len(), 1);
        assert!(!h.commit_adoption(&mut s, 91));
        assert!(h.commit_adoption(&mut s, 90));
        assert!(s.tabs.is_empty());
        assert!(h.mappings.is_empty());
        assert_eq!(h.editor_browser, 90);
        h.next(&s);
        assert!(h.pending.is_none()); // adoption must never close its browser tab
    }
    #[test]
    fn edit_caret_and_composition_races_keep_the_live_draft() {
        for change in 0..3 {
            let (mut s, mut h) = adoption();
            h.next(&s);
            match change {
                0 => {
                    s.editor(1, &[65, 66, 67, 69], 1, 2, false);
                }
                1 => {
                    s.editor(1, &[65, 66, 67, 68], 2, 0, false);
                }
                _ => {
                    s.editor(1, &[65, 66, 67, 68], 1, 2, true);
                }
            }
            assert!(h.complete(&mut s, 7, 2, 90, true));
            assert!(!h.commit_adoption(&mut s, 90));
            assert_eq!(s.tabs.len(), 1);
            assert_eq!(h.mappings.get(&1), Some(&90));
            h.next(&s);
            assert_eq!(h.pending.is_some(), change != 2);
        }
    }
    #[test]
    fn publication_race_and_new_submission_cannot_retire_draft() {
        let (mut s, mut h) = adoption();
        h.next(&s);
        h.complete(&mut s, 7, 2, 90, true);
        s.editor(1, &[65, 66, 67, 68], 0, 4, false);
        assert!(!h.commit_adoption(&mut s, 90));
        h.next(&s);
        s.submit(1, 3);
        h.complete(&mut s, 7, 3, 90, true);
        assert!(!h.commit_adoption(&mut s, 90));
        h.next(&s);
        assert!(
            matches!(&h.pending.as_ref().unwrap().action,Action::Submit {submission,..} if submission.modifiers==3)
        );
    }
    #[test]
    fn close_during_import_still_requires_explicit_browser_close() {
        let (mut s, mut h) = adoption();
        h.next(&s);
        s.close(1);
        h.complete(&mut s, 7, 2, 90, true);
        h.next(&s);
        assert!(matches!(
            h.pending.as_ref().unwrap().action,
            Action::Close { browser: 90, .. }
        ));
    }
    #[test]
    fn background_drafts_retire_before_selected_and_new_tabs_do_not_deadlock() {
        let (mut s, mut h) = adoption();
        h.next(&s);
        h.complete(&mut s, 7, 2, 90, true);
        s.add();
        s.select(1);
        assert!(!h.commit_adoption(&mut s, 90));
        h.next(&s);
        assert!(matches!(
            h.pending.as_ref().unwrap().action,
            Action::Create { draft: 2 }
        ));
        h.complete(&mut s, 7, 3, 91, true);
        h.next(&s);
        assert!(matches!(
            h.pending.as_ref().unwrap().action,
            Action::Adopt {
                browser: 91,
                selected: false,
                ..
            }
        ));
        h.complete(&mut s, 7, 4, 91, true);
        assert_eq!(s.selected, 1);
        assert_eq!(s.tabs.len(), 1);
        h.next(&s);
        h.complete(&mut s, 7, 5, 90, true);
        assert!(h.commit_adoption(&mut s, 90));
    }
    #[test]
    fn editor_switch_cannot_overtake_background_placeholder_retirement() {
        let mut s = Startup::default();
        s.add();
        s.add();
        let mut h = Handoff::default();
        h.prepare(42);
        h.bind(42, 7);
        assert!(h.create(&s, 2));
        assert!(h.complete(&mut s, 7, 1, 91, true));
        assert!(!h.select_editor(&s, 2, 90, false));
        assert!(h.adopt_placeholder(&mut s, 91));
        assert_eq!(s.selected, 2);
        assert_eq!(h.mappings.get(&2), Some(&91));
        assert_eq!(h.editor_browser, 0);
        assert!(h.select_editor(&s, 2, 90, false));
    }
    #[test]
    fn ending_empty_composition_wakes_placeholder_reconciliation() {
        let mut s = Startup::default();
        s.add();
        let mut h = Handoff::default();
        h.prepare(42);
        h.bind(42, 7);
        s.editor(1, &[], 0, 0, true);
        assert!(!h.notify(&s));
        s.editor(1, &[], 0, 0, false);
        assert!(h.notify(&s));
        assert!(!h.notify(&s));
        h.next(&s);
        assert!(h.pending.is_none());
        assert!(h.adopt_placeholder(&mut s, 90));
        assert!(!h.notify(&s));
    }
    #[test]
    fn placeholder_adoption_never_discards_user_intent() {
        let fresh = || {
            let mut s = Startup::default();
            s.add();
            let mut h = Handoff::default();
            h.prepare(42);
            h.bind(42, 7);
            (s, h)
        };
        let (mut s, mut h) = fresh();
        assert!(!h.adopt_placeholder(&mut s, 0));
        assert!(h.adopt_placeholder(&mut s, 90));
        assert!(s.is_closed());
        assert_eq!(h.editor_browser, 90);
        assert!(h.pending.is_none());
        assert!(!h.adopt_placeholder(&mut s, 91));
        let (mut s, mut h) = fresh();
        s.editor(1, &[], 0, 0, true);
        assert!(!h.adopt_placeholder(&mut s, 90));
        s.editor(1, &[], 0, 0, false);
        assert!(h.adopt_placeholder(&mut s, 90));
        let (mut s, mut h) = fresh();
        s.edit(1, &[65]);
        s.edit(1, &[]);
        assert!(!h.adopt_placeholder(&mut s, 90));
        let (mut s, mut h) = fresh();
        s.add();
        assert!(h.adopt_placeholder(&mut s, 90));
        assert_eq!(s.selected, 2);
        assert_eq!(s.tabs.len(), 1);
        assert_eq!(h.editor_browser, 0);
        let (mut s, mut h) = fresh();
        s.add();
        s.select(1);
        assert!(!h.adopt_placeholder(&mut s, 90));
        let (mut s, mut h) = fresh();
        h.create(&s, 1);
        assert!(!h.adopt_placeholder(&mut s, 90));
        let (mut s, mut h) = fresh();
        h.disconnect(7);
        assert!(!h.adopt_placeholder(&mut s, 90));
    }
    #[test]
    fn mixed_editor_switch_preserves_draft_and_requires_matching_ack() {
        let (mut s, mut h) = fixture();
        s.submissions.clear();
        h.create(&s, 1);
        h.complete(&mut s, 9, 1, 90, true);
        s.editor(1, &[66, 67, 68], 1, 1, true);
        assert!(!h.select_editor(&s, 1, 91, false));
        s.editor(1, &[66, 67, 68], 1, 1, false);
        let saved = s.tabs[0].clone();
        assert!(!h.select_editor(&s, 1, 90, false));
        assert!(h.select_editor(&s, 1, 91, false));
        let request = h.pending.clone();
        assert!(!h.select_editor(&s, 1, 92, false));
        assert_eq!(h.pending, request);
        assert_eq!(s.selected, 1);
        assert_eq!(h.editor_browser, 0);
        assert!(!h.complete(&mut s, 8, 2, 91, true));
        assert!(!h.complete(&mut s, 9, 2, 92, true));
        assert!(h.complete(&mut s, 9, 2, 91, true));
        assert_eq!(s.selected, 0);
        assert_eq!(h.editor_browser, 91);
        assert_eq!(s.tabs[0], saved);
        assert!(h.complete(&mut s, 9, 2, 91, true));
        assert!(h.select_editor(&s, 1, 90, true));
        assert!(h.complete(&mut s, 9, 3, 90, true));
        assert_eq!(s.selected, 1);
        assert_eq!(h.editor_browser, 0);
        assert_eq!(s.tabs[0], saved);
    }
    #[test]
    fn selection_refusal_is_recoverable_but_cannot_overtake_submissions() {
        let (mut s, mut h) = fixture();
        assert!(!h.select_editor(&s, 1, 91, false));
        h.create(&s, 1);
        h.complete(&mut s, 9, 1, 90, true);
        assert!(!h.select_editor(&s, 1, 91, false));
        h.submit(&s);
        h.complete(&mut s, 9, 2, 90, true);
        assert!(h.select_editor(&s, 1, 91, false));
        assert!(h.complete(&mut s, 9, 3, 91, false));
        assert_eq!(s.selected, 1);
        assert_eq!(h.editor_browser, 0);
        assert!(h.pending.is_none());
        assert!(h.select_editor(&s, 1, 92, false));
        assert!(h.complete(&mut s, 9, 4, 92, true));
    }
    #[test]
    fn editor_release_requires_terminal_cleanup_and_live_binding() {
        let (mut s, mut h) = fixture();
        assert!(!h.finish_editor(&s, 90));
        h.create(&s, 1);
        s.close(1);
        assert!(!h.finish_editor(&s, 90));
        h.complete(&mut s, 9, 1, 91, true);
        assert!(!h.finish_editor(&s, 90));
        h.close(&s, 1);
        assert!(!h.finish_editor(&s, 90));
        h.complete(&mut s, 9, 2, 91, true);
        assert!(!h.finish_editor(&s, 0));
        assert!(h.finish_editor(&s, 90));
        assert_eq!(h.editor_browser, 90);
        h.disconnect(9);
        assert!(!h.finish_editor(&s, 92));
        assert_eq!(h.editor_browser, 90);
    }
    #[test]
    fn closing_untouched_placeholder_wakes_editor_release_without_a_browser_command() {
        let mut s = Startup::default();
        s.add();
        let mut h = Handoff::default();
        h.prepare(42);
        h.bind(42, 7);
        assert!(h.notify(&s));
        h.next(&s);
        s.close(1);
        assert!(h.notify(&s));
        assert!(!h.notify(&s));
        assert!(h.next(&s));
        assert!(h.pending.is_none());
        assert!(h.finish_editor(&s, 90));
        assert!(!h.notify(&s));
    }
    #[test]
    fn wake_claims_coalesce_and_failed_posts_can_be_retried() {
        let (s, mut h) = fixture();
        assert!(h.notify(&s));
        assert!(!h.notify(&s));
        h.notification_failed();
        assert!(h.notify(&s));
        assert!(h.next(&s));
        assert!(h.notify(&s));
        assert!(!h.notify(&s));
        h.disconnect(9);
        assert!(!h.notify(&s));
    }
    #[test]
    fn untouched_placeholder_waits_for_restore_while_explicit_tabs_and_edits_survive() {
        let mut s = Startup::default();
        s.add();
        let mut h = Handoff::default();
        h.prepare(44);
        h.bind(44, 9);
        assert!(h.notify(&s));
        assert!(h.next(&s));
        assert!(h.pending.is_none());
        s.add();
        assert!(h.notify(&s));
        assert!(h.next(&s));
        assert!(matches!(
            h.pending.as_ref().unwrap().action,
            Action::Create { draft: 2 }
        ));
        h.complete(&mut s, 9, 1, 90, true);
        assert!(h.next(&s));
        assert!(h.pending.is_none());
        s.edit(1, &[65]);
        s.edit(1, &[]); // Clearing text does not erase interaction.
        assert!(h.notify(&s));
        assert!(h.next(&s));
        assert!(matches!(
            h.pending.as_ref().unwrap().action,
            Action::Create { draft: 1 }
        ));
    }
    #[test]
    fn next_leases_in_order_and_never_consumes_or_overwrites_edits() {
        let (mut s, mut h) = fixture();
        assert!(h.next(&s));
        let first = h.pending.clone();
        assert!(h.next(&s));
        assert_eq!(h.pending, first);
        s.edit(1, &[66]);
        assert!(h.complete(&mut s, 9, 1, 90, true));
        assert!(h.next(&s));
        assert!(matches!(&h.pending.as_ref().unwrap().action,
            Action::Submit {submission,..} if submission.text == [65]));
        assert!(h.complete(&mut s, 9, 2, 90, true));
        assert_eq!(s.tabs[0].text, [66]);
        assert!(h.next(&s));
        assert!(h.pending.is_none());
        s.close(1);
        assert!(h.next(&s));
        assert!(matches!(
            h.pending.as_ref().unwrap().action,
            Action::Close { draft: 1, .. }
        ));
        assert!(h.disconnect(9));
        assert!(!h.next(&s));
    }
    fn fixture() -> (Startup, Handoff) {
        let mut s = Startup::default();
        s.add();
        s.edit(1, &[65]);
        s.submit(1, 3);
        let mut h = Handoff::default();
        assert!(h.prepare(44));
        assert!(h.bind(44, 9));
        (s, h)
    }
    #[test]
    fn mapping_never_overwrites_newer_edits_and_submit_requires_completion() {
        let (mut s, mut h) = fixture();
        assert!(h.create(&s, 1));
        s.editor(1, &[66, 67], 1, 1, true);
        assert!(!h.complete(&mut s, 8, 1, 90, true));
        assert!(h.complete(&mut s, 9, 1, 90, true));
        assert_eq!(s.tabs[0].text, [66, 67]);
        assert!(s.tabs[0].composing);
        assert_eq!(s.tabs[0].selection_length, 1);
        assert!(h.submit(&s));
        let pending = h.pending.clone();
        assert!(!h.submit(&s));
        assert_eq!(h.pending, pending);
        match &h.pending.as_ref().unwrap().action {
            Action::Submit { submission, .. } => {
                assert_eq!(submission.text, [65]);
                assert_eq!(submission.modifiers, 3);
            }
            _ => panic!("expected submit"),
        }
        assert_eq!(s.submissions.len(), 1);
        assert!(h.complete(&mut s, 9, 2, 90, true));
        assert!(s.submissions.is_empty());
        assert!(h.complete(&mut s, 9, 2, 90, true));
        assert!(!h.complete(&mut s, 9, 2, 90, false));
    }
    #[test]
    fn close_during_create_requires_cleanup_and_denial_retains_mapping() {
        let (mut s, mut h) = fixture();
        h.create(&s, 1);
        s.close(1);
        assert!(h.complete(&mut s, 9, 1, 90, true));
        assert!(h.close(&s, 1));
        assert!(h.complete(&mut s, 9, 2, 90, false));
        assert_eq!(h.mappings.get(&1), Some(&90));
        assert!(h.close(&s, 1));
        assert!(h.complete(&mut s, 9, 3, 90, true));
        assert!(h.mappings.is_empty());
    }
    #[test]
    fn late_submit_ack_cannot_remove_another_tabs_submission() {
        let (mut s, mut h) = fixture();
        h.create(&s, 1);
        h.complete(&mut s, 9, 1, 90, true);
        h.submit(&s);
        s.add();
        s.edit(2, &[66]);
        s.submit(2, 0);
        s.close(1);
        assert!(h.complete(&mut s, 9, 2, 90, true));
        assert_eq!(s.submissions.len(), 1);
        assert_eq!(s.submissions[0].tab, 2);
    }
    #[test]
    fn context_endpoint_loss_and_duplicate_browser_ids_fail_closed() {
        let mut unbound = Handoff::default();
        assert!(unbound.prepare(44));
        assert!(!unbound.bind(45, 9));
        assert_eq!(unbound.generation, 0);
        assert!(unbound.bind(44, 9));
        let (mut s, mut h) = fixture();
        assert!(!h.bind(45, 10));
        assert!(!h.prepare(45));
        h.create(&s, 1);
        h.complete(&mut s, 9, 1, 90, true);
        s.add();
        h.create(&s, 2);
        assert!(!h.complete(&mut s, 9, 2, 90, true));
        assert!(h.pending.is_some());
        assert!(h.disconnect(9));
        assert!(!h.complete(&mut s, 9, 2, 91, true));
        assert!(!h.bind(44, 10));
        assert!(!h.submit(&s));
        assert_eq!(s.tabs.len(), 2);
        assert_eq!(s.submissions.len(), 1);
    }
    #[test]
    fn rejected_create_does_not_release_navigation() {
        let (mut s, mut h) = fixture();
        h.create(&s, 1);
        assert!(h.complete(&mut s, 9, 1, 0, false));
        assert!(!h.submit(&s));
        assert_eq!(s.submissions.len(), 1);
        assert_eq!(s.tabs[0].text, [65]);
    }
    #[test]
    fn orphan_mappings_are_bounded_and_cleanup_releases_capacity() {
        let (mut s, mut h) = fixture();
        h.create(&s, 1);
        h.complete(&mut s, 9, 1, 90, true);
        for draft in 2..=STARTUP_MAX_TABS as u64 {
            assert!(s.add());
            assert!(h.create(&s, draft));
            let request = h.pending.as_ref().unwrap().id;
            assert!(h.complete(&mut s, 9, request, 90 + draft, true));
            assert!(s.close(draft));
        }
        s.add();
        let draft = s.selected;
        assert!(!h.create(&s, draft));
        assert!(h.close(&s, 2));
        let request = h.pending.as_ref().unwrap().id;
        assert!(h.complete(&mut s, 9, request, 92, true));
        assert!(h.create(&s, draft));
    }
    #[test]
    fn mapping_a_later_tab_does_not_reorder_submissions() {
        let (mut s, mut h) = fixture();
        s.add();
        s.edit(2, &[66]);
        s.submit(2, 0);
        h.create(&s, 2);
        h.complete(&mut s, 9, 1, 91, true);
        assert!(!h.submit(&s));
        assert_eq!(s.submissions.len(), 2);
        h.create(&s, 1);
        h.complete(&mut s, 9, 2, 90, true);
        assert!(h.submit(&s));
        match &h.pending.as_ref().unwrap().action {
            Action::Submit { submission, .. } => assert_eq!(submission.tab, 1),
            _ => panic!("expected first tab submission"),
        }
    }
}
