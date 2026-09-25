/*
 * RT-Thread host-facing wrapper over the retained UI core.
 *
 * The ABI types in `pocketjs/ui_types.h` and the raw entry points in
 * `pocketjs/native_ui.h` are generated from `contracts/spec/idf-native.ts` by
 * `tools/esp-idf-contracts.ts`, which emits the ESP-IDF host's copies from the
 * same spec. Do not edit either by hand.
 *
 * This header is the part that is *not* generated: it is the RT-Thread-flavoured
 * surface. It differs from the ESP-IDF one in exactly one respect - errors are
 * `rt_err_t` (`RT_EOK`, `-RT_EINVAL`, `-RT_ENOMEM`) rather than `esp_err_t`.
 * The function set and the ownership rules are the same, because they are the
 * contract.
 *
 * One caller-selected thread owns each instance. Pointers in frame, texture and
 * font views stay valid until the next mutation or tick on that core;
 * `pocketjs_ui_core_frame_validate()` is what enforces that.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rtthread.h>

#include "pocketjs/ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POCKETJS_UI_CORE_ABI_VERSION 1U
#define POCKETJS_UI_MAX_TOUCHES 8U

typedef struct pocketjs_ui_core pocketjs_ui_core_t;

typedef enum {
  POCKETJS_UI_ASSET_STYLES = 1,
  POCKETJS_UI_ASSET_FONT = 2,
  POCKETJS_UI_ASSET_IMAGE = 3,
  POCKETJS_UI_ASSET_SPRITE = 4,
} pocketjs_ui_asset_kind_t;

/** Atomic asset batch. On any returned error core and handles are unchanged.
 * Outputs one handle per input (-1 for styles/fonts). Rust OOM is fatal. */
rt_err_t pocketjs_ui_core_load_assets(pocketjs_ui_core_t *core,
                                      const pocketjs_ui_asset_t *assets,
                                      size_t count, int32_t *handles);

void pocketjs_ui_core_config_defaults(pocketjs_ui_core_config_t *config);
rt_err_t pocketjs_ui_core_create(const pocketjs_ui_core_config_t *config,
                                 pocketjs_ui_core_t **out_core);
rt_err_t pocketjs_ui_core_get_config(const pocketjs_ui_core_t *core,
                                     pocketjs_ui_core_config_t *out_config);
void pocketjs_ui_core_destroy(pocketjs_ui_core_t *core);

int32_t pocketjs_ui_core_create_node(pocketjs_ui_core_t *core,
                                     uint32_t node_type);
void pocketjs_ui_core_destroy_node(pocketjs_ui_core_t *core, int32_t id);
void pocketjs_ui_core_insert_before(pocketjs_ui_core_t *core, int32_t parent,
                                    int32_t child, int32_t anchor);
void pocketjs_ui_core_remove_child(pocketjs_ui_core_t *core, int32_t parent,
                                   int32_t child);
void pocketjs_ui_core_set_style(pocketjs_ui_core_t *core, int32_t id,
                                int32_t style_id);
void pocketjs_ui_core_set_prop(pocketjs_ui_core_t *core, int32_t id,
                               uint32_t prop, double value);
rt_err_t pocketjs_ui_core_set_text(pocketjs_ui_core_t *core, int32_t id,
                                   const char *text, size_t size);
rt_err_t pocketjs_ui_core_replace_text(pocketjs_ui_core_t *core, int32_t id,
                                       const char *text, size_t size);
int32_t pocketjs_ui_core_animate(pocketjs_ui_core_t *core, int32_t id,
                                 uint32_t prop, double to, uint32_t duration_ms,
                                 uint32_t easing, uint32_t delay_ms);
void pocketjs_ui_core_cancel_animation(pocketjs_ui_core_t *core,
                                       int32_t animation_id);
void pocketjs_ui_core_set_focus(pocketjs_ui_core_t *core, int32_t id);
void pocketjs_ui_core_set_active(pocketjs_ui_core_t *core, int32_t id,
                                 bool active);
int32_t pocketjs_ui_core_hit_test(pocketjs_ui_core_t *core, float x, float y);
int32_t pocketjs_ui_core_hit_test_bounds(pocketjs_ui_core_t *core, float x,
                                         float y);
void pocketjs_ui_core_set_cursor(pocketjs_ui_core_t *core, int32_t texture,
                                 float hot_x, float hot_y, float width,
                                 float height);
void pocketjs_ui_core_set_cursor_position(pocketjs_ui_core_t *core, float x,
                                          float y);

rt_err_t pocketjs_ui_core_load_styles(pocketjs_ui_core_t *core,
                                      const void *data, size_t size);
rt_err_t pocketjs_ui_core_load_font_atlas(pocketjs_ui_core_t *core,
                                          const void *data, size_t size);
int32_t pocketjs_ui_core_upload_texture(pocketjs_ui_core_t *core,
                                        const void *data, size_t size,
                                        uint32_t width, uint32_t height,
                                        uint32_t psm);
int32_t pocketjs_ui_core_upload_img_entry(pocketjs_ui_core_t *core,
                                          const void *data, size_t size);
void pocketjs_ui_core_free_texture(pocketjs_ui_core_t *core, int32_t texture);
void pocketjs_ui_core_set_image(pocketjs_ui_core_t *core, int32_t id,
                                int32_t texture);
void pocketjs_ui_core_set_sprite(pocketjs_ui_core_t *core, int32_t id,
                                 int32_t atlas, uint32_t frames,
                                 uint32_t columns, uint32_t step);
float pocketjs_ui_core_measure_text(pocketjs_ui_core_t *core, const char *text,
                                    size_t size, uint32_t font_slot);
size_t pocketjs_ui_core_wrap_text(pocketjs_ui_core_t *core, const char *text,
                                  size_t size, uint32_t font_slot,
                                  float max_width, uint32_t *breaks,
                                  size_t capacity);

void pocketjs_ui_core_tick(pocketjs_ui_core_t *core);
rt_err_t pocketjs_ui_core_draw(pocketjs_ui_core_t *core,
                               pocketjs_ui_frame_view_t *out_frame);
size_t pocketjs_ui_core_touch_hits(pocketjs_ui_core_t *core,
                                   const uint32_t *touches, size_t touch_count,
                                   int32_t *out_hits, size_t hit_capacity);
/** Borrowed resource views expire on the next core mutation or tick. */
rt_err_t pocketjs_ui_core_texture(pocketjs_ui_core_t *core, int32_t handle,
                                  pocketjs_ui_texture_view_t *out_texture);
rt_err_t pocketjs_ui_core_font(pocketjs_ui_core_t *core, uint32_t slot,
                               pocketjs_ui_font_view_t *out_font);

/**
 * 0 if `frame` still describes the core's current draw buffer, non-zero if it
 * has been invalidated by any mutation, tick or further draw.
 *
 * This is the borrow contract in executable form. A frame view is a borrow, not
 * a snapshot: the draw words it points at are owned by the core and are replaced
 * on the next mutation. Checking the epoch is how a caller finds out that its
 * view went stale before it reads freed words.
 */
int32_t pocketjs_ui_core_frame_validate(const pocketjs_ui_frame_view_t *frame);

#ifdef __cplusplus
}
#endif
