/* Bump allocator standing in for the RT-Thread heap. See stub/rtthread.h.
 *
 * Two properties matter for the test:
 *   1. the base is offset by 4, so the first block is 4-but-not-8 aligned,
 *      exactly like the board's rt_malloc;
 *   2. rt_free only accepts a pointer that rt_malloc actually issued, so the
 *      over-alignment header round-trip is verified rather than assumed. A
 *      mismatched free shows up as bad_frees instead of passing silently.
 *
 * Freed blocks are not reused; the heap is large enough for the test.
 */
#include "rtthread.h"

#include <stdarg.h>
#include <stdio.h>

#define HEAP_SIZE (256 * 1024)
#define MAX_BLOCKS 512

static unsigned char heap_raw[HEAP_SIZE + 8];

/* The 4-but-not-8 aligned base that rt_malloc hands out from. Computed in
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

void *rt_malloc(size_t size)
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

void rt_free(void *ptr)
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
    /* A pointer rt_malloc never handed out: the header round-trip is broken. */
    bad_frees++;
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
