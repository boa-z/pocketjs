/* Minimal stand-in for RT-Thread, so src/pocketjs_alloc.c can be compiled and
 * run on the build host.
 *
 * It deliberately reproduces the condition that broke the first hardware run:
 * RT_ALIGN_SIZE == 4, and a heap whose base is 4-but-not-8 aligned, so
 * rt_malloc hands back pointers like 0x01CA766C. If that were not true the
 * test below would prove nothing about the real failure.
 *
 * Not part of the firmware. Never compiled into the SDK.
 */
#ifndef PJS_HOST_TEST_RTTHREAD_H
#define PJS_HOST_TEST_RTTHREAD_H

#include <stddef.h>
#include <stdint.h>

/* CONFIG_RT_ALIGN_SIZE=4 on the D50T-2-Lite build. */
#define RT_ALIGN_SIZE 4
#define RT_NULL ((void *)0)

void *rt_malloc(size_t size);
void rt_free(void *ptr);
int rt_kprintf(const char *fmt, ...);

/* Test-visible bookkeeping. */
void pjs_test_heap_reset(void);
size_t pjs_test_heap_live_blocks(void);
size_t pjs_test_heap_live_bytes(void);
size_t pjs_test_heap_bad_frees(void);
size_t pjs_test_heap_high_water(void);

#endif /* PJS_HOST_TEST_RTTHREAD_H */
