//! Windows connection-cost check used before background filter updates.
use serde_json::{json, Value};
use std::{ffi::c_void, ptr};
#[repr(C)]
struct Guid {
    a: u32,
    b: u16,
    c: u16,
    d: [u8; 8],
}
#[link(name = "ole32")]
unsafe extern "system" {
    fn CoInitializeEx(reserved: *mut c_void, flags: u32) -> i32;
    fn CoUninitialize();
    fn CoCreateInstance(
        clsid: *const Guid,
        outer: *mut c_void,
        context: u32,
        iid: *const Guid,
        result: *mut *mut c_void,
    ) -> i32;
}
#[repr(C)]
struct CostVtable {
    query: usize,
    add: usize,
    release: unsafe extern "system" fn(*mut c_void) -> u32,
    cost: unsafe extern "system" fn(*mut c_void, *mut u32, *const c_void) -> i32,
}
pub fn network_cost() -> Value {
    // SDK netlistmgr.h: INetworkCostManager/GetCost, preferred machine route.
    unsafe {
        let init = CoInitializeEx(ptr::null_mut(), 0);
        if init < 0 {
            return json!({"allowed":false,"error":"COM initialization"});
        }
        let cls = Guid {
            a: 0xdcb00c01,
            b: 0x570f,
            c: 0x4a9b,
            d: [0x8d, 0x69, 0x19, 0x9f, 0xdb, 0xa5, 0x72, 0x3b],
        };
        let iid = Guid {
            a: 0xdcb00008,
            ..cls
        };
        let mut object = ptr::null_mut();
        let hr = CoCreateInstance(&cls, ptr::null_mut(), 23, &iid, &mut object);
        let mut flags = 0;
        let ok = if hr >= 0 && !object.is_null() {
            let vtable = &**(object as *const *const CostVtable);
            let result = (vtable.cost)(object, &mut flags, ptr::null());
            (vtable.release)(object);
            result >= 0
        } else {
            false
        };
        CoUninitialize();
        json!({"allowed":ok && allowed(flags),"flags":flags})
    }
}
/// NLM_CONNECTION_COST flags. Following Windows' metered-connection guidance,
/// an unknown cost is treated as unrestricted: deferring it would let filter
/// lists expire on networks Windows cannot classify. Fixed and variable
/// (metered) plans, roaming, congestion and approaching or exceeding a data
/// limit defer. A failed query still defers.
fn allowed(flags: u32) -> bool {
    flags == 0 || flags == 1
}
#[cfg(test)]
mod tests {
    #[test]
    fn unknown_and_unrestricted_allow_metered_and_limits_defer() {
        assert!(super::allowed(0) && super::allowed(1));
        for flags in [
            2,
            4,
            1 | 0x10000,
            1 | 0x20000,
            1 | 0x40000,
            1 | 0x80000,
            0x20000,
        ] {
            assert!(!super::allowed(flags), "{flags:#x}");
        }
    }
}
