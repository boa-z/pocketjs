/*
 * PocketJS ArtInChip D13x port - Gate 1A firmware entry point.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GENERATED FILE - do not edit inside the SDK tree.
 * Source of truth: hosts/rt-thread/ports/artinchip-d13x/sdk/overlay/
 * Regenerate with: python tools/apply-sdk.py
 *
 * Still deliberately tiny. Gate 1A is Retained UI Core work, and it stops at the
 * allocator: no UI Core, no QuickJS, no framebuffer, no GE. `main` announces what
 * is running and then hands off to the probes in application/rt-thread/pocketjs-smoke/third_party/pocketjs.
 *
 * Two probes run once automatically, in this order, so the evidence reaches the
 * console without an operator at the MSH prompt - on bring-up hardware there is
 * often no interactive terminal:
 *
 *   pjs_abi       Gate 0 regression. Now allocates through the new PSRAM_SW
 *                 backend, so it is a real check that Gate 1A did not break
 *                 what Gate 0 validated.
 *   pjs_mem_test  Gate 1A. Alignment, Box/Vec/String, region, leak.
 *
 * Both re-runnable on demand, and `pjs_mem` reports the heap map at any time.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdint.h>

#ifdef RT_USING_ULOG
#include <ulog.h>
#endif

#include <rtconfig.h>

/* Build identity, written next to this file by tools/apply-sdk.py. The guard
 * keeps the SDK branch compilable on its own if the header is absent.
 *
 * Deliberately NOT __DATE__/__TIME__: those are per-translation-unit compile
 * times, so an unchanged main.c keeps a stale stamp while the rest of the
 * firmware is rebuilt. The first hardware re-run printed the *previous* build's
 * date for exactly that reason and nearly made the evidence ambiguous. A
 * revision names the tree that was actually built. */
#if defined(__has_include)
#  if __has_include("pocketjs_build.h")
#    include "pocketjs_build.h"
#  endif
#endif
#ifndef PJS_BUILD_REV
#define PJS_BUILD_REV "unknown (apply-sdk.py not run)"
#endif

/* bsp/artinchip/sys/d13x/ram_param.c. Declared here rather than by including
 * <ram_param.h>: that header lives under the chip directory, and this file is
 * built from an application package whose include path is not guaranteed to
 * reach it. `u32` and `uint32_t` are both `unsigned int` on this target. */
extern uint32_t aic_get_ram_size(void);

#if defined(LPKG_USING_POCKETJS)
/* Implemented in application/rt-thread/pocketjs-smoke/third_party/pocketjs/src/. */
extern int pjs_abi_run(void);
extern int pjs_mem_report(void);
extern int pjs_mem_test(uint32_t iters);

/* Rounds for the automatic Gate 1A run. 1000 is the figure the gate names;
 * `pjs_mem_test <n>` overrides it at the prompt. */
#define PJS_AUTORUN_STRESS_ROUNDS 1000u
#endif

int main(void)
{
#ifdef ULOG_USING_FILTER
    ulog_global_filter_lvl_set(ULOG_OUTPUT_LVL);
#endif

    rt_kprintf("\n");
    rt_kprintf("PocketJS D13x port - Gate 1A firmware (Retained UI Core: allocator)\n");
    rt_kprintf("  board    : d50t-2-lite (D133ECS, Xuantie E907FDP)\n");
    rt_kprintf("  abi      : RV32IMAFDC / ILP32D hard-float\n");
    rt_kprintf("  rev      : %s\n", PJS_BUILD_REV);

    /*
     * The efuse PSRAM size, printed before anything allocates.
     *
     * aic_memheap_init() (target/d13x/d50t-2-lite/board.c:85) grows the PSRAM_SW
     * heap at boot by (aic_get_ram_size() - AIC_PSRAM_SIZE) and then asserts
     * end > begin. The PSRAM size table's fallback entry reports 0, so a fuse
     * that does not match any entry would move the heap end below its start and
     * trip that assert - before main() runs. Gate 0 never exercised that path
     * because MEM_PSRAM_SW was not an enumerator then; Gate 1A does.
     *
     * Reaching this line at all therefore already says the assert passed. This
     * print says *why*, and turns a silent boot loop into a number.
     */
    rt_kprintf("  psram    : efuse=%u MiB, linked=%u MiB\n",
               (unsigned)(aic_get_ram_size() / 1024u / 1024u),
               (unsigned)(AIC_PSRAM_SIZE / 1024u / 1024u));

#if defined(LPKG_USING_POCKETJS)
    rt_kprintf("  runtime  : application/rt-thread/pocketjs-smoke/third_party/pocketjs\n");
    rt_kprintf("  commands : pjs_abi, pjs_mem, pjs_mem_test [rounds], pjs_abi_panic\n");

  #if defined(LPKG_POCKETJS_AUTORUN)
    /* Gate 0 first: if the backend swap broke the ABI probe, that is the more
     * informative failure and it should be the one at the top of the log.
     *
     * Neither probe is fatal on FAIL. A failing gate must leave the board alive
     * and reachable so the log can be read and the command re-run, rather than
     * turning a failed gate into a boot loop. */
    pjs_abi_run();
    pjs_mem_test(PJS_AUTORUN_STRESS_ROUNDS);
  #else
    rt_kprintf("  probes   : autorun disabled; type `pjs_abi` then `pjs_mem_test`\n");
  #endif
#else
    rt_kprintf("  runtime  : NOT BUILT (enable LPKG_USING_POCKETJS)\n");
#endif

    return 0;
}
