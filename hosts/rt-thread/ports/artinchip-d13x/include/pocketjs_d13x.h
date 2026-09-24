/*
 * PocketJS ArtInChip D13x port - Gate 0 ABI contract.
 *
 * This header is the single source of truth for the C <-> Rust boundary that
 * Gate 0 proves on real D133ECS silicon. Every type and signature here is
 * mirrored byte-for-byte by `rust/abi-probe/src/lib.rs`. If one side changes,
 * the other must change in the same commit.
 *
 * Target facts this contract depends on (RV32IMAFDC / ILP32D):
 *   pointer width : 32 bit
 *   f64           : 64 bit, 8-byte aligned, hardware double ABI
 *   struct return : psABI aggregate rules
 */
#ifndef POCKETJS_D13X_H
#define POCKETJS_D13X_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Shared value types - must match `#[repr(C)]` in the Rust crate.
 * ------------------------------------------------------------------ */

typedef struct pjs_abi_value {
    uint32_t id;
    float x;
    double value;
} pjs_abi_value_t;

typedef struct pjs_abi_nested {
    uint8_t tag;
    uint32_t count;
    pjs_abi_value_t inner;
    double tail;
} pjs_abi_nested_t;

/* Layout report. Rust fills this so C can compare against its own
 * `sizeof`/`offsetof` without trusting the compiler on either side. */
typedef struct pjs_abi_layout {
    uint32_t usize_size;
    uint32_t usize_align;
    uint32_t value_size;
    uint32_t value_align;
    uint32_t value_off_id;
    uint32_t value_off_x;
    uint32_t value_off_value;
    uint32_t nested_size;
    uint32_t nested_align;
    uint32_t nested_off_tag;
    uint32_t nested_off_count;
    uint32_t nested_off_inner;
    uint32_t nested_off_tail;
    uint32_t f64_size;
    uint32_t f64_align;
    uint32_t ptr_size;
} pjs_abi_layout_t;

/* Result of the allocator exercise. */
typedef struct pjs_abi_alloc_report {
    uint32_t box_ok;
    uint32_t vec_len;
    uint32_t vec_sum;
    uint32_t string_len;
    uint32_t string_hash;
    uint32_t align8_ok;     /* Box<f64>: an 8-byte request was honoured      */
    uint32_t align_ok;      /* 64-byte request: the over-alignment path      */
    uint32_t live_bytes;
    uint32_t peak_bytes;
} pjs_abi_alloc_report_t;

/* ------------------------------------------------------------------ *
 * Direction 1: C -> Rust. Implemented in Rust, called from C.
 * ------------------------------------------------------------------ */

/* Scalar round trips. The transform is deterministic so C can recompute
 * the expected value independently. */
uint32_t pjs_probe_u32(uint32_t v);
int32_t pjs_probe_i32(int32_t v);
uint32_t pjs_probe_usize(uint32_t v);
float pjs_probe_f32(float v);
double pjs_probe_f64(double v);

/* Pointer parameter: Rust reads `n` words through `p`. */
uint32_t pjs_probe_ptr_sum(const uint32_t *p, uint32_t n);

/* Struct by value, both directions of the call. */
pjs_abi_value_t pjs_probe_value(pjs_abi_value_t v);
pjs_abi_nested_t pjs_probe_nested(pjs_abi_nested_t v);

/* Mixed scalar + pointer signature, the worst case for register
 * allocation under ILP32D. */
double pjs_probe_mixed(uint32_t a, float b, double c, const uint32_t *p, uint32_t n);

/* Fill `out` with Rust's own view of the layout. */
void pjs_probe_layout(pjs_abi_layout_t *out);

/* Pointer round trip: the same address must come back unchanged. */
void *pjs_probe_roundtrip_ptr(void *p);

/* Byte-logging signature used by the real port; returns `len` unchanged. */
uint32_t pjs_probe_log_len(const char *msg, uint32_t len);

/* Exercise Box + Vec + String through the Rust GlobalAlloc. */
void pjs_probe_alloc(pjs_abi_alloc_report_t *out);

/* Read back the allocator telemetry counters. */
void pjs_probe_mem_stats(uint32_t *live_bytes, uint32_t *peak_bytes, uint32_t *alloc_count,
                         uint32_t *free_count, uint32_t *fail_count);

/* Must abort the firmware. Reached only when a panic is raised. */
void pjs_probe_panic(void);

/* ------------------------------------------------------------------ *
 * Direction 2: Rust -> C. Implemented in C, called from Rust.
 * ------------------------------------------------------------------ */

uint32_t pjs_host_u32(uint32_t v);
float pjs_host_f32(float v);
double pjs_host_f64(double v);
pjs_abi_value_t pjs_host_value(pjs_abi_value_t v);
pjs_abi_nested_t pjs_host_nested(pjs_abi_nested_t v);
double pjs_host_mixed(uint32_t a, float b, double c, const uint32_t *p, uint32_t n);

/* Rust entry points that call the host callbacks above, so the C harness
 * can drive the Rust -> C direction from a single place. */
uint32_t pjs_probe_call_host_u32(uint32_t v);
float pjs_probe_call_host_f32(float v);
double pjs_probe_call_host_f64(double v);
pjs_abi_value_t pjs_probe_call_host_value(pjs_abi_value_t v);
pjs_abi_nested_t pjs_probe_call_host_nested(pjs_abi_nested_t v);
double pjs_probe_call_host_mixed(uint32_t a, float b, double c, const uint32_t *p, uint32_t n);

/* ------------------------------------------------------------------ *
 * Host services the Rust crate requires. Implemented in C.
 * ------------------------------------------------------------------ */

/* Byte-oriented log sink. `msg` is not NUL terminated. */
void pjs_host_log(const char *msg, uint32_t len);

/* Raw allocator. Returns memory aligned to `align` (a power of two), or NULL.
 *
 * The caller states the alignment it needs instead of relying on a fixed heap
 * guarantee. That is deliberate: this board's RT-Thread heap only promises
 * RT_ALIGN_SIZE, which is 4 here, so an allocator that assumed 8 handed Rust a
 * 4-byte-aligned block for an 8-byte-aligned type. `align` is clamped up to
 * sizeof(void *) and anything above the heap's own alignment is served by
 * over-allocating.
 *
 * `pjs_host_free` must be given the same `align` that produced `ptr` - the
 * GlobalAlloc contract guarantees `dealloc` sees the allocating layout, so the
 * Rust side passes it straight back. */
void *pjs_host_alloc(uint32_t size, uint32_t align);
void pjs_host_free(void *ptr, uint32_t align);

/* Never returns. Used by the Rust panic handler (panic = abort). */
void pjs_host_abort(void);

#ifdef __cplusplus
}
#endif

#endif /* POCKETJS_D13X_H */
