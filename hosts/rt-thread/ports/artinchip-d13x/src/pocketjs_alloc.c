/*
 * PocketJS ArtInChip D13x - host allocator (C side).
 *
 * Split out of pocketjs_host.c for one reason: it is the only part of the host
 * layer whose correctness is arithmetic rather than plumbing, and it is the part
 * that failed the first hardware run. In its own translation unit it can be
 * compiled against a stubbed heap and exercised on the build host (see
 * tools/host-test/), instead of being argued about by inspection.
 *
 * The contract, from include/pocketjs_d13x.h: pjs_host_alloc(size, align)
 * returns memory aligned to `align`, or NULL. The RT-Thread heap only guarantees
 * RT_ALIGN_SIZE - 4 on this board - so anything stronger is served by
 * over-allocating. Nothing here assumes a particular RT_ALIGN_SIZE.
 *
 * Gate 1A changed exactly one thing: the backend. rt_malloc/rt_free became
 * aic_memheap_malloc(MEM_PSRAM_SW, ...)/aic_memheap_free(MEM_PSRAM_SW, ...).
 *
 * The alignment shim below is byte-for-byte the Gate 0 logic, deliberately.
 * Gate 0 validated that arithmetic on hardware, and the memheap offers the same
 * guarantee rt_malloc does: rt_memheap_alloc() rounds the request up to
 * RT_ALIGN_SIZE, and RT_MEMHEAP_SIZE is itself a multiple of RT_ALIGN_SIZE, so a
 * block start is aligned exactly as before. Changing the backend and the shim in
 * the same step would make a failure ambiguous, which is the one thing a gate
 * must not be.
 */

#include <rtthread.h>
#include <aic_osal.h>
#include <stddef.h>
#include <stdint.h>

#include "pocketjs_d13x.h"
#include "pocketjs_port.h"

/* Gate 1A: the allocator reports under the memory tag, because `pjs_mem` is
 * where its telemetry surfaces. Gate 0 used the ABI tag; the only message here
 * is a FATAL, and a parser matching PASS/FAIL is unaffected. */
#ifndef PJS_TAG
#define PJS_TAG "[pjs-mem] "
#endif

#define PJS_STATIC_ASSERT(cond, tag) typedef char pjs_sa_##tag[(cond) ? 1 : -1]

/* Alignment the heap itself guarantees - whatever the build was configured
 * with, NOT a hardcoded 8. On this board CONFIG_RT_ALIGN_SIZE is 4, so the
 * memheap returns 4-byte-aligned memory. The first hardware run proved the old
 * "the heap gives 8" assumption wrong: the probe aborted on its very first
 * Box<u32>, which only asked for 4.
 *
 * Requests above this are satisfied by over-allocating, so the contract holds
 * for any RT_ALIGN_SIZE. If a future build sets it to a non-power-of-two the
 * mask arithmetic breaks, so fail the compile rather than the board. */
#define PJS_HEAP_ALIGN ((uint32_t)RT_ALIGN_SIZE)

PJS_STATIC_ASSERT((RT_ALIGN_SIZE & (RT_ALIGN_SIZE - 1)) == 0, heap_align_pow2);
PJS_STATIC_ASSERT(RT_ALIGN_SIZE >= 4, heap_align_min);

/* ------------------------------------------------------------------ *
 * Gate 1A: which heap, and a compile-time refusal to drift
 * ------------------------------------------------------------------ */

/*
 * Every PocketJS allocation must come from PSRAM_SW.
 *
 *   - not CMA: reserved for framebuffer / GE / MPP / DMA, which Gate 2 and
 *     later will need;
 *   - not the system heap: that is 1 MiB of SRAM, and a UI core will not fit.
 *
 * The guard is written against AIC_PSRAM_SW_EN rather than `#ifdef
 * MEM_PSRAM_SW` on purpose: MEM_PSRAM_SW is an *enumerator*, not a macro, so
 * #ifdef would silently be false and the fallback would compile. This mirrors
 * the same guard in target/d13x/d50t-2-lite/board.c:63 and
 * bsp/common/include/aic_common.h:410.
 *
 * AIC_DEFAULT_SYS_HEAP_PSRAM is refused too: it would make PSRAM_SW the system
 * heap and remove MEM_PSRAM_SW from the enum, which is a different memory plan
 * that Gate 1A has not been reasoned about. Fail the build, not the board.
 */
#if !defined(AIC_PSRAM_SW_EN) || defined(AIC_DEFAULT_SYS_HEAP_PSRAM)
#error "PocketJS Gate 1A requires a dedicated PSRAM_SW memheap. Set CONFIG_AIC_PSRAM_SW_SIZE > 0 and keep AIC_DEFAULT_SYS_HEAP_SRAM selected. See GATE1A-MEMORY-MAP.md."
#endif

#define PJS_HEAP_REGION MEM_PSRAM_SW
#define PJS_HEAP_NAME   "heap_psram_sw"

/* The region the allocator promises to stay inside. These are the same symbols
 * board.c registers the memheap with, so they are the truth for a statically
 * correct build; pjs_mem also queries the live memheap and reports both, so a
 * runtime (fuse-driven) difference is visible rather than hidden.
 *
 * The host test has no PSRAM_SW - it links against a stub heap - so there the
 * window is a pair of variables the stub owns, defaulting to the whole address
 * space. Variables rather than constants for two reasons: a constant window
 * makes the bounds check compile to nothing and earns a -Wtype-limits warning,
 * and being able to narrow the window at runtime is what lets the host test
 * exercise the out-of-region path itself.
 *
 * That is the one deliberate difference between the two builds, and it is a
 * bounds constant only: the over-alignment arithmetic and the backend calls
 * below are the code that runs on the board.
 *
 * What the host test checks instead is the part of the contract that *is*
 * testable off-board - that the allocator asks for MEM_PSRAM_SW and never for
 * CMA or the system heap - via the region counters in
 * tools/host-test/stub/rtthread.c. Where the pointers land is a board-side
 * question, and pjs_mem_test answers it there. */
#ifdef PJS_HOST_TEST
extern uintptr_t pjs_host_test_region_lo;
extern uintptr_t pjs_host_test_region_hi;
#define PJS_REGION_LO (pjs_host_test_region_lo)
#define PJS_REGION_HI (pjs_host_test_region_hi)
#else
extern size_t __psram_sw_heap_start;
extern size_t __psram_sw_heap_end;
#define PJS_REGION_LO ((uintptr_t)&__psram_sw_heap_start)
#define PJS_REGION_HI ((uintptr_t)&__psram_sw_heap_end)
#endif

/* ------------------------------------------------------------------ *
 * Telemetry
 * ------------------------------------------------------------------ */

/*
 * Every Rust allocation funnels through pjs_host_alloc, so counting here is the
 * only way to assert "no PocketJS allocation landed outside PSRAM_SW" for *all*
 * of Box, Vec, String and whatever the UI core allocates later - not just for
 * the handful of pointers a test happens to look at.
 *
 * A handful of increments per call. Worth it: the region guarantee is the whole
 * point of Gate 1A, and an assertion nobody can evidence is not an assertion.
 */
static pjs_heap_stats_t g_stats;

void pjs_host_alloc_stats(pjs_heap_stats_t *out)
{
    if (out != RT_NULL) {
        *out = g_stats;
    }
}

void pjs_host_alloc_stats_reset(void)
{
    g_stats.allocs = 0u;
    g_stats.frees = 0u;
    g_stats.fails = 0u;
    g_stats.bad_align = 0u;
    g_stats.out_of_region = 0u;
    g_stats.max_size = 0u;
    /* The range is zeroed, not set to an inverted sentinel, so that this
     * produces *exactly* the state of a freshly-booted static struct. That
     * matters: the range is seeded by the first accounted allocation (see
     * pjs_account), so "after a reset" and "before any allocation" are the same
     * state and the host test can exercise the boot path.
     *
     * The first version set lo = PJS_REGION_HI here and relied on
     * `if (lo < g_stats.lo)` to lower it. That worked only because this
     * function was called - and on the target it never was, so lo stayed at the
     * zero-initialised 0, no payload could ever be lower, and the low bound was
     * reported as 0x00000000. The first hardware run caught it
     * (test.region_lo FAIL). A bound that is only correct when somebody
     * remembers to call a reset is not a bound. */
    g_stats.lo = 0u;
    g_stats.hi = 0u;
}

/* ------------------------------------------------------------------ *
 * The allocator
 * ------------------------------------------------------------------ */

/*
 * Backend seam. Nothing above this line knows which heap is in use.
 *
 * aic_memheap_malloc() takes the memheap's mutex, so this must not be called
 * from interrupt context - the same constraint rt_malloc already carried. Every
 * caller in this port is an MSH command or the Rust global allocator reached
 * from one.
 */
static void *pjs_heap_alloc(uint32_t size)
{
    return aic_memheap_malloc(PJS_HEAP_REGION, (size_t)size);
}

static void pjs_heap_free(void *ptr)
{
    aic_memheap_free(PJS_HEAP_REGION, ptr);
}

/* Record one successful allocation and return it. */
static void *pjs_account(void *payload, uint32_t size)
{
    uintptr_t lo = (uintptr_t)payload;
    uintptr_t hi = lo + (uintptr_t)size;

    /* The payload must lie wholly inside the region: a block that starts inside
     * but ends past the end would corrupt whatever follows the heap. Checked
     * before anything is recorded, so a refused block cannot seed or widen the
     * reported range. */
    if (lo < PJS_REGION_LO || hi > PJS_REGION_HI) {
        g_stats.out_of_region++;
        rt_kprintf(PJS_TAG "FATAL: %s returned %p..%p, outside [%p,%p)\n",
                   PJS_HEAP_NAME, (void *)lo, (void *)hi,
                   (void *)PJS_REGION_LO, (void *)PJS_REGION_HI);
        pjs_heap_free(payload);
        return RT_NULL;
    }

    if (size > g_stats.max_size) {
        g_stats.max_size = size;
    }
    /* Seed the range from the first accepted allocation, then only widen it.
     * Keyed on allocs == 0 rather than on a sentinel value, because a static
     * struct is zero-initialised and the target never calls the reset: seeding
     * is what makes the boot state correct, not the sentinel. See
     * pjs_host_alloc_stats_reset(). */
    if (g_stats.allocs == 0u) {
        g_stats.lo = lo;
        g_stats.hi = hi;
    } else {
        if (lo < g_stats.lo) {
            g_stats.lo = lo;
        }
        if (hi > g_stats.hi) {
            g_stats.hi = hi;
        }
    }
    g_stats.allocs++;
    return payload;
}

/*
 * No clamping of `align` happens here, and that is deliberate. Anything at or
 * below PJS_HEAP_ALIGN is already satisfied by the heap, and anything above it
 * is at least 8 - larger than a pointer on this target - so a clamp would be a
 * no-op in both branches. Leaving it out keeps the arithmetic identical on a
 * 64-bit build host, where sizeof(void *) is 8 and a clamp would silently
 * change what the host test exercises.
 */
void *pjs_host_alloc(uint32_t size, uint32_t align)
{
    void *raw;
    uintptr_t p;

    if (size == 0u) {
        size = 1u;
    }
    /* Rust's Layout::align() is always a power of two; refuse anything else
     * rather than round it to something the caller did not ask for. */
    if (align == 0u || (align & (align - 1u)) != 0u) {
        g_stats.fails++;
        return RT_NULL;
    }

    if (align <= PJS_HEAP_ALIGN) {
        /* The heap's own alignment already satisfies the request. */
        raw = pjs_heap_alloc(size);
        if (raw == RT_NULL) {
            g_stats.fails++;
            return RT_NULL;
        }
        /* Prove the heap's promise instead of assuming it. */
        if (((uintptr_t)raw & (PJS_HEAP_ALIGN - 1u)) != 0u) {
            rt_kprintf(PJS_TAG "FATAL: %s returned %p, not %u-byte aligned\n",
                       PJS_HEAP_NAME, raw, (unsigned)PJS_HEAP_ALIGN);
            g_stats.bad_align++;
            g_stats.fails++;
            pjs_heap_free(raw);
            return RT_NULL;
        }
        return pjs_account(raw, size);
    }

    /* Over-aligned: reserve the payload, its worst-case padding, and one word
     * in front for the raw pointer that the heap needs back.
     *
     * Worst case the payload starts at raw + sizeof(void*) + (align - 1), so
     * the block must be size + align + sizeof(void*) - which is what is asked
     * for below. The header sits at payload - sizeof(void*), always >= raw. */
    raw = pjs_heap_alloc(size + align + (uint32_t)sizeof(void *));
    if (raw == RT_NULL) {
        g_stats.fails++;
        return RT_NULL;
    }
    p = (uintptr_t)raw + (uintptr_t)sizeof(void *);
    p = (p + (uintptr_t)align - 1u) & ~((uintptr_t)align - 1u);
    ((void **)p)[-1] = raw;

    /* Account the *payload*, not the over-allocation: the header and padding
     * are the allocator's overhead, and the region check is about where the
     * caller's bytes live. */
    if (pjs_account((void *)p, size) == RT_NULL) {
        return RT_NULL;
    }
    return (void *)p;
}

void pjs_host_free(void *ptr, uint32_t align)
{
    if (ptr == RT_NULL) {
        return;
    }
    g_stats.frees++;
    if (align <= PJS_HEAP_ALIGN) {
        pjs_heap_free(ptr);
        return;
    }
    pjs_heap_free(((void **)ptr)[-1]);
}
