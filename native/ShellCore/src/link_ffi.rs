/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! The C boundary of the engine-shell link, under the rules of [`crate::ffi`]:
//! a link belongs to the thread that created it, every call runs inside a
//! panic guard, and every buffer this crate hands out has a release function
//! here. Frame buffers are plain bytes, so they may be released on another
//! thread than the one that produced them.

use crate::ffi::ShellStatus;
use crate::frame::{self, Channel};
use crate::link::{Endpoint, Limits, ReceiveError, Received, SendOutcome, Windows};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::thread::ThreadId;

/// Channel and kind numbers for C. They mirror [`frame::Channel`] and
/// [`frame::Lifetime`]; a test keeps them equal.
pub const SHELL_FRAME_CONTROL: u16 = 0;
pub const SHELL_FRAME_LIFETIME: u16 = 1;
pub const SHELL_FRAME_GEOMETRY: u16 = 2;
pub const SHELL_FRAME_TABS: u16 = 3;
pub const SHELL_FRAME_ADDRESS: u16 = 4;
pub const SHELL_FRAME_TAB_MENU: u16 = 5;
pub const SHELL_FRAME_FOCUS: u16 = 6;
pub const SHELL_FRAME_WINDOW: u16 = 7;
pub const SHELL_FRAME_REQUESTS: u16 = 8;
pub const SHELL_FRAME_STARTUP: u16 = 9;
pub const SHELL_FRAME_CLOSE: u16 = 10;
pub const SHELL_FRAME_PAGE: u16 = 11;
pub const SHELL_FRAME_LIFETIME_BIND: u16 = 0;
pub const SHELL_FRAME_LIFETIME_DESTROY: u16 = 1;
pub const SHELL_FRAME_LIFETIME_WAKEUP_ON: u16 = 2;
pub const SHELL_FRAME_LIFETIME_WAKEUP_OFF: u16 = 3;
pub const SHELL_FRAME_LIFETIME_INTRODUCE: u16 = 4;
pub const SHELL_FRAME_LIFETIME_DESTROY_HOST: u16 = 5;
pub const SHELL_FRAME_LIFETIME_CREATE: u16 = 6;
pub const SHELL_FRAME_LIFETIME_BOUND: u16 = 7;
pub const SHELL_FRAME_LIFETIME_PAGE_READY: u16 = 8;
pub const SHELL_FRAME_LIFETIME_PROFILE: u16 = 9;
pub const SHELL_FRAME_LIFETIME_CONFIRM: u16 = 10;
pub const SHELL_FRAME_LIFETIME_PRESENTED: u16 = 11;
pub const SHELL_FRAME_LIFETIME_RECOVERY_COMMIT: u16 = 12;
pub const SHELL_FRAME_LIFETIME_RECOVERY_DEFERRED: u16 = 13;
pub const SHELL_FRAME_LIFETIME_RECOVERY_RESUMED: u16 = 14;
pub const SHELL_FRAME_LIFETIME_RECOVERY_EMPTY: u16 = 15;
pub const SHELL_FRAME_LIFETIME_RECOVERY_SUPPRESS: u16 = 16;
pub const SHELL_FRAME_REFUSED_STRIP: u16 = 0;
pub const SHELL_FRAME_REFUSED_ADDRESS: u16 = 1;
pub const SHELL_FRAME_REFUSED_BIND: u16 = 2;

pub struct ShellLinkEnd {
    owner: ThreadId,
    poisoned: bool,
    endpoint: Endpoint,
    windows: Windows,
}

/// Bounds for one link end. Zero in any field keeps the built-in default.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ShellLinkLimits {
    pub in_flight_frames: u32,
    pub in_flight_bytes: u32,
    pub outbox_frames: u32,
    pub outbox_bytes: u32,
    pub snapshot_keys: u32,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShellLinkSend {
    Queued = 0,
    Coalesced = 1,
    Overflow = 2,
    Closed = 3,
    Invalid = 4,
}

/// One received frame. `payload` points into the buffer given to
/// `shell_link_receive` and lives as long as that buffer.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ShellFrameView {
    /// False for a credit return or a frame of a retired window.
    pub delivered: bool,
    pub channel: u16,
    pub kind: u16,
    pub window: u32,
    pub revision: u64,
    pub payload: *const u8,
    pub length: usize,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ShellLinkCounts {
    pub sent: u64,
    pub received: u64,
    pub coalesced: u64,
    pub overflowed: u64,
    pub dropped: u64,
    pub outbox: u64,
    pub in_flight: u64,
}

/// A tab row to encode; `title` holds `title_length` UTF-16 units.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ShellFrameTabIn {
    pub id: u32,
    pub commands: u32,
    pub title: *const u16,
    pub title_length: usize,
}

/// A decoded tab row; `title` points into the frame payload.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ShellFrameTab {
    pub id: u32,
    pub commands: u32,
    pub title: *const u16,
    pub title_length: usize,
}

/// The window-request payload, byte for byte: 32 bytes, no padding.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ShellFrameWindowRequest {
    pub kind: u32,
    pub flags: u32,
    pub rect: [i32; 4],
    pub value: i64,
}

unsafe fn guard<F>(link: *mut ShellLinkEnd, body: F) -> ShellStatus
where
    F: FnOnce(&mut ShellLinkEnd) -> ShellStatus,
{
    let Some(link) = link.as_mut() else {
        return ShellStatus::InvalidArgument;
    };
    if link.owner != std::thread::current().id() {
        return ShellStatus::WrongThread;
    }
    if link.poisoned {
        return ShellStatus::Panic;
    }
    match catch_unwind(AssertUnwindSafe(|| body(link))) {
        Ok(status) => status,
        Err(_) => {
            link.poisoned = true;
            ShellStatus::Panic
        }
    }
}

fn limits(input: Option<&ShellLinkLimits>) -> Limits {
    let d = Limits::default();
    let Some(l) = input else {
        return d;
    };
    let pick = |v: u32, d: usize| if v == 0 { d } else { v as usize };
    Limits {
        in_flight_frames: pick(l.in_flight_frames, d.in_flight_frames),
        in_flight_bytes: pick(l.in_flight_bytes, d.in_flight_bytes),
        outbox_frames: pick(l.outbox_frames, d.outbox_frames),
        outbox_bytes: pick(l.outbox_bytes, d.outbox_bytes),
        snapshot_keys: pick(l.snapshot_keys, d.snapshot_keys),
    }
}

/// A link end owned by the calling thread. Returns null for generation 0.
/// # Safety
/// `limits` must be null or readable.
#[no_mangle]
pub unsafe extern "C" fn shell_link_create(
    generation: u32,
    limits_in: *const ShellLinkLimits,
) -> *mut ShellLinkEnd {
    if generation == 0 {
        return std::ptr::null_mut();
    }
    match catch_unwind(|| {
        Box::new(ShellLinkEnd {
            owner: std::thread::current().id(),
            poisoned: false,
            endpoint: Endpoint::new(generation, limits(limits_in.as_ref())),
            windows: Windows::default(),
        })
    }) {
        Ok(link) => Box::into_raw(link),
        Err(_) => std::ptr::null_mut(),
    }
}

/// # Safety
/// `link` must be null or from `shell_link_create`, and not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn shell_link_destroy(link: *mut ShellLinkEnd) {
    if !link.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(link))));
    }
}

/// Queues one frame. Never blocks; `outcome` says what happened to it.
/// # Safety
/// `payload` must be readable for `length` bytes (or null with 0); `outcome` writable.
#[no_mangle]
pub unsafe extern "C" fn shell_link_send(
    link: *mut ShellLinkEnd,
    window: u32,
    channel: u16,
    kind: u16,
    revision: u64,
    payload: *const u8,
    length: usize,
    outcome: *mut ShellLinkSend,
) -> ShellStatus {
    guard(link, |link| {
        if outcome.is_null() || (payload.is_null() && length != 0) {
            return ShellStatus::InvalidArgument;
        }
        let Some(channel) = Channel::from_u16(channel) else {
            *outcome = ShellLinkSend::Invalid;
            return ShellStatus::Ok;
        };
        let bytes = if length == 0 {
            &[][..]
        } else {
            std::slice::from_raw_parts(payload, length)
        };
        *outcome = match link.endpoint.send(window, channel, kind, revision, bytes) {
            SendOutcome::Queued => ShellLinkSend::Queued,
            SendOutcome::Coalesced => ShellLinkSend::Coalesced,
            SendOutcome::Overflow => ShellLinkSend::Overflow,
            SendOutcome::Closed => ShellLinkSend::Closed,
            SendOutcome::Invalid => ShellLinkSend::Invalid,
        };
        ShellStatus::Ok
    })
}

/// The next frame that may go to the transport, or null when none may. With
/// `force`, credit is ignored: for a caller that will wait for the other side
/// and must not overtake queued frames. The buffer is released with
/// `shell_link_free_frame`, on any thread.
/// # Safety
/// `data` and `length` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_link_take(
    link: *mut ShellLinkEnd,
    force: bool,
    data: *mut *mut u8,
    length: *mut usize,
) -> ShellStatus {
    guard(link, |link| {
        if data.is_null() || length.is_null() {
            return ShellStatus::InvalidArgument;
        }
        *data = std::ptr::null_mut();
        *length = 0;
        let next = if force {
            link.endpoint.take_forced()
        } else {
            link.endpoint.take()
        };
        if let Some(bytes) = next {
            let boxed = bytes.into_boxed_slice();
            *length = boxed.len();
            *data = Box::into_raw(boxed) as *mut u8;
        }
        ShellStatus::Ok
    })
}

/// # Safety
/// `data` and `length` must come from one `shell_link_take`, released once.
#[no_mangle]
pub unsafe extern "C" fn shell_link_free_frame(data: *mut u8, length: usize) {
    if !data.is_null() {
        let _ = catch_unwind(|| {
            drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
                data, length,
            )))
        });
    }
}

/// Validates and accounts one whole frame. `Rejected`: from another
/// generation, dropped. `Closed`: lost or forged frames closed the link.
/// `InvalidArgument`: not a frame.
/// # Safety
/// `data` readable for `length` bytes and kept alive while `view` is used.
#[no_mangle]
pub unsafe extern "C" fn shell_link_receive(
    link: *mut ShellLinkEnd,
    data: *const u8,
    length: usize,
    view: *mut ShellFrameView,
) -> ShellStatus {
    guard(link, |link| {
        if data.is_null() || view.is_null() {
            return ShellStatus::InvalidArgument;
        }
        let bytes = std::slice::from_raw_parts(data, length);
        *view = ShellFrameView {
            delivered: false,
            channel: 0,
            kind: 0,
            window: 0,
            revision: 0,
            payload: std::ptr::null(),
            length: 0,
        };
        match link.endpoint.receive(bytes) {
            Ok(Received::Frame(h)) => {
                *view = ShellFrameView {
                    delivered: true,
                    channel: h.channel as u16,
                    kind: h.kind,
                    window: h.window,
                    revision: h.revision,
                    payload: data.add(frame::HEADER_BYTES),
                    length: h.length as usize,
                };
                ShellStatus::Ok
            }
            Ok(Received::Control | Received::Dropped) => ShellStatus::Ok,
            Err(ReceiveError::Decode(_)) => ShellStatus::InvalidArgument,
            Err(ReceiveError::Generation) => ShellStatus::Rejected,
            Err(ReceiveError::Sequence | ReceiveError::Credit | ReceiveError::Closed) => {
                ShellStatus::Closed
            }
        }
    })
}

/// # Safety
/// See `guard`.
#[no_mangle]
pub unsafe extern "C" fn shell_link_retire(link: *mut ShellLinkEnd, window: u32) -> ShellStatus {
    guard(link, |link| {
        link.endpoint.retire(window);
        link.windows.forget(window);
        ShellStatus::Ok
    })
}

/// # Safety
/// See `guard`.
#[no_mangle]
pub unsafe extern "C" fn shell_link_close(link: *mut ShellLinkEnd) -> ShellStatus {
    guard(link, |link| {
        link.endpoint.close();
        ShellStatus::Ok
    })
}

/// The serial for a local handle; with `create`, a new one if it has none.
/// `UnknownTab` when it has none and `create` is false.
/// # Safety
/// `serial` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_link_serial(
    link: *mut ShellLinkEnd,
    handle: usize,
    create: bool,
    serial: *mut u32,
) -> ShellStatus {
    guard(link, |link| {
        if serial.is_null() {
            return ShellStatus::InvalidArgument;
        }
        match link.windows.serial(handle, create) {
            Some(value) => {
                *serial = value;
                ShellStatus::Ok
            }
            None => {
                *serial = 0;
                if handle == 0 {
                    ShellStatus::InvalidArgument
                } else {
                    ShellStatus::UnknownTab
                }
            }
        }
    })
}

/// Binds a serial the other side chose to a local handle.
/// # Safety
/// See `guard`.
#[no_mangle]
pub unsafe extern "C" fn shell_link_adopt(
    link: *mut ShellLinkEnd,
    serial: u32,
    handle: usize,
) -> ShellStatus {
    guard(link, |link| {
        if link.windows.adopt(serial, handle) {
            ShellStatus::Ok
        } else {
            ShellStatus::Rejected
        }
    })
}

/// # Safety
/// `handle` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_link_handle(
    link: *mut ShellLinkEnd,
    serial: u32,
    handle: *mut usize,
) -> ShellStatus {
    guard(link, |link| {
        if handle.is_null() {
            return ShellStatus::InvalidArgument;
        }
        match link.windows.handle(serial) {
            Some(value) => {
                *handle = value;
                ShellStatus::Ok
            }
            None => {
                *handle = 0;
                ShellStatus::UnknownTab
            }
        }
    })
}

/// # Safety
/// `counts` must be writable.
#[no_mangle]
pub unsafe extern "C" fn shell_link_counts(
    link: *mut ShellLinkEnd,
    counts: *mut ShellLinkCounts,
) -> ShellStatus {
    guard(link, |link| {
        if counts.is_null() {
            return ShellStatus::InvalidArgument;
        }
        let c = link.endpoint.counts();
        *counts = ShellLinkCounts {
            sent: c.sent,
            received: c.received,
            coalesced: c.coalesced,
            overflowed: c.overflowed,
            dropped: c.dropped,
            outbox: c.outbox,
            in_flight: c.in_flight,
        };
        ShellStatus::Ok
    })
}

/// Encodes complete tab rows into a buffer released with `shell_link_free_frame`.
/// # Safety
/// `rows` readable for `count` entries and each title for its length; outputs writable.
#[no_mangle]
pub unsafe extern "C" fn shell_frame_tabs_encode(
    rows: *const ShellFrameTabIn,
    count: usize,
    selected: u32,
    revision: u32,
    data: *mut *mut u8,
    length: *mut usize,
) -> ShellStatus {
    if data.is_null()
        || length.is_null()
        || (rows.is_null() && count != 0)
        || count > frame::tabs::MAX_ROWS
    {
        return ShellStatus::InvalidArgument;
    }
    *data = std::ptr::null_mut();
    *length = 0;
    let built = catch_unwind(|| {
        let input = if count == 0 {
            &[][..]
        } else {
            std::slice::from_raw_parts(rows, count)
        };
        let mut out = frame::tabs::Rows {
            selected,
            revision,
            rows: Vec::with_capacity(count),
        };
        for row in input {
            if row.title_length > frame::tabs::MAX_TITLE
                || (row.title.is_null() && row.title_length != 0)
            {
                return None;
            }
            let title = if row.title_length == 0 {
                Vec::new()
            } else {
                std::slice::from_raw_parts(row.title, row.title_length).to_vec()
            };
            out.rows.push(frame::tabs::Row {
                id: row.id,
                commands: row.commands,
                title,
            });
        }
        let bytes = frame::tabs::encode(&out);
        frame::tabs::decode(&bytes).map(|_| bytes)
    });
    match built {
        Ok(Some(bytes)) => {
            let boxed = bytes.into_boxed_slice();
            *length = boxed.len();
            *data = Box::into_raw(boxed) as *mut u8;
            ShellStatus::Ok
        }
        Ok(None) => ShellStatus::Rejected,
        Err(_) => ShellStatus::Panic,
    }
}

/// Decodes complete tab rows. Titles point into `payload`.
/// # Safety
/// `payload` readable for `length` bytes; `rows` writable for `capacity`; scalars writable.
#[no_mangle]
pub unsafe extern "C" fn shell_frame_tabs_decode(
    payload: *const u8,
    length: usize,
    rows: *mut ShellFrameTab,
    capacity: usize,
    count: *mut usize,
    selected: *mut u32,
    revision: *mut u32,
) -> ShellStatus {
    if payload.is_null()
        || count.is_null()
        || selected.is_null()
        || revision.is_null()
        || (rows.is_null() && capacity != 0)
    {
        return ShellStatus::InvalidArgument;
    }
    let bytes = std::slice::from_raw_parts(payload, length);
    let Ok(Some(decoded)) = catch_unwind(|| frame::tabs::decode(bytes)) else {
        return ShellStatus::Rejected;
    };
    if decoded.rows.len() > capacity {
        *count = decoded.rows.len();
        return ShellStatus::TooLarge;
    }
    // Titles follow each 12-byte row prefix, after the 12-byte table prefix.
    let mut at = 12usize;
    for (i, row) in decoded.rows.iter().enumerate() {
        at += 12;
        *rows.add(i) = ShellFrameTab {
            id: row.id,
            commands: row.commands,
            title: payload.add(at) as *const u16,
            title_length: row.title.len(),
        };
        at += row.title.len() * 2;
    }
    *count = decoded.rows.len();
    *selected = decoded.selected;
    *revision = decoded.revision;
    ShellStatus::Ok
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe fn take(link: *mut ShellLinkEnd) -> Option<Vec<u8>> {
        let (mut data, mut length) = (std::ptr::null_mut(), 0usize);
        assert_eq!(
            shell_link_take(link, false, &mut data, &mut length),
            ShellStatus::Ok
        );
        if data.is_null() {
            return None;
        }
        let out = std::slice::from_raw_parts(data, length).to_vec();
        shell_link_free_frame(data, length);
        Some(out)
    }

    #[test]
    fn a_frame_crosses_and_its_credit_returns() {
        unsafe {
            let a = shell_link_create(5, std::ptr::null());
            let b = shell_link_create(5, std::ptr::null());
            let mut outcome = ShellLinkSend::Invalid;
            assert_eq!(
                shell_link_send(
                    a,
                    2,
                    Channel::Focus as u16,
                    1,
                    0,
                    [1u8].as_ptr(),
                    1,
                    &mut outcome
                ),
                ShellStatus::Ok
            );
            assert_eq!(outcome, ShellLinkSend::Queued);
            let bytes = take(a).unwrap();
            let mut view = std::mem::zeroed::<ShellFrameView>();
            assert_eq!(
                shell_link_receive(b, bytes.as_ptr(), bytes.len(), &mut view),
                ShellStatus::Ok
            );
            assert!(view.delivered);
            assert_eq!(
                (view.channel, view.kind, view.window, *view.payload),
                (Channel::Focus as u16, 1, 2, 1)
            );
            let ack = take(b).unwrap();
            assert_eq!(
                shell_link_receive(a, ack.as_ptr(), ack.len(), &mut view),
                ShellStatus::Ok
            );
            assert!(!view.delivered);
            let mut counts = ShellLinkCounts::default();
            shell_link_counts(a, &mut counts);
            assert_eq!((counts.sent, counts.in_flight), (1, 0));
            shell_link_destroy(a);
            shell_link_destroy(b);
        }
    }

    #[test]
    fn another_thread_is_refused() {
        unsafe {
            let a = shell_link_create(1, std::ptr::null()) as usize;
            let status = std::thread::spawn(move || {
                let mut serial = 0u32;
                shell_link_serial(a as *mut ShellLinkEnd, 1, true, &mut serial)
            })
            .join()
            .unwrap();
            assert_eq!(status, ShellStatus::WrongThread);
            shell_link_destroy(a as *mut ShellLinkEnd);
        }
    }

    #[test]
    fn tab_rows_encode_and_decode_through_the_boundary() {
        unsafe {
            let one: Vec<u16> = "One".encode_utf16().collect();
            let input = [
                ShellFrameTabIn {
                    id: 7,
                    commands: 1,
                    title: one.as_ptr(),
                    title_length: one.len(),
                },
                ShellFrameTabIn {
                    id: 8,
                    commands: 0,
                    title: std::ptr::null(),
                    title_length: 0,
                },
            ];
            let (mut data, mut length) = (std::ptr::null_mut(), 0usize);
            assert_eq!(
                shell_frame_tabs_encode(input.as_ptr(), 2, 8, 3, &mut data, &mut length),
                ShellStatus::Ok
            );
            let mut rows = [std::mem::zeroed::<ShellFrameTab>(); 4];
            let (mut count, mut selected, mut revision) = (0usize, 0u32, 0u32);
            assert_eq!(
                shell_frame_tabs_decode(
                    data,
                    length,
                    rows.as_mut_ptr(),
                    4,
                    &mut count,
                    &mut selected,
                    &mut revision
                ),
                ShellStatus::Ok
            );
            assert_eq!((count, selected, revision), (2, 8, 3));
            assert_eq!(
                std::slice::from_raw_parts(rows[0].title, rows[0].title_length),
                &one[..]
            );
            assert_eq!(rows[1].title_length, 0);
            assert_eq!(
                shell_frame_tabs_decode(
                    data,
                    length,
                    rows.as_mut_ptr(),
                    1,
                    &mut count,
                    &mut selected,
                    &mut revision
                ),
                ShellStatus::TooLarge
            );
            shell_link_free_frame(data, length);
            let dup = [input[0], input[0]];
            assert_eq!(
                shell_frame_tabs_encode(dup.as_ptr(), 2, 0, 1, &mut data, &mut length),
                ShellStatus::Rejected
            );
        }
    }

    #[test]
    fn exported_numbers_match_the_protocol() {
        use frame::Lifetime;
        let channels = [
            (SHELL_FRAME_CONTROL, Channel::Control),
            (SHELL_FRAME_LIFETIME, Channel::Lifetime),
            (SHELL_FRAME_GEOMETRY, Channel::Geometry),
            (SHELL_FRAME_TABS, Channel::Tabs),
            (SHELL_FRAME_ADDRESS, Channel::Address),
            (SHELL_FRAME_TAB_MENU, Channel::TabMenu),
            (SHELL_FRAME_FOCUS, Channel::Focus),
            (SHELL_FRAME_WINDOW, Channel::Window),
            (SHELL_FRAME_REQUESTS, Channel::Requests),
            (SHELL_FRAME_STARTUP, Channel::Startup),
            (SHELL_FRAME_CLOSE, Channel::Close),
            (SHELL_FRAME_PAGE, Channel::Page),
        ];
        for (number, channel) in channels {
            assert_eq!(number, channel as u16);
            assert_eq!(Channel::from_u16(number), Some(channel));
        }
        assert_eq!(Channel::from_u16(12), None, "every channel is exported");
        for (number, kind) in [
            (SHELL_FRAME_LIFETIME_BIND, Lifetime::Bind),
            (SHELL_FRAME_LIFETIME_DESTROY, Lifetime::Destroy),
            (SHELL_FRAME_LIFETIME_WAKEUP_ON, Lifetime::WakeupOn),
            (SHELL_FRAME_LIFETIME_WAKEUP_OFF, Lifetime::WakeupOff),
            (SHELL_FRAME_LIFETIME_INTRODUCE, Lifetime::Introduce),
            (SHELL_FRAME_LIFETIME_DESTROY_HOST, Lifetime::DestroyHost),
        ] {
            assert_eq!(number, kind as u16);
        }
        for (number, kind) in [
            (SHELL_FRAME_LIFETIME_CREATE, Lifetime::Create),
            (SHELL_FRAME_LIFETIME_BOUND, Lifetime::Bound),
            (SHELL_FRAME_LIFETIME_PAGE_READY, Lifetime::PageReady),
            (SHELL_FRAME_LIFETIME_PROFILE, Lifetime::Profile),
            (SHELL_FRAME_LIFETIME_CONFIRM, Lifetime::Confirm),
            (SHELL_FRAME_LIFETIME_PRESENTED, Lifetime::Presented),
            (
                SHELL_FRAME_LIFETIME_RECOVERY_COMMIT,
                Lifetime::RecoveryCommit,
            ),
            (
                SHELL_FRAME_LIFETIME_RECOVERY_DEFERRED,
                Lifetime::RecoveryDeferred,
            ),
            (
                SHELL_FRAME_LIFETIME_RECOVERY_RESUMED,
                Lifetime::RecoveryResumed,
            ),
            (SHELL_FRAME_LIFETIME_RECOVERY_EMPTY, Lifetime::RecoveryEmpty),
            (
                SHELL_FRAME_LIFETIME_RECOVERY_SUPPRESS,
                Lifetime::RecoverySuppress,
            ),
        ] {
            assert_eq!(number, kind as u16);
        }
        assert!(
            frame::rule(Channel::Lifetime, 17).is_none(),
            "every lifetime kind is exported"
        );
    }

    #[test]
    fn window_request_layout_is_the_payload_layout() {
        assert_eq!(
            std::mem::size_of::<ShellFrameWindowRequest>(),
            frame::window_request::BYTES
        );
        let r = ShellFrameWindowRequest {
            kind: 3,
            flags: 1,
            rect: [1, 2, 3, 4],
            value: 9,
        };
        let raw = unsafe {
            std::slice::from_raw_parts(&r as *const _ as *const u8, frame::window_request::BYTES)
        };
        let decoded = frame::window_request::decode(raw).unwrap();
        assert_eq!(
            (decoded.kind, decoded.flags, decoded.rect, decoded.value),
            (3, 1, [1, 2, 3, 4], 9)
        );
    }
}

/// Validate an ephemeral Share request against the current address/document identity.
/// Borrowed payload; no state, allocation ownership or replay across connections.
#[no_mangle]
pub unsafe extern "C" fn shell_frame_share_current(
    payload: *const u8,
    length: usize,
    session: u64,
    document: u64,
    now: u64,
) -> bool {
    if payload.is_null() || length > frame::share_request::MAX_BYTES {
        return false;
    }
    catch_unwind(|| {
        frame::share_request::current(
            std::slice::from_raw_parts(payload, length),
            session,
            document,
            now,
        )
    })
    .unwrap_or(false)
}
