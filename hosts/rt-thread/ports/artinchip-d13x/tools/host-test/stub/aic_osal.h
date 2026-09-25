/* Minimal stand-in for <aic_osal.h>, so src/pocketjs_alloc.c can be compiled and
 * run on the build host.
 *
 * The real header pulls in bsp/common/include/aic_common.h for the MEM_* enum
 * and kernel/common/include/osal/aic_osal_rtthread.h for the memheap API. Only
 * the names the allocator actually uses are reproduced here.
 *
 * The point of stubbing it rather than skipping the include: Gate 1A's whole
 * contract is *which* heap the allocator uses. If the test could not see the
 * region, it could not tell PSRAM_SW from CMA from the system heap, and would
 * pass no matter what the allocator did.
 *
 * Not part of the firmware. Never compiled into the SDK.
 */
#ifndef PJS_HOST_TEST_AIC_OSAL_H
#define PJS_HOST_TEST_AIC_OSAL_H

#include <stddef.h>

#include "rtthread.h"

/* Mirrors the guard in the real bsp/common/include/aic_common.h:410 - MEM_PSRAM_SW
 * exists only when PSRAM_SW is enabled and is not itself the system heap. */
#define AIC_PSRAM_SW_EN 1

/* Mirrors the MEM_* enum. MEM_PSRAM_SW is deliberately *not* the first
 * enumerator, so a test that passed MEM_DEFAULT by mistake would not
 * accidentally look correct. */
enum {
    MEM_DEFAULT = 0,
    MEM_PSRAM_SW,
    MEM_CMA,
};

/* Mirrors kernel/common/include/osal/aic_osal_rtthread.h:303-304. Note the
 * argument order: the region comes first, as in the SDK. */
void *aic_memheap_malloc(int type, size_t size);
void aic_memheap_free(int type, void *rmem);

/* Test-visible region counters.
 *
 * These are what makes the Gate 1A claim checkable off-board: every PocketJS
 * allocation must arrive through MEM_PSRAM_SW, and none through MEM_CMA or the
 * system heap. Counting the entry point used is the only way to see that from
 * the outside. */
size_t pjs_test_allocs_via_psram_sw(void);
size_t pjs_test_allocs_via_other_region(void);
size_t pjs_test_allocs_via_default(void);
size_t pjs_test_frees_via_psram_sw(void);
size_t pjs_test_frees_via_other_region(void);

/* The PSRAM_SW window src/pocketjs_alloc.c checks payloads against.
 *
 * On the board these are the linker's __psram_sw_heap_start/__psram_sw_heap_end.
 * Here they default to the whole address space, because there is no PSRAM_SW to
 * point at. They are variables rather than constants so that test_alloc.c can
 * narrow the window and prove the out-of-region path actually fires - a check
 * that can only ever be taken on faith is not a check.
 *
 * pjs_test_heap_reset() restores the default. */
extern uintptr_t pjs_host_test_region_lo;
extern uintptr_t pjs_host_test_region_hi;

#endif /* PJS_HOST_TEST_AIC_OSAL_H */
