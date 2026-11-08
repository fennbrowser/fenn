/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! One end of the engine-shell link (M5b).
//!
//! Each side owns one [`Endpoint`] on one thread. The host moves the bytes
//! between the two (a posted message in one process, a pipe later) and wakes
//! the other side; nothing here is shared between threads.
//!
//! Sending never blocks. Frames wait in an outbox until the receiver has
//! returned enough credit, and the outbox applies each channel's
//! [`Class`](crate::frame::Class): a snapshot replaces its unsent predecessor
//! in place, so it keeps that predecessor's position among ordered frames;
//! deltas and ordered frames are refused when the outbox is full. Credit
//! comes back in cumulative control frames, which the same bytes can carry
//! over a pipe.

use crate::frame::{self, Channel, Class, Header};
use std::collections::{BTreeMap, BTreeSet, VecDeque};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Limits {
    /// Frames and bytes handed to the transport and not yet acknowledged.
    pub in_flight_frames: usize,
    pub in_flight_bytes: usize,
    /// Frames and bytes waiting for credit. Snapshots beyond this are still
    /// admitted while they coalesce, up to `snapshot_keys` distinct keys.
    pub outbox_frames: usize,
    pub outbox_bytes: usize,
    pub snapshot_keys: usize,
}

impl Default for Limits {
    fn default() -> Self {
        // The bounds the posted-call queue had before M5b: 1024 calls and 16M
        // UTF-16 units, in flight and again waiting.
        Self { in_flight_frames: 1024, in_flight_bytes: 32 << 20, outbox_frames: 1024,
            outbox_bytes: 32 << 20, snapshot_keys: 1024 }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SendOutcome {
    Queued,
    /// Replaced an unsent frame of the same window, channel and kind.
    Coalesced,
    /// The outbox is full: an ordered frame is lost to its caller, a delta's
    /// producer answers with one complete publication.
    Overflow,
    /// The window or the link is retired.
    Closed,
    /// Not a frame the protocol defines.
    Invalid,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReceiveError {
    Decode(frame::DecodeError),
    /// From another connection generation. Dropped; the link stays open.
    Generation,
    /// Out of order or repeated. The link closes: frames were lost.
    Sequence,
    /// Credit for frames never sent. The link closes.
    Credit,
    Closed,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Received {
    /// A credit return, applied internally.
    Control,
    /// For a retired window: counted and acknowledged, not delivered.
    Dropped,
    Frame(Header),
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Counts {
    pub sent: u64,
    pub received: u64,
    pub coalesced: u64,
    pub overflowed: u64,
    pub dropped: u64,
    pub outbox: u64,
    pub in_flight: u64,
}

struct Pending {
    window: u32,
    channel: Channel,
    kind: u16,
    revision: u64,
    class: Class,
    payload: Vec<u8>,
}

pub struct Endpoint {
    limits: Limits,
    generation: u32,
    closed: bool,
    outbox: VecDeque<Pending>,
    outbox_bytes: usize,
    next_sequence: u64,
    sent_frames: u64,
    sent_bytes: u64,
    acked_frames: u64,
    acked_bytes: u64,
    expected: u64,
    consumed_frames: u64,
    consumed_bytes: u64,
    ack_due: bool,
    retired: BTreeSet<u32>,
    counts: Counts,
}

impl Endpoint {
    pub fn new(generation: u32, limits: Limits) -> Self {
        Self { limits, generation, closed: false, outbox: VecDeque::new(), outbox_bytes: 0,
            next_sequence: 1, sent_frames: 0, sent_bytes: 0, acked_frames: 0, acked_bytes: 0,
            expected: 1, consumed_frames: 0, consumed_bytes: 0, ack_due: false,
            retired: BTreeSet::new(), counts: Counts::default() }
    }

    pub fn is_closed(&self) -> bool { self.closed }
    pub fn close(&mut self) { self.closed = true; self.outbox.clear(); self.outbox_bytes = 0; }

    pub fn counts(&self) -> Counts {
        Counts { outbox: self.outbox.len() as u64, in_flight: self.sent_frames - self.acked_frames, ..self.counts }
    }

    /// After this, frames for `window` are refused here and dropped on receipt.
    pub fn retire(&mut self, window: u32) { if window != 0 { self.retired.insert(window); } }

    pub fn send(&mut self, window: u32, channel: Channel, kind: u16, revision: u64, payload: &[u8]) -> SendOutcome {
        if self.closed || self.retired.contains(&window) { return SendOutcome::Closed; }
        let Some((class, bound)) = frame::rule(channel, kind) else { return SendOutcome::Invalid; };
        if class == Class::Control || payload.len() > bound || !frame::valid_payload(channel, kind, payload) {
            return SendOutcome::Invalid;
        }
        if class == Class::Snapshot {
            if let Some(slot) = self.outbox.iter_mut().find(|p| p.class == Class::Snapshot &&
                p.window == window && p.channel == channel && p.kind == kind) {
                self.outbox_bytes = self.outbox_bytes - slot.payload.len() + payload.len();
                slot.payload = payload.to_vec();
                slot.revision = revision;
                self.counts.coalesced += 1;
                return SendOutcome::Coalesced;
            }
            let keys = self.outbox.iter().filter(|p| p.class == Class::Snapshot).count();
            if keys >= self.limits.snapshot_keys { self.counts.overflowed += 1; return SendOutcome::Overflow; }
        } else if self.outbox.len() >= self.limits.outbox_frames ||
            self.outbox_bytes + payload.len() > self.limits.outbox_bytes {
            self.counts.overflowed += 1;
            return SendOutcome::Overflow;
        }
        self.outbox_bytes += payload.len();
        self.outbox.push_back(Pending { window, channel, kind, revision, class, payload: payload.to_vec() });
        SendOutcome::Queued
    }

    fn header(&mut self, channel: Channel, kind: u16, window: u32, revision: u64, length: usize) -> Header {
        let sequence = self.next_sequence;
        self.next_sequence += 1;
        Header { channel, kind, generation: self.generation, window, sequence, revision, length: length as u32 }
    }

    /// The next frame to hand to the transport, if one may go now. A due
    /// credit return goes first and needs no credit itself.
    pub fn take(&mut self) -> Option<Vec<u8>> { self.take_inner(false) }

    /// As [`take`](Self::take), ignoring credit. For a caller about to wait for
    /// the other side anyway, which must not overtake what is queued.
    pub fn take_forced(&mut self) -> Option<Vec<u8>> { self.take_inner(true) }

    fn take_inner(&mut self, force: bool) -> Option<Vec<u8>> {
        if self.closed { return None; }
        let mut out = Vec::new();
        if self.ack_due {
            self.ack_due = false;
            let mut payload = Vec::with_capacity(16);
            payload.extend_from_slice(&self.consumed_frames.to_le_bytes());
            payload.extend_from_slice(&self.consumed_bytes.to_le_bytes());
            let header = self.header(Channel::Control, 0, 0, 0, payload.len());
            frame::encode(&header, &payload, &mut out);
            return Some(out);
        }
        let front = self.outbox.front()?;
        let in_flight = self.sent_frames - self.acked_frames;
        let in_flight_bytes = self.sent_bytes - self.acked_bytes;
        let fits = force || in_flight == 0 || (in_flight < self.limits.in_flight_frames as u64 &&
            in_flight_bytes + front.payload.len() as u64 <= self.limits.in_flight_bytes as u64);
        if !fits { return None; }
        let next = self.outbox.pop_front().unwrap();
        self.outbox_bytes -= next.payload.len();
        let header = self.header(next.channel, next.kind, next.window, next.revision, next.payload.len());
        frame::encode(&header, &next.payload, &mut out);
        self.sent_frames += 1;
        self.sent_bytes += next.payload.len() as u64;
        self.counts.sent += 1;
        Some(out)
    }

    /// Validates and accounts one received frame. The payload is the bytes
    /// after [`frame::HEADER_BYTES`] of the input.
    pub fn receive(&mut self, bytes: &[u8]) -> Result<Received, ReceiveError> {
        if self.closed { return Err(ReceiveError::Closed); }
        let (header, payload) = frame::decode(bytes).map_err(ReceiveError::Decode)?;
        if header.generation != self.generation { return Err(ReceiveError::Generation); }
        if header.sequence != self.expected { self.close(); return Err(ReceiveError::Sequence); }
        self.expected += 1;
        if header.channel == Channel::Control {
            let frames = u64::from_le_bytes(payload[0..8].try_into().unwrap());
            let sent_bytes = u64::from_le_bytes(payload[8..16].try_into().unwrap());
            if frames < self.acked_frames || frames > self.sent_frames ||
                sent_bytes < self.acked_bytes || sent_bytes > self.sent_bytes {
                self.close();
                return Err(ReceiveError::Credit);
            }
            self.acked_frames = frames;
            self.acked_bytes = sent_bytes;
            return Ok(Received::Control);
        }
        self.consumed_frames += 1;
        self.consumed_bytes += payload.len() as u64;
        self.ack_due = true;
        self.counts.received += 1;
        if self.retired.contains(&header.window) { self.counts.dropped += 1; return Ok(Received::Dropped); }
        Ok(Received::Frame(header))
    }
}

/// Window serials for one side: a handle (an HWND, opaque here) maps to a
/// serial that is never reused, so a reused handle cannot reach a retired
/// window's state.
#[derive(Default)]
pub struct Windows {
    next: u32,
    by_handle: BTreeMap<usize, u32>,
    by_serial: BTreeMap<u32, usize>,
}

impl Windows {
    pub fn serial(&mut self, handle: usize, create: bool) -> Option<u32> {
        if handle == 0 { return None; }
        if let Some(serial) = self.by_handle.get(&handle) { return Some(*serial); }
        if !create { return None; }
        self.next = self.next.checked_add(1)?;
        self.by_handle.insert(handle, self.next);
        self.by_serial.insert(self.next, handle);
        Some(self.next)
    }

    /// Binds a serial the other side chose to a local handle.
    pub fn adopt(&mut self, serial: u32, handle: usize) -> bool {
        if serial == 0 || handle == 0 || self.by_serial.contains_key(&serial) || self.by_handle.contains_key(&handle) {
            return false;
        }
        self.by_serial.insert(serial, handle);
        self.by_handle.insert(handle, serial);
        true
    }

    pub fn handle(&self, serial: u32) -> Option<usize> { self.by_serial.get(&serial).copied() }

    pub fn forget(&mut self, serial: u32) {
        if let Some(handle) = self.by_serial.remove(&serial) { self.by_handle.remove(&handle); }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frame::{rects, text_bytes};

    fn pair(limits: Limits) -> (Endpoint, Endpoint) { (Endpoint::new(1, limits), Endpoint::new(1, limits)) }

    fn json(s: &str) -> Vec<u8> { text_bytes(&s.encode_utf16().collect::<Vec<_>>()) }

    fn shown(r: rects::Rect) -> Vec<u8> { let mut p = rects::encode(&[r]); p.push(1); p }

    /// Moves every frame that may go, returning the delivered headers.
    fn pump(from: &mut Endpoint, to: &mut Endpoint) -> Vec<Header> {
        let mut out = Vec::new();
        while let Some(bytes) = from.take() {
            if let Received::Frame(h) = to.receive(&bytes).unwrap() { out.push(h); }
        }
        out
    }

    fn ack(from: &mut Endpoint, to: &mut Endpoint) {
        while let Some(bytes) = to.take() { assert_eq!(from.receive(&bytes).unwrap(), Received::Control); }
    }

    #[test]
    fn frames_arrive_in_order_with_contiguous_sequence() {
        let (mut a, mut b) = pair(Limits::default());
        for i in 0..5u64 { assert_eq!(a.send(1, Channel::Requests, 0, i, &json("{}")), SendOutcome::Queued); }
        let got = pump(&mut a, &mut b);
        assert_eq!(got.iter().map(|h| h.revision).collect::<Vec<_>>(), vec![0, 1, 2, 3, 4]);
        assert_eq!(got.iter().map(|h| h.sequence).collect::<Vec<_>>(), vec![1, 2, 3, 4, 5]);
    }

    #[test]
    fn credit_stops_sending_until_acknowledged() {
        let limits = Limits { in_flight_frames: 2, ..Limits::default() };
        let (mut a, mut b) = pair(limits);
        for i in 0..4u64 { a.send(1, Channel::Requests, 0, i, &json("{}")); }
        assert_eq!(pump(&mut a, &mut b).len(), 2, "two frames of credit");
        assert!(a.take().is_none());
        ack(&mut a, &mut b);
        assert_eq!(pump(&mut a, &mut b).len(), 2);
    }

    #[test]
    fn stalled_snapshots_coalesce_in_place() {
        let limits = Limits { in_flight_frames: 1, ..Limits::default() };
        let (mut a, mut b) = pair(limits);
        a.send(1, Channel::Requests, 0, 1, &json("{}"));
        assert_eq!(pump(&mut a, &mut b).len(), 1);
        // The receiver is stalled: everything below waits in the outbox.
        a.send(1, Channel::Geometry, 0, 10, &shown([0, 0, 1, 1]));
        a.send(1, Channel::Requests, 0, 2, &json("{}"));
        assert_eq!(a.send(1, Channel::Geometry, 0, 11, &shown([0, 0, 9, 9])), SendOutcome::Coalesced);
        a.send(2, Channel::Geometry, 0, 12, &shown([0, 0, 2, 2]));
        let mut seen = Vec::new();
        loop {
            ack(&mut a, &mut b);
            let got = pump(&mut a, &mut b);
            if got.is_empty() { break; }
            seen.extend(got.into_iter().map(|h| (h.window, h.channel, h.revision)));
        }
        assert_eq!(seen, vec![(1, Channel::Geometry, 11), (1, Channel::Requests, 2), (2, Channel::Geometry, 12)],
            "the newest geometry, at the first geometry's place");
        assert_eq!(a.counts().coalesced, 1);
    }

    #[test]
    fn acknowledged_icons_survive_backpressure_individually() {
        let (mut a, mut b) = pair(Limits { in_flight_frames: 1, ..Limits::default() });
        a.send(1, Channel::Requests, 0, 0, &json("{}"));
        pump(&mut a, &mut b);
        let icon = [1u32.to_le_bytes(), 1u32.to_le_bytes(), 0xffffffffu32.to_le_bytes()].concat();
        for (revision, kind) in [(1, 4), (2, 4), (3, 5), (4, 5)] {
            assert_eq!(a.send(1, Channel::Window, kind, revision, &icon), SendOutcome::Queued);
        }
        let mut delivered = Vec::new();
        loop {
            ack(&mut a, &mut b);
            let got = pump(&mut a, &mut b);
            if got.is_empty() { break; }
            delivered.extend(got.into_iter().map(|h| (h.kind, h.revision)));
        }
        assert_eq!(delivered, vec![(4, 1), (4, 2), (5, 3), (5, 4)],
            "every accepted icon request must reach its per-revision acknowledgement");
        assert_eq!(a.counts().coalesced, 0);
    }

    #[test]
    fn acknowledged_icons_refuse_overflow_explicitly() {
        let (mut a, mut b) = pair(Limits { in_flight_frames: 1, outbox_frames: 1, ..Limits::default() });
        a.send(1, Channel::Requests, 0, 0, &json("{}"));
        pump(&mut a, &mut b);
        let icon = [1u32.to_le_bytes(), 1u32.to_le_bytes(), 0xffffffffu32.to_le_bytes()].concat();
        assert_eq!(a.send(1, Channel::Window, 4, 1, &icon), SendOutcome::Queued);
        assert_eq!(a.send(1, Channel::Window, 4, 2, &icon), SendOutcome::Overflow,
            "the caller must unwind a refused request instead of waiting for a missing acknowledgement");
        ack(&mut a, &mut b);
        let delivered = pump(&mut a, &mut b);
        assert_eq!(delivered.iter().map(|h| h.revision).collect::<Vec<_>>(), vec![1]);
    }

    #[test]
    fn ordered_and_delta_frames_overflow_explicitly() {
        let limits = Limits { in_flight_frames: 1, outbox_frames: 2, ..Limits::default() };
        let (mut a, mut b) = pair(limits);
        a.send(1, Channel::Requests, 0, 0, &json("{}"));
        pump(&mut a, &mut b);
        assert_eq!(a.send(1, Channel::Requests, 0, 1, &json("{}")), SendOutcome::Queued);
        assert_eq!(a.send(1, Channel::Tabs, 1, 2, &json("{}")), SendOutcome::Queued);
        assert_eq!(a.send(1, Channel::Tabs, 1, 3, &json("{}")), SendOutcome::Overflow);
        assert_eq!(a.send(1, Channel::Window, 0, 4, &[0; 32]), SendOutcome::Overflow);
        assert_eq!(a.send(1, Channel::Geometry, 0, 5, &shown([0, 0, 1, 1])), SendOutcome::Queued, "snapshots are still admitted");
        assert_eq!(a.send(1, Channel::Focus, 0, 6, &[1]), SendOutcome::Overflow);
        assert_eq!(a.counts().overflowed, 3);
    }

    #[test]
    fn invalid_frames_are_refused_at_send() {
        let (mut a, _) = pair(Limits::default());
        assert_eq!(a.send(1, Channel::Control, 0, 0, &[0; 16]), SendOutcome::Invalid);
        assert_eq!(a.send(1, Channel::Focus, 0, 0, &[2]), SendOutcome::Invalid);
        assert_eq!(a.send(1, Channel::Address, 7, 0, &[]), SendOutcome::Invalid);
    }

    #[test]
    fn other_generations_are_dropped_and_gaps_close_the_link() {
        let mut a = Endpoint::new(1, Limits::default());
        let mut stale = Endpoint::new(2, Limits::default());
        stale.send(1, Channel::Focus, 0, 0, &[1]);
        assert_eq!(a.receive(&stale.take().unwrap()), Err(ReceiveError::Generation));
        assert!(!a.is_closed());
        let mut b = Endpoint::new(1, Limits::default());
        b.send(1, Channel::Focus, 0, 0, &[1]);
        b.send(1, Channel::Focus, 1, 0, &[1]);
        b.take().unwrap();
        assert_eq!(a.receive(&b.take().unwrap()), Err(ReceiveError::Sequence));
        assert!(a.is_closed());
    }

    #[test]
    fn retired_windows_are_refused_and_dropped() {
        let (mut a, mut b) = pair(Limits::default());
        a.send(3, Channel::Focus, 0, 0, &[1]);
        b.retire(3);
        assert!(pump(&mut a, &mut b).is_empty());
        assert_eq!(b.counts().dropped, 1);
        a.retire(3);
        assert_eq!(a.send(3, Channel::Focus, 0, 0, &[1]), SendOutcome::Closed);
        assert_eq!(a.send(4, Channel::Focus, 0, 0, &[1]), SendOutcome::Queued);
    }

    #[test]
    fn credit_for_unsent_frames_closes_the_link() {
        let (mut a, mut b) = pair(Limits::default());
        b.send(1, Channel::Focus, 0, 0, &[1]);
        a.receive(&b.take().unwrap()).unwrap();
        let mut forged = Endpoint::new(1, Limits::default());
        forged.consumed_frames = 5; forged.ack_due = true; forged.next_sequence = 1;
        assert_eq!(b.receive(&forged.take().unwrap()), Err(ReceiveError::Credit));
    }

    #[test]
    fn a_forced_take_ignores_credit() {
        let limits = Limits { in_flight_frames: 1, ..Limits::default() };
        let (mut a, _) = pair(limits);
        a.send(1, Channel::Requests, 0, 0, &json("{}"));
        a.send(1, Channel::Requests, 0, 1, &json("{}"));
        assert!(a.take().is_some());
        assert!(a.take().is_none());
        assert!(a.take_forced().is_some());
        assert_eq!(a.counts().in_flight, 2);
    }

    #[test]
    fn one_large_frame_goes_when_nothing_is_in_flight() {
        let limits = Limits { in_flight_bytes: 8, ..Limits::default() };
        let (mut a, mut b) = pair(limits);
        a.send(1, Channel::Address, 0, 0, &json("a long publication"));
        assert_eq!(pump(&mut a, &mut b).len(), 1);
    }

    #[test]
    fn window_serials_are_never_reused() {
        let mut w = Windows::default();
        let first = w.serial(0x10, true).unwrap();
        assert_eq!(w.serial(0x10, false), Some(first));
        w.forget(first);
        let second = w.serial(0x10, true).unwrap();
        assert_ne!(first, second);
        assert_eq!(w.serial(0, true), None);
        let mut other = Windows::default();
        assert!(other.adopt(second, 0x99));
        assert!(!other.adopt(second, 0x98));
        assert_eq!(other.handle(second), Some(0x99));
    }
}
