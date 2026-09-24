/*
 * PocketJS ArtInChip D13x port - Gate 0 firmware entry point.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GENERATED FILE - do not edit inside the SDK tree.
 * Source of truth: hosts/rt-thread/ports/artinchip-d13x/sdk/overlay/
 * Regenerate with: python tools/apply-sdk.py
 *
 * Deliberately tiny. Gate 0 is not allowed to contain UI Core, QuickJS, a
 * framebuffer or GE work, so `main` announces what is running and then hands
 * off to the conformance probe in packages/third-party/pocketjs.
 *
 * The probe runs once automatically so the evidence reaches the console
 * without an operator at the MSH prompt - on bring-up hardware there is often
 * no interactive terminal. `pjs_abi` re-runs it on demand.
 */

#include <rtthread.h>
#include <rtdevice.h>

#ifdef RT_USING_ULOG
#include <ulog.h>
#endif

#include <rtconfig.h>

#if defined(LPKG_USING_POCKETJS)
/* Implemented in packages/third-party/pocketjs/src/pocketjs_host.c.
 * Also wired to the `pjs_abi` MSH command. */
extern int pjs_abi_run(void);
#endif

int main(void)
{
#ifdef ULOG_USING_FILTER
    ulog_global_filter_lvl_set(ULOG_OUTPUT_LVL);
#endif

    rt_kprintf("\n");
    rt_kprintf("PocketJS D13x port - Gate 0 firmware\n");
    rt_kprintf("  board    : d50t-2-lite (D133ECS, Xuantie E907FDP)\n");
    rt_kprintf("  abi      : RV32IMAFDC / ILP32D hard-float\n");
    rt_kprintf("  built    : %s %s\n", __DATE__, __TIME__);

#if defined(LPKG_USING_POCKETJS)
    rt_kprintf("  runtime  : packages/third-party/pocketjs\n");
    rt_kprintf("  commands : pjs_abi (re-run probe), pjs_abi_panic (abort path)\n");

  #if defined(LPKG_POCKETJS_AUTORUN)
    /* Automatic first run. A FAIL still leaves the board alive and reachable,
     * so a failing Gate 0 is diagnosable rather than a boot loop. */
    pjs_abi_run();
  #else
    rt_kprintf("  probe    : autorun disabled; type `pjs_abi` to run\n");
  #endif
#else
    rt_kprintf("  runtime  : NOT BUILT (enable LPKG_USING_POCKETJS)\n");
#endif

    return 0;
}
