/*
 * RT-Thread host-facing wrapper over the retained UI core.
 *
 * This file is only the thunk layer. Unlike the ESP-IDF component, it does NOT
 * implement the Rust runtime symbols (`alloc` / `dealloc` / `panic`), because
 * this port already owns that seam and validated it on hardware:
 *
 *   pocketjs_alloc.c  -> pjs_host_alloc / pjs_host_free
 *   pocketjs_host.c   -> pjs_host_abort / pjs_host_log
 *
 * `pocketjs-rtthread-runtime` binds the Rust `GlobalAlloc` and `#[panic_handler]`
 * to those. Adding a second implementation here would create two allocators for
 * one heap, which is the kind of split the Gate 1A work exists to prevent.
 *
 * What is left is argument checking and error-code translation. The int returns
 * from the native ABI are mapped to `rt_err_t` the same way the ESP-IDF
 * component maps them to `esp_err_t`, so a caller reading either host sees the
 * same classification.
 */
#include "pocketjs/ui_core.h"
#include "pocketjs/native_ui.h"

#define PJS_UI_TAG "pjs-ui"

/* Native returns 0 on success, negative on failure. The distinctions that carry
 * meaning to a caller are "no memory" (retryable) and "bad argument" (a bug);
 * everything else collapses to a generic error. */
static rt_err_t pjs_ui_err(int32_t result) {
    if (result == 0) {
        return RT_EOK;
    }
    if (result == -2) {
        return -RT_ENOMEM;
    }
    return -RT_EINVAL;
}

rt_err_t pocketjs_ui_core_load_assets(pocketjs_ui_core_t *core,
                                      const pocketjs_ui_asset_t *assets,
                                      size_t count, int32_t *handles) {
    return pjs_ui_err(pocketjs_native_ui_load_assets(core, assets, count, handles));
}

void pocketjs_ui_core_config_defaults(pocketjs_ui_core_config_t *config) {
    if (config != NULL) {
        *config = (pocketjs_ui_core_config_t){
            .struct_size = sizeof(*config),
            /* Provisional. The D50T-2-Lite panel geometry is a Gate 2
             * (framebuffer) decision; until then this matches the reference
             * default so the core has a sane viewport to lay out against. */
            .logical_width = 480,
            .logical_height = 272,
            .raster_density = 1,
            .tick_hz = 60,
        };
    }
}

rt_err_t pocketjs_ui_core_create(const pocketjs_ui_core_config_t *config,
                                 pocketjs_ui_core_t **out_core) {
    return pjs_ui_err(pocketjs_native_ui_create(config, out_core));
}

rt_err_t pocketjs_ui_core_get_config(const pocketjs_ui_core_t *core,
                                     pocketjs_ui_core_config_t *out_config) {
    return pjs_ui_err(pocketjs_native_ui_get_config(core, out_config));
}

void pocketjs_ui_core_destroy(pocketjs_ui_core_t *core) {
    pocketjs_native_ui_destroy(core);
}

int32_t pocketjs_ui_core_create_node(pocketjs_ui_core_t *core, uint32_t type) {
    return pocketjs_native_ui_create_node(core, type);
}

void pocketjs_ui_core_destroy_node(pocketjs_ui_core_t *core, int32_t id) {
    pocketjs_native_ui_destroy_node(core, id);
}

void pocketjs_ui_core_insert_before(pocketjs_ui_core_t *core, int32_t parent,
                                    int32_t child, int32_t anchor) {
    pocketjs_native_ui_insert_before(core, parent, child, anchor);
}

void pocketjs_ui_core_remove_child(pocketjs_ui_core_t *core, int32_t parent,
                                   int32_t child) {
    pocketjs_native_ui_remove_child(core, parent, child);
}

void pocketjs_ui_core_set_style(pocketjs_ui_core_t *core, int32_t id,
                                int32_t style) {
    pocketjs_native_ui_set_style(core, id, style);
}

void pocketjs_ui_core_set_prop(pocketjs_ui_core_t *core, int32_t id,
                               uint32_t prop, double value) {
    pocketjs_native_ui_set_prop(core, id, prop, value);
}

rt_err_t pocketjs_ui_core_set_text(pocketjs_ui_core_t *core, int32_t id,
                                   const char *text, size_t size) {
    return pjs_ui_err(
        pocketjs_native_ui_set_text(core, id, (const uint8_t *)text, size));
}

rt_err_t pocketjs_ui_core_replace_text(pocketjs_ui_core_t *core, int32_t id,
                                       const char *text, size_t size) {
    return pjs_ui_err(
        pocketjs_native_ui_replace_text(core, id, (const uint8_t *)text, size));
}

int32_t pocketjs_ui_core_animate(pocketjs_ui_core_t *core, int32_t id,
                                 uint32_t prop, double to, uint32_t duration,
                                 uint32_t easing, uint32_t delay) {
    return pocketjs_native_ui_animate(core, id, prop, to, duration, easing, delay);
}

void pocketjs_ui_core_cancel_animation(pocketjs_ui_core_t *core,
                                       int32_t animation) {
    pocketjs_native_ui_cancel_animation(core, animation);
}

void pocketjs_ui_core_set_focus(pocketjs_ui_core_t *core, int32_t id) {
    pocketjs_native_ui_set_focus(core, id);
}

void pocketjs_ui_core_set_active(pocketjs_ui_core_t *core, int32_t id,
                                 bool active) {
    pocketjs_native_ui_set_active(core, id, active ? 1 : 0);
}

int32_t pocketjs_ui_core_hit_test(pocketjs_ui_core_t *core, float x, float y) {
    return pocketjs_native_ui_hit_test(core, x, y);
}

int32_t pocketjs_ui_core_hit_test_bounds(pocketjs_ui_core_t *core, float x,
                                         float y) {
    return pocketjs_native_ui_hit_test_bounds(core, x, y);
}

void pocketjs_ui_core_set_cursor(pocketjs_ui_core_t *core, int32_t texture,
                                 float hot_x, float hot_y, float width,
                                 float height) {
    pocketjs_native_ui_set_cursor(core, texture, hot_x, hot_y, width, height);
}

void pocketjs_ui_core_set_cursor_position(pocketjs_ui_core_t *core, float x,
                                          float y) {
    pocketjs_native_ui_set_cursor_position(core, x, y);
}

rt_err_t pocketjs_ui_core_load_styles(pocketjs_ui_core_t *core,
                                      const void *data, size_t size) {
    return pjs_ui_err(pocketjs_native_ui_load_styles(core, data, size));
}

rt_err_t pocketjs_ui_core_load_font_atlas(pocketjs_ui_core_t *core,
                                          const void *data, size_t size) {
    return pjs_ui_err(pocketjs_native_ui_load_font(core, data, size));
}

int32_t pocketjs_ui_core_upload_texture(pocketjs_ui_core_t *core,
                                        const void *data, size_t size,
                                        uint32_t width, uint32_t height,
                                        uint32_t psm) {
    return pocketjs_native_ui_upload_texture(core, data, size, width, height, psm);
}

int32_t pocketjs_ui_core_upload_img_entry(pocketjs_ui_core_t *core,
                                          const void *data, size_t size) {
    return pocketjs_native_ui_upload_img_entry(core, data, size);
}

void pocketjs_ui_core_free_texture(pocketjs_ui_core_t *core, int32_t texture) {
    pocketjs_native_ui_free_texture(core, texture);
}

void pocketjs_ui_core_set_image(pocketjs_ui_core_t *core, int32_t id,
                                int32_t texture) {
    pocketjs_native_ui_set_image(core, id, texture);
}

void pocketjs_ui_core_set_sprite(pocketjs_ui_core_t *core, int32_t id,
                                 int32_t atlas, uint32_t frames,
                                 uint32_t columns, uint32_t step) {
    pocketjs_native_ui_set_sprite(core, id, atlas, frames, columns, step);
}

float pocketjs_ui_core_measure_text(pocketjs_ui_core_t *core, const char *text,
                                    size_t size, uint32_t slot) {
    return pocketjs_native_ui_measure_text(core, (const uint8_t *)text, size, slot);
}

size_t pocketjs_ui_core_wrap_text(pocketjs_ui_core_t *core, const char *text,
                                  size_t size, uint32_t slot, float max_width,
                                  uint32_t *breaks, size_t capacity) {
    return pocketjs_native_ui_wrap_text(core, (const uint8_t *)text, size, slot,
                                        max_width, breaks, capacity);
}

void pocketjs_ui_core_tick(pocketjs_ui_core_t *core) {
    pocketjs_native_ui_tick(core);
}

rt_err_t pocketjs_ui_core_draw(pocketjs_ui_core_t *core,
                               pocketjs_ui_frame_view_t *out_frame) {
    return pjs_ui_err(pocketjs_native_ui_draw(core, out_frame));
}

size_t pocketjs_ui_core_touch_hits(pocketjs_ui_core_t *core,
                                   const uint32_t *touches, size_t touch_count,
                                   int32_t *hits, size_t capacity) {
    return pocketjs_native_ui_touch_hits(core, touches, touch_count, hits,
                                         capacity);
}

rt_err_t pocketjs_ui_core_texture(pocketjs_ui_core_t *core, int32_t handle,
                                  pocketjs_ui_texture_view_t *out_texture) {
    /* "not found" is a normal outcome here, not a caller bug, so it does not
     * collapse into -RT_EINVAL with the argument errors. */
    return pocketjs_native_ui_texture(core, handle, out_texture) == 0
               ? RT_EOK
               : -RT_ERROR;
}

rt_err_t pocketjs_ui_core_font(pocketjs_ui_core_t *core, uint32_t slot,
                               pocketjs_ui_font_view_t *out_font) {
    return pocketjs_native_ui_font(core, slot, out_font) == 0 ? RT_EOK
                                                              : -RT_ERROR;
}

int32_t pocketjs_ui_core_frame_validate(const pocketjs_ui_frame_view_t *frame) {
    return pocketjs_native_ui_frame_validate(frame);
}
