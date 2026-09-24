/*
 * PocketJS ArtInChip D13x - Gate 0 ABI conformance harness (C side).
 *
 * Implements the host services the Rust crate declares in
 * include/pocketjs_d13x.h, then drives every C <-> Rust case in both
 * directions and reports PASS/FAIL per case.
 *
 * Two MSH commands:
 *   pjs_abi        - run every non-fatal case, print a summary
 *   pjs_abi_panic  - run the deliberate panic (must abort; run it last)
 *
 * Output uses rt_kprintf rather than ulog on purpose: a validation harness
 * needs deterministic, unfilterable lines that a host-side parser can match.
 * Log-level configuration must not be able to hide a failing case.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "pocketjs_d13x.h"

#define PJS_TAG "[pjs-abi] "

/* __alignof__ is a GCC builtin available in every -std mode the SDK uses;
   _Alignof would require C11 and the SDK is not guaranteed to be built that
   way. */
#define PJS_ALIGNOF(t) ((uint32_t)__alignof__(t))

/* Alignment the RT-Thread heap itself guarantees - whatever the build was
 * configured with, NOT a hardcoded 8. On this board CONFIG_RT_ALIGN_SIZE is 4,
 * so rt_malloc returns 4-byte-aligned memory. The first hardware run proved the
 * old "the heap gives 8" assumption wrong: the probe aborted on its very first
 * Box<u32>, which only asked for 4.
 *
 * Requests above this are satisfied by over-allocating, so the contract holds
 * for any RT_ALIGN_SIZE. The allocator lives in pocketjs_alloc.c; the constant
 * is repeated here only so this file can describe the failure it caused. */
#define PJS_HEAP_ALIGN ((uint32_t)RT_ALIGN_SIZE)

/* ------------------------------------------------------------------ */
/* Compile-time cross-check of the shared layout against the C compiler */
/* ------------------------------------------------------------------ */

#define PJS_STATIC_ASSERT(cond, tag) typedef char pjs_sa_##tag[(cond) ? 1 : -1]

PJS_STATIC_ASSERT(sizeof(pjs_abi_value_t) == 16, value_size);
PJS_STATIC_ASSERT(offsetof(pjs_abi_value_t, id) == 0, value_off_id);
PJS_STATIC_ASSERT(offsetof(pjs_abi_value_t, x) == 4, value_off_x);
PJS_STATIC_ASSERT(offsetof(pjs_abi_value_t, value) == 8, value_off_value);

PJS_STATIC_ASSERT(sizeof(pjs_abi_nested_t) == 32, nested_size);
PJS_STATIC_ASSERT(offsetof(pjs_abi_nested_t, tag) == 0, nested_off_tag);
PJS_STATIC_ASSERT(offsetof(pjs_abi_nested_t, count) == 4, nested_off_count);
PJS_STATIC_ASSERT(offsetof(pjs_abi_nested_t, inner) == 8, nested_off_inner);
PJS_STATIC_ASSERT(offsetof(pjs_abi_nested_t, tail) == 24, nested_off_tail);

PJS_STATIC_ASSERT(sizeof(double) == 8, f64_size);
PJS_STATIC_ASSERT(sizeof(void *) == 4, ptr_size);

/* ------------------------------------------------------------------ */
/* Host services required by the Rust crate                            */
/* ------------------------------------------------------------------ */

void pjs_host_log(const char *msg, uint32_t len)
{
    /* rt_kprintf takes NUL-terminated strings; chunk without assuming the
     * Rust side ever terminates its buffer. */
    char buf[96];
    uint32_t off = 0;

    if (msg == RT_NULL) {
        return;
    }
    while (off < len) {
        uint32_t n = len - off;
        if (n > sizeof(buf) - 1) {
            n = sizeof(buf) - 1;
        }
        memcpy(buf, msg + off, n);
        buf[n] = '\0';
        rt_kprintf("%s", buf);
        off += n;
    }
}

/*
 * The allocator seam (pjs_host_alloc / pjs_host_free) lives in
 * pocketjs_alloc.c, so it can be compiled and exercised on the host with a
 * stubbed heap - see tools/host-test/.
 */

void pjs_host_abort(void)
{
    rt_kprintf("\r\n");
    rt_kprintf(PJS_TAG "ABORT: Rust panic reached the host (panic=abort).\n");
    rt_kprintf(PJS_TAG "ABORT: firmware halted by design; reset the board.\n");

    /* No unwinder on this target: stop rather than continue in a corrupt
     * state. A visible halt on the console is the evidence Gate 0 wants. */
    rt_enter_critical();
    for (;;) {
        ;
    }
}

/* ---- Rust -> C callbacks. Deterministic so Rust can predict them. ---- */

uint32_t pjs_host_u32(uint32_t v)
{
    return v ^ 0x5A5A5A5Au;
}

float pjs_host_f32(float v)
{
    return v * 4.0f - 1.0f;
}

double pjs_host_f64(double v)
{
    return v / 2.0 - 0.75;
}

pjs_abi_value_t pjs_host_value(pjs_abi_value_t v)
{
    pjs_abi_value_t r;

    r.id = v.id + 7u;
    r.x = v.x - 0.5f;
    r.value = v.value * 4.0;
    return r;
}

pjs_abi_nested_t pjs_host_nested(pjs_abi_nested_t v)
{
    pjs_abi_nested_t r;

    r.tag = (uint8_t)(v.tag + 2u);
    r.count = v.count + 11u;
    r.inner.id = v.inner.id * 3u;
    r.inner.x = v.inner.x + 1.5f;
    r.inner.value = v.inner.value - 2.25;
    r.tail = v.tail * 10.0;
    return r;
}

double pjs_host_mixed(uint32_t a, float b, double c, const uint32_t *p, uint32_t n)
{
    uint32_t i;
    uint32_t sum = 0;

    for (i = 0; i < n; i++) {
        sum += p[i];
    }
    return (double)a * 2.0 + (double)b + c + (double)sum;
}

/* ------------------------------------------------------------------ */
/* Test bookkeeping                                                    */
/* ------------------------------------------------------------------ */

static int g_pass;
static int g_fail;

static void report(const char *name, int ok)
{
    if (ok) {
        g_pass++;
        rt_kprintf(PJS_TAG "PASS  %s\n", name);
    } else {
        g_fail++;
        rt_kprintf(PJS_TAG "FAIL  %s\n", name);
    }
}

static void expect_u32(const char *name, uint32_t got, uint32_t want)
{
    int ok = (got == want);

    if (!ok) {
        rt_kprintf(PJS_TAG "      u32 got=0x%08x want=0x%08x\n",
                   (unsigned)got, (unsigned)want);
    }
    report(name, ok);
}

static void expect_i32(const char *name, int32_t got, int32_t want)
{
    int ok = (got == want);

    if (!ok) {
        rt_kprintf(PJS_TAG "      i32 got=%d want=%d\n", (int)got, (int)want);
    }
    report(name, ok);
}

static void expect_f32(const char *name, float got, float want)
{
    /* Every value used here is exactly representable, so bitwise equality is
     * the correct comparison - an epsilon would hide a real ABI fault. */
    int ok = (got == want);

    if (!ok) {
        rt_kprintf(PJS_TAG "      f32 got=%.9g want=%.9g\n", (double)got, (double)want);
    }
    report(name, ok);
}

static void expect_f64(const char *name, double got, double want)
{
    int ok = (got == want);

    if (!ok) {
        rt_kprintf(PJS_TAG "      f64 got=%.17g want=%.17g\n", got, want);
    }
    report(name, ok);
}

/* ------------------------------------------------------------------ */
/* Cases                                                               */
/* ------------------------------------------------------------------ */

static void case_c_to_rust_scalars(void)
{
    rt_kprintf(PJS_TAG "-- C -> Rust: scalars --\n");

    expect_u32("c2r.u32", pjs_probe_u32(0x12345678u),
               (uint32_t)(0x12345678u * 2654435761u) ^ 0x9E3779B9u);

    expect_i32("c2r.i32", pjs_probe_i32(-123456),
               (int32_t)((-123456 * -3) + 7));

    expect_u32("c2r.usize", pjs_probe_usize(0x1000u),
               (uint32_t)(0x1000u + 0x12345678u));

    expect_f32("c2r.f32", pjs_probe_f32(1.5f), 1.5f * 2.0f + 0.5f);
    expect_f64("c2r.f64", pjs_probe_f64(2.5), 2.5 * 3.0 + 0.25);
}

static void case_c_to_rust_pointer(void)
{
    uint32_t buf[8];
    uint32_t want = 0;
    int i;

    rt_kprintf(PJS_TAG "-- C -> Rust: pointer --\n");

    for (i = 0; i < 8; i++) {
        buf[i] = (uint32_t)(0x100u * (uint32_t)i + 7u);
        want += buf[i];
    }

    expect_u32("c2r.ptr_sum", pjs_probe_ptr_sum(buf, 8), want);

    /* NULL must be tolerated, not dereferenced. */
    expect_u32("c2r.ptr_null", pjs_probe_ptr_sum(RT_NULL, 8), 0u);

    /* The same address must survive a round trip. */
    report("c2r.ptr_roundtrip",
           pjs_probe_roundtrip_ptr((void *)buf) == (void *)buf);

    report("c2r.log_len", pjs_probe_log_len("pocketjs", 8) == 8u);
}

static void case_c_to_rust_structs(void)
{
    pjs_abi_value_t v, r;
    pjs_abi_nested_t n, rn;

    rt_kprintf(PJS_TAG "-- C -> Rust: structs by value --\n");

    v.id = 0xABCDu;
    v.x = 1.25f;
    v.value = 2.5;

    r = pjs_probe_value(v);
    expect_u32("c2r.value.id", r.id, v.id ^ 0xFFFFu);
    expect_f32("c2r.value.x", r.x, v.x * 2.0f);
    expect_f64("c2r.value.value", r.value, v.value + 1.5);

    n.tag = 0x11u;
    n.count = 100u;
    n.inner.id = 5u;
    n.inner.x = 0.5f;
    n.inner.value = 3.0;
    n.tail = 8.0;

    rn = pjs_probe_nested(n);
    expect_u32("c2r.nested.tag", rn.tag, (uint32_t)(uint8_t)(n.tag + 1u));
    expect_u32("c2r.nested.count", rn.count, n.count * 2u);
    expect_u32("c2r.nested.inner.id", rn.inner.id, n.inner.id + 1000u);
    expect_f32("c2r.nested.inner.x", rn.inner.x, n.inner.x + 0.25f);
    expect_f64("c2r.nested.inner.value", rn.inner.value, n.inner.value * 2.0);
    expect_f64("c2r.nested.tail", rn.tail, n.tail - 0.5);
}

static void case_c_to_rust_mixed(void)
{
    uint32_t buf[4] = { 1u, 2u, 3u, 4u };
    double want;

    rt_kprintf(PJS_TAG "-- C -> Rust: mixed scalar + pointer --\n");

    want = (double)10u + (double)2.5f * 2.0 + 4.25 + (double)(1u + 2u + 3u + 4u);
    expect_f64("c2r.mixed", pjs_probe_mixed(10u, 2.5f, 4.25, buf, 4), want);
}

static void case_layout(void)
{
    pjs_abi_layout_t l;
    int ok;

    rt_kprintf(PJS_TAG "-- layout agreement (Rust vs C compiler) --\n");

    memset(&l, 0, sizeof(l));
    pjs_probe_layout(&l);

    ok = (l.usize_size == sizeof(size_t)) &&
         (l.usize_align == PJS_ALIGNOF(size_t)) &&
         (l.ptr_size == sizeof(void *)) &&
         (l.f64_size == sizeof(double)) &&
         (l.f64_align == PJS_ALIGNOF(double)) &&
         (l.value_size == sizeof(pjs_abi_value_t)) &&
         (l.value_align == PJS_ALIGNOF(pjs_abi_value_t)) &&
         (l.value_off_id == offsetof(pjs_abi_value_t, id)) &&
         (l.value_off_x == offsetof(pjs_abi_value_t, x)) &&
         (l.value_off_value == offsetof(pjs_abi_value_t, value)) &&
         (l.nested_size == sizeof(pjs_abi_nested_t)) &&
         (l.nested_align == PJS_ALIGNOF(pjs_abi_nested_t)) &&
         (l.nested_off_tag == offsetof(pjs_abi_nested_t, tag)) &&
         (l.nested_off_count == offsetof(pjs_abi_nested_t, count)) &&
         (l.nested_off_inner == offsetof(pjs_abi_nested_t, inner)) &&
         (l.nested_off_tail == offsetof(pjs_abi_nested_t, tail));

    rt_kprintf(PJS_TAG "      rust: usize=%u/%u f64=%u/%u value=%u/%u nested=%u/%u ptr=%u\n",
               (unsigned)l.usize_size, (unsigned)l.usize_align,
               (unsigned)l.f64_size, (unsigned)l.f64_align,
               (unsigned)l.value_size, (unsigned)l.value_align,
               (unsigned)l.nested_size, (unsigned)l.nested_align,
               (unsigned)l.ptr_size);
    rt_kprintf(PJS_TAG "      c:    usize=%u/%u f64=%u/%u value=%u/%u nested=%u/%u ptr=%u\n",
               (unsigned)sizeof(size_t), (unsigned)PJS_ALIGNOF(size_t),
               (unsigned)sizeof(double), (unsigned)PJS_ALIGNOF(double),
               (unsigned)sizeof(pjs_abi_value_t), (unsigned)PJS_ALIGNOF(pjs_abi_value_t),
               (unsigned)sizeof(pjs_abi_nested_t), (unsigned)PJS_ALIGNOF(pjs_abi_nested_t),
               (unsigned)sizeof(void *));

    report("layout.f64_is_8_byte_aligned", l.f64_align == 8u);
    report("layout.rust_matches_c", ok);
}

static void case_rust_to_c(void)
{
    pjs_abi_value_t v, r;
    pjs_abi_nested_t n, rn;
    uint32_t buf[4] = { 5u, 6u, 7u, 8u };
    double want;

    rt_kprintf(PJS_TAG "-- Rust -> C: callbacks --\n");

    expect_u32("r2c.u32", pjs_probe_call_host_u32(0x0F0F0F0Fu),
               0x0F0F0F0Fu ^ 0x5A5A5A5Au);
    expect_f32("r2c.f32", pjs_probe_call_host_f32(3.0f), 3.0f * 4.0f - 1.0f);
    expect_f64("r2c.f64", pjs_probe_call_host_f64(9.0), 9.0 / 2.0 - 0.75);

    v.id = 42u;
    v.x = 2.0f;
    v.value = 6.0;
    r = pjs_probe_call_host_value(v);
    expect_u32("r2c.value.id", r.id, 42u + 7u);
    expect_f32("r2c.value.x", r.x, 2.0f - 0.5f);
    expect_f64("r2c.value.value", r.value, 6.0 * 4.0);

    n.tag = 3u;
    n.count = 9u;
    n.inner.id = 2u;
    n.inner.x = 1.0f;
    n.inner.value = 5.5;
    n.tail = 0.5;
    rn = pjs_probe_call_host_nested(n);
    expect_u32("r2c.nested.tag", rn.tag, 5u);
    expect_u32("r2c.nested.count", rn.count, 20u);
    expect_u32("r2c.nested.inner.id", rn.inner.id, 6u);
    expect_f32("r2c.nested.inner.x", rn.inner.x, 2.5f);
    expect_f64("r2c.nested.inner.value", rn.inner.value, 3.25);
    expect_f64("r2c.nested.tail", rn.tail, 5.0);

    want = (double)3u * 2.0 + (double)1.5f + 7.5 + (double)(5u + 6u + 7u + 8u);
    expect_f64("r2c.mixed", pjs_probe_call_host_mixed(3u, 1.5f, 7.5, buf, 4), want);
}

static void case_alloc(void)
{
    pjs_abi_alloc_report_t rep;
    uint32_t live = 0, peak = 0, allocs = 0, frees = 0, fails = 0;
    uint32_t want_sum = 0;
    uint32_t want_hash = 2166136261u;
    const char *s = "pocketjs-d13x-abi-probe";
    uint32_t i;

    rt_kprintf(PJS_TAG "-- allocator: Box / Vec / String / over-alignment --\n");

    for (i = 0; i < 64u; i++) {
        want_sum += i * 2654435761u;
    }
    for (i = 0; s[i] != '\0'; i++) {
        want_hash = (want_hash ^ (uint32_t)(uint8_t)s[i]) * 16777619u;
    }

    memset(&rep, 0, sizeof(rep));
    pjs_probe_alloc(&rep);

    report("alloc.box", rep.box_ok == 1u);
    expect_u32("alloc.vec_len", rep.vec_len, 64u);
    expect_u32("alloc.vec_sum", rep.vec_sum, want_sum);
    expect_u32("alloc.string_len", rep.string_len, (uint32_t)strlen(s));
    expect_u32("alloc.string_hash", rep.string_hash, want_hash);
    report("alloc.align8", rep.align8_ok == 1u);
    report("alloc.over_aligned_64", rep.align_ok == 1u);

    /* The host allocator's own contract, checked with no Rust in the loop.
     * This is the exact thing that failed on the first hardware run, so it is
     * verified directly rather than only through the GlobalAlloc wrapper. */
    {
        void *p8 = pjs_host_alloc(37u, 8u);
        void *p64 = pjs_host_alloc(37u, 64u);
        void *p4 = pjs_host_alloc(37u, 4u);

        report("host.alloc_align4", p4 != RT_NULL && ((uintptr_t)p4 & 3u) == 0u);
        report("host.alloc_align8", p8 != RT_NULL && ((uintptr_t)p8 & 7u) == 0u);
        report("host.alloc_align64", p64 != RT_NULL && ((uintptr_t)p64 & 63u) == 0u);

        pjs_host_free(p4, 4u);
        pjs_host_free(p8, 8u);
        pjs_host_free(p64, 64u);
    }

    pjs_probe_mem_stats(&live, &peak, &allocs, &frees, &fails);

    rt_kprintf(PJS_TAG "      live=%u peak=%u allocs=%u frees=%u fails=%u\n",
               (unsigned)live, (unsigned)peak, (unsigned)allocs,
               (unsigned)frees, (unsigned)fails);

    /* Everything the probe allocated has been dropped, so a non-zero live
     * count means a leak inside the Rust allocator, not a workload artifact. */
    expect_u32("alloc.no_leak_live_zero", live, 0u);
    expect_u32("alloc.no_failed_allocation", fails, 0u);
    report("alloc.peak_nonzero", peak > 0u);
}

/* ------------------------------------------------------------------ */
/* MSH entry points                                                    */
/* ------------------------------------------------------------------ */

/* Non-static: the Gate 0 application calls this once at boot so the evidence
 * lands on the console without anyone having to type at the MSH prompt, and
 * `pjs_abi` re-runs it on demand. */
int pjs_abi_run(void)
{
    rt_size_t heap_total = 0, heap_used = 0, heap_max = 0;

    g_pass = 0;
    g_fail = 0;

    rt_kprintf("\n");
    rt_kprintf(PJS_TAG "==============================================\n");
    rt_kprintf(PJS_TAG " PocketJS D13x - Gate 0 ABI conformance\n");
    rt_kprintf(PJS_TAG " RV32IMAFDC / ILP32D, C <-> Rust\n");
    rt_kprintf(PJS_TAG "==============================================\n");

    rt_memory_info(&heap_total, &heap_used, &heap_max);
    rt_kprintf(PJS_TAG "heap before: total=%u used=%u max_used=%u\n",
               (unsigned)heap_total, (unsigned)heap_used, (unsigned)heap_max);

    case_c_to_rust_scalars();
    case_c_to_rust_pointer();
    case_c_to_rust_structs();
    case_c_to_rust_mixed();
    case_layout();
    case_rust_to_c();
    case_alloc();

    rt_kprintf(PJS_TAG "----------------------------------------------\n");
    rt_kprintf(PJS_TAG "SUMMARY pass=%d fail=%d\n", g_pass, g_fail);
    rt_kprintf(PJS_TAG "RESULT %s\n", (g_fail == 0) ? "PASS" : "FAIL");
    rt_kprintf(PJS_TAG "==============================================\n");

    return (g_fail == 0) ? 0 : -1;
}

MSH_CMD_EXPORT_ALIAS(pjs_abi_run, pjs_abi, Gate 0 C/Rust ABI conformance probe);

static int pjs_abi_panic(void)
{
    rt_kprintf(PJS_TAG "invoking Rust panic; expect an abort and a halted board\n");
    pjs_probe_panic();

    /* Unreachable when panic=abort works. Reaching here is itself a FAIL. */
    rt_kprintf(PJS_TAG "FAIL  panic.returned: panic returned instead of aborting\n");
    return -1;
}

MSH_CMD_EXPORT_ALIAS(pjs_abi_panic, pjs_abi_panic, Trigger the Rust panic=abort path);
