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
//!
//! # This crate is the only owner of the Rust runtime lang items
//!
//! `#[global_allocator]` and `#[panic_handler]` are declared here and nowhere
//! else in the image. That is a hard requirement, not a style choice.
//!
//! rustc lowers those two lang items into `#[rustc_std_internal_symbol]` shims
//! whose symbol names are *fixed* (`__rustc` plus a constant disambiguator)
//! precisely so that one copy can serve a whole link. Because the name is fixed,
//! `-Cmetadata` cannot rename it, and because a Rust `staticlib` always bundles
//! its dependency graph, two archives that each declare the lang items put the
//! same five symbols on the link line twice - `__rust_alloc`, `__rust_dealloc`,
//! `__rust_realloc`, `__rust_alloc_zeroed` and `rust_begin_unwind`. That is a
//! hard "multiple definition" error.
//!
//! The ESP-IDF host has the same shape and avoids it the same way: its two
//! archives (`ui-core`, `render-rgb565`) both bundle `pocketjs-idf-runtime`,
//! which is the only crate there that declares these lang items. Feature crates
//! consume the runtime; they never provide one.
//!
//! So: a new crate that needs an allocator depends on this one. It does not
//! declare its own.

#![cfg_attr(not(feature = "std"), no_std)]

use core::alloc::Layout;
use core::sync::atomic::{AtomicU32, Ordering};

#[cfg(not(feature = "std"))]
use core::alloc::GlobalAlloc;
#[cfg(not(feature = "std"))]
use core::ptr;

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

// ---------------------------------------------------------------------------
// Allocation telemetry
// ---------------------------------------------------------------------------

static LIVE_BYTES: AtomicU32 = AtomicU32::new(0);
static PEAK_BYTES: AtomicU32 = AtomicU32::new(0);
static ALLOC_COUNT: AtomicU32 = AtomicU32::new(0);
static FREE_COUNT: AtomicU32 = AtomicU32::new(0);
static FAIL_COUNT: AtomicU32 = AtomicU32::new(0);

/// Snapshot of the counters below.
///
/// Read by the port's Gate 0 probe, which reports them on the console. They are
/// **image-wide**, not per-crate: there is one allocator in the image, so these
/// count every Rust allocation in it. On a probe-only firmware that is exactly
/// the probe's own traffic, which is what Gate 0's report records.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct AllocStats {
    pub live_bytes: u32,
    pub peak_bytes: u32,
    pub alloc_count: u32,
    pub free_count: u32,
    pub fail_count: u32,
}

/// Read the allocator counters. Cheap, lock-free, safe to call from any thread.
pub fn alloc_stats() -> AllocStats {
    AllocStats {
        live_bytes: LIVE_BYTES.load(Ordering::Relaxed),
        peak_bytes: PEAK_BYTES.load(Ordering::Relaxed),
        alloc_count: ALLOC_COUNT.load(Ordering::Relaxed),
        free_count: FREE_COUNT.load(Ordering::Relaxed),
        fail_count: FAIL_COUNT.load(Ordering::Relaxed),
    }
}

/// Bytes requested from the host for `layout`. Deterministic, so `dealloc` can
/// recompute it without extra bookkeeping. This is the *caller's* size: the
/// host's own header and padding are its business, not the guest's.
fn request_size(layout: &Layout) -> usize {
    if layout.size() == 0 {
        1
    } else {
        layout.size()
    }
}

// ---------------------------------------------------------------------------
// The allocator
// ---------------------------------------------------------------------------

#[cfg(not(feature = "std"))]
struct RtThreadAllocator;

#[cfg(not(feature = "std"))]
unsafe impl GlobalAlloc for RtThreadAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let size = request_size(&layout);

        // Cannot overflow for realistic layouts, but keep the guard explicit:
        // a bogus Layout must fail, not wrap.
        if size > (u32::MAX as usize) {
            FAIL_COUNT.fetch_add(1, Ordering::Relaxed);
            return ptr::null_mut();
        }

        // ILP32: usize is 32 bits, so the narrowing is lossless. The cast is
        // explicit because the host seam is `uint32_t`-typed.
        let raw = unsafe { pjs_host_alloc(size as u32, layout.align() as u32) };
        if raw.is_null() {
            FAIL_COUNT.fetch_add(1, Ordering::Relaxed);
            return ptr::null_mut();
        }

        ALLOC_COUNT.fetch_add(1, Ordering::Relaxed);
        let live = LIVE_BYTES.fetch_add(size as u32, Ordering::Relaxed) + size as u32;
        // Single-threaded owner by contract (see the RT-Thread thread model),
        // so a plain compare-and-store keeps the peak accurate enough for
        // telemetry.
        let mut peak = PEAK_BYTES.load(Ordering::Relaxed);
        while live > peak {
            match PEAK_BYTES.compare_exchange_weak(
                peak,
                live,
                Ordering::Relaxed,
                Ordering::Relaxed,
            ) {
                Ok(_) => break,
                Err(observed) => peak = observed,
            }
        }

        raw
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        if pointer.is_null() {
            return;
        }
        // GlobalAlloc guarantees `dealloc` sees the allocating layout, so the
        // same alignment goes back and the host knows whether it used a header.
        unsafe { pjs_host_free(pointer, layout.align() as u32) };

        FREE_COUNT.fetch_add(1, Ordering::Relaxed);
        LIVE_BYTES.fetch_sub(request_size(&layout) as u32, Ordering::Relaxed);
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
