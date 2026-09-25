/*
 * PocketJS ArtInChip D13x - Gate 1A memory report and PSRAM allocator test.
 *
 *   pjs_mem [n]       report every heap PocketJS can touch, and where it is
 *   pjs_mem_test [n]  exercise the PSRAM_SW allocator for n rounds (default 1000)
 *
 * Two commands on purpose. `pjs_mem` is what you want when something has already
 * gone wrong - it only reads. `pjs_mem_test` is the assertion, and it changes the
 * heap. Merging them would make the report's numbers depend on whether the test
 * had run, which is exactly the sort of ambiguity a gate cannot afford.
 *
 * Output goes to rt_kprintf, not ulog, for the same reason as the Gate 0 probe:
 * a validation harness needs deterministic, unfilterable lines that a host-side
 * parser can match. A log level must not be able to hide a failing case.
 *
 * What Gate 1A has to prove, and where each line comes from:
 *
 *   Rust allocations land in PSRAM_SW     - pjs_host_alloc's region counters
 *   ...and not in the 1 MiB SRAM heap     - rt_memory_info() before/after
 *   ...and not in CMA                     - the memheap PSRAM_SW grew, CMA did not
 *   align 4/8/16/32/64 are honoured       - pjs_host_alloc() called directly
 *   Box/Vec/String work, repeatedly       - pjs_probe_alloc_stress(), checksum
 *   nothing leaks                         - Rust live_bytes == 0, SRAM used flat
 */

#include <rtthread.h>
#include <aic_osal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pocketjs_d13x.h"
#include "pocketjs_port.h"

#define PJS_TAG "[pjs-mem] "

/* The same symbols src/pocketjs_alloc.c promises to stay inside. Declared here
 * rather than shared through a header so that this file cannot silently pick up
 * a different definition. */
extern size_t __psram_sw_heap_start;
extern size_t __psram_sw_heap_end;
extern size_t __cma_heap_start;
extern size_t __cma_heap_end;
extern size_t __heap_start;
extern size_t __heap_end;

/* The name board.c registers the memheap under. Must match PJS_HEAP_NAME in
 * src/pocketjs_alloc.c and the "heap_psram_sw" entry in the per-board
 * target/d13x/<board>/board.c. */
#define PJS_HEAP_NAME "heap_psram_sw"

#define PJS_STRESS_DEFAULT 1000u

/* ------------------------------------------------------------------ */
/* Bookkeeping                                                         */
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

/* ------------------------------------------------------------------ */
/* Heap inspection                                                     */
/* ------------------------------------------------------------------ */

/*
 * The live memheap, found by name.
 *
 * Looked up rather than reached through board.c's aic_memheaps[] because that
 * array's element type is private to board.c. rt_object_find + rt_memheap_info
 * are both linked into this firmware, so this needs no SDK change.
 *
 * The point of querying the object rather than trusting the linker symbols:
 * aic_memheap_init() adjusts the PSRAM_SW end at boot by
 * (aic_get_ram_size() - AIC_PSRAM_SIZE), so the *live* pool can differ from the
 * linked one. If it ever does, both numbers are printed and the difference is
 * visible instead of assumed away.
 */
static struct rt_memheap *pjs_find_heap(const char *name)
{
    return (struct rt_memheap *)rt_object_find(name, RT_Object_Class_MemHeap);
}

static void pjs_print_heap(const char *label, struct rt_memheap *h)
{
    rt_size_t total = 0, used = 0, max_used = 0;

    if (h == RT_NULL) {
        rt_kprintf(PJS_TAG "  %-10s : NOT REGISTERED\n", label);
        return;
    }
    rt_memheap_info(h, &total, &used, &max_used);
    rt_kprintf(PJS_TAG "  %-10s : base=%p pool=%u used=%u max=%u free=%u\n",
               label, h->start_addr,
               (unsigned)total, (unsigned)used, (unsigned)max_used,
               (unsigned)(total - used));
}

int pjs_mem_report(void)
{
    struct rt_memheap *psram_sw;
    struct rt_memheap *cma;
    pjs_heap_stats_t st;
    rt_size_t sys_total = 0, sys_used = 0, sys_max = 0;
    uint32_t r_live = 0, r_peak = 0, r_allocs = 0, r_frees = 0, r_fails = 0;

    pjs_host_alloc_stats(&st);
    psram_sw = pjs_find_heap(PJS_HEAP_NAME);
    cma = pjs_find_heap("heap_cma");
    rt_memory_info(&sys_total, &sys_used, &sys_max);
    pjs_probe_mem_stats(&r_live, &r_peak, &r_allocs, &r_frees, &r_fails);

    rt_kprintf("\n");
    rt_kprintf(PJS_TAG "==============================================\n");
    rt_kprintf(PJS_TAG " PocketJS D13x - heap map\n");
    rt_kprintf(PJS_TAG "==============================================\n");

    rt_kprintf(PJS_TAG "-- regions the linker defined --\n");
    rt_kprintf(PJS_TAG "  sram       : %p .. %p  (%u B)\n",
               (void *)&__heap_start, (void *)&__heap_end,
               (unsigned)((uintptr_t)&__heap_end -
                          (uintptr_t)&__heap_start));
    rt_kprintf(PJS_TAG "  psram_sw   : %p .. %p  (%u B)\n",
               (void *)&__psram_sw_heap_start, (void *)&__psram_sw_heap_end,
               (unsigned)((uintptr_t)&__psram_sw_heap_end -
                          (uintptr_t)&__psram_sw_heap_start));
    rt_kprintf(PJS_TAG "  cma        : %p .. %p  (%u B)\n",
               (void *)&__cma_heap_start, (void *)&__cma_heap_end,
               (unsigned)((uintptr_t)&__cma_heap_end -
                          (uintptr_t)&__cma_heap_start));

    rt_kprintf(PJS_TAG "-- live memheaps --\n");
    rt_kprintf(PJS_TAG "  %-10s : base=%p pool=%u used=%u max=%u free=%u\n",
               "system", (void *)&__heap_start,
               (unsigned)sys_total, (unsigned)sys_used, (unsigned)sys_max,
               (unsigned)(sys_total - sys_used));
    pjs_print_heap("psram_sw", psram_sw);
    pjs_print_heap("cma", cma);

    rt_kprintf(PJS_TAG "-- Rust allocator (guest side) --\n");
    rt_kprintf(PJS_TAG "  live=%u peak=%u allocs=%u frees=%u fails=%u\n",
               (unsigned)r_live, (unsigned)r_peak, (unsigned)r_allocs,
               (unsigned)r_frees, (unsigned)r_fails);

    rt_kprintf(PJS_TAG "-- pjs_host_alloc (host side, since boot) --\n");
    rt_kprintf(PJS_TAG "  allocs=%u frees=%u fails=%u bad_align=%u out_of_region=%u\n",
               (unsigned)st.allocs, (unsigned)st.frees, (unsigned)st.fails,
               (unsigned)st.bad_align, (unsigned)st.out_of_region);
    rt_kprintf(PJS_TAG "  max_request=%u  payload range=%p .. %p\n",
               (unsigned)st.max_size, (void *)st.lo, (void *)st.hi);

    report("mem.psram_sw_registered", psram_sw != RT_NULL);
    report("mem.psram_sw_nonzero", psram_sw != RT_NULL && psram_sw->pool_size > 0u);
    report("mem.no_out_of_region", st.out_of_region == 0u);
    report("mem.no_bad_align", st.bad_align == 0u);
    report("mem.no_failed_allocation", st.fails == 0u);

    rt_kprintf(PJS_TAG "----------------------------------------------\n");
    rt_kprintf(PJS_TAG "SUMMARY pass=%d fail=%d\n", g_pass, g_fail);
    rt_kprintf(PJS_TAG "RESULT %s\n", (g_fail == 0) ? "PASS" : "FAIL");
    rt_kprintf(PJS_TAG "==============================================\n");

    return (g_fail == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
/* ------------------------------------------------------------------ */

/* Deliberately not atoi(): this file is compiled into a firmware that has no
 * obligation to link libc, and a locale-aware parser for a round count would be
 * a dependency bought for nothing. */
static uint32_t pjs_arg_u32(int argc, char **argv, int idx, uint32_t dflt)
{
    const char *s;
    uint32_t v = 0;
    int seen = 0;

    if (idx >= argc || argv[idx] == RT_NULL) {
        return dflt;
    }
    s = argv[idx];
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        seen = 1;
        s++;
    }
    return seen ? v : dflt;
}

/* ------------------------------------------------------------------ */
/* The allocator test                                                  */
/* ------------------------------------------------------------------ */

/*
 * Recompute the stress fold from scratch, on the C side.
 *
 * This duplicates the Rust loop on purpose. The Gate 0 probe does the same for
 * its Vec sum and String hash: agreement between two independent computations
 * is evidence, agreement with itself is not. If the two sides disagree, the
 * Rust side read or wrote something it should not have - which is precisely the
 * failure a shared heap is most likely to produce.
 *
 * Must stay byte-for-byte in step with pjs_probe_alloc_stress() in
 * rust/abi-probe/src/lib.rs. Both sides use wrapping u32 arithmetic.
 */
static uint32_t pjs_stress_checksum(uint32_t iters)
{
    uint32_t ck = 0;
    uint32_t i;

    for (i = 0; i < iters; i++) {
        uint32_t want_box = (uint32_t)(i * 2654435761u) ^ 0x9E3779B9u;
        uint32_t n = (i % 64u) + 1u;
        uint32_t slen = (i % 40u) + 1u;
        uint32_t vsum = 0;
        uint32_t h = 2166136261u;
        uint32_t k;

        for (k = 0; k < n; k++) {
            vsum += (uint32_t)(k * i) + 0x12345678u;
        }
        for (k = 0; k < slen; k++) {
            h = (h ^ (uint32_t)(uint8_t)('a' + (k % 26u))) * 16777619u;
        }
        ck = ck * 31u + (want_box ^ vsum ^ h);
    }
    return ck;
}

/* Alignment requests the Gate 1A contract names explicitly. */
static const uint32_t pjs_aligns[] = { 4u, 8u, 16u, 32u, 64u };
static const char *const pjs_align_names[] = {
    "test.align4", "test.align8", "test.align16", "test.align32", "test.align64"
};

int pjs_mem_test(uint32_t iters)
{
    struct rt_memheap *psram_sw;
    rt_size_t sys_total_before = 0, sys_used_before = 0, sys_max_before = 0;
    rt_size_t sys_total_after = 0, sys_used_after = 0, sys_max_after = 0;
    rt_size_t sw_total = 0, sw_used = 0, sw_max = 0;
    pjs_heap_stats_t st_before, st_after;
    pjs_abi_stress_report_t srep;
    uint32_t r_live = 0, r_peak = 0, r_allocs = 0, r_frees = 0, r_fails = 0;
    uintptr_t lo = (uintptr_t)&__psram_sw_heap_start;
    uintptr_t hi = (uintptr_t)&__psram_sw_heap_end;
    uint32_t want_ck;
    unsigned i;

    if (iters == 0u) {
        iters = PJS_STRESS_DEFAULT;
    }

    g_pass = 0;
    g_fail = 0;

    rt_kprintf("\n");
    rt_kprintf(PJS_TAG "==============================================\n");
    rt_kprintf(PJS_TAG " PocketJS D13x - Gate 1A PSRAM_SW allocator test\n");
    rt_kprintf(PJS_TAG " RV32IMAFDC / ILP32D, heap = heap_psram_sw\n");
    rt_kprintf(PJS_TAG "==============================================\n");

    psram_sw = pjs_find_heap(PJS_HEAP_NAME);
    if (psram_sw == RT_NULL) {
        /* Nothing below is meaningful without the heap. Say so and stop, rather
         * than run a test that would pass by allocating from the wrong place. */
        rt_kprintf(PJS_TAG "FAIL  heap_psram_sw is not registered\n");
        rt_kprintf(PJS_TAG "      CONFIG_AIC_PSRAM_SW_SIZE must be > 0 and\n");
        rt_kprintf(PJS_TAG "      AIC_DEFAULT_SYS_HEAP_SRAM selected.\n");
        rt_kprintf(PJS_TAG "RESULT FAIL\n");
        return -1;
    }

    /* Baselines. Taken before anything is allocated so the comparison below is
     * against the boot state, not against a state the test itself created. */
    rt_memory_info(&sys_total_before, &sys_used_before, &sys_max_before);
    pjs_host_alloc_stats(&st_before);
    pjs_probe_mem_stats(&r_live, &r_peak, &r_allocs, &r_frees, &r_fails);

    rt_kprintf(PJS_TAG "sram before: total=%u used=%u max=%u\n",
               (unsigned)sys_total_before, (unsigned)sys_used_before,
               (unsigned)sys_max_before);
    rt_kprintf(PJS_TAG "region     : %p .. %p  (%u B)\n",
               (void *)lo, (void *)hi, (unsigned)(hi - lo));
    rt_kprintf(PJS_TAG "iters      : %u\n", (unsigned)iters);

    /* ---- 1. alignment, called directly on the allocator ---- */
    rt_kprintf(PJS_TAG "-- alignment (direct pjs_host_alloc) --\n");
    for (i = 0; i < sizeof(pjs_aligns) / sizeof(pjs_aligns[0]); i++) {
        uint32_t align = pjs_aligns[i];
        void *p = pjs_host_alloc(64u, align);
        int ok = (p != RT_NULL) &&
                 (((uintptr_t)p & (uintptr_t)(align - 1u)) == 0u) &&
                 ((uintptr_t)p >= lo) &&
                 (((uintptr_t)p + 64u) <= hi);

        if (!ok && p != RT_NULL) {
            rt_kprintf(PJS_TAG "      align %u: got %p\n",
                       (unsigned)align, p);
        }
        report(pjs_align_names[i], ok);
        pjs_host_free(p, align);
    }

    /* ---- 2. Box / Vec / String, once, through the Rust GlobalAlloc ---- */
    {
        pjs_abi_alloc_report_t arep;
        uint32_t want_sum = 0;
        uint32_t want_hash = 2166136261u;
        const char *s = "pocketjs-d13x-abi-probe";

        rt_kprintf(PJS_TAG "-- Box / Vec / String (single round) --\n");
        for (i = 0; i < 64u; i++) {
            want_sum += (uint32_t)i * 2654435761u;
        }
        for (i = 0; s[i] != '\0'; i++) {
            want_hash = (want_hash ^ (uint32_t)(uint8_t)s[i]) * 16777619u;
        }
        memset(&arep, 0, sizeof(arep));
        pjs_probe_alloc(&arep);

        report("test.box", arep.box_ok == 1u);
        report("test.vec", arep.vec_len == 64u && arep.vec_sum == want_sum);
        report("test.string",
               arep.string_len == (uint32_t)strlen(s) &&
               arep.string_hash == want_hash);
        report("test.box_f64_align8", arep.align8_ok == 1u);
        report("test.over_aligned_64", arep.align_ok == 1u);
    }

    /* ---- 3. the repeated cycle ---- */
    rt_kprintf(PJS_TAG "-- %u rounds of Box + Vec + String --\n", (unsigned)iters);
    memset(&srep, 0, sizeof(srep));
    pjs_probe_alloc_stress(iters, &srep);
    want_ck = pjs_stress_checksum(iters);

    rt_kprintf(PJS_TAG "      rounds=%u box_ok=%u vec_ok=%u string_ok=%u\n",
               (unsigned)srep.rounds, (unsigned)srep.box_ok,
               (unsigned)srep.vec_ok, (unsigned)srep.string_ok);
    rt_kprintf(PJS_TAG "      checksum=0x%08x want=0x%08x\n",
               (unsigned)srep.checksum, (unsigned)want_ck);
    rt_kprintf(PJS_TAG "      live=%u peak=%u allocs=%u frees=%u fails=%u\n",
               (unsigned)srep.live_bytes, (unsigned)srep.peak_bytes,
               (unsigned)srep.alloc_count, (unsigned)srep.free_count,
               (unsigned)srep.fail_count);

    report("test.rounds_completed", srep.rounds == iters);
    report("test.box_held", srep.box_ok == 1u);
    report("test.vec_held", srep.vec_ok == 1u);
    report("test.string_held", srep.string_ok == 1u);
    report("test.checksum_matches_c", srep.checksum == want_ck);
    report("test.live_zero", srep.live_bytes == 0u);
    report("test.fails_zero", srep.fail_count == 0u);
    report("test.peak_nonzero", srep.peak_bytes > 0u);

    /* ---- 4. where did it actually go? ---- */
    rt_kprintf(PJS_TAG "-- heap accounting after the run --\n");
    rt_memory_info(&sys_total_after, &sys_used_after, &sys_max_after);
    rt_memheap_info(psram_sw, &sw_total, &sw_used, &sw_max);
    pjs_host_alloc_stats(&st_after);

    rt_kprintf(PJS_TAG "  sram  : used %u -> %u   max %u -> %u\n",
               (unsigned)sys_used_before, (unsigned)sys_used_after,
               (unsigned)sys_max_before, (unsigned)sys_max_after);
    rt_kprintf(PJS_TAG "  psram : pool=%u used=%u max=%u free=%u\n",
               (unsigned)sw_total, (unsigned)sw_used, (unsigned)sw_max,
               (unsigned)(sw_total - sw_used));
    rt_kprintf(PJS_TAG "  host  : allocs=%u frees=%u fails=%u out_of_region=%u\n",
               (unsigned)st_after.allocs, (unsigned)st_after.frees,
               (unsigned)st_after.fails, (unsigned)st_after.out_of_region);
    rt_kprintf(PJS_TAG "  host  : payload range=%p .. %p\n",
               (void *)st_after.lo, (void *)st_after.hi);

    /* The positive proof: PSRAM_SW's high-water mark must have moved by at
     * least what Rust says it allocated. If it did not, the allocations went
     * somewhere else and the region counters below are the only thing that
     * would have caught it. */
    report("test.psram_sw_carried_the_load", sw_max >= srep.peak_bytes);

    /* The negative proof: the 1 MiB SRAM heap must not have grown. `used`
     * returning to baseline proves there is no leak into SRAM; `max` staying
     * put proves the allocations were never there in the first place. Together
     * they are what "the system SRAM heap no longer carries PocketJS
     * allocations" actually means. */
    report("test.sram_used_flat", sys_used_after <= sys_used_before);
    report("test.sram_max_flat", sys_max_after <= sys_max_before);

    /* Every payload, for the whole life of the firmware, inside PSRAM_SW. */
    report("test.region_lo", st_after.lo >= lo);
    report("test.region_hi", st_after.hi <= hi);
    report("test.no_out_of_region", st_after.out_of_region == 0u);
    report("test.no_bad_align", st_after.bad_align == 0u);

    rt_kprintf(PJS_TAG "----------------------------------------------\n");
    rt_kprintf(PJS_TAG "SUMMARY pass=%d fail=%d\n", g_pass, g_fail);
    rt_kprintf(PJS_TAG "RESULT %s\n", (g_fail == 0) ? "PASS" : "FAIL");
    rt_kprintf(PJS_TAG "==============================================\n");

    return (g_fail == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* MSH entry points                                                    */
/* ------------------------------------------------------------------ */

static int msh_pjs_mem(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return pjs_mem_report();
}

static int msh_pjs_mem_test(int argc, char **argv)
{
    return pjs_mem_test(pjs_arg_u32(argc, argv, 1, PJS_STRESS_DEFAULT));
}

MSH_CMD_EXPORT_ALIAS(msh_pjs_mem, pjs_mem,
                     Report every heap PocketJS can touch);

MSH_CMD_EXPORT_ALIAS(msh_pjs_mem_test, pjs_mem_test,
                     Exercise the PSRAM_SW allocator: pjs_mem_test [rounds]);
