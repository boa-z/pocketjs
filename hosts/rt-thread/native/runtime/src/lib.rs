//! RT-Thread runtime shim: the allocator, panic handler and log channel that
//! `pocketjs-core` needs from its host.
//!
//! The ESP-IDF host's equivalent (`hosts/esp-idf/native/runtime`) resolves these
//! to `pocketjs_idf_rust_alloc` / `_dealloc` / `_panic`, which `ui_core.c`
//! implements over `heap_caps_aligned_alloc`. Here they resolve to the port's
//! existing host-services seam in `include/pocketjs_d13x.h`:
//!
//!   * `pjs_host_alloc(size, align)` / `pjs_host_free(ptr, align)` - the same
//!     pair Gate 0 and Gate 1A already validated on hardware. The backend is
//!     `aic_memheap_malloc(MEM_PSRAM_SW, ...)`, so the UI core's allocations
//!     land in PSRAM_SW and never in CMA.
//!   * `pjs_host_abort()` - the abort path captured for Gate 0 item 0c.
//!   * `pjs_host_log(msg, len)` - the Rust -> C log direction.
//!
//! `pjs_host_log` has no counterpart in the ESP-IDF runtime, and it is the
//! point of this crate existing separately rather than sharing one. Gate 0's
//! report section 8.4 records that `pjs_host_log` was declared and defined but
//! never called from Rust, so `--gc-sections` dropped it and the Rust -> C log
//! direction was never exercised. The UI core calls `host_log` on instance
//! create and destroy, which exercises it on every run.
//!
//! Alignment is passed through unchanged, `layout.align()` straight to the host.
//! Do not add a "host-guaranteed alignment" fast path: a hardcoded alignment
//! assumption is exactly what the Gate 0 hardware failure was.

#![cfg_attr(not(feature = "std"), no_std)]

#[cfg(not(feature = "std"))]
use core::alloc::{GlobalAlloc, Layout};

#[cfg(not(feature = "std"))]
unsafe extern "C" {
    fn pjs_host_alloc(size: u32, align: u32) -> *mut u8;
    fn pjs_host_free(ptr: *mut u8, align: u32);
    fn pjs_host_abort();
    fn pjs_host_log(msg: *const u8, len: u32);
}

/// Hand a log line to the host console.
///
/// `msg` is not required to be NUL-terminated; the length is explicit, which is
/// why the host side can be a plain `rt_kprintf` over a slice.
pub fn host_log(msg: &[u8]) {
    #[cfg(not(feature = "std"))]
    unsafe {
        pjs_host_log(msg.as_ptr(), msg.len() as u32);
    }
    #[cfg(feature = "std")]
    {
        // Host-side unit tests have no RT-Thread console; the call is a no-op so
        // the same test code links under `cargo test`.
        let _ = msg;
    }
}

#[cfg(not(feature = "std"))]
struct RtThreadAllocator;

#[cfg(not(feature = "std"))]
unsafe impl GlobalAlloc for RtThreadAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // ILP32: usize is 32 bits, so the narrowing is lossless. The cast is
        // explicit because the host seam is `uint32_t`-typed.
        pjs_host_alloc(layout.size() as u32, layout.align() as u32)
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        pjs_host_free(pointer, layout.align() as u32);
    }
}

#[cfg(not(feature = "std"))]
#[global_allocator]
static ALLOCATOR: RtThreadAllocator = RtThreadAllocator;

#[cfg(not(feature = "std"))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { pjs_host_abort() };
    // `pjs_host_abort` halts with interrupts disabled, so this is unreachable.
    // It exists so the signature is `!` without asserting a lie about the C side.
    loop {
        core::hint::spin_loop();
    }
}
