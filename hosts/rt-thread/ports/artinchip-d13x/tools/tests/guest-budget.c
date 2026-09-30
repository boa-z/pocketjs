/* Deterministic clock tests against the real guest/QuickJS implementation. */
#include "pocketjs/guest.h"
#include "pocketjs_port.h"
#include <string.h>

void pjs_test_clock(int enabled, uint32_t step);
uint32_t pjs_test_clock_ticks(void);

int pjs_budget_selftest(void) {
  pocketjs_guest_t *guest = NULL;
  pocketjs_guest_config_t config;
  pocketjs_guest_frame_t frame = {.struct_size = sizeof(frame)};
  pjs_heap_stats_t before, after;
  unsigned checks = 0;
  int result = -1;
  const char *finite =
      "globalThis.frame=()=>{for(;;){}};for(let i=0;i<10000;i++){}";
  const char *loop = "for(;;){}";
#define CHECK(name, expr)                                                      \
  do {                                                                         \
    if (!(expr)) {                                                             \
      rt_kprintf("[pjs-budget] %s FAIL\n", name);                              \
      goto done;                                                               \
    }                                                                          \
    ++checks;                                                                  \
    rt_kprintf("[pjs-budget] %s PASS\n", name);                                \
  } while (0)
#define CREATE()                                                               \
  do {                                                                         \
    pjs_test_clock(0, 0);                                                      \
    pocketjs_guest_destroy(guest);                                             \
    guest = NULL;                                                              \
    CHECK("create", pocketjs_guest_create(&config, &guest) == RT_EOK);         \
  } while (0)
  pjs_host_alloc_stats(&before);
  pocketjs_guest_config_defaults(&config);
  config.heap_limit = 512U * 1024U;
  CREATE();
  CHECK("zero timeout rejected",
        pocketjs_guest_eval_with_timeout(guest, finite, strlen(finite),
                                         "budget.js", 0) == -RT_EINVAL);
  CHECK("out-of-range timeout rejected",
        pocketjs_guest_eval_with_timeout(guest, finite, strlen(finite),
                                         "budget.js", 60001) == -RT_EINVAL);
  CHECK("invalid timeout preserves guest",
        pocketjs_guest_eval(guest, "globalThis.frame=()=>{}",
                            strlen("globalThis.frame=()=>{}"),
                            "budget.js") == RT_EOK);
  /* 500 ticks/read: begin at 500, then the one-second deadline at 1500.
   * Check elapsed ticks so a wrong limit cannot pass via the polling cap. */
  pjs_test_clock(1, 500);
  CHECK("default rejects slow startup",
        pocketjs_guest_eval(guest, finite, strlen(finite), "budget.js") !=
                RT_EOK &&
            pjs_test_clock_ticks() == 1500U);
  CHECK("exhausted guest stays rejected",
        pocketjs_guest_eval_with_timeout(guest, finite, strlen(finite),
                                         "budget.js", 30000) == -RT_ETIMEOUT);
  CREATE();
  pjs_test_clock(1, 500);
  CHECK("bounded startup accepts same work",
        pocketjs_guest_eval_with_timeout(guest, finite, strlen(finite),
                                         "budget.js", 30000) == RT_EOK &&
            pjs_test_clock_ticks() > 1500U);
  pjs_test_clock(1, 500);
  CHECK("frame restores one second limit",
        pocketjs_guest_frame(guest, &frame) != RT_EOK &&
            pjs_test_clock_ticks() == 1500U);
  CREATE();
  pjs_test_clock(1, 500);
  CHECK("second startup",
        pocketjs_guest_eval_with_timeout(guest, finite, strlen(finite),
                                         "budget.js", 30000) == RT_EOK);
  pjs_test_clock(1, 500);
  CHECK("eval restores one second limit",
        pocketjs_guest_eval(guest, finite, strlen(finite), "budget.js") !=
                RT_EOK &&
            pjs_test_clock_ticks() == 1500U);
  CREATE();
  pjs_test_clock(1, 0);
  CHECK("poll limit survives frozen clock",
        pocketjs_guest_eval_with_timeout(guest, loop, strlen(loop), "budget.js",
                                         30000) != RT_EOK);
  CREATE();
  pjs_test_clock(1, 30000);
  CHECK("startup deadline enforced",
        pocketjs_guest_eval_with_timeout(guest, loop, strlen(loop), "budget.js",
                                         30000) != RT_EOK &&
            pjs_test_clock_ticks() == 60000U);
  CREATE();
  pjs_test_clock(1, 0);
  pocketjs_guest_interrupt(guest);
  CHECK("startup cancellation enforced",
        pocketjs_guest_eval_with_timeout(guest, loop, strlen(loop), "budget.js",
                                         30000) != RT_EOK);
  result = 0;
done:
  pjs_test_clock(0, 0);
  pocketjs_guest_destroy(guest);
  pjs_host_alloc_stats(&after);
  if (after.allocs - after.frees != before.allocs - before.frees ||
      after.fails != before.fails)
    result = -1;
  rt_kprintf("[pjs-budget] checks=%u live=%u RESULT %s\n", checks,
             after.allocs - after.frees, result ? "FAIL" : "PASS");
  return result;
}
