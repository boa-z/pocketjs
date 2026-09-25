//! PocketJS ArtInChip D13x ABI conformance probe (Gate 0).
//!
//! This crate exists to answer exactly one question: can a Rust
//! `RV32IMAFDC` / `ILP32D` staticlib be linked by the ArtInChip Xuantie GCC
//! toolchain into a Luban-Lite / RT-Thread firmware and exchange values with
//! C correctly on real D133ECS silicon?
//!
//! It is deliberately independent of `engine/core`. If the ABI is broken, the
//! failure must be attributable to the toolchain bridge, not to PocketJS UI
//! code.
//!
//! The C side of this contract lives in `include/pocketjs_d13x.h`.

#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::{c_char, c_void};
use core::panic::PanicInfo;
use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};

// ---------------------------------------------------------------------------
// Shared value types. Mirrored by `pjs_abi_*` in include/pocketjs_d13x.h.
// ---------------------------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq)]
pub struct AbiValue {
    pub id: u32,
    pub x: f32,
    pub value: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq)]
pub struct AbiNested {
    pub tag: u8,
    pub count: u32,
    pub inner: AbiValue,
    pub tail: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct AbiLayout {
    pub usize_size: u32,
    pub usize_align: u32,
    pub value_size: u32,
    pub value_align: u32,
    pub value_off_id: u32,
    pub value_off_x: u32,
    pub value_off_value: u32,
    pub nested_size: u32,
    pub nested_align: u32,
    pub nested_off_tag: u32,
    pub nested_off_count: u32,
    pub nested_off_inner: u32,
    pub nested_off_tail: u32,
    pub f64_size: u32,
    pub f64_align: u32,
    pub ptr_size: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct AbiAllocReport {
    pub box_ok: u32,
    pub vec_len: u32,
    pub vec_sum: u32,
    pub string_len: u32,
    pub string_hash: u32,
    pub align8_ok: u32,
    pub align_ok: u32,
    pub live_bytes: u32,
    pub peak_bytes: u32,
}

/// Gate 1A. Mirrored by `pjs_abi_stress_report_t` in include/pocketjs_d13x.h.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct AbiStressReport {
    pub rounds: u32,
    pub box_ok: u32,
    pub vec_ok: u32,
    pub string_ok: u32,
    pub checksum: u32,
    pub live_bytes: u32,
    pub peak_bytes: u32,
    pub alloc_count: u32,
    pub free_count: u32,
    pub fail_count: u32,
}

// Compile-time proof of the layout this port assumes. If the custom target
// JSON ever drifts (for example losing `+d`, which would drop f64 alignment to
// 4), the build fails here instead of corrupting memory on the board.
const _: () = {
    assert!(core::mem::size_of::<usize>() == 4);
    assert!(core::mem::align_of::<usize>() == 4);
    assert!(core::mem::size_of::<*const u8>() == 4);

    // f64 must be 64-bit and 8-byte aligned under ILP32D. This is the single
    // most important assertion in the crate.
    assert!(core::mem::size_of::<f64>() == 8);
    assert!(core::mem::align_of::<f64>() == 8);

    assert!(core::mem::size_of::<AbiValue>() == 16);
    assert!(core::mem::align_of::<AbiValue>() == 8);
    assert!(core::mem::offset_of!(AbiValue, id) == 0);
    assert!(core::mem::offset_of!(AbiValue, x) == 4);
    assert!(core::mem::offset_of!(AbiValue, value) == 8);

    assert!(core::mem::size_of::<AbiNested>() == 32);
    assert!(core::mem::align_of::<AbiNested>() == 8);
    assert!(core::mem::offset_of!(AbiNested, tag) == 0);
    assert!(core::mem::offset_of!(AbiNested, count) == 4);
    assert!(core::mem::offset_of!(AbiNested, inner) == 8);
    assert!(core::mem::offset_of!(AbiNested, tail) == 24);
};

// ---------------------------------------------------------------------------
// Host services, implemented in C.
// ---------------------------------------------------------------------------

unsafe extern "C" {
    fn pjs_host_u32(v: u32) -> u32;
    fn pjs_host_f32(v: f32) -> f32;
    fn pjs_host_f64(v: f64) -> f64;
    fn pjs_host_value(v: AbiValue) -> AbiValue;
    fn pjs_host_nested(v: AbiNested) -> AbiNested;
    fn pjs_host_mixed(a: u32, b: f32, c: f64, p: *const u32, n: u32) -> f64;
    fn pjs_host_alloc(size: u32, align: u32) -> *mut u8;
    fn pjs_host_free(p: *mut u8, align: u32);
    fn pjs_host_abort() -> !;
}

// ---------------------------------------------------------------------------
// Allocator
// ---------------------------------------------------------------------------

/// Alignment is *not* assumed here. The layout's own requirement is handed to
/// the host, which is responsible for honouring it - including over-allocating
/// when the heap cannot. An earlier revision hardcoded "the host gives 8 bytes"
/// and aborted the probe on the first `Box<u32>` on real silicon, because this
/// board's RT-Thread heap only guarantees RT_ALIGN_SIZE == 4.
static LIVE_BYTES: AtomicU32 = AtomicU32::new(0);
static PEAK_BYTES: AtomicU32 = AtomicU32::new(0);
static ALLOC_COUNT: AtomicU32 = AtomicU32::new(0);
static FREE_COUNT: AtomicU32 = AtomicU32::new(0);
static FAIL_COUNT: AtomicU32 = AtomicU32::new(0);

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

struct HostAllocator;

unsafe impl GlobalAlloc for HostAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let size = request_size(&layout);

        // Cannot overflow for realistic layouts, but keep the guard explicit:
        // a bogus Layout must fail, not wrap.
        if size > (u32::MAX as usize) {
            FAIL_COUNT.fetch_add(1, Ordering::Relaxed);
            return ptr::null_mut();
        }

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

    unsafe fn dealloc(&self, p: *mut u8, layout: Layout) {
        if p.is_null() {
            return;
        }
        // GlobalAlloc guarantees `dealloc` sees the allocating layout, so the
        // same alignment goes back and the host knows whether it used a header.
        unsafe { pjs_host_free(p, layout.align() as u32) };

        FREE_COUNT.fetch_add(1, Ordering::Relaxed);
        LIVE_BYTES.fetch_sub(request_size(&layout) as u32, Ordering::Relaxed);
    }
}

#[global_allocator]
static ALLOCATOR: HostAllocator = HostAllocator;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // panic = abort: there is no unwinder on this target, so a panic must
    // stop the firmware rather than try to recover.
    unsafe { pjs_host_abort() }
}

// ---------------------------------------------------------------------------
// Direction 1: C -> Rust
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn pjs_probe_u32(v: u32) -> u32 {
    v.wrapping_mul(2_654_435_761) ^ 0x9E37_79B9
}

#[no_mangle]
pub extern "C" fn pjs_probe_i32(v: i32) -> i32 {
    v.wrapping_mul(-3).wrapping_add(7)
}

#[no_mangle]
pub extern "C" fn pjs_probe_usize(v: u32) -> u32 {
    (v as usize).wrapping_add(0x1234_5678) as u32
}

#[no_mangle]
pub extern "C" fn pjs_probe_f32(v: f32) -> f32 {
    v * 2.0 + 0.5
}

#[no_mangle]
pub extern "C" fn pjs_probe_f64(v: f64) -> f64 {
    v * 3.0 + 0.25
}

#[no_mangle]
pub extern "C" fn pjs_probe_ptr_sum(p: *const u32, n: u32) -> u32 {
    if p.is_null() {
        return 0;
    }
    let mut acc = 0u32;
    for i in 0..n {
        // Reading through a caller-owned buffer is the whole point: it proves
        // a 32-bit pointer survives the C -> Rust crossing intact.
        acc = acc.wrapping_add(unsafe { p.add(i as usize).read() });
    }
    acc
}

#[no_mangle]
pub extern "C" fn pjs_probe_value(v: AbiValue) -> AbiValue {
    AbiValue {
        id: v.id ^ 0x0000_FFFF,
        x: v.x * 2.0,
        value: v.value + 1.5,
    }
}

#[no_mangle]
pub extern "C" fn pjs_probe_nested(v: AbiNested) -> AbiNested {
    AbiNested {
        tag: v.tag.wrapping_add(1),
        count: v.count.wrapping_mul(2),
        inner: AbiValue {
            id: v.inner.id.wrapping_add(1000),
            x: v.inner.x + 0.25,
            value: v.inner.value * 2.0,
        },
        tail: v.tail - 0.5,
    }
}

#[no_mangle]
pub extern "C" fn pjs_probe_mixed(a: u32, b: f32, c: f64, p: *const u32, n: u32) -> f64 {
    let sum = pjs_probe_ptr_sum(p, n);
    (a as f64) + (b as f64) * 2.0 + c + (sum as f64)
}

#[no_mangle]
pub extern "C" fn pjs_probe_layout(out: *mut AbiLayout) {
    if out.is_null() {
        return;
    }
    let l = AbiLayout {
        usize_size: core::mem::size_of::<usize>() as u32,
        usize_align: core::mem::align_of::<usize>() as u32,
        value_size: core::mem::size_of::<AbiValue>() as u32,
        value_align: core::mem::align_of::<AbiValue>() as u32,
        value_off_id: core::mem::offset_of!(AbiValue, id) as u32,
        value_off_x: core::mem::offset_of!(AbiValue, x) as u32,
        value_off_value: core::mem::offset_of!(AbiValue, value) as u32,
        nested_size: core::mem::size_of::<AbiNested>() as u32,
        nested_align: core::mem::align_of::<AbiNested>() as u32,
        nested_off_tag: core::mem::offset_of!(AbiNested, tag) as u32,
        nested_off_count: core::mem::offset_of!(AbiNested, count) as u32,
        nested_off_inner: core::mem::offset_of!(AbiNested, inner) as u32,
        nested_off_tail: core::mem::offset_of!(AbiNested, tail) as u32,
        f64_size: core::mem::size_of::<f64>() as u32,
        f64_align: core::mem::align_of::<f64>() as u32,
        ptr_size: core::mem::size_of::<*const u8>() as u32,
    };
    unsafe { out.write(l) };
}

/// FNV-1a, matching the C harness so both sides can agree on a string hash
/// without depending on libc.
fn fnv1a(bytes: &[u8]) -> u32 {
    let mut h = 2_166_136_261u32;
    for &b in bytes {
        h = (h ^ b as u32).wrapping_mul(16_777_619);
    }
    h
}

#[no_mangle]
pub extern "C" fn pjs_probe_alloc(out: *mut AbiAllocReport) {
    if out.is_null() {
        return;
    }
    let mut r = AbiAllocReport::default();

    // Box: one small heap allocation through the Rust global allocator.
    let b = Box::new(0xDEAD_BEEFu32);
    r.box_ok = u32::from(*b == 0xDEAD_BEEF);
    drop(b);

    // Vec: a growable allocation, exercising capacity + element writes.
    let n: u32 = 64;
    let mut v: Vec<u32> = Vec::with_capacity(n as usize);
    for i in 0..n {
        v.push(i.wrapping_mul(2_654_435_761));
    }
    r.vec_len = v.len() as u32;
    r.vec_sum = v.iter().fold(0u32, |a, &x| a.wrapping_add(x));
    drop(v);

    // String: UTF-8 text, the type the JS guest will hand across this boundary.
    let mut s = String::new();
    s.push_str("pocketjs-d13x-abi-probe");
    r.string_len = s.len() as u32;
    r.string_hash = fnv1a(s.as_bytes());
    drop(s);

    // 8-byte alignment: what every f64-bearing type in this ABI needs, and the
    // exact case the first hardware run failed on - the host heap only aligns
    // to RT_ALIGN_SIZE == 4. `Box<f64>` requests align 8, so this asserts the
    // host honoured a request it cannot satisfy from its own heap alignment.
    let bf = Box::new(1.5f64);
    r.align8_ok = u32::from((bf.as_ref() as *const f64 as usize) % 8 == 0);
    drop(bf);

    // Over-aligned allocation: f64-bearing aggregates need 8 bytes, and the UI
    // core will want more. Prove the wrapper honours a request the host
    // allocator cannot satisfy on its own.
    if let Ok(lay) = Layout::from_size_align(64, 64) {
        let p = unsafe { alloc::alloc::alloc(lay) };
        if !p.is_null() {
            r.align_ok = u32::from((p as usize) % 64 == 0);
            unsafe { alloc::alloc::dealloc(p, lay) };
        }
    }

    r.live_bytes = LIVE_BYTES.load(Ordering::Relaxed);
    r.peak_bytes = PEAK_BYTES.load(Ordering::Relaxed);
    unsafe { out.write(r) };
}

/// Gate 1A: `iters` rounds of Box + Vec + String through the global allocator.
///
/// Sizes vary per round, so this is a real alloc/free cycle rather than the same
/// block handed back repeatedly: a heap that never coalesces, or a header
/// misplaced by the over-alignment path, shows up as growing `live_bytes` or a
/// non-zero `fail_count` instead of passing.
///
/// Each round's data is folded into `checksum` so that a single corrupted round
/// cannot hide behind a good one. The C side recomputes the same fold from
/// scratch - the same technique the Gate 0 probe uses for its Vec sum and String
/// hash, and for the same reason: agreement between two independent computations
/// is evidence, agreement with itself is not.
///
/// `String` growth is included on purpose. It goes through `realloc`, which the
/// default `GlobalAlloc` implementation serves as alloc + copy + dealloc, so it
/// exercises both paths and the over-alignment header in one operation.
#[no_mangle]
pub extern "C" fn pjs_probe_alloc_stress(iters: u32, out: *mut AbiStressReport) {
    if out.is_null() {
        return;
    }
    let mut r = AbiStressReport {
        box_ok: 1,
        vec_ok: 1,
        string_ok: 1,
        ..AbiStressReport::default()
    };

    let mut i: u32 = 0;
    while i < iters {
        // Box: a value that depends on the round, so a stale reuse is caught.
        let want_box = i.wrapping_mul(2_654_435_761) ^ 0x9E37_79B9;
        let b = Box::new(want_box);
        if *b != want_box {
            r.box_ok = 0;
        }
        drop(b);

        // Vec: length varies 1..=64, so the heap sees many distinct sizes.
        let n = (i % 64) + 1;
        let mut v: Vec<u32> = Vec::with_capacity(n as usize);
        let mut vsum: u32 = 0;
        let mut k: u32 = 0;
        while k < n {
            let x = k.wrapping_mul(i).wrapping_add(0x1234_5678);
            v.push(x);
            vsum = vsum.wrapping_add(x);
            k += 1;
        }
        if v.len() as u32 != n {
            r.vec_ok = 0;
        }
        if v.iter().fold(0u32, |a, &x| a.wrapping_add(x)) != vsum {
            r.vec_ok = 0;
        }
        drop(v);

        // String: length varies 1..=40.
        let slen = (i % 40) + 1;
        let mut s = String::new();
        let mut k: u32 = 0;
        while k < slen {
            s.push((b'a' + (k % 26) as u8) as char);
            k += 1;
        }
        if s.len() as u32 != slen {
            r.string_ok = 0;
        }
        let shash = fnv1a(s.as_bytes());
        drop(s);

        r.checksum = r
            .checksum
            .wrapping_mul(31)
            .wrapping_add(want_box ^ vsum ^ shash);

        r.rounds += 1;
        i += 1;
    }

    r.live_bytes = LIVE_BYTES.load(Ordering::Relaxed);
    r.peak_bytes = PEAK_BYTES.load(Ordering::Relaxed);
    r.alloc_count = ALLOC_COUNT.load(Ordering::Relaxed);
    r.free_count = FREE_COUNT.load(Ordering::Relaxed);
    r.fail_count = FAIL_COUNT.load(Ordering::Relaxed);
    unsafe { out.write(r) };
}

#[no_mangle]
pub extern "C" fn pjs_probe_mem_stats(
    live: *mut u32,
    peak: *mut u32,
    allocs: *mut u32,
    frees: *mut u32,
    fails: *mut u32,
) {
    unsafe {
        if !live.is_null() {
            live.write(LIVE_BYTES.load(Ordering::Relaxed));
        }
        if !peak.is_null() {
            peak.write(PEAK_BYTES.load(Ordering::Relaxed));
        }
        if !allocs.is_null() {
            allocs.write(ALLOC_COUNT.load(Ordering::Relaxed));
        }
        if !frees.is_null() {
            frees.write(FREE_COUNT.load(Ordering::Relaxed));
        }
        if !fails.is_null() {
            fails.write(FAIL_COUNT.load(Ordering::Relaxed));
        }
    }
}

/// Deliberate panic. Must abort the firmware; the C harness runs this last.
#[no_mangle]
pub extern "C" fn pjs_probe_panic() {
    panic!("pocketjs d13x abi probe: intentional panic (panic=abort expected)");
}

// ---------------------------------------------------------------------------
// Direction 2: Rust -> C
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn pjs_probe_call_host_u32(v: u32) -> u32 {
    unsafe { pjs_host_u32(v) }
}

#[no_mangle]
pub extern "C" fn pjs_probe_call_host_f32(v: f32) -> f32 {
    unsafe { pjs_host_f32(v) }
}

#[no_mangle]
pub extern "C" fn pjs_probe_call_host_f64(v: f64) -> f64 {
    unsafe { pjs_host_f64(v) }
}

#[no_mangle]
pub extern "C" fn pjs_probe_call_host_value(v: AbiValue) -> AbiValue {
    unsafe { pjs_host_value(v) }
}

#[no_mangle]
pub extern "C" fn pjs_probe_call_host_nested(v: AbiNested) -> AbiNested {
    unsafe { pjs_host_nested(v) }
}

#[no_mangle]
pub extern "C" fn pjs_probe_call_host_mixed(a: u32, b: f32, c: f64, p: *const u32, n: u32) -> f64 {
    unsafe { pjs_host_mixed(a, b, c, p, n) }
}

/// Touch `c_void` so the type stays exercised in the ABI surface.
#[no_mangle]
pub extern "C" fn pjs_probe_roundtrip_ptr(p: *mut c_void) -> *mut c_void {
    p
}

/// Same, for the byte-logging signature used by the real port.
#[no_mangle]
pub extern "C" fn pjs_probe_log_len(msg: *const c_char, len: u32) -> u32 {
    if msg.is_null() {
        return 0;
    }
    len
}
