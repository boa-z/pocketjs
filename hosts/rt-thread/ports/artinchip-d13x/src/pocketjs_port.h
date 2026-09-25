/*
 * PocketJS ArtInChip D13x - port-internal declarations.
 *
 * Not part of the C <-> Rust ABI. That contract lives in
 * include/pocketjs_d13x.h and is mirrored by the crate; nothing here is visible
 * to Rust, so this header can change without touching the Rust side.
 *
 * It exists so src/pocketjs_mem.c can read the allocator's region telemetry
 * without duplicating the struct definition.
 */
#ifndef POCKETJS_PORT_H
#define POCKETJS_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Region telemetry kept by src/pocketjs_alloc.c.
 *
 * `out_of_region` is the one that matters for Gate 1A: it counts allocations
 * whose payload did not lie wholly inside PSRAM_SW. It must be 0. `lo`/`hi`
 * bracket every payload ever handed out, so they can be compared against the
 * region the memheap was actually registered with - not just the linker's idea
 * of it.
 */
typedef struct pjs_heap_stats {
    uint32_t  allocs;         /* successful pjs_host_alloc calls             */
    uint32_t  frees;          /* pjs_host_free calls                         */
    uint32_t  fails;          /* allocations refused (NULL returned)         */
    uint32_t  bad_align;      /* heap handed back under-aligned memory       */
    uint32_t  out_of_region;  /* payload fell outside PSRAM_SW               */
    uint32_t  max_size;       /* largest single request                      */
    uintptr_t lo;             /* lowest payload address ever returned        */
    uintptr_t hi;             /* highest payload *end* ever returned         */
} pjs_heap_stats_t;

void pjs_host_alloc_stats(pjs_heap_stats_t *out);
void pjs_host_alloc_stats_reset(void);

/* src/pocketjs_mem.c */
int pjs_mem_report(void);
int pjs_mem_test(uint32_t iters);

#ifdef __cplusplus
}
#endif

#endif /* POCKETJS_PORT_H */
