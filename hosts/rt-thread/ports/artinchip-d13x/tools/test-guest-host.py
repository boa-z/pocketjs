"""Run the actual QuickJS guest and application probes with a tracked host heap."""
import os
import argparse
import shutil
import subprocess
import sys
from pathlib import Path
import portenv as pe

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--ui', action='store_true', help='link actual Rust UI/renderer; mock only framebuffer I/O')
    ap.add_argument('--package', action='store_true', help='compile and execute the real TSX package')
    args = ap.parse_args()
    args.ui = args.ui or args.package
    repo = pe.repo_root()
    dest = repo / '.pocket-build/host-guest'
    dest.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get('CC') or shutil.which('gcc')
    if not cc:
        print('No native GCC; guest tests NOT RUN')
        return 2
    subprocess.run([sys.executable, str(pe.TOOLS_DIR/'prepare-quickjs.py')], check=True)
    if args.package:
        subprocess.run([sys.executable, str(pe.TOOLS_DIR/'build-app.py')], check=True)
    (dest/'rtthread.h').write_text(r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef int rt_err_t;
typedef uint32_t rt_tick_t;
#define RT_TICK_PER_SECOND 1000
#define RT_NULL NULL
#define RT_EOK 0
#define RT_ERROR 1
#define RT_ETIMEOUT 2
#define RT_ENOMEM 5
#define RT_ENOSYS 6
#define RT_EINVAL 10
#define RT_EBUSY 7
#define rt_kprintf printf
#define MSH_CMD_EXPORT(a,b)
rt_tick_t rt_tick_get(void);
''', encoding='utf-8')
    (dest/'host.c').write_text(r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rtthread.h"
#include "pocketjs_port.h"
static pjs_heap_stats_t stats;
void *pjs_host_alloc(uint32_t size, uint32_t align) {
    void *raw = malloc((size_t)size + align + sizeof(void *));
    if (!raw) { ++stats.fails; return NULL; }
    uintptr_t aligned = ((uintptr_t)raw + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1);
    ((void **)aligned)[-1] = raw;
    ++stats.allocs;
    return (void *)aligned;
}
void pjs_host_free(void *p, uint32_t align) { (void)align; if (p) { ++stats.frees; free(((void **)p)[-1]); } }
void pjs_host_alloc_stats(pjs_heap_stats_t *out) { *out = stats; }
static int test_clock_enabled;
static uint32_t test_ticks, test_tick_step;
void pjs_test_clock(int enabled, uint32_t step) {
    test_clock_enabled = enabled; test_ticks = 0; test_tick_step = step;
}
uint32_t pjs_test_clock_ticks(void) { return test_ticks; }
uint32_t rt_tick_get(void) {
    if (test_clock_enabled) { test_ticks += test_tick_step; return test_ticks; }
    return (uint32_t)((uint64_t)clock() * 1000 / CLOCKS_PER_SEC);
}
int pjs_js_selftest(void);
int pjs_budget_selftest(void);
static int (*test_frame_handler)(void);
static unsigned test_health_failures;
int pjs_guest_run(int (*operation)(void)) { return operation(); }
int pjs_guest_set_frame_handler(int (*frame)(void), unsigned hz) {
    if (frame && (!hz || hz > RT_TICK_PER_SECOND)) return -RT_EINVAL;
    test_frame_handler = frame;
    return RT_EOK;
}
int pjs_test_pump_frame(void) {
    return test_frame_handler ? test_frame_handler() : -RT_ENOSYS;
}
void pjs_ota_mark_unhealthy(void) { ++test_health_failures; }
unsigned pjs_test_health_failures(void) { return test_health_failures; }
#ifdef LPKG_USING_POCKETJS_PACKAGE
#include "pjs_touch_device.h"
static pjs_touch_state_t test_touch;
int pjs_touch_device_start(unsigned w, unsigned h) { pjs_touch_state_init(&test_touch, w, h); return RT_EOK; }
void pjs_touch_device_stop(void) { pjs_touch_state_init(&test_touch, 0, 0); }
int pjs_touch_device_read(pocketjs_ui_touch_t *p) {
    return test_touch.overflows || test_touch.invalid ? -RT_ERROR : (int)pjs_touch_state_read(&test_touch, p);
}
void pjs_touch_device_status(void) {}
void pjs_test_touch(unsigned event, unsigned x, unsigned y) {
    pjs_touch_sample_t sample = {0, event, x, y};
    pjs_touch_state_feed(&test_touch, &sample, 1);
}
#endif
int main(void) {
    int result = pjs_budget_selftest();
    result |= pjs_js_selftest();
#ifdef LPKG_USING_POCKETJS_GUEST
    extern int pjs_render_selftest(void), pjs_js_display(void), pjs_touch_selftest(void);
    result |= pjs_touch_selftest();
    result |= pjs_render_selftest();
    result |= pjs_js_display();
#endif
#ifdef LPKG_USING_POCKETJS_PACKAGE
    extern int pjs_package_selftest(void);
    result |= pjs_package_selftest();
    extern int pjs_package_runtime_selftest(void);
    result |= pjs_package_runtime_selftest();
    extern int pjs_touch_queue_selftest(void);
    result |= pjs_touch_queue_selftest();
#endif
    return result ? 1 : 0;
}
''', encoding='utf-8')
    qjs = repo/'.pocket-build/quickjs'
    guest = repo/'hosts/rt-thread/components/pocketjs_guest'
    app = pe.sdk_root()/'application/rt-thread/pocketjs-smoke'
    exe = dest/('guest-test.exe' if os.name == 'nt' else 'guest-test')
    cmd = [cc, '-std=gnu11', '-O2', '-DNDEBUG', '-DPJS_FREESTANDING', '-DPJS_HOST_TEST']
    for include in (dest, qjs, guest/'include', pe.PORT_ROOT/'include', pe.PORT_ROOT/'src'):
        cmd += ['-I', str(include)]
    cmd += [str(qjs/name) for name in ('quickjs.c','dtoa.c','libregexp.c','libunicode.c')]
    cmd += [str(guest/'src/guest.c'), str(app/'pjs_js.c'), str(dest/'host.c')]
    cmd += [str(pe.TOOLS_DIR/'tests/guest-budget.c')]
    if args.package:
        app_name = os.environ.get('PJS_APP', 'counter')
        package = repo/'hosts/rt-thread/components/pocketjs_package'
        embedded = repo/f'.pocket-build/d13x/{app_name}/embedded'
        cmd += ['-DLPKG_USING_POCKETJS_PACKAGE', '-DPJS_CAN_OTA', '-I', str(package/'include'), '-I', str(embedded),
                str(package/'src/package.c'), str(embedded/f'pocketjs_package_{app_name}.c'),
                '-I', str(app), str(app/'pjs_package.c'), str(app/'pjs_hero_session.c'),
                str(app/'pjs_touch_input.c'), str(app/'tests/touch_queue_test.c')]
    if args.ui:
        target = 'x86_64-pc-windows-gnu' if os.name == 'nt' else 'x86_64-unknown-linux-gnu'
        rust = dest/'rust'
        env = dict(os.environ, CARGO_TARGET_DIR=str(rust))
        host = repo/'hosts/rt-thread'
        for crate in ('ui-core', 'render-rgb565'):
            subprocess.run(['cargo', '+stable', 'build', '--release', '--target', target,
                '--manifest-path', str(host/'native'/crate/'Cargo.toml')], env=env, check=True)
        (dest/'mpp_fb.h').write_text(r'''
#pragma once
#include <stdint.h>
struct mpp_fb { int open; };
struct aicfb_screeninfo {
    uint8_t *framebuffer;
    unsigned width, height, stride, format, bits_per_pixel, smem_len;
};
#define MPP_FMT_RGB_565 1
#define AICFB_GET_SCREENINFO 1
#define AICFB_WAIT_FOR_VSYNC 2
struct mpp_fb *mpp_fb_open(void);
int mpp_fb_ioctl(struct mpp_fb *, int, void *);
void mpp_fb_close(struct mpp_fb *);
''', encoding='utf-8')
        (dest/'artinchip_fb.h').write_text('#include "mpp_fb.h"\n', encoding='utf-8')
        (dest/'aic_osal.h').write_text('#define aicos_dcache_clean_range(p,n) ((void)0)\n', encoding='utf-8')
        (dest/'scanout.c').write_text(r'''
#include "mpp_fb.h"
#include <stdlib.h>
#include <stdio.h>
static struct mpp_fb fb;
static uint8_t *pixels;
static int fail_vsync, bad_screen;
void pjs_test_fail_vsync(void) { fail_vsync = 1; }
void pjs_test_bad_screen(int enabled) { bad_screen = enabled; }
struct mpp_fb *mpp_fb_open(void) {
    pixels = calloc(480,1600);
    return pixels ? &fb : NULL;
}
int mpp_fb_ioctl(struct mpp_fb *f, int request, void *out) {
    (void)f;
    if (request == AICFB_WAIT_FOR_VSYNC) {
        if (fail_vsync) { fail_vsync = 0; return -1; }
        return 0;
    }
    if (request != AICFB_GET_SCREENINFO || !out) return -1;
    *(struct aicfb_screeninfo *)out = (struct aicfb_screeninfo){pixels,800,480,1600,1,16,480*1600};
    if (bad_screen) ((struct aicfb_screeninfo *)out)->stride = 1;
    return 0;
}
void mpp_fb_close(struct mpp_fb *f) {
    (void)f; const char *path = getenv("PJS_FRAME_CAPTURE");
    if (path && pixels) { FILE *out = fopen(path, "wb"); if(out) { fwrite(pixels,1,480*1600,out); fclose(out); } }
    free(pixels); pixels = NULL;
}
''', encoding='utf-8')
        cmd += ['-DLPKG_USING_POCKETJS_GUEST', str(app/'pjs_render.c'), str(app/'pjs_touch.c'), str(dest/'scanout.c')]
        for component, source in (('ui_core','ui_core.c'), ('ui_qjs','ui_qjs.c'), ('render_rgb565','render_rgb565.c')):
            base = host/'components'/('pocketjs_'+component)
            cmd += ['-I', str(base/'include'), str(base/'src'/source)]
        cmd += [str(rust/target/'release'/('libpocketjs_rtthread_'+c+'.a')) for c in ('render_rgb565','ui_core')]
        if os.name == 'nt':
            cmd += ['-lws2_32', '-lbcrypt', '-luserenv', '-lntdll']
        else:
            cmd += ['-lpthread', '-ldl']
        print('HOST ONLY: real QuickJS/UI/RGB565, simulated scanout; Rust uses host std allocator', flush=True)
    cmd += ['-lm', '-o', str(exe)]
    subprocess.run(cmd, check=True)
    return subprocess.run([str(exe)], timeout=90).returncode

if __name__ == '__main__':
    sys.exit(main())
