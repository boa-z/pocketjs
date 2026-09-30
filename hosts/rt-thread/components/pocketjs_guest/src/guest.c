#include "pocketjs/host_memory.h"
#include "pocketjs/guest.h"
#include "pocketjs/guest_quickjs.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_POCKETJS_GUEST_HEAP_LIMIT
#define CONFIG_POCKETJS_GUEST_HEAP_LIMIT (4U * 1024U * 1024U)
#endif

typedef union {
  size_t size;
  max_align_t alignment;
} allocation_header_t;

typedef struct rejection {
  JSValue promise;
  JSValue reason;
  struct rejection *next;
} rejection_t;

typedef struct surface {
  char *name;
  struct surface *next;
} surface_t;

struct pocketjs_guest {
  JSRuntime *runtime;
  JSContext *context;
  JSValue frame;
  size_t heap_limit;
  bool prefer_psram;
  atomic_uint interrupt_epoch;
  unsigned int handled_interrupt_epoch;
  rejection_t *rejections;
  bool rejection_tracking_failed;
  surface_t *surfaces;
  uint32_t frames;
  uint32_t frame_errors;
  uint32_t jobs;
  rt_tick_t turn_start;
  uint32_t interrupts_left;
  bool exhausted;
};

/* Date uses uptime until the application supplies an RTC wall clock. */
int64_t pjs_quickjs_time_us(void) {
  return (int64_t)rt_tick_get() * 1000000 / RT_TICK_PER_SECOND;
}

static void guest_begin_turn(pocketjs_guest_t *guest) {
  JS_UpdateStackTop(guest->runtime);
  guest->turn_start = rt_tick_get();
  guest->interrupts_left = 10000;
}

static void guest_dump_error(JSContext *ctx) {
  JSValue error = JS_GetException(ctx);
  const char *message = JS_ToCString(ctx, error);
  rt_kprintf("[pjs-js] exception: %s\n", message ? message : "<unprintable>");
  if (message) JS_FreeCString(ctx, message);
  else JS_FreeValue(ctx, JS_GetException(ctx));
  JS_FreeValue(ctx, error);
}

static void *guest_malloc(void *opaque, size_t size) {
  (void)opaque;
  if (size == 0U || size > SIZE_MAX - sizeof(allocation_header_t)) {
    return NULL;
  }
  const size_t total = sizeof(allocation_header_t) + size;
  allocation_header_t *header = pjs_malloc(total);
  if (header == NULL) {
    return NULL;
  }
  header->size = size;
  return header + 1;
}

static void *guest_calloc(void *opaque, size_t count, size_t size) {
  if (count != 0U && size > SIZE_MAX / count) {
    return NULL;
  }
  const size_t total = count * size;
  void *memory = guest_malloc(opaque, total);
  if (memory != NULL) {
    memset(memory, 0, total);
  }
  return memory;
}

static void guest_free(void *opaque, void *pointer) {
  (void)opaque;
  if (pointer != NULL) {
    pjs_free(((allocation_header_t *)pointer) - 1);
  }
}

static size_t guest_usable_size(const void *pointer) {
  return pointer == NULL ? 0U
                         : (((const allocation_header_t *)pointer) - 1)->size;
}

static void *guest_realloc(void *opaque, void *pointer, size_t size) {
  if (pointer == NULL) {
    return guest_malloc(opaque, size);
  }
  if (size == 0U) {
    guest_free(opaque, pointer);
    return NULL;
  }
  const size_t previous_size = guest_usable_size(pointer);
  void *next = guest_malloc(opaque, size);
  if (next == NULL) {
    return NULL;
  }
  memcpy(next, pointer, previous_size < size ? previous_size : size);
  guest_free(opaque, pointer);
  return next;
}

static const JSMallocFunctions GUEST_ALLOCATOR = {
    .js_calloc = guest_calloc,
    .js_malloc = guest_malloc,
    .js_free = guest_free,
    .js_realloc = guest_realloc,
    .js_malloc_usable_size = guest_usable_size,
};

static int guest_interrupt(JSRuntime *runtime, void *opaque) {
  (void)runtime;
  pocketjs_guest_t *guest = opaque;
  if (guest == NULL)
    return 0;
  const unsigned int requested =
      atomic_load_explicit(&guest->interrupt_epoch, memory_order_relaxed);
  if (guest->exhausted || requested != guest->handled_interrupt_epoch ||
      !guest->interrupts_left ||
      (rt_tick_t)(rt_tick_get() - guest->turn_start) >= RT_TICK_PER_SECOND) {
    guest->exhausted = true;
    return 1;
  }
  --guest->interrupts_left;
  return 0;
}

static void promise_rejection(JSContext *context, JSValueConst promise,
                              JSValueConst reason, bool handled, void *opaque) {
  pocketjs_guest_t *guest = opaque;
  rejection_t **slot = &guest->rejections;
  while (*slot &&
         JS_VALUE_GET_PTR((*slot)->promise) != JS_VALUE_GET_PTR(promise))
    slot = &(*slot)->next;
  if (handled) {
    if (*slot) {
      rejection_t *entry = *slot;
      *slot = entry->next;
      JS_FreeValue(context, entry->promise);
      JS_FreeValue(context, entry->reason);
      pjs_free(entry);
    }
    return;
  }
  if (*slot)
    return;
  rejection_t *entry = pjs_calloc(1, sizeof(*entry));
  if (!entry) {
    guest->rejection_tracking_failed = true;
    return;
  }
  entry->promise = JS_DupValue(context, promise);
  entry->reason = JS_DupValue(context, reason);
  *slot = entry;
}

static rt_err_t drain_jobs(pocketjs_guest_t *guest) {
  JSContext *context = NULL;
  int result = 0;
  unsigned count = 0;
  while ((result = JS_ExecutePendingJob(guest->runtime, &context)) > 0) {
    guest->jobs++;
    if (++count >= 256 && JS_IsJobPending(guest->runtime)) {
      guest->exhausted = true;
      rt_kprintf("[pjs-js] Promise job budget exhausted; recreate guest\n");
      return -RT_ETIMEOUT;
    }
  }
  if (result < 0) {
    if (context != NULL) {
      guest_dump_error(context);
    }
    return -RT_ERROR;
  }
  bool failed = guest->rejection_tracking_failed;
  guest->rejection_tracking_failed = false;
  size_t pending = 0;
  for (rejection_t *entry = guest->rejections; entry; entry = entry->next)
    ++pending;
  while (pending--) {
    rejection_t *entry = guest->rejections;
    if (!entry)
      break;
    guest->rejections = entry->next;
    failed = true;
    /* Detach before toString can reenter the tracker. Reported promises no
     * longer need a retained reference; a later handled event is ignored. */
    const char *text = JS_ToCString(guest->context, entry->reason);
    rt_kprintf("[pjs-js] Unhandled Promise rejection: %s\n", text ? text : "<value>");
    if (text)
      JS_FreeCString(guest->context, text);
    else
      JS_FreeValue(guest->context, JS_GetException(guest->context));
    JS_FreeValue(guest->context, entry->reason);
    JS_FreeValue(guest->context, entry->promise);
    pjs_free(entry);
  }
  return failed ? -RT_ERROR : RT_EOK;
}

void pocketjs_guest_config_defaults(pocketjs_guest_config_t *config) {
  if (config == NULL) {
    return;
  }
  *config = (pocketjs_guest_config_t){
      .struct_size = sizeof(*config),
      .heap_limit = CONFIG_POCKETJS_GUEST_HEAP_LIMIT,
      .stack_limit = 64U * 1024U,
      .prefer_psram = true,
  };
}

rt_err_t pocketjs_guest_create(const pocketjs_guest_config_t *config,
                                pocketjs_guest_t **out_guest) {
  if (config == NULL || out_guest == NULL ||
      config->struct_size < sizeof(*config) || config->heap_limit == 0U ||
      config->stack_limit == 0U) {
    return -RT_EINVAL;
  }
  *out_guest = NULL;
  pocketjs_guest_t *guest = pjs_calloc(1, sizeof(*guest));
  if (guest == NULL) {
    return -RT_ENOMEM;
  }
  guest->frame = JS_UNDEFINED;
  guest->heap_limit = config->heap_limit;
  guest->prefer_psram = config->prefer_psram;
  atomic_init(&guest->interrupt_epoch, 0U);
  guest->runtime = JS_NewRuntime2(&GUEST_ALLOCATOR, guest);
  if (guest->runtime == NULL) {
    pocketjs_guest_destroy(guest);
    return -RT_ENOMEM;
  }
  JS_SetMemoryLimit(guest->runtime, config->heap_limit);
  JS_SetMaxStackSize(guest->runtime, config->stack_limit);
  JS_SetRuntimeInfo(guest->runtime, "PocketJS RT-Thread guest");
  JS_SetInterruptHandler(guest->runtime, guest_interrupt, guest);
  JS_SetHostPromiseRejectionTracker(guest->runtime, promise_rejection, guest);
  guest_begin_turn(guest);
  guest->context = JS_NewContext(guest->runtime);
  if (guest->context == NULL) {
    pocketjs_guest_destroy(guest);
    return -RT_ENOMEM;
  }
  *out_guest = guest;
  return RT_EOK;
}

rt_err_t
pocketjs_guest_quickjs_install(pocketjs_guest_t *guest,
                               pocketjs_guest_quickjs_install_fn install,
                               void *user_data) {
  if (guest == NULL || guest->context == NULL || install == NULL) {
    return -RT_EINVAL;
  }
  return install(guest->context, user_data);
}

JSContext *pocketjs_guest_quickjs_context(pocketjs_guest_t *guest) {
  return guest != NULL ? guest->context : NULL;
}

rt_err_t
pocketjs_guest_quickjs_install_once(pocketjs_guest_t *guest, const char *name,
                                    pocketjs_guest_quickjs_install_fn install,
                                    void *user_data) {
  if (!guest || !name || !*name || !install)
    return -RT_EINVAL;
  for (surface_t *entry = guest->surfaces; entry; entry = entry->next)
    if (!strcmp(entry->name, name))
      return -RT_ERROR;
  surface_t *entry = pjs_calloc(1, sizeof(*entry));
  if (!entry)
    return -RT_ENOMEM;
  entry->name = pjs_strdup(name);
  if (!entry->name) {
    pjs_free(entry);
    return -RT_ENOMEM;
  }
  entry->next = guest->surfaces;
  guest->surfaces = entry;
  rt_err_t result = pocketjs_guest_quickjs_install(guest, install, user_data);
  if (result != RT_EOK) {
    if (JS_HasException(guest->context))
      JS_FreeValue(guest->context, JS_GetException(guest->context));
    surface_t **slot = &guest->surfaces;
    while (*slot && *slot != entry)
      slot = &(*slot)->next;
    if (*slot)
      *slot = entry->next;
    pjs_free(entry->name);
    pjs_free(entry);
  }
  return result;
}

rt_err_t pocketjs_guest_eval(pocketjs_guest_t *guest, const char *source,
                              size_t source_size, const char *label) {
  if (guest == NULL || guest->context == NULL || source == NULL ||
      source_size == 0U) {
    return -RT_EINVAL;
  }
  if (guest->exhausted) return -RT_ETIMEOUT;
  guest_begin_turn(guest);
  JSValue result =
      JS_Eval(guest->context, source, source_size,
              label != NULL ? label : "<pocket-app>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(result)) {
    guest_dump_error(guest->context);
    JS_FreeValue(guest->context, result);
    return -RT_ERROR;
  }
  JS_FreeValue(guest->context, result);
  JS_FreeValue(guest->context, guest->frame);
  JSValue global = JS_GetGlobalObject(guest->context);
  guest->frame = JS_GetPropertyStr(guest->context, global, "frame");
  JS_FreeValue(guest->context, global);
  if (!JS_IsFunction(guest->context, guest->frame)) {
    return -RT_ENOSYS;
  }
  return drain_jobs(guest);
}

static JSValue make_u32_array(JSContext *context, const uint32_t *values,
                              size_t count) {
  JSValue array = JS_NewArray(context);
  if (JS_IsException(array)) {
    return array;
  }
  for (size_t index = 0; index < count; ++index) {
    if (JS_SetPropertyUint32(context, array, (uint32_t)index,
                             JS_NewUint32(context, values[index])) < 0) {
      JS_FreeValue(context, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

static JSValue make_i32_array(JSContext *context, const int32_t *values,
                              size_t count) {
  JSValue array = JS_NewArray(context);
  if (JS_IsException(array)) {
    return array;
  }
  for (size_t index = 0; index < count; ++index) {
    if (JS_SetPropertyUint32(context, array, (uint32_t)index,
                             JS_NewInt32(context, values[index])) < 0) {
      JS_FreeValue(context, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

rt_err_t pocketjs_guest_frame(pocketjs_guest_t *guest,
                               const pocketjs_guest_frame_t *frame) {
  if (guest == NULL || guest->context == NULL || frame == NULL ||
      frame->struct_size < sizeof(*frame) ||
      frame->touch_count > POCKETJS_GUEST_MAX_TOUCHES ||
      (frame->touch_count != 0U && frame->touches == NULL)) {
    return -RT_EINVAL;
  }
  if (!JS_IsFunction(guest->context, guest->frame)) {
    return -RT_ERROR;
  }
  if (guest->exhausted) return -RT_ETIMEOUT;
  guest_begin_turn(guest);
  JSValue arguments[4] = {
      JS_NewUint32(guest->context, frame->buttons),
      JS_NewUint32(guest->context, frame->analog),
      JS_UNDEFINED,
      JS_UNDEFINED,
  };
  int argument_count = 2;
  if (frame->touch_count != 0U) {
    arguments[2] =
        make_u32_array(guest->context, frame->touches, frame->touch_count);
    if (JS_IsException(arguments[2])) {
      guest_dump_error(guest->context);
      JS_FreeValue(guest->context, arguments[1]);
      JS_FreeValue(guest->context, arguments[0]);
      guest->frame_errors++;
      return -RT_ENOMEM;
    }
    argument_count = 3;
    if (frame->touch_hits != NULL) {
      arguments[3] =
          make_i32_array(guest->context, frame->touch_hits, frame->touch_count);
      if (JS_IsException(arguments[3])) {
        guest_dump_error(guest->context);
        JS_FreeValue(guest->context, arguments[2]);
        JS_FreeValue(guest->context, arguments[1]);
        JS_FreeValue(guest->context, arguments[0]);
        guest->frame_errors++;
        return -RT_ENOMEM;
      }
      argument_count = 4;
    }
  }
  JSValue result = JS_Call(guest->context, guest->frame, JS_UNDEFINED,
                           argument_count, arguments);
  for (int index = argument_count - 1; index >= 0; --index) {
    JS_FreeValue(guest->context, arguments[index]);
  }
  guest->frames++;
  if (JS_IsException(result)) {
    guest_dump_error(guest->context);
    JS_FreeValue(guest->context, result);
    guest->frame_errors++;
    return -RT_ERROR;
  }
  JS_FreeValue(guest->context, result);
  const rt_err_t jobs = drain_jobs(guest);
  if (jobs != RT_EOK) {
    guest->frame_errors++;
  }
  return jobs;
}

void pocketjs_guest_interrupt(pocketjs_guest_t *guest) {
  if (guest != NULL) {
    (void)atomic_fetch_add_explicit(&guest->interrupt_epoch, 1U,
                                    memory_order_relaxed);
  }
}

rt_err_t pocketjs_guest_stats(pocketjs_guest_t *guest,
                               pocketjs_guest_stats_t *out_stats) {
  if (guest == NULL || out_stats == NULL ||
      out_stats->struct_size < sizeof(*out_stats)) {
    return -RT_EINVAL;
  }
  JSMemoryUsage usage = {0};
  JS_ComputeMemoryUsage(guest->runtime, &usage);
  const size_t output_size = out_stats->struct_size;
  *out_stats = (pocketjs_guest_stats_t){
      .struct_size = output_size,
      .frames = guest->frames,
      .frame_errors = guest->frame_errors,
      .jobs = guest->jobs,
      .heap_used = usage.malloc_size,
      .heap_limit = guest->heap_limit,
  };
  return RT_EOK;
}

void pocketjs_guest_destroy(pocketjs_guest_t *guest) {
  if (guest == NULL) {
    return;
  }
  if (guest->context != NULL) {
    while (guest->rejections) {
      rejection_t *entry = guest->rejections;
      guest->rejections = entry->next;
      JS_FreeValue(guest->context, entry->promise);
      JS_FreeValue(guest->context, entry->reason);
      pjs_free(entry);
    }
    JS_FreeValue(guest->context, guest->frame);
    JS_FreeContext(guest->context);
  }
  if (guest->runtime != NULL) {
    JS_FreeRuntime(guest->runtime);
  }
  while (guest->surfaces) {
    surface_t *entry = guest->surfaces;
    guest->surfaces = entry->next;
    pjs_free(entry->name);
    pjs_free(entry);
  }
  pjs_free(guest);
}
