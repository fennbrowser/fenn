use crate::pipe::{self, Queue, ShellPipeBinding};
use std::panic::{catch_unwind, AssertUnwindSafe};

/// Keep credited frames plus return credits below each OS adapter route's slots.
pub const SHELL_PIPE_FLIGHT_FRAMES: u32 = 4;
pub const SHELL_PIPE_FLIGHT_BYTES: u32 = 4 * 1024 * 1024;

fn guard(body: impl FnOnce() -> bool) -> bool {
    catch_unwind(AssertUnwindSafe(body)).unwrap_or(false)
}

/// Caller serializes access; this byte queue has no thread-affine UI objects.
pub struct ShellPipeQueue { queue: Queue }
#[no_mangle]
pub extern "C" fn shell_pipe_queue_create() -> *mut ShellPipeQueue {
    catch_unwind(|| Box::into_raw(Box::new(ShellPipeQueue { queue: Queue::default() })))
        .unwrap_or(std::ptr::null_mut())
}
#[no_mangle]
pub unsafe extern "C" fn shell_pipe_queue_destroy(queue: *mut ShellPipeQueue) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !queue.is_null() { drop(Box::from_raw(queue)); }
    }));
}
#[no_mangle]
pub unsafe extern "C" fn shell_pipe_queue_push(queue: *mut ShellPipeQueue, route: u32,
    data: *const u8, length: usize) -> bool {
    guard(|| {
        let Some(q) = queue.as_mut() else { return false; };
        if data.is_null() || length > pipe::MAX_RECORD { return false; }
        q.queue.push(route as usize, std::slice::from_raw_parts(data, length))
    })
}
#[no_mangle]
pub unsafe extern "C" fn shell_pipe_queue_pop(queue: *mut ShellPipeQueue, route: *mut u32,
    data: *mut *mut u8, length: *mut usize) -> bool {
    guard(|| {
        if queue.is_null() || route.is_null() || data.is_null() || length.is_null() { return false; }
        *data = std::ptr::null_mut(); *length = 0;
        let Some((r, bytes)) = (*queue).queue.pop() else { return false; };
        let mut bytes = bytes.into_boxed_slice();
        *route = r as u32; *length = bytes.len(); *data = bytes.as_mut_ptr();
        std::mem::forget(bytes); true
    })
}
#[no_mangle]
pub unsafe extern "C" fn shell_pipe_header(route: u32, length: usize, out: *mut u8) -> bool {
    guard(|| {
        if out.is_null() { return false; }
        let Some(header) = pipe::header(route, length) else { return false; };
        std::ptr::copy_nonoverlapping(header.as_ptr(), out, pipe::HEADER); true
    })
}
/// Take only the calling owner's route. Other routes remain queued for their own threads.
#[no_mangle]
pub unsafe extern "C" fn shell_pipe_queue_pop_route(queue: *mut ShellPipeQueue, route: u32,
    data: *mut *mut u8, length: *mut usize) -> bool {
    guard(|| {
        if queue.is_null() || data.is_null() || length.is_null() { return false; }
        *data = std::ptr::null_mut(); *length = 0;
        let Some(bytes) = (*queue).queue.pop_route(route as usize) else { return false; };
        let mut bytes = bytes.into_boxed_slice();
        *length = bytes.len(); *data = bytes.as_mut_ptr();
        std::mem::forget(bytes); true
    })
}
#[no_mangle]
pub unsafe extern "C" fn shell_pipe_decode_header(data: *const u8, route: *mut u32, length: *mut usize) -> bool {
    guard(|| {
        if data.is_null() || route.is_null() || length.is_null() { return false; }
        let Some((r, n)) = pipe::decode(std::slice::from_raw_parts(data, pipe::HEADER)) else { return false; };
        *route = r; *length = n; true
    })
}
#[no_mangle]
pub unsafe extern "C" fn shell_pipe_binding(binding: *mut ShellPipeBinding, operation: u32,
    serial: u32, shell: u64) -> bool {
    guard(|| binding.as_mut().is_some_and(|b| b.apply(operation, serial, shell)))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn null_contracts_and_owned_buffer_round_trip() { unsafe {
        assert!(!shell_pipe_binding(std::ptr::null_mut(), 0, 1, 0));
        let queue = shell_pipe_queue_create();
        let bytes = [5; 40];
        assert!(!shell_pipe_queue_push(queue, 0, std::ptr::null(), 40));
        assert!(shell_pipe_queue_push(queue, 1, bytes.as_ptr(), bytes.len()));
        let (mut route, mut length, mut data) = (0, 0, std::ptr::null_mut());
        assert!(shell_pipe_queue_pop(queue, &mut route, &mut data, &mut length));
        assert_eq!(route, 1);
        assert_eq!(std::slice::from_raw_parts(data, length), bytes);
        crate::link_ffi::shell_link_free_frame(data, length);
        assert!(!shell_pipe_queue_pop(queue, &mut route, &mut data, &mut length));
        assert!(data.is_null());
        assert_eq!(length, 0);
        shell_pipe_queue_destroy(queue);
    }}
}
