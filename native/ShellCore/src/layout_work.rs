/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Bounded invalidation for expensive native layout consumers. The host owns
//! measurement and must invalidate for topology/realization changes that a
//! geometry sample cannot express. No XAML objects or browser state live here.

#[repr(u32)]
pub enum ShellLayoutWork {
    Widths = 0,
    Ambient = 1,
    Caption = 2,
}

/// Adapter-defined finite geometry values, in stable order for each consumer.
/// Exact comparison preserves subpixel movement and DPI changes. Unused slots
/// are zero. This is an observation, never an authoritative layout model.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ShellLayoutInputs {
    pub values: [f64; 16],
}

#[derive(Default)]
pub struct LayoutWork {
    previous: [Option<ShellLayoutInputs>; 3],
}

impl LayoutWork {
    pub fn invalidate(&mut self, consumer: u32) -> Option<()> {
        *self.previous.get_mut(consumer as usize)? = None;
        Some(())
    }

    /// Record before host work: a reentrant invalidation during that work must
    /// survive for the next pass. The host invalidates again if its work fails.
    pub fn observe(&mut self, consumer: u32, inputs: ShellLayoutInputs, force: bool) -> Option<bool> {
        let previous = self.previous.get_mut(consumer as usize)?;
        if !inputs.values.iter().all(|value| value.is_finite()) {
            return None;
        }
        let changed = force || previous.as_ref() != Some(&inputs);
        *previous = Some(inputs);
        Some(changed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn inputs() -> ShellLayoutInputs { ShellLayoutInputs { values: [0.0; 16] } }

    #[test]
    fn unrelated_layouts_do_not_repeat_work() {
        let mut work = LayoutWork::default();
        assert_eq!(work.observe(0, inputs(), false), Some(true));
        for _ in 0..1000 { assert_eq!(work.observe(0, inputs(), false), Some(false)); }
    }

    #[test]
    fn every_geometry_component_and_subpixel_change_invalidates() {
        let mut work = LayoutWork::default();
        work.observe(0, inputs(), false);
        for slot in 0..16 {
            let mut sample = inputs();
            sample.values[slot] = 0.001;
            assert_eq!(work.observe(0, sample, false), Some(true));
            assert_eq!(work.observe(0, sample, false), Some(false));
            assert_eq!(work.observe(0, inputs(), false), Some(true));
        }
    }

    #[test]
    fn consumers_and_explicit_invalidation_are_independent() {
        let mut work = LayoutWork::default();
        for kind in 0..3 { assert_eq!(work.observe(kind, inputs(), false), Some(true)); }
        work.invalidate(1);
        assert_eq!(work.observe(0, inputs(), false), Some(false));
        assert_eq!(work.observe(1, inputs(), false), Some(true));
        assert_eq!(work.observe(2, inputs(), false), Some(false));
    }

    #[test]
    fn motion_and_reentrant_or_failed_work_can_retry() {
        let mut work = LayoutWork::default();
        work.observe(1, inputs(), false);
        for _ in 0..3 { assert_eq!(work.observe(1, inputs(), true), Some(true)); }
        assert_eq!(work.observe(1, inputs(), false), Some(false));
        work.invalidate(1); // realization or failure inside the host callback
        assert_eq!(work.observe(1, inputs(), false), Some(true));
    }

    #[test]
    fn invalid_input_does_not_poison_the_previous_observation() {
        let mut work = LayoutWork::default();
        work.observe(0, inputs(), false);
        for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
            let mut sample = inputs(); sample.values[5] = bad;
            assert_eq!(work.observe(0, sample, false), None);
            assert_eq!(work.observe(0, inputs(), false), Some(false));
        }
        assert_eq!(work.observe(3, inputs(), false), None);
        assert_eq!(work.invalidate(u32::MAX), None);
    }
}
