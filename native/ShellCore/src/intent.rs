//! Which address-bar intent may be staged, and what happens to the one already
//! staged.
//!
//! The address surface can only ever have one intent outstanding. When the user
//! produces a second one before the first has been answered, something has to
//! give: the new one may replace the old, amend it, wait for a precondition, or
//! be dropped. Those rules exist because input arrives faster than the browser
//! answers -- typing while a query is in flight, pressing Enter before the edit
//! that produced the suggestions has been acknowledged, clicking a row while a
//! caret move is staged.
//!
//! The rules previously lived in `native/WinUIPane/Address.h` as a chain of
//! `wcscmp` comparisons against a JSON string the same file had just written,
//! and nothing tested them. They are framework-independent logic over two
//! enumerations and a handful of booleans, so by the project's language rule
//! they belong here, where every pair can be enumerated.
//!
//! What stays in C++ is everything that reads WinUI or Win32 to *produce* the
//! inputs: where focus is and whether an IME composition is active. AddressState
//! derives acknowledgment/readiness from applied browser publications.

/// The kinds of intent that share the single staging slot. Mode-menu intents
/// (reveal, open, pick, exit) travel on their own slot and are represented here
/// only by [`Conditions::mode_intent_staged`], which blocks this slot while one
/// is outstanding.
///
/// The numeric values cross the C boundary.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u32)]
pub enum IntentKind {
    /// The text changed (or an IME composition started or ended).
    Edit = 1,
    /// The caret moved or the selection range changed, with no text change.
    TextSelection = 2,
    /// A suggestion row was highlighted.
    Select = 3,
    /// A suggestion row, or the typed text, was committed.
    Activate = 4,
    /// The native field took focus.
    Focus = 5,
    /// The native field lost focus.
    Blur = 6,
    /// The surface was dismissed (Escape).
    Dismiss = 7,
    /// A global action attached to a suggestion row was invoked.
    GlobalAction = 8,
    /// The user asked to hand the surface back to Firefox's own address bar.
    /// It negotiates like any ordinary intent: it does not interrupt, because
    /// whatever is already staged is a thing the user did first.
    Fallback = 9,
    /// Firefox clipboard command carrying the current text and selection.
    Clipboard = 10,
}

impl IntentKind {
    pub fn from_raw(value: u32) -> Option<Self> {
        match value {
            1 => Some(Self::Edit),
            2 => Some(Self::TextSelection),
            3 => Some(Self::Select),
            4 => Some(Self::Activate),
            5 => Some(Self::Focus),
            6 => Some(Self::Blur),
            7 => Some(Self::Dismiss),
            8 => Some(Self::GlobalAction),
            9 => Some(Self::Fallback),
            10 => Some(Self::Clipboard),
            _ => None,
        }
    }

    /// Whether this kind is one of the two that may always interrupt whatever
    /// is staged, because the surface is going away regardless of what was
    /// about to be said about it.
    fn interrupts(self) -> bool {
        matches!(self, Self::Dismiss | Self::Blur)
    }
}

/// What the caller observed about the surface at the moment the intent arrived.
/// Control observations come from C++; AddressState supplies acknowledgment
/// and query readiness from the last applied publication.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
#[repr(C)]
pub struct Conditions {
    /// A publication is being applied to the controls right now. Anything the
    /// controls raise during that is an echo of the publication, not the user.
    pub applying: bool,
    /// An IME composition is in progress. Its intermediate states are not
    /// intents; the composition's end is.
    pub composing: bool,
    /// A session has been published. Without one there is nothing to name.
    pub has_session: bool,
    /// A mode-menu intent is outstanding on its own slot, which takes priority
    /// over this one.
    pub mode_intent_staged: bool,
    /// The browser has acknowledged the most recent edit. Committing before
    /// this would commit against suggestions computed for older text.
    pub edit_acknowledged: bool,
    /// The query has either finished or already produced rows, so there is
    /// something to commit to.
    pub query_settled: bool,
}

/// What the caller should do with the arriving intent.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u32)]
pub enum Decision {
    /// Nothing is staged: stage this one and queue it.
    Stage = 1,
    /// Replace the staged intent and queue the replacement. The queue reports
    /// the replaced request as superseded rather than dropping it silently.
    Replace = 2,
    /// Fold this into the staged intent instead of queueing a new one. Only
    /// ever a caret move arriving while an edit is staged: the edit is the
    /// thing in flight and it carries a selection range of its own.
    Amend = 3,
    /// Hold this intent until its precondition is met. The caller re-offers it
    /// when the next publication arrives, which is what makes the precondition
    /// observable.
    Defer = 4,
    /// Discard it. The staged intent is more important, or the surface is in a
    /// state where this intent means nothing.
    Drop = 5,
}

/// Decide what to do with `incoming`, given what is already `staged`.
///
/// `staged` is `None` when the slot is empty -- which, once delivery moved to
/// the request queue, means "no address request is outstanding" rather than
/// "nothing is waiting to be collected".
pub fn decide(
    incoming: IntentKind,
    staged: Option<IntentKind>,
    conditions: Conditions,
) -> Decision {
    // An echo of a publication is never an intent.
    if conditions.applying {
        return Decision::Drop;
    }

    // A text change is the one intent an IME composition produces rather than
    // suppresses: composition start and end are themselves edits, and carry the
    // composing flag so the browser can treat them as provisional. It also does
    // not negotiate -- the text the user can see is by definition the newest
    // thing they have said, so it replaces whatever was staged.
    if incoming == IntentKind::Edit {
        if !conditions.has_session {
            return Decision::Drop;
        }
        return if staged.is_some() { Decision::Replace } else { Decision::Stage };
    }

    // Every other kind is suppressed mid-composition: the intermediate states
    // of a composition are not commands.
    if conditions.composing {
        return Decision::Drop;
    }

    // A global action is the one kind that does not need a session of its own:
    // it names a row from the publication that produced it. It does require the
    // browser to be level with our edits, and it refuses to disturb anything
    // except a caret move.
    if incoming == IntentKind::GlobalAction {
        if !conditions.edit_acknowledged {
            return Decision::Drop;
        }
        return match staged {
            None => Decision::Stage,
            Some(IntentKind::TextSelection) => Decision::Replace,
            Some(_) => Decision::Drop,
        };
    }

    if !conditions.has_session {
        return Decision::Drop;
    }

    // A caret move is the least important intent there is. It never displaces
    // anything; it only folds into an edit, which already carries a range.
    if incoming == IntentKind::TextSelection {
        return match staged {
            None => Decision::Stage,
            Some(IntentKind::Edit) => Decision::Amend,
            Some(_) => Decision::Drop,
        };
    }

    // The mode menu owns the surface while one of its intents is outstanding.
    // Checked after the caret case so a selection change during a mode
    // interaction is dropped for the reason it is actually dropped.
    if conditions.mode_intent_staged {
        return Decision::Drop;
    }

    // Committing has two preconditions of its own, both about the browser
    // having caught up with what the user typed.
    if incoming == IntentKind::Activate
        && (!conditions.edit_acknowledged || !conditions.query_settled)
    {
        return Decision::Defer;
    }

    match staged {
        None => Decision::Stage,
        // The edit that is in flight is what the commit would commit against,
        // so wait for it rather than racing it.
        Some(IntentKind::Edit) if incoming == IntentKind::Activate => Decision::Defer,
        // Clipboard carries the current edit/range, so it can replace those
        // publications without waiting for suggestions.
        Some(IntentKind::Edit | IntentKind::TextSelection) if incoming == IntentKind::Clipboard => Decision::Replace,
        // A commit or a highlight supersedes a mere caret move.
        Some(IntentKind::TextSelection)
            if matches!(incoming, IntentKind::Select | IntentKind::Activate) =>
        {
            Decision::Replace
        }
        // Committing the row that was about to be highlighted: the highlight
        // was on its way to this anyway.
        Some(IntentKind::Select) if incoming == IntentKind::Activate => Decision::Replace,
        // Focus returning cancels a caret move or a blur that is now wrong.
        Some(IntentKind::TextSelection) | Some(IntentKind::Blur)
            if incoming == IntentKind::Focus =>
        {
            Decision::Replace
        }
        // Leaving always wins: the surface is going away regardless of what
        // was about to be said about it.
        Some(_) if incoming.interrupts() => Decision::Replace,
        Some(_) => Decision::Drop,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const ALL: [IntentKind; 10] = [
        IntentKind::Edit,
        IntentKind::TextSelection,
        IntentKind::Select,
        IntentKind::Activate,
        IntentKind::Focus,
        IntentKind::Blur,
        IntentKind::Dismiss,
        IntentKind::GlobalAction,
        IntentKind::Fallback,
        IntentKind::Clipboard,
    ];

    /// Everything the browser could have caught up with, so only the pairwise
    /// rules are under test.
    fn ready() -> Conditions {
        Conditions {
            applying: false,
            composing: false,
            has_session: true,
            mode_intent_staged: false,
            edit_acknowledged: true,
            query_settled: true,
        }
    }

    #[test]
    fn an_empty_slot_stages_anything() {
        for kind in ALL {
            assert_eq!(decide(kind, None, ready()), Decision::Stage, "{kind:?}");
        }
    }

    #[test]
    fn every_pair_has_a_decision_and_none_of_them_panics() {
        // The point of the table: all 64 combinations are reachable from real
        // input, and before this each was decided by a chain of string
        // comparisons no test covered.
        for incoming in ALL {
            for staged in ALL {
                let decision = decide(incoming, Some(staged), ready());
                // Amend is only ever a caret move folding into an edit.
                if decision == Decision::Amend {
                    assert_eq!(incoming, IntentKind::TextSelection);
                    assert_eq!(staged, IntentKind::Edit);
                }
            }
        }
    }

    #[test]
    fn clipboard_carries_the_edit_without_waiting_for_results() {
        let behind = Conditions { edit_acknowledged: false, query_settled: false, ..ready() };
        for staged in [IntentKind::Edit, IntentKind::TextSelection] {
            assert_eq!(decide(IntentKind::Clipboard, Some(staged), behind), Decision::Replace);
        }
        assert_eq!(decide(IntentKind::Clipboard, Some(IntentKind::Clipboard), ready()), Decision::Drop);
        assert_eq!(decide(IntentKind::Clipboard, Some(IntentKind::Activate), ready()), Decision::Drop);
    }

    #[test]
    fn handing_back_to_firefox_stages_but_does_not_interrupt() {
        // The button exists precisely for when the native surface is not doing
        // what the user wants, so it must not be silently swallowed -- but it
        // also has no business displacing an action already under way.
        assert_eq!(decide(IntentKind::Fallback, None, ready()), Decision::Stage);
        for staged in ALL {
            assert_eq!(
                decide(IntentKind::Fallback, Some(staged), ready()),
                Decision::Drop,
                "fallback over {staged:?}"
            );
        }
    }

    #[test]
    fn a_text_change_replaces_whatever_was_staged() {
        // The text the user can see is by definition the newest thing they have
        // said. Edit() in the C++ never consulted the staged intent at all, and
        // getting this wrong here would have silently dropped keystrokes that
        // arrived while any other intent was outstanding.
        for staged in ALL {
            assert_eq!(
                decide(IntentKind::Edit, Some(staged), ready()),
                Decision::Replace,
                "edit over {staged:?}"
            );
        }
    }

    #[test]
    fn a_composition_produces_edits_and_suppresses_everything_else() {
        // Composition start and end call Edit() in the C++ and carry the
        // composing flag; treating "composing" as a blanket suppression would
        // break IME input entirely.
        let composing = Conditions { composing: true, ..ready() };
        assert_eq!(decide(IntentKind::Edit, None, composing), Decision::Stage);
        assert_eq!(
            decide(IntentKind::Edit, Some(IntentKind::Select), composing),
            Decision::Replace
        );
        for kind in ALL.iter().filter(|k| **k != IntentKind::Edit) {
            assert_eq!(decide(*kind, None, composing), Decision::Drop, "{kind:?}");
        }
    }

    #[test]
    fn leaving_the_surface_always_wins() {
        for staged in ALL {
            for incoming in [IntentKind::Dismiss, IntentKind::Blur] {
                assert_eq!(
                    decide(incoming, Some(staged), ready()),
                    Decision::Replace,
                    "{incoming:?} over {staged:?}"
                );
            }
        }
    }

    #[test]
    fn a_caret_move_never_displaces_anything() {
        for staged in ALL {
            let decision = decide(IntentKind::TextSelection, Some(staged), ready());
            let expected = if staged == IntentKind::Edit { Decision::Amend } else { Decision::Drop };
            assert_eq!(decision, expected, "caret move over {staged:?}");
        }
    }

    #[test]
    fn committing_waits_for_the_edit_it_would_commit_against() {
        assert_eq!(
            decide(IntentKind::Activate, Some(IntentKind::Edit), ready()),
            Decision::Defer
        );
        let unacknowledged = Conditions { edit_acknowledged: false, ..ready() };
        assert_eq!(decide(IntentKind::Activate, None, unacknowledged), Decision::Defer);
        let unsettled = Conditions { query_settled: false, ..ready() };
        assert_eq!(decide(IntentKind::Activate, None, unsettled), Decision::Defer);
    }

    #[test]
    fn committing_supersedes_a_highlight_or_a_caret_move() {
        for staged in [IntentKind::Select, IntentKind::TextSelection] {
            assert_eq!(
                decide(IntentKind::Activate, Some(staged), ready()),
                Decision::Replace,
                "{staged:?}"
            );
        }
    }

    #[test]
    fn focus_returning_cancels_a_caret_move_or_a_stale_blur() {
        for staged in [IntentKind::TextSelection, IntentKind::Blur] {
            assert_eq!(decide(IntentKind::Focus, Some(staged), ready()), Decision::Replace);
        }
        // But focus does not get to displace a commit already on its way.
        assert_eq!(decide(IntentKind::Focus, Some(IntentKind::Activate), ready()), Decision::Drop);
    }

    #[test]
    fn a_global_action_needs_the_browser_level_and_yields_to_everything_but_a_caret() {
        let behind = Conditions { edit_acknowledged: false, ..ready() };
        assert_eq!(decide(IntentKind::GlobalAction, None, behind), Decision::Drop);
        assert_eq!(
            decide(IntentKind::GlobalAction, Some(IntentKind::TextSelection), ready()),
            Decision::Replace
        );
        for staged in ALL.iter().filter(|k| **k != IntentKind::TextSelection) {
            assert_eq!(
                decide(IntentKind::GlobalAction, Some(*staged), ready()),
                Decision::Drop,
                "{staged:?}"
            );
        }
        // It is also the one kind that does not require a session of its own.
        let sessionless = Conditions { has_session: false, ..ready() };
        assert_eq!(decide(IntentKind::GlobalAction, None, sessionless), Decision::Stage);
    }

    #[test]
    fn an_echo_of_a_publication_is_never_an_intent() {
        // Applying suppresses everything, including an edit: a text change the
        // publication itself just wrote is not the user typing.
        for kind in ALL {
            let applying = Conditions { applying: true, ..ready() };
            assert_eq!(decide(kind, None, applying), Decision::Drop, "{kind:?}");
        }
    }

    #[test]
    fn without_a_session_only_a_global_action_means_anything() {
        let sessionless = Conditions { has_session: false, ..ready() };
        for kind in ALL {
            let expected =
                if kind == IntentKind::GlobalAction { Decision::Stage } else { Decision::Drop };
            assert_eq!(decide(kind, None, sessionless), expected, "{kind:?}");
            // Including an edit: Edit() returns early without a session.
            if kind == IntentKind::Edit {
                assert_eq!(decide(kind, Some(IntentKind::Select), sessionless), Decision::Drop);
            }
        }
    }

    #[test]
    fn the_mode_menu_holds_the_slot_while_one_of_its_intents_is_outstanding() {
        let mode = Conditions { mode_intent_staged: true, ..ready() };
        for kind in ALL {
            // A caret move is decided before the mode check and a global action
            // before that, so those two keep their own answers.
            let expected = match kind {
                // Decided before the mode check, so these keep their answers.
                IntentKind::Edit | IntentKind::TextSelection | IntentKind::GlobalAction => {
                    Decision::Stage
                }
                _ => Decision::Drop,
            };
            assert_eq!(decide(kind, None, mode), expected, "{kind:?}");
        }
    }

    #[test]
    fn kind_values_round_trip_across_the_boundary() {
        for kind in ALL {
            assert_eq!(IntentKind::from_raw(kind as u32), Some(kind));
        }
        assert_eq!(IntentKind::from_raw(0), None);
        assert_eq!(IntentKind::from_raw(11), None, "one past the last kind");
    }
}
