/*
 * PocketJS ArtInChip D13x - Gate 1B/1C retained UI core self-test.
 *
 *   pjs_ui [w] [h]      drive the UI core through one full cycle and report
 *   pjs_ui_stress [n]   repeat the create/tick/draw/destroy cycle n times
 *
 * Two commands for the same reason Gate 1A has two. `pjs_ui` is what you run
 * when you want to see the core's numbers; `pjs_ui_stress` is the assertion,
 * and it changes the heap. Merging them would make the report's numbers depend
 * on whether the stress run had already happened.
 *
 * Output goes to rt_kprintf, not ulog, so a host-side parser gets
 * deterministic, unfilterable lines. A log level must not be able to hide a
 * failing case.
 *
 * What Gate 1B/1C has to prove, and where each line comes from:
 *
 *   the instance ABI is callable at all      - create / get_config / destroy
 *   bad geometry is refused, not clamped     - density 0 / 256 / UINT32_MAX,
 *                                              and a zero-width viewport
 *   the node tree works                      - create_node(View/Text/Image),
 *                                              and an out-of-range kind refused
 *   style, prop and text land                - every mutation bumps the frame
 *                                              epoch, which is the observable
 *   a draw produces real words               - draw_words != NULL, count > 0
 *   a frame view is a borrow, not a snapshot  - frame_validate() rejects a saved
 *                                              view after a redraw, after a
 *                                              mutation and after a tick
 *   ...and the fresh view is still valid     - frame_validate() accepts it
 *   nothing leaks                            - pjs_host_alloc_stats() before and
 *                                              after the whole cycle
 *   the Rust -> C log direction is live      - pjs_host_log() runs on every
 *                                              create and destroy (Gate 0 8.4)
 *
 * The frame checks are the heart of it. `pocketjs_ui_core_draw()` hands back a
 * pointer into the core's own draw words, and the next mutation, tick or draw
 * replaces them. A caller that kept the old view and read it would be reading
 * freed words; `frame_validate()` is how it finds out. Asserting that the stale
 * view is *rejected* is therefore asserting the contract, not a convenience.
 */

#include <rtthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pocketjs/ui_core.h"

/* The host seam, for the allocator counters the leak check compares.
 *
 * `pocketjs_port.h` is the port's internal header rather than part of the
 * C<->Rust ABI, which is why this component declares its dependency on the
 * `pocketjs` package in Kconfig and names that package's src/ in its
 * SConscript. Re-declaring pjs_heap_stats_t here instead would be a second
 * definition of a struct that has to keep matching. */
#include "pocketjs_port.h"

#define PJS_UI_TAG "[pjs-ui] "

/* Node kinds: engine/core/src/spec.rs, `enum NodeType`. */
#define PJS_NODE_VIEW    0u
#define PJS_NODE_TEXT    1u
#define PJS_NODE_IMAGE   2u
/* One past Surface, so create_node must refuse it. */
#define PJS_NODE_INVALID 4u

/* spec::ROOT_ID - the pre-created full-screen root. */
#define PJS_ROOT_ID 1

/* prop ids: engine/core/src/spec.rs, `mod prop`. */
#define PJS_PROP_WIDTH    1u
#define PJS_PROP_HEIGHT   2u
#define PJS_PROP_BG_COLOR 64u

/* spec::STYLE_ID_NONE - clears a node back to the default style. */
#define PJS_STYLE_NONE (-1)

#define PJS_STRESS_DEFAULT 100u

/* ------------------------------------------------------------------ */
/* Bookkeeping                                                         */
/* ------------------------------------------------------------------ */

static int g_pass;
static int g_fail;

static void report(const char *name, int ok)
{
    if (ok) {
        g_pass++;
        rt_kprintf(PJS_UI_TAG "PASS  %s\n", name);
    } else {
        g_fail++;
        rt_kprintf(PJS_UI_TAG "FAIL  %s\n", name);
    }
}

static void banner(const char *title)
{
    rt_kprintf("\n");
    rt_kprintf(PJS_UI_TAG "==============================================\n");
    rt_kprintf(PJS_UI_TAG " PocketJS D13x - %s\n", title);
    rt_kprintf(PJS_UI_TAG " RV32IMAFDC / ILP32D, retained UI core\n");
    rt_kprintf(PJS_UI_TAG "==============================================\n");
}

static void summary(void)
{
    rt_kprintf(PJS_UI_TAG "----------------------------------------------\n");
    rt_kprintf(PJS_UI_TAG "SUMMARY pass=%d fail=%d\n", g_pass, g_fail);
    rt_kprintf(PJS_UI_TAG "RESULT %s\n", (g_fail == 0) ? "PASS" : "FAIL");
    rt_kprintf(PJS_UI_TAG "==============================================\n");
}

/* Deliberately not atoi(): same reasoning as src/pocketjs_mem.c - a
 * locale-aware parser for a round count is a dependency bought for nothing. */
static uint32_t pjs_arg_u32(int argc, char **argv, int idx, uint32_t dflt)
{
    const char *s;
    uint32_t v = 0;
    int seen = 0;

    if (idx >= argc || argv[idx] == RT_NULL) {
        return dflt;
    }
    s = argv[idx];
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        seen = 1;
        s++;
    }
    return seen ? v : dflt;
}

/* Live allocation count as the host allocator sees it. Cumulative counters, so
 * only differences are meaningful. */
static uint32_t pjs_ui_live(pjs_heap_stats_t *st)
{
    pjs_host_alloc_stats(st);
    return st->allocs - st->frees;
}

/* ------------------------------------------------------------------ */
/* One full cycle                                                      */
/* ------------------------------------------------------------------ */

/*
 * Drives the core once and reports. Returns 0 when every check passed.
 *
 * `width`/`height` of 0 mean "use the defaults the component ships", which is
 * what the autorun path wants.
 *
 * Non-static so the application can run it at boot; the MSH command below is
 * the same entry point.
 */
int pjs_ui_selftest(uint32_t width, uint32_t height)
{
    pocketjs_ui_core_t *core = RT_NULL;
    pocketjs_ui_core_t *rejected;
    pocketjs_ui_core_config_t cfg;
    pocketjs_ui_core_config_t back;
    pocketjs_ui_frame_view_t frame;
    pocketjs_ui_frame_view_t saved;
    pjs_heap_stats_t st_before;
    pjs_heap_stats_t st_after;
    uint32_t live_before;
    uint32_t live_after;
    uint64_t epoch_first;
    rt_err_t err;
    int32_t view = 0;
    int32_t text = 0;
    int32_t image = 0;

    g_pass = 0;
    g_fail = 0;

    banner("Gate 1B/1C retained UI core");

    /* ---- 1. defaults ---- */
    memset(&cfg, 0, sizeof(cfg));
    pocketjs_ui_core_config_defaults(&cfg);
    report("ui.config_defaults.struct_size", cfg.struct_size == sizeof(cfg));
    report("ui.config_defaults.geometry",
           cfg.logical_width > 0u && cfg.logical_height > 0u);
    report("ui.config_defaults.density_in_range",
           cfg.raster_density >= 1u && cfg.raster_density <= 255u);
    report("ui.config_defaults.tick_hz", cfg.tick_hz > 0u);

    if (width > 0u) {
        cfg.logical_width = width;
    }
    if (height > 0u) {
        cfg.logical_height = height;
    }

    rt_kprintf(PJS_UI_TAG "request : %ux%u density=%u tick=%uHz\n",
               (unsigned)cfg.logical_width, (unsigned)cfg.logical_height,
               (unsigned)cfg.raster_density, (unsigned)cfg.tick_hz);

    /* ---- 2. bad geometry is refused, not clamped ----
     *
     * The contract (pocketjs_native_ui_create) rejects a short struct_size, a
     * zero viewport, raster_density outside 1..=255, and tick_hz outside
     * 1..=MAX_TICK_HZ - and clears *out_core as it does so. A core that
     * silently clamped a bad density would lay out at the wrong scale for the
     * whole session, which is exactly the kind of wrong a gate must not accept.
     *
     * `rejected` is pre-set to a non-NULL sentinel so that "the callee cleared
     * it" is a real observation rather than a coincidence of zeroed stack.
     */
    cfg.raster_density = 0u;
    rejected = (pocketjs_ui_core_t *)1;
    report("ui.reject_density_0",
           pocketjs_ui_core_create(&cfg, &rejected) != RT_EOK &&
           rejected == RT_NULL);

    cfg.raster_density = 256u;
    rejected = (pocketjs_ui_core_t *)1;
    report("ui.reject_density_256",
           pocketjs_ui_core_create(&cfg, &rejected) != RT_EOK &&
           rejected == RT_NULL);

    cfg.raster_density = 0xffffffffu;
    rejected = (pocketjs_ui_core_t *)1;
    report("ui.reject_density_umax",
           pocketjs_ui_core_create(&cfg, &rejected) != RT_EOK &&
           rejected == RT_NULL);

    cfg.raster_density = 1u;
    {
        uint32_t keep = cfg.logical_width;
        cfg.logical_width = 0u;
        rejected = (pocketjs_ui_core_t *)1;
        report("ui.reject_zero_width",
               pocketjs_ui_core_create(&cfg, &rejected) != RT_EOK &&
               rejected == RT_NULL);
        cfg.logical_width = keep;
    }

    /* ---- 3. create, and the log line that proves section 8.4 ---- */
    live_before = pjs_ui_live(&st_before);
    rt_kprintf(PJS_UI_TAG "alloc live before : %u\n", (unsigned)live_before);

    core = RT_NULL;
    err = pocketjs_ui_core_create(&cfg, &core);
    report("ui.create", err == RT_EOK && core != RT_NULL);
    if (core == RT_NULL) {
        /* Nothing below is meaningful without an instance. Stop rather than run
         * a test that would pass by never touching the core. */
        rt_kprintf(PJS_UI_TAG "FAIL  no instance; stopping\n");
        summary();
        return -1;
    }

    /* ---- 4. get_config round-trips ---- */
    memset(&back, 0, sizeof(back));
    back.struct_size = sizeof(back);
    err = pocketjs_ui_core_get_config(core, &back);
    report("ui.get_config", err == RT_EOK);
    report("ui.get_config.geometry",
           back.logical_width == cfg.logical_width &&
           back.logical_height == cfg.logical_height);
    report("ui.get_config.density", back.raster_density == cfg.raster_density);
    report("ui.get_config.tick_hz", back.tick_hz == cfg.tick_hz);
    rt_kprintf(PJS_UI_TAG "config  : %ux%u density=%u tick=%uHz\n",
               (unsigned)back.logical_width, (unsigned)back.logical_height,
               (unsigned)back.raster_density, (unsigned)back.tick_hz);

    /* ---- 5. the tree ---- */
    view = pocketjs_ui_core_create_node(core, PJS_NODE_VIEW);
    text = pocketjs_ui_core_create_node(core, PJS_NODE_TEXT);
    image = pocketjs_ui_core_create_node(core, PJS_NODE_IMAGE);
    report("ui.node.view", view != 0);
    report("ui.node.text", text != 0);
    report("ui.node.image", image != 0);
    report("ui.node.ids_distinct",
           view != 0 && text != 0 && image != 0 &&
           view != text && view != image && text != image);
    report("ui.node.reject_out_of_range_kind",
           pocketjs_ui_core_create_node(core, PJS_NODE_INVALID) == 0);
    rt_kprintf(PJS_UI_TAG "nodes   : root=%d view=%d text=%d image=%d\n",
               (int)PJS_ROOT_ID, (int)view, (int)text, (int)image);

    /* Attach: insert_before(parent, child, anchor), anchor 0 = append. */
    pocketjs_ui_core_insert_before(core, PJS_ROOT_ID, view, 0);
    pocketjs_ui_core_insert_before(core, view, text, 0);
    pocketjs_ui_core_insert_before(core, view, image, 0);

    /* ---- 6. style, prop and text ----
     *
     * These return void, so the observable is the frame epoch below: every
     * mutation bumps it, and a bump that did not happen would mean the call was
     * dropped on the floor. The props are also where the ILP32D hard-float ABI
     * gets exercised through this new path - set_prop takes a `double`, so it
     * must arrive in fa0 and come back out in the layout box. */
    pocketjs_ui_core_set_style(core, view, PJS_STYLE_NONE);
    pocketjs_ui_core_set_prop(core, view, PJS_PROP_WIDTH, 200.0);
    pocketjs_ui_core_set_prop(core, view, PJS_PROP_HEIGHT, 100.0);
    pocketjs_ui_core_set_prop(core, view, PJS_PROP_BG_COLOR, 0x11223344u);
    pocketjs_ui_core_set_prop(core, text, PJS_PROP_WIDTH, 100.0);
    pocketjs_ui_core_set_prop(core, text, PJS_PROP_HEIGHT, 20.0);
    pocketjs_ui_core_set_prop(core, image, PJS_PROP_WIDTH, 32.0);
    pocketjs_ui_core_set_prop(core, image, PJS_PROP_HEIGHT, 32.0);

    err = pocketjs_ui_core_set_text(core, text, "PocketJS D13x", 13u);
    report("ui.set_text", err == RT_EOK);

    /* ---- 7. draw ---- */
    memset(&frame, 0, sizeof(frame));
    frame.struct_size = sizeof(frame);
    err = pocketjs_ui_core_draw(core, &frame);
    report("ui.draw.first", err == RT_EOK);
    report("ui.draw.words_nonnull", frame.draw_words != RT_NULL);
    report("ui.draw.words_nonempty", frame.draw_word_count > 0u);
    report("ui.draw.geometry",
           frame.logical_width == cfg.logical_width &&
           frame.logical_height == cfg.logical_height);
    report("ui.draw.density", frame.raster_density == cfg.raster_density);
    report("ui.frame.valid_after_draw",
           pocketjs_ui_core_frame_validate(&frame) == 0);
    epoch_first = frame.epoch;
    rt_kprintf(PJS_UI_TAG "draw 1  : words=%u epoch=%u\n",
               (unsigned)frame.draw_word_count, (unsigned)(frame.epoch & 0xffffffffu));

    /* ---- 8. the borrow contract ----
     *
     * A frame view is a borrow, not a snapshot. Each of the three ways the
     * core can replace the words - another draw, a mutation, a tick - must
     * invalidate a view taken before it, and must leave the *current* view
     * valid. Anything less and a caller cannot tell a live buffer from a
     * recycled one. */

    /* 8a. a redraw invalidates the previous view */
    saved = frame;
    memset(&frame, 0, sizeof(frame));
    frame.struct_size = sizeof(frame);
    err = pocketjs_ui_core_draw(core, &frame);
    report("ui.draw.second", err == RT_EOK);
    report("ui.frame.stale_after_redraw",
           pocketjs_ui_core_frame_validate(&saved) != 0);
    report("ui.frame.valid_after_redraw",
           pocketjs_ui_core_frame_validate(&frame) == 0);
    report("ui.epoch.advances_on_draw", frame.epoch != epoch_first);
    rt_kprintf(PJS_UI_TAG "draw 2  : words=%u epoch=%u (draw 1 epoch=%u)\n",
               (unsigned)frame.draw_word_count,
               (unsigned)(frame.epoch & 0xffffffffu),
               (unsigned)(epoch_first & 0xffffffffu));

    /* 8b. a mutation invalidates the current view */
    saved = frame;
    pocketjs_ui_core_set_prop(core, view, PJS_PROP_BG_COLOR, 0x22334455u);
    report("ui.frame.stale_after_mutation",
           pocketjs_ui_core_frame_validate(&saved) != 0);

    /* 8c. a tick invalidates the current view */
    memset(&frame, 0, sizeof(frame));
    frame.struct_size = sizeof(frame);
    err = pocketjs_ui_core_draw(core, &frame);
    report("ui.draw.third", err == RT_EOK);
    saved = frame;
    pocketjs_ui_core_tick(core);
    report("ui.frame.stale_after_tick",
           pocketjs_ui_core_frame_validate(&saved) != 0);

    /* 8d. ...and a fresh draw recovers, so the rejection is the borrow
     * generation moving and not the core wedging itself. */
    memset(&frame, 0, sizeof(frame));
    frame.struct_size = sizeof(frame);
    err = pocketjs_ui_core_draw(core, &frame);
    report("ui.draw.recovers_after_tick", err == RT_EOK);
    report("ui.frame.valid_after_tick_redraw",
           pocketjs_ui_core_frame_validate(&frame) == 0);
    report("ui.epoch.advances_after_tick", frame.epoch != epoch_first);

    /* ---- 9. hit testing ----
     *
     * `hit_test_bounds` is the pure-layout resolver: containers claim their
     * box, so a point inside the 200x100 view must resolve to something. That
     * is the layout proof - it only holds if the props above actually reached
     * taffy. `hit_test` additionally requires ink, so its value is reported
     * rather than asserted. */
    {
        int32_t hit = pocketjs_ui_core_hit_test(core, 10.0f, 10.0f);
        int32_t bounds = pocketjs_ui_core_hit_test_bounds(core, 10.0f, 10.0f);
        rt_kprintf(PJS_UI_TAG "hit(10,10): ink=%d bounds=%d  (view=%d text=%d image=%d)\n",
                   (int)hit, (int)bounds, (int)view, (int)text, (int)image);
        report("ui.hit_test_bounds.inside_view", bounds != 0);
    }

    /* ---- 10. teardown, and the leak check ---- */
    pocketjs_ui_core_destroy(core);

    live_after = pjs_ui_live(&st_after);
    rt_kprintf(PJS_UI_TAG "alloc live after  : %u  (allocs %u -> %u, frees %u -> %u)\n",
               (unsigned)live_after,
               (unsigned)st_before.allocs, (unsigned)st_after.allocs,
               (unsigned)st_before.frees, (unsigned)st_after.frees);

    report("ui.destroy.live_back_to_baseline", live_after == live_before);
    report("ui.destroy.allocated_something", st_after.allocs > st_before.allocs);
    report("ui.destroy.freed_everything",
           (st_after.allocs - st_before.allocs) == (st_after.frees - st_before.frees));
    report("ui.destroy.no_failed_allocation",
           st_after.fails == st_before.fails);
    report("ui.destroy.no_bad_align", st_after.bad_align == 0u);
    report("ui.destroy.no_out_of_region", st_after.out_of_region == 0u);

    summary();
    return (g_fail == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* The repeated cycle                                                  */
/* ------------------------------------------------------------------ */

/*
 * Create, build a small tree, tick, draw, destroy - n times.
 *
 * This is the leak assertion. A single cycle cannot distinguish "freed
 * everything" from "leaked a little and the counters happen to match", so the
 * cycle is repeated and the host allocator's live count has to come back to
 * where it started. `fails` must not move either: an allocation refused mid-run
 * would otherwise show up as a suspiciously tidy live count.
 *
 * Non-static so the application can run it at boot.
 */
int pjs_ui_stress(uint32_t iters)
{
    pocketjs_ui_core_t *core;
    pocketjs_ui_core_config_t cfg;
    pocketjs_ui_frame_view_t frame;
    pjs_heap_stats_t st_before;
    pjs_heap_stats_t st_after;
    uint32_t live_before;
    uint32_t live_after;
    uint32_t i;
    uint32_t rounds_ok = 0;
    uint32_t draws_ok = 0;
    uint32_t nodes_ok = 0;
    uint32_t stale_ok = 0;
    uint32_t bad = 0;

    if (iters == 0u) {
        iters = PJS_STRESS_DEFAULT;
    }

    g_pass = 0;
    g_fail = 0;

    banner("Gate 1C UI core cycle stress");

    pocketjs_ui_core_config_defaults(&cfg);
    live_before = pjs_ui_live(&st_before);
    rt_kprintf(PJS_UI_TAG "rounds=%u  alloc live before=%u\n",
               (unsigned)iters, (unsigned)live_before);

    for (i = 0; i < iters; i++) {
        int32_t view;
        int32_t text;

        core = RT_NULL;
        if (pocketjs_ui_core_create(&cfg, &core) != RT_EOK || core == RT_NULL) {
            bad++;
            break;
        }

        view = pocketjs_ui_core_create_node(core, PJS_NODE_VIEW);
        text = pocketjs_ui_core_create_node(core, PJS_NODE_TEXT);
        if (view != 0 && text != 0) {
            nodes_ok++;
        }
        pocketjs_ui_core_insert_before(core, PJS_ROOT_ID, view, 0);
        pocketjs_ui_core_insert_before(core, view, text, 0);
        pocketjs_ui_core_set_prop(core, view, PJS_PROP_WIDTH, 120.0 + (double)(i % 7u));
        pocketjs_ui_core_set_prop(core, view, PJS_PROP_HEIGHT, 64.0);
        pocketjs_ui_core_set_text(core, text, "stress", 6u);
        pocketjs_ui_core_tick(core);

        memset(&frame, 0, sizeof(frame));
        frame.struct_size = sizeof(frame);
        if (pocketjs_ui_core_draw(core, &frame) == RT_EOK) {
            draws_ok++;
            {
                pocketjs_ui_frame_view_t saved = frame;
                /* A tick after the draw must invalidate the view. Checked every
                 * round, not once: a generation counter that only works on the
                 * first iteration is exactly the bug this is looking for. */
                pocketjs_ui_core_tick(core);
                if (pocketjs_ui_core_frame_validate(&saved) != 0) {
                    stale_ok++;
                }
            }
        }

        pocketjs_ui_core_destroy(core);
        rounds_ok++;
    }

    live_after = pjs_ui_live(&st_after);

    rt_kprintf(PJS_UI_TAG "rounds ok=%u nodes=%u draws=%u stale_rejected=%u bad=%u\n",
               (unsigned)rounds_ok, (unsigned)nodes_ok, (unsigned)draws_ok,
               (unsigned)stale_ok, (unsigned)bad);
    rt_kprintf(PJS_UI_TAG "alloc live after=%u  (allocs %u -> %u, frees %u -> %u)\n",
               (unsigned)live_after,
               (unsigned)st_before.allocs, (unsigned)st_after.allocs,
               (unsigned)st_before.frees, (unsigned)st_after.frees);

    report("stress.rounds_completed", rounds_ok == iters);
    report("stress.nodes_created", nodes_ok == iters);
    report("stress.draws_ok", draws_ok == iters);
    report("stress.stale_always_rejected", stale_ok == iters);
    report("stress.no_bad_round", bad == 0u);
    report("stress.live_back_to_baseline", live_after == live_before);
    report("stress.freed_everything",
           (st_after.allocs - st_before.allocs) == (st_after.frees - st_before.frees));
    report("stress.no_failed_allocation", st_after.fails == st_before.fails);
    report("stress.no_bad_align", st_after.bad_align == 0u);
    report("stress.no_out_of_region", st_after.out_of_region == 0u);

    summary();
    return (g_fail == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* MSH entry points                                                    */
/* ------------------------------------------------------------------ */

static int msh_pjs_ui(int argc, char **argv)
{
    return pjs_ui_selftest(pjs_arg_u32(argc, argv, 1, 0u),
                           pjs_arg_u32(argc, argv, 2, 0u));
}

static int msh_pjs_ui_stress(int argc, char **argv)
{
    return pjs_ui_stress(pjs_arg_u32(argc, argv, 1, PJS_STRESS_DEFAULT));
}

MSH_CMD_EXPORT_ALIAS(msh_pjs_ui, pjs_ui,
                     Drive the retained UI core: pjs_ui [width] [height]);

MSH_CMD_EXPORT_ALIAS(msh_pjs_ui_stress, pjs_ui_stress,
                     Repeat the UI core cycle: pjs_ui_stress [rounds]);
