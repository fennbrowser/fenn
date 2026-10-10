/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Framework-independent shell logic for the native Firefox/WinUI shell.
//!
//! This crate owns the parts of the shell that are neither WinUI objects nor
//! browser semantics: bounded request queues, drain scheduling, request and
//! endpoint identity, explicit operation outcomes, and an incremental tab
//! presentation model with producer-side dirty tracking.
//!
//! Deliberate non-goals, so this stays reviewable and testable:
//!
//! * No live controls, no borrowed Firefox objects, no browser policy. The
//!   model holds ordinary owned data and opaque identities only.
//! * No threads, no async runtime, no locks. Every core instance is owned by
//!   one thread (the browser's owning STA, which is also Gecko's main thread);
//!   [`ShellCore::owner`] enforces that at the FFI boundary.
//! * No allocation-sharing assumptions. Every buffer this crate hands out has
//!   a matching release function in this crate.
//!
//! Strings cross the boundary as UTF-16 (`u16`) because both sides already
//! speak UTF-16 (`winrt::hstring`, `nsAString`), so nothing is transcoded.

pub mod address;
pub mod toolbar;
pub mod tab_drag;
pub mod group_drag;
pub mod group_glow;
pub mod group_hover;
pub mod group_surface;
pub mod group_motion;
pub mod group_drop;
pub mod foreign_drag;
pub mod intent;
pub mod model;
pub mod menu;
pub mod surface_menu;
pub mod pending;
pub mod publication;
pub mod queue;
pub mod settle;
pub mod strip;

#[cfg(not(test))]
pub mod ffi;
#[cfg(test)]
pub mod ffi;

/// Version of the C boundary in `bridge/v2/shell_core.h`. Bumped whenever the
/// layout or meaning of anything in [`ffi`] changes; the native host refuses to
/// use a core whose version it was not compiled against.
pub const SHELL_CORE_ABI_VERSION: u32 = 35;
