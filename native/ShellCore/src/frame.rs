/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! The frame format between the engine and the shell (M5b).
//!
//! A frame is a fixed little-endian header and a bounded payload. The codec
//! knows every channel's delivery class and payload bound, and validates a
//! frame completely before handing its payload to anyone. It holds no state:
//! sequencing, credits and coalescing live in [`crate::link`].
//!
//! Payloads are either UTF-16 text (the JSON both sides already speak, never
//! transcoded) or one of the fixed binary layouts below. Nothing in a frame is
//! a pointer, so the same bytes can later cross a process boundary.

pub const MAGIC: u16 = 0x4E46; // "FN"
pub const VERSION: u16 = 1;
pub const HEADER_BYTES: usize = 40;

/// How a channel's frames are delivered under backpressure. These are the
/// [`crate::publication::StreamKind`] rules applied to transport.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Class {
    /// Credit returns. Never queued behind other frames and never refused.
    Control,
    /// Complete publications: an unsent frame with the same window, channel
    /// and kind is replaced in place, so a stalled receiver gets the newest.
    Snapshot,
    /// Revisioned deltas: refused when the queue is full, and the producer
    /// recovers with one complete publication.
    Delta,
    /// Requests and lifetime changes: kept in order, refused when full.
    Ordered,
}

#[repr(u16)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Channel {
    Control = 0,
    /// Bind, destroy, wakeup registration, introduction: kind is [`Lifetime`].
    Lifetime = 1,
    /// Kind 0 strip rectangle, 1 address rectangle: one rectangle and a
    /// visibility byte. Kind 2 clip list ([`rects`]).
    Geometry = 2,
    /// Kind 0 complete tab rows ([`tabs`]), 1 JSON delta.
    Tabs = 3,
    Address = 4,
    TabMenu = 5,
    /// Kind 0 page focus, 1 address focus; payload one byte, 0 or 1.
    Focus = 6,
    /// Kind 0 window request ([`window_request`]), 1 title text, 2 applied
    /// acknowledgement (shell to engine; payload the endpoint handle to report
    /// to, or 0), 3 refusal (shell to engine; payload u16: 0 strip surface,
    /// 1 address surface, 2 bind).
    Window = 7,
    /// Engine to shell: completed outcomes. Shell to engine: request batches.
    Requests = 8,
    /// Startup handoff: requests one way, replies the other, paired by id.
    Startup = 9,
    /// Close resolution: two u64, generation and request.
    Close = 10,
    /// Shell to the engine's input thread (M5c): kind 0, the page input window's shape
    /// ([`page_shape`]), which only its owner can apply.
    Page = 11,
}

#[repr(u16)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Lifetime {
    Bind = 0,
    Destroy = 1,
    WakeupOn = 2,
    WakeupOff = 3,
    /// The first frame for a window: the shell's own handle for it, which the
    /// shell checks is one of its windows. In one process this is the HWND.
    Introduce = 4,
    /// The window itself goes, after everything queued before it.
    DestroyHost = 5,
    Create = 6,
    Bound = 7,
    PageReady = 8,
    Profile = 9,
    Confirm = 10,
    Presented = 11,
    RecoveryCommit = 12,
    RecoveryDeferred = 13,
    RecoveryResumed = 14,
    RecoveryEmpty = 15,
    RecoverySuppress = 16,
}

impl Channel {
    pub fn from_u16(value: u16) -> Option<Self> {
        use Channel::*;
        Some(match value {
            0 => Control,
            1 => Lifetime,
            2 => Geometry,
            3 => Tabs,
            4 => Address,
            5 => TabMenu,
            6 => Focus,
            7 => Window,
            8 => Requests,
            9 => Startup,
            10 => Close,
            11 => Page,
            _ => return None,
        })
    }
}

/// The delivery class and payload bound of one channel and kind, or `None`
/// for a pair the protocol does not define.
pub fn rule(channel: Channel, kind: u16) -> Option<(Class, usize)> {
    use Channel::*;
    // The JSON bound every handler applies: 1048576 UTF-16 units.
    const TEXT: usize = 1048576 * 2;
    Some(match (channel, kind) {
        (Control, 0) => (Class::Control, 16),
        (Lifetime, 0) => (Class::Ordered, 16),
        (Lifetime, 1..=3) => (Class::Ordered, 0),
        (Lifetime, 4) => (Class::Ordered, 8),
        (Lifetime, 5) => (Class::Ordered, 0),
        (Lifetime, 6) => (Class::Ordered, 104),
        (Lifetime, 7 | 8) => (Class::Ordered, 8),
        (Lifetime, 9) => (Class::Ordered, 131074),
        (Lifetime, 10..=14 | 16) => (Class::Ordered, 0),
        (Lifetime, 15) => (Class::Ordered, 128 * 16),
        // Already latest-wins before M5b: the only coalesced channel so far.
        (Geometry, 0 | 1) => (Class::Snapshot, rects::BYTES + 1),
        (Geometry, 2) => (Class::Snapshot, rects::BYTES * rects::MAX_CLIP),
        (Tabs, 0) => (Class::Ordered, tabs::MAX_BYTES),
        (Tabs, 1) => (Class::Delta, TEXT),
        (Address | TabMenu, 0) => (Class::Ordered, TEXT),
        (Focus, 0 | 1) => (Class::Ordered, 1),
        (Window, 0) => (Class::Ordered, window_request::BYTES),
        (Window, 1) => (Class::Ordered, 32768 * 2),
        (Window, 2) => (Class::Ordered, 8),
        (Window, 3) => (Class::Ordered, 2),
        // Icons carry request revisions with an applied acknowledgement each.
        // Coalescing would strand the superseded request's pending counter.
        (Window, 4 | 5) => (Class::Ordered, 8 + 256 * 256 * 4),
        (Window, 6) => (Class::Snapshot, 64),
        (Window, 7) => (Class::Ordered, share_request::MAX_BYTES),
        (Window, 8) => (Class::Ordered, 24),
        (Requests, 0) => (Class::Ordered, TEXT),
        (Startup, 0) => (Class::Ordered, TEXT),
        (Close, 0) => (Class::Ordered, 16),
        (Page, 0) => (Class::Snapshot, page_shape::MAX_BYTES),
        _ => return None,
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Header {
    pub channel: Channel,
    pub kind: u16,
    /// Connection generation. A restarted engine or shell starts a new one.
    pub generation: u32,
    /// Window serial, never reused within a connection; 0 for the connection.
    pub window: u32,
    /// Per-direction sequence, contiguous from 1.
    pub sequence: u64,
    /// The publication revision or request id the payload carries; 0 if none.
    pub revision: u64,
    pub length: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DecodeError {
    Short,
    Magic,
    Version,
    Channel,
    Kind,
    Length,
    Reserved,
    Payload,
}

pub fn encode(header: &Header, payload: &[u8], out: &mut Vec<u8>) {
    debug_assert_eq!(header.length as usize, payload.len());
    out.reserve(HEADER_BYTES + payload.len());
    out.extend_from_slice(&MAGIC.to_le_bytes());
    out.extend_from_slice(&VERSION.to_le_bytes());
    out.extend_from_slice(&(header.channel as u16).to_le_bytes());
    out.extend_from_slice(&header.kind.to_le_bytes());
    out.extend_from_slice(&header.generation.to_le_bytes());
    out.extend_from_slice(&header.window.to_le_bytes());
    out.extend_from_slice(&header.sequence.to_le_bytes());
    out.extend_from_slice(&header.revision.to_le_bytes());
    out.extend_from_slice(&header.length.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    out.extend_from_slice(payload);
}

fn u16_at(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}
fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}
fn u64_at(b: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(b[at..at + 8].try_into().unwrap())
}

/// Validates one whole frame (exactly `bytes`) and returns its header and payload.
pub fn decode(bytes: &[u8]) -> Result<(Header, &[u8]), DecodeError> {
    if bytes.len() < HEADER_BYTES {
        return Err(DecodeError::Short);
    }
    if u16_at(bytes, 0) != MAGIC {
        return Err(DecodeError::Magic);
    }
    if u16_at(bytes, 2) != VERSION {
        return Err(DecodeError::Version);
    }
    let channel = Channel::from_u16(u16_at(bytes, 4)).ok_or(DecodeError::Channel)?;
    let kind = u16_at(bytes, 6);
    let (_, bound) = rule(channel, kind).ok_or(DecodeError::Kind)?;
    let length = u32_at(bytes, 32);
    if u32_at(bytes, 36) != 0 {
        return Err(DecodeError::Reserved);
    }
    if length as usize > bound || bytes.len() != HEADER_BYTES + length as usize {
        return Err(DecodeError::Length);
    }
    let header = Header {
        channel,
        kind,
        generation: u32_at(bytes, 8),
        window: u32_at(bytes, 12),
        sequence: u64_at(bytes, 16),
        revision: u64_at(bytes, 24),
        length,
    };
    let payload = &bytes[HEADER_BYTES..];
    if !valid_payload(channel, kind, payload) {
        return Err(DecodeError::Payload);
    }
    Ok((header, payload))
}

pub(crate) fn valid_payload(channel: Channel, kind: u16, p: &[u8]) -> bool {
    use Channel::*;
    match (channel, kind) {
        (Control, _) => p.len() == 16,
        (Lifetime, 0) | (Close, 0) => p.len() == 16,
        (Lifetime, 4) => p.len() == 8 && p != [0; 8],
        (Lifetime, 6) => {
            p.len() == 104
                && p[..8] != [0; 8]
                && p[8..16] != [0; 8]
                && rects::decode(&p[16..32]).is_some()
                && u32_at(p, 32) <= 3
                && u32_at(p, 36) == 0
                && (p[40..] == [0; 64] || (u32_at(p, 32) & 2 != 0 && valid_placement(&p[40..])))
        }
        (Lifetime, 7 | 8) => p.len() == 8 && p != [0; 8],
        (Lifetime, 9) => {
            p.len() >= 2 && p.len() % 2 == 0 && text(p).iter().filter(|&&v| v == 0).count() == 1
        }
        (Window, 2) => p.len() == 8,
        (Window, 6) => valid_placement(p),
        (Window, 7) => share_request::valid(p),
        (Window, 8) => p.len() == 24,
        (Window, 3) => p.len() == 2 && u16::from_le_bytes([p[0], p[1]]) <= 2,
        (Window, 4 | 5) => {
            p.len() >= 8 && {
                let (w, h) = (u32_at(p, 0) as usize, u32_at(p, 4) as usize);
                (1..=256).contains(&w) && (1..=256).contains(&h) && p.len() == 8 + w * h * 4
            }
        }
        (Lifetime, 15) => {
            !p.is_empty() && p.len() % 16 == 0 && p.chunks_exact(16).all(|key| key != [0; 16]) && {
                let mut keys = std::collections::BTreeSet::new();
                p.chunks_exact(16).all(|key| keys.insert(key))
            }
        }
        (Lifetime, _) => p.is_empty(),
        (Geometry, 0 | 1) => {
            p.len() == rects::BYTES + 1
                && p[rects::BYTES] <= 1
                && rects::decode(&p[..rects::BYTES]).is_some()
        }
        (Geometry, 2) => rects::decode(p).is_some(),
        (Tabs, 0) => tabs::decode(p).is_some(),
        (Focus, _) => p.len() == 1 && p[0] <= 1,
        (Window, 0) => window_request::decode(p).is_some(),
        (Page, 0) => page_shape::decode(p).is_some(),
        // Text: whole UTF-16 units. JSON content is validated by its handler.
        _ => p.len() % 2 == 0,
    }
}

// Native placement snapshot: outer/normal RECTs, min/max POINTs, then
// WINDOWPLACEMENT flags, normalized show state, foreground bool and reserved.
fn valid_placement(p: &[u8]) -> bool {
    p.len() == 64
        && rects::decode(&p[..32]).is_some()
        && u32_at(p, 48) <= 7
        && matches!(u32_at(p, 52), 1 | 2 | 3)
        && u32_at(p, 56) <= 1
        && u32_at(p, 60) == 0
}

pub fn text(payload: &[u8]) -> Vec<u16> {
    payload
        .chunks_exact(2)
        .map(|c| u16::from_le_bytes([c[0], c[1]]))
        .collect()
}

pub fn text_bytes(units: &[u16]) -> Vec<u8> {
    units.iter().flat_map(|u| u.to_le_bytes()).collect()
}

/// Rectangles as four i32 each: left, top, right, bottom, with right >= left
/// and bottom >= top.
pub mod rects {
    pub const BYTES: usize = 16;
    pub const MAX_CLIP: usize = 64;
    pub type Rect = [i32; 4];

    pub fn encode(rects: &[Rect]) -> Vec<u8> {
        rects
            .iter()
            .flat_map(|r| r.iter().flat_map(|v| v.to_le_bytes()))
            .collect()
    }

    pub fn decode(p: &[u8]) -> Option<Vec<Rect>> {
        if p.len() % BYTES != 0 || p.len() / BYTES > MAX_CLIP {
            return None;
        }
        let out: Vec<Rect> = p
            .chunks_exact(BYTES)
            .map(|c| {
                let v = |i: usize| i32::from_le_bytes(c[i * 4..i * 4 + 4].try_into().unwrap());
                [v(0), v(1), v(2), v(3)]
            })
            .collect();
        out.iter()
            .all(|r| r[2] >= r[0] && r[3] >= r[1])
            .then_some(out)
    }
}

/// The page input window's shape: u32 chrome count, then the client rectangle, the visible
/// chrome cut out of it, and Firefox's exclusions added back, all client-relative ([`rects`]).
pub mod page_shape {
    use super::rects::{self, Rect};
    /// Strip and address, plus the clip list's bound.
    pub const MAX_CHROME: usize = 2;
    pub const MAX_BYTES: usize = 4 + rects::BYTES * (1 + MAX_CHROME + rects::MAX_CLIP);

    #[derive(Clone, Debug, PartialEq, Eq)]
    pub struct Shape {
        pub client: Rect,
        pub chrome: Vec<Rect>,
        pub exclusions: Vec<Rect>,
    }

    pub fn encode(s: &Shape) -> Option<Vec<u8>> {
        if s.chrome.len() > MAX_CHROME || s.exclusions.len() > rects::MAX_CLIP {
            return None;
        }
        let mut out = (s.chrome.len() as u32).to_le_bytes().to_vec();
        out.extend(rects::encode(&[s.client]));
        out.extend(rects::encode(&s.chrome));
        out.extend(rects::encode(&s.exclusions));
        Some(out)
    }

    pub fn decode(p: &[u8]) -> Option<Shape> {
        if p.len() < 4 + rects::BYTES || p.len() > MAX_BYTES || (p.len() - 4) % rects::BYTES != 0 {
            return None;
        }
        let chrome = u32::from_le_bytes(p[..4].try_into().unwrap()) as usize;
        let all: Vec<Rect> = p[4..]
            .chunks_exact(rects::BYTES)
            .map(|c| {
                let v = |i: usize| i32::from_le_bytes(c[i * 4..i * 4 + 4].try_into().unwrap());
                [v(0), v(1), v(2), v(3)]
            })
            .collect();
        if chrome > MAX_CHROME
            || chrome + 1 > all.len()
            || all.len() - 1 - chrome > rects::MAX_CLIP
            || !all.iter().all(|r| r[2] >= r[0] && r[3] >= r[1])
        {
            return None;
        }
        Some(Shape {
            client: all[0],
            chrome: all[1..1 + chrome].to_vec(),
            exclusions: all[1 + chrome..].to_vec(),
        })
    }
}

/// A window request: kind u32, flags u32, rect (4 x i32), value i64. The
/// shell answers each applied request with a [`Channel::Window`] kind 2
/// frame, which replaces the pending counter Gecko used to share by pointer.
pub mod window_request {
    pub const BYTES: usize = 32;

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub struct WindowRequest {
        pub kind: u32,
        pub flags: u32,
        pub rect: [i32; 4],
        pub value: i64,
    }

    pub fn encode(r: &WindowRequest) -> Vec<u8> {
        let mut out = Vec::with_capacity(BYTES);
        out.extend_from_slice(&r.kind.to_le_bytes());
        out.extend_from_slice(&r.flags.to_le_bytes());
        for v in r.rect {
            out.extend_from_slice(&v.to_le_bytes());
        }
        out.extend_from_slice(&r.value.to_le_bytes());
        out
    }

    pub fn decode(p: &[u8]) -> Option<WindowRequest> {
        if p.len() != BYTES {
            return None;
        }
        let i = |at: usize| i32::from_le_bytes(p[at..at + 4].try_into().unwrap());
        Some(WindowRequest {
            kind: i(0) as u32,
            flags: i(4) as u32,
            rect: [i(8), i(12), i(16), i(20)],
            value: i64::from_le_bytes(p[24..32].try_into().unwrap()),
        })
    }
}

/// Complete tab rows: selected u32, publication revision u32, count u32, then
/// per row id u32 (nonzero, unique), commands u32, title length u32 (UTF-16
/// units, at most [`MAX_TITLE`]) and the title.
pub mod tabs {
    pub const MAX_ROWS: usize = 10000;
    pub const MAX_TITLE: usize = 32768;
    pub const MAX_BYTES: usize = 32 * 1024 * 1024;

    #[derive(Clone, Debug, PartialEq, Eq)]
    pub struct Row {
        pub id: u32,
        pub commands: u32,
        pub title: Vec<u16>,
    }

    #[derive(Clone, Debug, PartialEq, Eq)]
    pub struct Rows {
        pub selected: u32,
        pub revision: u32,
        pub rows: Vec<Row>,
    }

    pub fn encode(t: &Rows) -> Vec<u8> {
        let mut out = Vec::new();
        for v in [t.selected, t.revision, t.rows.len() as u32] {
            out.extend_from_slice(&v.to_le_bytes());
        }
        for row in &t.rows {
            for v in [row.id, row.commands, row.title.len() as u32] {
                out.extend_from_slice(&v.to_le_bytes());
            }
            for u in &row.title {
                out.extend_from_slice(&u.to_le_bytes());
            }
        }
        out
    }

    pub fn decode(p: &[u8]) -> Option<Rows> {
        fn u32_next(p: &[u8], at: &mut usize) -> Option<u32> {
            let v = p.get(*at..*at + 4)?;
            *at += 4;
            Some(u32::from_le_bytes(v.try_into().ok()?))
        }
        let mut at = 0usize;
        let selected = u32_next(p, &mut at)?;
        let revision = u32_next(p, &mut at)?;
        let count = u32_next(p, &mut at)? as usize;
        if count > MAX_ROWS {
            return None;
        }
        let mut rows = Vec::with_capacity(count);
        for _ in 0..count {
            let id = u32_next(p, &mut at)?;
            let commands = u32_next(p, &mut at)?;
            let len = u32_next(p, &mut at)? as usize;
            if id == 0 || len > MAX_TITLE {
                return None;
            }
            let raw = p.get(at..at + len * 2)?;
            at += len * 2;
            rows.push(Row {
                id,
                commands,
                title: raw
                    .chunks_exact(2)
                    .map(|c| u16::from_le_bytes([c[0], c[1]]))
                    .collect(),
            });
        }
        if at != p.len() {
            return None;
        }
        let mut ids: Vec<u32> = rows.iter().map(|r| r.id).collect();
        ids.sort_unstable();
        if ids.windows(2).any(|w| w[0] == w[1]) {
            return None;
        }
        if selected != 0 && !rows.iter().any(|r| r.id == selected) {
            return None;
        }
        Some(Rows {
            selected,
            revision,
            rows,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(channel: Channel, kind: u16, payload: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        encode(
            &Header {
                channel,
                kind,
                generation: 3,
                window: 7,
                sequence: 9,
                revision: 11,
                length: payload.len() as u32,
            },
            payload,
            &mut out,
        );
        out
    }

    #[test]
    fn round_trip_keeps_every_field() {
        let payload = text_bytes(&"{\"a\":1}".encode_utf16().collect::<Vec<_>>());
        let bytes = frame(Channel::Address, 0, &payload);
        let (h, p) = decode(&bytes).unwrap();
        assert_eq!(
            (
                h.channel,
                h.kind,
                h.generation,
                h.window,
                h.sequence,
                h.revision
            ),
            (Channel::Address, 0, 3, 7, 9, 11)
        );
        assert_eq!(p, &payload[..]);
        assert_eq!(text(p), "{\"a\":1}".encode_utf16().collect::<Vec<_>>());
    }

    #[test]
    fn malformed_headers_are_refused() {
        let good = frame(Channel::Focus, 1, &[1]);
        assert_eq!(
            decode(&good[..HEADER_BYTES - 1]).unwrap_err(),
            DecodeError::Short
        );
        let mut b = good.clone();
        b[0] ^= 1;
        assert_eq!(decode(&b).unwrap_err(), DecodeError::Magic);
        let mut b = good.clone();
        b[2] = 9;
        assert_eq!(decode(&b).unwrap_err(), DecodeError::Version);
        let mut b = good.clone();
        b[4] = 99;
        assert_eq!(decode(&b).unwrap_err(), DecodeError::Channel);
        let mut b = good.clone();
        b[6] = 9;
        assert_eq!(decode(&b).unwrap_err(), DecodeError::Kind);
        let mut b = good.clone();
        b[36] = 1;
        assert_eq!(decode(&b).unwrap_err(), DecodeError::Reserved);
        let mut b = good.clone();
        b.push(0);
        assert_eq!(decode(&b).unwrap_err(), DecodeError::Length);
        let mut b = good.clone();
        *b.last_mut().unwrap() = 2;
        assert_eq!(decode(&b).unwrap_err(), DecodeError::Payload);
    }

    #[test]
    fn bounds_are_per_channel() {
        assert!(decode(&frame(Channel::Focus, 0, &[0, 0])).is_err());
        let over = vec![0u8; rects::BYTES * (rects::MAX_CLIP + 1)];
        assert!(decode(&frame(Channel::Geometry, 2, &over)).is_err());
        assert!(
            decode(&frame(Channel::Address, 0, &[1, 2, 3])).is_err(),
            "odd byte count is not UTF-16"
        );
        assert!(decode(&frame(Channel::Lifetime, 1, &[0])).is_err());
        assert!(
            decode(&frame(Channel::Lifetime, 4, &[0; 8])).is_err(),
            "no null handle"
        );
        assert!(decode(&frame(Channel::Lifetime, 4, &[1, 0, 0, 0, 0, 0, 0, 0])).is_ok());
        assert!(decode(&frame(Channel::Lifetime, 5, &[])).is_ok());
        assert!(decode(&frame(Channel::Window, 3, &[2, 0])).is_ok());
        assert!(
            decode(&frame(Channel::Window, 3, &[3, 0])).is_err(),
            "unknown refusal"
        );
    }

    #[test]
    fn process_bind_and_icon_payloads_reject_malformed_data() {
        let mut create = vec![0u8; 104];
        create[..8].copy_from_slice(&1u64.to_le_bytes());
        create[8..16].copy_from_slice(&2u64.to_le_bytes());
        create[16..32].copy_from_slice(&rects::encode(&[[0, 0, 800, 600]]));
        assert!(decode(&frame(Channel::Lifetime, 6, &create)).is_ok());
        for offset in [0, 8] {
            let mut bad = create.clone();
            bad[offset..offset + 8].fill(0);
            assert!(decode(&frame(Channel::Lifetime, 6, &bad)).is_err());
        }
        let mut bad = create.clone();
        bad[36] = 1;
        assert!(decode(&frame(Channel::Lifetime, 6, &bad)).is_err());
        for flags in 1..=3 {
            let mut recovery = create.clone();
            recovery[32] = flags;
            assert!(decode(&frame(Channel::Lifetime, 6, &recovery)).is_ok());
        }
        let mut bad = create.clone();
        bad[32] = 4;
        assert!(decode(&frame(Channel::Lifetime, 6, &bad)).is_err());
        let mut placement = vec![0u8; 64];
        placement[..32]
            .copy_from_slice(&rects::encode(&[[0, 0, 1920, 1040], [180, 60, 1080, 710]]));
        placement[52] = 3;
        placement[56] = 1;
        assert!(decode(&frame(Channel::Window, 6, &placement)).is_ok());
        let mut recovery = create.clone();
        recovery[32] = 2;
        recovery[40..].copy_from_slice(&placement);
        assert!(decode(&frame(Channel::Lifetime, 6, &recovery)).is_ok());
        recovery[32] = 0;
        assert!(decode(&frame(Channel::Lifetime, 6, &recovery)).is_err());
        for (offset, value) in [(48, 8), (52, 0), (52, 4), (56, 2), (60, 1)] {
            let mut bad = placement.clone();
            bad[offset] = value;
            assert!(decode(&frame(Channel::Window, 6, &bad)).is_err());
            recovery[32] = 2;
            recovery[40..].copy_from_slice(&bad);
            assert!(decode(&frame(Channel::Lifetime, 6, &recovery)).is_err());
        }
        assert!(decode(&frame(Channel::Window, 6, &placement[..63])).is_err());
        assert_eq!(rule(Channel::Window, 6), Some((Class::Snapshot, 64)));
        for kind in [7, 8] {
            assert!(decode(&frame(Channel::Lifetime, kind, &[0; 8])).is_err());
            assert!(decode(&frame(Channel::Lifetime, kind, &1u64.to_le_bytes())).is_ok());
        }
        assert!(decode(&frame(Channel::Lifetime, 9, &[0, 0])).is_ok());
        assert!(decode(&frame(Channel::Lifetime, 9, &[0, 0, 0, 0])).is_err());
        assert!(decode(&frame(Channel::Lifetime, 9, &[1, 0])).is_err());
        let mut icon = vec![0u8; 12];
        icon[..4].copy_from_slice(&1u32.to_le_bytes());
        icon[4..8].copy_from_slice(&1u32.to_le_bytes());
        assert!(decode(&frame(Channel::Window, 4, &icon)).is_ok());
        assert!(decode(&frame(Channel::Window, 5, &icon[..11])).is_err());
        icon[..4].copy_from_slice(&257u32.to_le_bytes());
        assert!(decode(&frame(Channel::Window, 4, &icon)).is_err());
    }

    #[test]
    fn recovery_empty_keys_are_bounded_nonzero_and_unique() {
        assert!(decode(&frame(Channel::Lifetime, 15, &[1; 16])).is_ok());
        for bytes in [
            vec![],
            vec![0; 16],
            vec![1; 15],
            vec![1; 32],
            vec![1; 129 * 16],
        ] {
            assert!(decode(&frame(Channel::Lifetime, 15, &bytes)).is_err());
        }
        let keys: Vec<u8> = (1u8..=128).flat_map(|n| [n; 16]).collect();
        assert!(decode(&frame(Channel::Lifetime, 15, &keys)).is_ok());
    }

    #[test]
    fn rectangles_validate_orientation_and_count() {
        let shown = |r: rects::Rect, visible: u8| {
            let mut p = rects::encode(&[r]);
            p.push(visible);
            p
        };
        assert!(decode(&frame(Channel::Geometry, 0, &shown([1, 2, 3, 4], 1))).is_ok());
        assert!(decode(&frame(Channel::Geometry, 0, &shown([3, 2, 1, 4], 1))).is_err());
        assert!(decode(&frame(Channel::Geometry, 1, &shown([0, 0, 1, 1], 2))).is_err());
        assert!(
            decode(&frame(
                Channel::Geometry,
                1,
                &rects::encode(&[[0, 0, 1, 1]])
            ))
            .is_err(),
            "no visibility byte"
        );
        assert_eq!(
            rects::decode(&rects::encode(&[[0, 0, 5, 5], [1, 1, 2, 2]]))
                .unwrap()
                .len(),
            2
        );
        let shape = page_shape::Shape {
            client: [0, 0, 800, 600],
            chrome: vec![[0, 0, 800, 40]],
            exclusions: vec![[700, 0, 760, 40]],
        };
        let bytes = page_shape::encode(&shape).unwrap();
        assert_eq!(page_shape::decode(&bytes), Some(shape.clone()));
        assert!(valid_payload(Channel::Page, 0, &bytes));
        let mut wrong = bytes.clone();
        wrong[0] = 3;
        assert!(
            page_shape::decode(&wrong).is_none(),
            "more chrome than rectangles or than the bound"
        );
        assert!(page_shape::decode(&bytes[..bytes.len() - 1]).is_none());
        let crowded = page_shape::Shape {
            exclusions: vec![[0, 0, 1, 1]; rects::MAX_CLIP + 1],
            ..shape
        };
        assert!(page_shape::encode(&crowded).is_none());
        assert!(
            decode(&frame(Channel::Geometry, 2, &[])).is_ok(),
            "an empty clip restores the surface"
        );
    }

    #[test]
    fn tab_rows_round_trip_and_reject_bad_identity() {
        let rows = tabs::Rows {
            selected: 2,
            revision: 5,
            rows: vec![
                tabs::Row {
                    id: 1,
                    commands: 3,
                    title: "One".encode_utf16().collect(),
                },
                tabs::Row {
                    id: 2,
                    commands: 0,
                    title: vec![],
                },
            ],
        };
        assert_eq!(tabs::decode(&tabs::encode(&rows)).unwrap(), rows);
        let mut dup = rows.clone();
        dup.rows[1].id = 1;
        assert!(tabs::decode(&tabs::encode(&dup)).is_none());
        let mut zero = rows.clone();
        zero.rows[0].id = 0;
        assert!(tabs::decode(&tabs::encode(&zero)).is_none());
        let mut missing = rows.clone();
        missing.selected = 9;
        assert!(tabs::decode(&tabs::encode(&missing)).is_none());
        let mut bytes = tabs::encode(&rows);
        bytes.push(0);
        assert!(tabs::decode(&bytes).is_none(), "trailing bytes");
        bytes.truncate(bytes.len() - 3);
        assert!(tabs::decode(&bytes).is_none(), "truncated title");
    }

    #[test]
    fn window_requests_round_trip() {
        let r = window_request::WindowRequest {
            kind: 2,
            flags: 0x10,
            rect: [-8, -8, 100, 200],
            value: -1,
        };
        assert_eq!(window_request::decode(&window_request::encode(&r)), Some(r));
        assert!(decode(&frame(Channel::Window, 0, &window_request::encode(&r))).is_ok());
        assert!(decode(&frame(Channel::Window, 0, &[0; 31])).is_err());
    }
}

/// Ephemeral Windows Share request. Never a snapshot or recovery publication.
/// Session/document/deadline are u64; title and URL lengths are u32 UTF-16 units.
pub mod share_request {
    use super::{text, u32_at, u64_at};
    pub const MAX_BYTES: usize = 32 + 2 * (4096 + 32768);
    pub fn valid(p: &[u8]) -> bool {
        if p.len() < 32
            || p.len() > MAX_BYTES
            || u64_at(p, 0) == 0
            || u64_at(p, 8) == 0
            || u64_at(p, 16) == 0
        {
            return false;
        }
        let title = u32_at(p, 24) as usize;
        let url = u32_at(p, 28) as usize;
        if !(1..=4096).contains(&title)
            || !(1..=32768).contains(&url)
            || p.len() != 32 + 2 * (title + url)
        {
            return false;
        }
        [&p[32..32 + 2 * title], &p[32 + 2 * title..]]
            .iter()
            .all(|bytes| {
                let units = text(bytes);
                !units.contains(&0) && String::from_utf16(&units).is_ok()
            })
    }
    pub fn current(p: &[u8], session: u64, document: u64, now: u64) -> bool {
        valid(p)
            && u64_at(p, 0) == session
            && u64_at(p, 8) == document
            && now <= u64_at(p, 16)
            && u64_at(p, 16) - now <= 5000
    }
    #[cfg(test)]
    mod tests {
        use super::*;
        fn request() -> Vec<u8> {
            let mut p = Vec::new();
            for n in [2u64, 3, 6000] {
                p.extend(n.to_le_bytes());
            }
            for n in [1u32, 1] {
                p.extend(n.to_le_bytes());
            }
            p.extend([65, 0, 66, 0]);
            p
        }
        #[test]
        fn scope_and_expiry() {
            let p = request();
            assert!(current(&p, 2, 3, 1000));
            for (s, d, n) in [(1, 3, 1000), (2, 4, 1000), (2, 3, 6001), (2, 3, 999)] {
                assert!(!current(&p, s, d, n));
            }
        }
        #[test]
        fn rejects_malformed() {
            let p = request();
            for n in 0..p.len() {
                assert!(!valid(&p[..n]));
            }
            let mut q = p.clone();
            q[32] = 0;
            assert!(!valid(&q));
            let mut q = p.clone();
            q[33] = 0xd8;
            assert!(!valid(&q));
            let mut q = p;
            q[24..28].copy_from_slice(&4097u32.to_le_bytes());
            assert!(!valid(&q));
        }
    }
}
