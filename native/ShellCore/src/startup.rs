//! Local interaction before a browser endpoint exists. IDs in this model are
//! draft identities, never Firefox tab IDs. Submission text is owned/immutable.
use std::collections::VecDeque;

pub const STARTUP_MAX_TABS: usize = 32;
pub const STARTUP_MAX_TEXT: usize = 32768;
pub const STARTUP_MAX_SUBMISSIONS: usize = 64;
pub const STARTUP_MAX_UNITS: usize = 262144;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Draft {
    pub id: u64,
    pub revision: u64,
    pub text: Vec<u16>,
    pub touched: bool,
    pub selection_start: u32,
    pub selection_length: u32,
    pub composing: bool,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Submission {
    pub id: u64,
    pub tab: u64,
    pub revision: u64,
    pub modifiers: u32,
    pub text: Vec<u16>,
}
#[derive(Default)]
pub struct Startup {
    pub tabs: Vec<Draft>,
    pub selected: u64,
    pub submissions: VecDeque<Submission>,
    next_tab: u64,
    next_request: u64,
    closed: bool,
}
impl Startup {
    pub fn is_closed(&self) -> bool {
        self.closed
    }
    fn composing(&self) -> bool {
        self.tabs
            .iter()
            .any(|t| t.id == self.selected && t.composing)
    }
    pub fn park_editor(&mut self) -> bool {
        if self.closed || self.selected == 0 || self.composing() {
            return false;
        }
        self.selected = 0;
        true
    }
    fn units(&self) -> usize {
        self.tabs.iter().map(|t| t.text.len()).sum::<usize>()
            + self.submissions.iter().map(|s| s.text.len()).sum::<usize>()
    }
    pub fn add(&mut self) -> bool {
        if self.closed || self.composing() || self.tabs.len() == STARTUP_MAX_TABS {
            return false;
        }
        let Some(id) = self.next_tab.checked_add(1) else {
            return false;
        };
        self.next_tab = id;
        self.tabs.push(Draft {
            id,
            revision: 0,
            text: vec![],
            touched: id != 1,
            selection_start: 0,
            selection_length: 0,
            composing: false,
        });
        self.selected = id;
        true
    }
    pub fn edit(&mut self, id: u64, text: &[u16]) -> bool {
        let Some(index) = self.tabs.iter().position(|t| t.id == id) else {
            return false;
        };
        if self.closed
            || text.len() > STARTUP_MAX_TEXT
            || self.units() - self.tabs[index].text.len() + text.len() > STARTUP_MAX_UNITS
        {
            return false;
        }
        let tab = &mut self.tabs[index];
        if tab.text == text {
            return true;
        }
        let Some(revision) = tab.revision.checked_add(1) else {
            return false;
        };
        tab.text = text.to_vec();
        tab.revision = revision;
        tab.touched = true;
        tab.selection_start = text.len() as u32;
        tab.selection_length = 0;
        true
    }
    /// Capture text, selection and composition as one validated observation.
    /// Rejection leaves the whole draft unchanged, including its edit revision.
    pub fn editor(
        &mut self,
        id: u64,
        text: &[u16],
        start: u32,
        length: u32,
        composing: bool,
    ) -> bool {
        let Some(end) = start.checked_add(length) else {
            return false;
        };
        let boundary = |n: usize| {
            n <= text.len()
                && (n == 0
                    || n == text.len()
                    || !(matches!(text[n - 1], 0xd800..=0xdbff)
                        && matches!(text[n], 0xdc00..=0xdfff)))
        };
        if !boundary(start as usize) || !boundary(end as usize) {
            return false;
        }
        if !self.edit(id, text) {
            return false;
        }
        let tab = self.tabs.iter_mut().find(|t| t.id == id).unwrap();
        tab.selection_start = start;
        tab.selection_length = length;
        tab.composing = composing;
        true
    }
    pub fn select(&mut self, id: u64) -> bool {
        if self.closed
            || (id != self.selected && self.composing())
            || !self.tabs.iter().any(|t| t.id == id)
        {
            return false;
        }
        // Explicitly returning to the initial blank is user intent to retain it.
        if self.selected != id {
            self.tabs.iter_mut().find(|t| t.id == id).unwrap().touched = true;
        }
        self.selected = id;
        true
    }
    pub fn reorder(&mut self, id: u64, index: usize) -> bool {
        if self.closed || index >= self.tabs.len() {
            return false;
        }
        let Some(old) = self.tabs.iter().position(|t| t.id == id) else {
            return false;
        };
        let tab = self.tabs.remove(old);
        self.tabs.insert(index, tab);
        true
    }
    pub fn close(&mut self, id: u64) -> bool {
        // Cancellation never needs spare text/queue capacity. The native
        // presenter prevents replacing its live editor during IME composition.
        if self.closed {
            return false;
        }
        let Some(index) = self.tabs.iter().position(|t| t.id == id) else {
            return false;
        };
        self.tabs.remove(index);
        self.submissions.retain(|s| s.tab != id);
        if self.selected == id {
            self.selected = self
                .tabs
                .get(index.min(self.tabs.len().saturating_sub(1)))
                .map_or(0, |t| t.id);
        }
        if self.tabs.is_empty() {
            self.closed = true;
        }
        true
    }
    pub fn submit(&mut self, id: u64, modifiers: u32) -> bool {
        let Some(tab) = self.tabs.iter().find(|t| t.id == id) else {
            return false;
        };
        if self.closed
            || tab.composing
            || tab.text.is_empty()
            || modifiers & !7 != 0
            || self.submissions.len() == STARTUP_MAX_SUBMISSIONS
            || self.units() + tab.text.len() > STARTUP_MAX_UNITS
        {
            return false;
        }
        let Some(request) = self.next_request.checked_add(1) else {
            return false;
        };
        self.submissions.push_back(Submission {
            id: request,
            tab: id,
            revision: tab.revision,
            modifiers,
            text: tab.text.clone(),
        });
        self.next_request = request;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn submissions_survive_edits_and_keep_order_and_modifiers() {
        let mut s = Startup::default();
        assert!(s.add());
        assert!(s.edit(1, &[65]));
        assert!(s.submit(1, 3));
        assert!(s.edit(1, &[66]));
        assert!(s.submit(1, 0));
        assert_eq!(s.submissions[0].text, [65]);
        assert_eq!(s.submissions[0].modifiers, 3);
        assert_eq!(s.submissions[1].text, [66]);
        assert_ne!(s.submissions[0].id, s.submissions[1].id);
    }
    #[test]
    fn cancellation_remains_available_at_capacity() {
        let mut s = Startup::default();
        s.add();
        s.edit(1, &[65]);
        for _ in 0..STARTUP_MAX_SUBMISSIONS {
            assert!(s.submit(1, 0));
        }
        assert!(!s.submit(1, 0));
        assert!(s.close(1));
        assert!(s.submissions.is_empty());
        assert!(!s.add());
    }
    #[test]
    fn text_budget_rejection_is_atomic() {
        let mut s = Startup::default();
        s.add();
        assert!(s.edit(1, &vec![65; STARTUP_MAX_TEXT]));
        for _ in 0..7 {
            assert!(s.submit(1, 0));
        }
        assert!(!s.submit(1, 0));
        assert!(!s.edit(1, &vec![66; STARTUP_MAX_TEXT + 1]));
        assert_eq!(s.tabs[0].revision, 1);
        assert!(s.edit(1, &[66]));
    }
    #[test]
    fn tabs_have_stable_ids_and_local_selection() {
        let mut s = Startup::default();
        s.add();
        s.add();
        s.add();
        assert!(s.reorder(3, 0));
        assert_eq!(s.tabs[0].id, 3);
        assert!(s.select(2));
        assert!(s.close(2));
        assert_eq!(s.selected, 1);
        assert!(!s.edit(2, &[65]));
        assert!(!s.submit(2, 0));
        assert!(!s.reorder(1, 9));
        s.add();
        assert_eq!(s.selected, 4);
    }
    #[test]
    fn tab_limit_and_unmodified_initial_draft() {
        let mut s = Startup::default();
        for _ in 0..STARTUP_MAX_TABS {
            assert!(s.add());
        }
        assert!(!s.add());
        assert!(!s.tabs[0].touched);
        assert!(s.tabs[1].touched);
        assert!(!s.submit(1, 0));
        assert!(s.edit(1, &[0xd83d, 0xde00]));
        assert!(s.submit(1, 0));
        assert!(!s.submit(1, 8));
    }
    #[test]
    fn selection_survives_switches_and_invalid_ranges_are_atomic() {
        let mut s = Startup::default();
        s.add();
        assert!(s.editor(1, &[65, 66, 67], 1, 1, false));
        s.add();
        s.select(1);
        assert_eq!(
            (s.tabs[0].selection_start, s.tabs[0].selection_length),
            (1, 1)
        );
        let before = s.tabs[0].clone();
        assert!(!s.editor(1, &[0xd83d, 0xde00], 1, 0, false));
        assert!(!s.editor(1, &[65], u32::MAX, 2, false));
        assert_eq!(s.tabs[0], before);
    }
    #[test]
    fn composition_blocks_submit_and_destructive_switch_but_not_editing() {
        let mut s = Startup::default();
        s.add();
        s.add();
        s.select(1);
        assert!(s.editor(1, &[65], 1, 0, true));
        assert!(!s.add());
        assert!(!s.select(2));
        assert!(!s.submit(1, 0));
        assert!(s.close(2));
        assert!(s.editor(1, &[66], 1, 0, false));
        assert!(s.submit(1, 0));
        assert!(s.add());
        assert_eq!(s.submissions[0].text, [66]);
    }
    #[test]
    fn rejected_editor_observation_cannot_prevent_cancellation() {
        let mut s = Startup::default();
        s.add();
        s.add();
        s.select(1);
        assert!(s.editor(1, &vec![65; STARTUP_MAX_TEXT], 0, 0, true));
        assert!(!s.editor(1, &vec![66; STARTUP_MAX_TEXT + 1], 0, 0, false));
        assert!(s.close(1));
        assert_eq!(s.selected, 2);
    }
}
