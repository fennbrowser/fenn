//! M5d's transport envelope and bounded byte slots. No OS or browser state.
//! Each route retains its own link sequencing and credits, including control frames.
use std::collections::VecDeque;

pub(crate) const HEADER: usize = 12;
pub(crate) const MAX_RECORD: usize = crate::frame::tabs::MAX_BYTES + crate::frame::HEADER_BYTES;
pub(crate) const BYTE_BUDGET: usize = MAX_RECORD + 2 * 1024 * 1024;
pub(crate) const CAPACITY: usize = 16;

pub fn header(route: u32, length: usize) -> Option<[u8; HEADER]> {
    if route > 1 || !(crate::frame::HEADER_BYTES..=MAX_RECORD).contains(&length) {
        return None;
    }
    let mut out = [0; HEADER];
    out[..4].copy_from_slice(b"FNP1");
    out[4..8].copy_from_slice(&route.to_le_bytes());
    out[8..].copy_from_slice(&(length as u32).to_le_bytes());
    Some(out)
}
pub fn decode(bytes: &[u8]) -> Option<(u32, usize)> {
    if bytes.len() != HEADER || &bytes[..4] != b"FNP1" {
        return None;
    }
    let route = u32::from_le_bytes(bytes[4..8].try_into().ok()?);
    let length = u32::from_le_bytes(bytes[8..12].try_into().ok()?) as usize;
    header(route, length)?;
    Some((route, length))
}

/// Host serializes access. Every route has a fixed memory and count budget.
#[derive(Default)]
pub struct Queue {
    routes: [VecDeque<Vec<u8>>; 2],
    bytes: [usize; 2],
    next: usize,
}
impl Queue {
    pub fn push(&mut self, route: usize, bytes: &[u8]) -> bool {
        if route > 1
            || bytes.len() > MAX_RECORD
            || self.routes[route].len() >= CAPACITY
            || self.bytes[route] + bytes.len() > BYTE_BUDGET
        {
            return false;
        }
        self.routes[route].push_back(bytes.to_vec());
        self.bytes[route] += bytes.len();
        true
    }
    pub fn pop(&mut self) -> Option<(usize, Vec<u8>)> {
        for route in [self.next, 1 - self.next] {
            if let Some(bytes) = self.routes[route].pop_front() {
                self.bytes[route] -= bytes.len();
                self.next = 1 - route;
                return Some((route, bytes));
            }
        }
        None
    }
    pub fn pop_route(&mut self, route: usize) -> Option<Vec<u8>> {
        let bytes = self.routes.get_mut(route)?.pop_front()?;
        self.bytes[route] -= bytes.len();
        Some(bytes)
    }
}

/// Once retired, a late reply cannot bind or revive the window.
#[repr(C)]
#[derive(Default)]
pub struct ShellPipeBinding {
    pub phase: u32,
    pub serial: u32,
    pub shell: u64,
}
impl ShellPipeBinding {
    pub fn apply(&mut self, operation: u32, serial: u32, shell: u64) -> bool {
        if serial == 0 {
            return false;
        }
        match operation {
            0 if self.phase == 0 => {
                self.phase = 1;
                self.serial = serial;
                true
            }
            1 if self.phase == 1 && self.serial == serial && shell != 0 => {
                self.phase = 2;
                self.shell = shell;
                true
            }
            2 if self.phase != 0 && self.phase != 3 && self.serial == serial => {
                self.phase = 3;
                self.shell = 0;
                true
            }
            _ => false,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn product_burst_fits_the_adapter_under_credit_backpressure() {
        use crate::frame::Channel;
        use crate::link::{Endpoint, Limits, SendOutcome};
        use crate::pipe_ffi::{SHELL_PIPE_FLIGHT_BYTES, SHELL_PIPE_FLIGHT_FRAMES};
        assert!((SHELL_PIPE_FLIGHT_FRAMES as usize) * 2 < CAPACITY);
        let limits = Limits {
            in_flight_frames: SHELL_PIPE_FLIGHT_FRAMES as usize,
            in_flight_bytes: SHELL_PIPE_FLIGHT_BYTES as usize,
            ..Limits::default()
        };
        let (mut producer, mut consumer) = (Endpoint::new(9, limits), Endpoint::new(9, limits));
        for _ in 0..512 {
            assert_eq!(
                producer.send(1, Channel::Focus, 0, 0, &[0]),
                SendOutcome::Queued
            );
        }
        let mut queue = Queue::default();
        let mut delivered = 0;
        while delivered < 512 {
            let mut flight = 0;
            while let Some(bytes) = producer.take() {
                assert!(queue.push(0, &bytes));
                flight += 1;
            }
            assert!(flight <= SHELL_PIPE_FLIGHT_FRAMES);
            while let Some((_, bytes)) = queue.pop() {
                consumer.receive(&bytes).unwrap();
                delivered += 1;
            }
            while let Some(credit) = consumer.take() {
                producer.receive(&credit).unwrap();
            }
        }
        assert_eq!(producer.counts().overflowed, 0);
    }

    #[test]
    fn envelope_rejects_untrusted_lengths_and_routes() {
        for route in 0..2 {
            assert_eq!(decode(&header(route, 40).unwrap()), Some((route, 40)));
        }
        assert!(header(2, 40).is_none());
        assert!(header(0, 39).is_none());
        assert!(header(0, MAX_RECORD + 1).is_none());
        let mut h = header(0, 40).unwrap();
        h[0] = 0;
        assert!(decode(&h).is_none());
        assert!(decode(&h[..11]).is_none());
    }
    #[test]
    fn saturation_is_bounded_fair_and_independent() {
        let mut q = Queue::default();
        for _ in 0..CAPACITY {
            assert!(q.push(0, &[0; 40]));
        }
        assert!(!q.push(0, &[0; 40]));
        assert!(q.push(1, &[1; 40]));
        assert_eq!(q.pop().unwrap().0, 0);
        assert_eq!(q.pop().unwrap().0, 1);
        assert!(!q.push(2, &[0; 40]));
        assert!(!q.push(1, &vec![0; MAX_RECORD + 1]));
    }
    #[test]
    fn retirement_rejects_late_bind_and_reused_identity() {
        let mut b = ShellPipeBinding::default();
        assert!(b.apply(0, 1, 0));
        assert!(!b.apply(1, 2, 123));
        assert!(b.apply(2, 1, 0));
        assert!(!b.apply(1, 1, 123));
        assert!(!b.apply(0, 1, 0));
        assert_eq!(b.shell, 0);
    }
    #[test]
    fn byte_budget_is_enforced_before_count_budget() {
        let mut q = Queue::default();
        let bytes = vec![0; MAX_RECORD];
        assert!(q.push(0, &bytes));
        assert!(!q.push(0, &bytes));
        assert!(q.push(1, &bytes));
        assert_eq!(q.pop().unwrap().0, 0);
        assert!(q.push(0, &bytes));
        assert_eq!(q.pop().unwrap().0, 1);
    }
    #[test]
    fn stalled_main_consumer_does_not_spend_input_route_credit() {
        use crate::{
            frame::{Channel, Lifetime},
            link::{Endpoint, Limits, Received, SendOutcome},
        };
        let limits = Limits {
            in_flight_frames: 2,
            ..Limits::default()
        };
        let (mut main, mut input, mut input_receiver) = (
            Endpoint::new(7, limits),
            Endpoint::new(7, limits),
            Endpoint::new(7, limits),
        );
        let mut transport = Queue::default();
        for _ in 0..2 {
            assert_eq!(
                main.send(1, Channel::Lifetime, Lifetime::WakeupOn as u16, 0, &[]),
                SendOutcome::Queued
            );
            assert!(transport.push(0, &main.take().unwrap()));
        }
        assert_eq!(
            main.send(1, Channel::Lifetime, Lifetime::DestroyHost as u16, 0, &[]),
            SendOutcome::Queued
        );
        assert!(main.take().is_none());
        assert_eq!(
            input.send(
                1,
                Channel::Lifetime,
                Lifetime::Introduce as u16,
                0,
                &99u64.to_le_bytes()
            ),
            SendOutcome::Queued
        );
        assert!(transport.push(1, &input.take().unwrap()));
        assert_eq!(transport.pop().unwrap().0, 0); // main deliberately does not consume or return credit
        let (route, bytes) = transport.pop().unwrap();
        assert_eq!(route, 1);
        assert!(matches!(
            input_receiver.receive(&bytes),
            Ok(Received::Frame(_))
        ));
        assert_eq!(
            input.receive(&input_receiver.take().unwrap()),
            Ok(Received::Control)
        );
        assert_eq!(input.counts().in_flight, 0);
        assert_eq!(main.counts().in_flight, 2);
    }
}
