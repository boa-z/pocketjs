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
 */

#include <rtthread.h>
#include <stddef.h>
#include <stdint.h>

#include "pocketjs_d13x.h"

#ifndef PJS_TAG
#define PJS_TAG "[pjs-abi] "
#endif

#define PJS_STATIC_ASSERT(cond, tag) typedef char pjs_sa_##tag[(cond) ? 1 : -1]

/* Alignment the RT-Thread heap itself guarantees - whatever the build was
 * configured with, NOT a hardcoded 8. On this board CONFIG_RT_ALIGN_SIZE is 4,
 * so rt_malloc returns 4-byte-aligned memory. The first hardware run proved the
 * old "the heap gives 8" assumption wrong: the probe aborted on its very first
 * Box<u32>, which only asked for 4.
 *
 * Requests above this are satisfied by over-allocating, so the contract holds
 * for any RT_ALIGN_SIZE. If a future build sets it to a non-power-of-two the
 * mask arithmetic breaks, so fail the compile rather than the board. */
#define PJS_HEAP_ALIGN ((uint32_t)RT_ALIGN_SIZE)

PJS_STATIC_ASSERT((RT_ALIGN_SIZE & (RT_ALIGN_SIZE - 1)) == 0, heap_align_pow2);
PJS_STATIC_ASSERT(RT_ALIGN_SIZE >= 4, heap_align_min);

/*
 * Gate 0 allocator seam.
 *
 * Backed by the RT-Thread system heap for this gate. Phase 1 replaces the body
 * with aic_memheap_malloc(MEM_PSRAM_SW) - the Rust side never changes, because
 * it only knows this callback.
 *
 * No clamping of `align` happens here, and that is deliberate. Anything at or
 * below PJS_HEAP_ALIGN is already satisfied by rt_malloc, and anything above it
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
        return RT_NULL;
    }

    if (align <= PJS_HEAP_ALIGN) {
        /* The heap's own alignment already satisfies the request. */
        raw = rt_malloc(size);
        if (raw == RT_NULL) {
            return RT_NULL;
        }
        /* Prove the heap's promise instead of assuming it. */
        if (((uintptr_t)raw & (PJS_HEAP_ALIGN - 1u)) != 0u) {
            rt_kprintf(PJS_TAG "FATAL: rt_malloc returned %p, not %u-byte aligned\n",
                       raw, (unsigned)PJS_HEAP_ALIGN);
            rt_free(raw);
            return RT_NULL;
        }
        return raw;
    }

    /* Over-aligned: reserve the payload, its worst-case padding, and one word
     * in front for the raw pointer that rt_free needs back.
     *
     * Worst case the payload starts at raw + sizeof(void*) + (align - 1), so
     * the block must be size + align + sizeof(void*) - which is what is asked
     * for below. The header sits at payload - sizeof(void*), always >= raw. */
    raw = rt_malloc(size + align + (uint32_t)sizeof(void *));
    if (raw == RT_NULL) {
        return RT_NULL;
    }
    p = (uintptr_t)raw + (uintptr_t)sizeof(void *);
    p = (p + (uintptr_t)align - 1u) & ~((uintptr_t)align - 1u);
    ((void **)p)[-1] = raw;
    return (void *)p;
}

void pjs_host_free(void *ptr, uint32_t align)
{
    if (ptr == RT_NULL) {
        return;
    }
    if (align <= PJS_HEAP_ALIGN) {
        rt_free(ptr);
        return;
    }
    rt_free(((void **)ptr)[-1]);
}
