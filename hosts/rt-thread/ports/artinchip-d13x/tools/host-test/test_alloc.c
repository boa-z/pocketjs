/* Host-side test for the D13x host allocator (src/pocketjs_alloc.c).
 *
 * Why this exists: the first Gate 0 hardware run passed every C<->Rust ABI case
 * and then aborted in the allocator, because rt_malloc on this board returns
 * 4-byte-aligned memory while both sides assumed 8. That is arithmetic, so it
 * can be tested here against a stubbed heap - no board required - and this test
 * is the regression guard for it.
 *
 * Gate 1A added a second thing to guard: the backend moved from rt_malloc to
 * aic_memheap_malloc(MEM_PSRAM_SW). The alignment arithmetic must be unchanged,
 * and the allocator must never ask for CMA or the system heap. Both are checked
 * below; the second one is what region_contract() is for.
 *
 * Run: python tools/test-alloc-host.py
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtthread.h"        /* the stub: RT_ALIGN_SIZE, rt_malloc, heap probes */
#include "aic_osal.h"        /* the stub: MEM_PSRAM_SW, region counters        */
#include "pocketjs_d13x.h"   /* the real contract: pjs_host_alloc/free         */
#include "pocketjs_port.h"   /* the real allocator's telemetry                 */

static int failures;
static int checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
    }
    printf("  %s  %s\n", cond ? "PASS" : "FAIL", what);
}

static int aligned_to(void *p, uint32_t a)
{
    return ((uintptr_t)p & (uintptr_t)(a - 1u)) == 0u;
}

/* The stub heap must actually be 4-but-not-8 aligned, otherwise nothing below
 * says anything about the failure this test is meant to prevent. */
static void guard_stub_reproduces_board(void)
{
    void *raw;
    int four;
    int not_eight;

    /* The heap base is computed in reset(), so it must run first. */
    pjs_test_heap_reset();
    raw = rt_malloc(8);
    four = ((uintptr_t)raw & 3u) == 0u;
    not_eight = ((uintptr_t)raw & 7u) != 0u;

    printf("-- stub heap reproduces the board --\n");
    printf("       rt_malloc(8) -> %p\n", raw);
    check(four && not_eight,
          "raw rt_malloc is 4-byte aligned but not 8-byte aligned (as on silicon)");
    rt_free(raw);
}

/* Every alignment/size pair: honoured, fully writable, and freed cleanly. */
static void sweep_size_and_align(void)
{
    static const uint32_t aligns[] = { 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u };
    static const uint32_t sizes[] = { 1u, 2u, 3u, 4u, 5u, 7u, 8u, 15u, 16u,
                                      37u, 64u, 100u, 255u, 256u, 1024u };
    size_t a, s;

    printf("-- size x alignment sweep --\n");
    pjs_test_heap_reset();

    for (a = 0; a < sizeof(aligns) / sizeof(aligns[0]); a++) {
        for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            void *p = pjs_host_alloc(sizes[s], aligns[a]);

            if (p == NULL) {
                printf("  FAIL  alloc(size=%u, align=%u) returned NULL\n",
                       (unsigned)sizes[s], (unsigned)aligns[a]);
                failures++;
                checks++;
                continue;
            }
            if (!aligned_to(p, aligns[a])) {
                printf("  FAIL  alloc(size=%u, align=%u) -> %p not %u-aligned\n",
                       (unsigned)sizes[s], (unsigned)aligns[a], p,
                       (unsigned)aligns[a]);
                failures++;
                checks++;
            }
            /* The whole requested size must be writable. */
            memset(p, 0xA5, sizes[s]);
            pjs_host_free(p, aligns[a]);
        }
    }
    check(pjs_test_heap_live_blocks() == 0, "no blocks left live");
    check(pjs_test_heap_live_bytes() == 0, "no bytes left live");
    check(pjs_test_heap_bad_frees() == 0, "every free matched a block the allocator issued");
}

/* The exact case the board failed on: align 8 on a 4-aligned heap. */
static void align_eight_is_the_failing_case(void)
{
    void *p;

    printf("-- align 8 (the case that aborted the first hardware run) --\n");
    pjs_test_heap_reset();

    p = pjs_host_alloc(16, 8u);
    check(p != NULL, "alloc(size=16, align=8) succeeded");
    check(p != NULL && aligned_to(p, 8u), "returned pointer is 8-byte aligned");
    if (p != NULL) {
        double *d = (double *)p;
        *d = 3.5;
        check(*d == 3.5, "an f64 stored through it round-trips");
        pjs_host_free(p, 8u);
    }
    check(pjs_test_heap_bad_frees() == 0, "free with align=8 returned the original block");
}

/* Adjacent over-aligned blocks, each filled to the brim. If the padding were
 * short by even one byte, filling block N would clobber block N+1's header and
 * the frees below would report a mismatch. */
static void adjacent_over_aligned_blocks(void)
{
    enum { N = 32, SZ = 100 };
    void *p[N];
    int i;
    int intact = 1;

    printf("-- adjacent over-aligned blocks, fully written --\n");
    pjs_test_heap_reset();

    for (i = 0; i < N; i++) {
        p[i] = pjs_host_alloc(SZ, 64u);
        if (p[i] == NULL) {
            printf("  FAIL  allocation %d returned NULL\n", i);
            failures++;
            checks++;
            return;
        }
        memset(p[i], 0x10 + i, SZ);
    }

    for (i = 0; i < N; i++) {
        unsigned char *b = (unsigned char *)p[i];
        int j;
        if (!aligned_to(p[i], 64u)) {
            intact = 0;
        }
        for (j = 0; j < SZ; j++) {
            if (b[j] != (unsigned char)(0x10 + i)) {
                intact = 0;
                break;
            }
        }
    }
    check(intact, "all 32 blocks stayed 64-aligned with their contents intact");

    /* Free in reverse so a corrupted header cannot be masked by ordering. */
    for (i = N - 1; i >= 0; i--) {
        pjs_host_free(p[i], 64u);
    }
    check(pjs_test_heap_bad_frees() == 0, "all 32 frees matched their original blocks");
    check(pjs_test_heap_live_blocks() == 0, "no blocks left live");
}

/* Degenerate inputs must fail safely, not corrupt anything. */
static void degenerate_inputs(void)
{
    void *z;

    printf("-- degenerate inputs --\n");
    pjs_test_heap_reset();

    z = pjs_host_alloc(0, 4u);
    check(z != NULL, "size 0 still returns a usable pointer");
    if (z != NULL) {
        pjs_host_free(z, 4u);
    }

    pjs_host_free(NULL, 4u);
    pjs_host_free(NULL, 64u);
    check(pjs_test_heap_bad_frees() == 0, "freeing NULL is a no-op");

    /* Sub-word alignments are powers of two and are already satisfied by the
     * heap, so they take the fast path and come back 4-aligned. */
    {
        void *a1 = pjs_host_alloc(16, 1u);
        void *a2 = pjs_host_alloc(16, 2u);

        check(a1 != NULL && aligned_to(a1, 4u) &&
              a2 != NULL && aligned_to(a2, 4u),
              "align 1/2 are honoured by the heap's own 4-byte alignment");
        pjs_host_free(a1, 1u);
        pjs_host_free(a2, 2u);
    }

    /* A non-power-of-two cannot be satisfied by the mask arithmetic, so it must
     * be refused rather than silently rounded to something else. */
    check(pjs_host_alloc(16, 3u) == NULL, "align 3 (non-power-of-two) is refused");
    check(pjs_host_alloc(16, 6u) == NULL, "align 6 (non-power-of-two) is refused");
    check(pjs_host_alloc(16, 24u) == NULL, "align 24 (non-power-of-two) is refused");
    check(pjs_host_alloc(16, 0u) == NULL, "align 0 is refused");
    check(pjs_test_heap_bad_frees() == 0, "the refusals did not disturb the heap");
}

/* Gate 1A: every PocketJS allocation must come from the PSRAM_SW memheap.
 *
 * Checked here rather than only on the board because it is a property of *which
 * entry point the allocator calls*, not of the hardware. pjs_mem_test asks where
 * the pointers landed; this asks which heap was asked. Neither alone is enough:
 * a pointer can be inside the right region while having been obtained from the
 * wrong heap if the two happen to be adjacent.
 *
 * This is the regression guard for the backend swap. If someone puts rt_malloc
 * back, `allocs_via_default` goes non-zero and this fails.
 */
static void region_contract(void)
{
    void *p4;
    void *p64;

    printf("-- Gate 1A region contract (PSRAM_SW only, never CMA / system heap) --\n");
    pjs_test_heap_reset();

    p4 = pjs_host_alloc(37, 4u);    /* the fast path: one heap call */
    p64 = pjs_host_alloc(37, 64u);  /* the over-aligned path: also one heap call */
    check(p4 != NULL && p64 != NULL,
          "both the fast path and the over-aligned path allocated");

    check(pjs_test_allocs_via_psram_sw() == 2u,
          "both allocations went through MEM_PSRAM_SW");
    check(pjs_test_allocs_via_other_region() == 0u,
          "nothing went to MEM_CMA or any other named region");
    check(pjs_test_allocs_via_default() == 0u,
          "nothing fell back to the system heap");

    pjs_host_free(p4, 4u);
    pjs_host_free(p64, 64u);
    check(pjs_test_frees_via_psram_sw() == 2u, "both frees went back to MEM_PSRAM_SW");
    check(pjs_test_frees_via_other_region() == 0u, "no free was directed elsewhere");
    check(pjs_test_heap_bad_frees() == 0, "both frees matched their original blocks");
    check(pjs_test_heap_live_blocks() == 0, "no blocks left live");
}

/* The region check itself.
 *
 * A check that can only be taken on faith is not a check. On the board the
 * PSRAM_SW window is real and this is the path that catches a mis-partitioned
 * region; here the window is a variable, so narrowing it is the only way to see
 * that path run at all.
 *
 * Also confirms the refusal does not leak: a block that was allocated and then
 * rejected for being outside the region must go back to the heap.
 */
static void region_window_is_enforced(void)
{
    pjs_heap_stats_t st;
    void *p;

    printf("-- region window is enforced --\n");
    pjs_test_heap_reset();
    pjs_host_alloc_stats_reset();

    /* A window the stub heap cannot possibly hand out a block inside. */
    pjs_host_test_region_lo = (uintptr_t)0x10000000u;
    pjs_host_test_region_hi = (uintptr_t)0x10000100u;

    p = pjs_host_alloc(16, 4u);
    check(p == NULL, "an allocation outside the region is refused");
    check(pjs_test_heap_live_blocks() == 0,
          "the refused block went back to the heap, not leaked");

    pjs_host_alloc_stats(&st);
    check(st.out_of_region == 1u, "the refusal is counted as out-of-region");
    check(st.allocs == 0u, "a refused allocation is not counted as a success");

    /* Restore, and confirm the allocator is not left stuck in a bad state. */
    pjs_host_test_region_lo = 0;
    pjs_host_test_region_hi = ~(uintptr_t)0;
    p = pjs_host_alloc(16, 4u);
    check(p != NULL, "with the window restored, allocation succeeds again");
    pjs_host_free(p, 4u);
    check(pjs_test_heap_live_blocks() == 0, "no blocks left live at the end");
}

int main(void)
{
    printf("PocketJS D13x host allocator test (stubbed RT-Thread heap, RT_ALIGN_SIZE=%d)\n\n",
           RT_ALIGN_SIZE);

    guard_stub_reproduces_board();
    printf("\n");
    sweep_size_and_align();
    printf("\n");
    align_eight_is_the_failing_case();
    printf("\n");
    adjacent_over_aligned_blocks();
    printf("\n");
    degenerate_inputs();
    printf("\n");
    region_contract();
    printf("\n");
    region_window_is_enforced();

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    if (failures == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL\n");
    return 1;
}
