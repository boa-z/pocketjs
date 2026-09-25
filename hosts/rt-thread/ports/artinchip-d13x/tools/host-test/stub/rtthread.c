/* Bump allocator standing in for the RT-Thread heap. See stub/rtthread.h.
 *
 * Two properties matter for the test:
 *   1. the base is offset by 4, so the first block is 4-but-not-8 aligned,
 *      exactly like the board's rt_malloc;
 *   2. a free only accepts a pointer the allocator actually issued, so the
 *      over-alignment header round-trip is verified rather than assumed. A
 *      mismatched free shows up as bad_frees instead of passing silently.
 *
 * Gate 1A added a third: the heap is reachable through two distinct entry
 * points - the system heap (rt_malloc) and the PSRAM_SW memheap
 * (aic_memheap_malloc) - and they are counted separately. Without that split the
 * test could not tell whether the allocator had honoured its region contract.
 *
 * Freed blocks are not reused; the heap is large enough for the test.
 */
#include "rtthread.h"
#include "aic_osal.h"

#include <stdarg.h>
#include <stdio.h>

#define HEAP_SIZE (256 * 1024)
#define MAX_BLOCKS 512

static unsigned char heap_raw[HEAP_SIZE + 8];

/* The 4-but-not-8 aligned base that the allocator hands out from. Computed in
 * reset() rather than assumed, so the reproduction does not depend on where the
 * linker happened to place the array. */
static unsigned char *heap;

typedef struct {
    void *p;
    size_t n;
    int live;
} block_t;

static block_t blocks[MAX_BLOCKS];
static size_t bump;
static size_t live_blocks;
static size_t live_bytes;
static size_t bad_frees;
static size_t high_water;

/* Which entry point was used, and with which region. */
static size_t allocs_via_psram_sw;
static size_t allocs_via_other_region;
static size_t allocs_via_default;
static size_t frees_via_psram_sw;
static size_t frees_via_other_region;

/* The PSRAM_SW window src/pocketjs_alloc.c checks payloads against. Defaults to
 * the whole address space - there is no PSRAM_SW on the host - and test_alloc.c
 * narrows it to prove the out-of-region path fires. */
uintptr_t pjs_host_test_region_lo = 0;
uintptr_t pjs_host_test_region_hi = ~(uintptr_t)0;

void pjs_test_heap_reset(void)
{
    size_t i;
    uintptr_t base = (uintptr_t)heap_raw;

    /* Align up to 8, then offset by RT_ALIGN_SIZE: the base is 4-aligned and
     * provably not 8-aligned, so every block is too. That is the board's
     * behaviour (rt_malloc returned 0x01CA766C) and the whole point of the
     * stub. */
    base = (base + 7u) & ~(uintptr_t)7u;
    heap = (unsigned char *)(base + RT_ALIGN_SIZE);

    bump = 0;
    live_blocks = 0;
    live_bytes = 0;
    bad_frees = 0;
    high_water = 0;
    allocs_via_psram_sw = 0;
    allocs_via_other_region = 0;
    allocs_via_default = 0;
    frees_via_psram_sw = 0;
    frees_via_other_region = 0;
    pjs_host_test_region_lo = 0;
    pjs_host_test_region_hi = ~(uintptr_t)0;
    for (i = 0; i < MAX_BLOCKS; i++) {
        blocks[i].p = RT_NULL;
        blocks[i].n = 0;
        blocks[i].live = 0;
    }
}

size_t pjs_test_heap_live_blocks(void) { return live_blocks; }
size_t pjs_test_heap_live_bytes(void) { return live_bytes; }
size_t pjs_test_heap_bad_frees(void) { return bad_frees; }
size_t pjs_test_heap_high_water(void) { return high_water; }

size_t pjs_test_allocs_via_psram_sw(void) { return allocs_via_psram_sw; }
size_t pjs_test_allocs_via_other_region(void) { return allocs_via_other_region; }
size_t pjs_test_allocs_via_default(void) { return allocs_via_default; }
size_t pjs_test_frees_via_psram_sw(void) { return frees_via_psram_sw; }
size_t pjs_test_frees_via_other_region(void) { return frees_via_other_region; }

/* ---- the underlying heap, shared by both entry points ---- */

static void *stub_alloc(size_t size)
{
    size_t aligned;
    size_t i;
    void *p;

    if (size == 0) {
        return RT_NULL;
    }
    /* RT-Thread rounds the request up to RT_ALIGN_SIZE. */
    aligned = (size + (RT_ALIGN_SIZE - 1)) & ~(size_t)(RT_ALIGN_SIZE - 1);
    if (bump + aligned > HEAP_SIZE) {
        return RT_NULL;
    }

    p = &heap[bump];
    bump += aligned;
    if (bump > high_water) {
        high_water = bump;
    }

    for (i = 0; i < MAX_BLOCKS; i++) {
        if (!blocks[i].live) {
            blocks[i].p = p;
            blocks[i].n = aligned;
            blocks[i].live = 1;
            break;
        }
    }

    live_blocks++;
    live_bytes += aligned;
    return p;
}

static void stub_free(void *ptr)
{
    size_t i;

    if (ptr == RT_NULL) {
        return;
    }
    for (i = 0; i < MAX_BLOCKS; i++) {
        if (blocks[i].live && blocks[i].p == ptr) {
            blocks[i].live = 0;
            live_blocks--;
            live_bytes -= blocks[i].n;
            return;
        }
    }
    /* A pointer the allocator never handed out: the header round-trip is
     * broken. */
    bad_frees++;
}

/* ---- system heap entry point ---- */

void *rt_malloc(size_t size)
{
    allocs_via_default++;
    return stub_alloc(size);
}

void rt_free(void *ptr)
{
    stub_free(ptr);
}

/* ---- PSRAM_SW / CMA memheap entry point ---- */

void *aic_memheap_malloc(int type, size_t size)
{
    if (type == MEM_PSRAM_SW) {
        allocs_via_psram_sw++;
    } else {
        /* MEM_CMA, MEM_DEFAULT, or a region this stub does not model. All of
         * them are a Gate 1A contract violation, so they are counted together
         * rather than silently served. */
        allocs_via_other_region++;
    }
    return stub_alloc(size);
}

void aic_memheap_free(int type, void *rmem)
{
    if (type == MEM_PSRAM_SW) {
        frees_via_psram_sw++;
    } else {
        frees_via_other_region++;
    }
    stub_free(rmem);
}

int rt_kprintf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}
