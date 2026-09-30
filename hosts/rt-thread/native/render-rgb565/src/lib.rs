#![cfg_attr(not(feature = "std"), no_std)]
#![allow(clippy::missing_safety_doc)]
extern crate alloc;
extern crate pocketjs_rtthread_runtime;
use alloc::boxed::Box;
use core::ffi::c_void;
use core::{ptr, slice};
use pocketjs_core::{
    resources::{FontView, RenderResources},
    TexView,
};
use pocketjs_rtthread_abi::*;
use pocketjs_render_rgb565::{
    PpaOps, QuarterTurn, Rect, RenderTargetState, Renderer, RendererConfig, SrmTransform,
};

pub struct NativeRenderer {
    renderer: Renderer,
}

pub struct NativeRenderTarget {
    state: RenderTargetState,
    prepared_epoch: u64,
    prepared_core: *mut c_void,
    prepared: bool,
}

struct CAccelerator<'a> {
    value: Option<&'a NativeAccelerator>,
}

impl PpaOps for CAccelerator<'_> {
    fn fill_rgb565(
        &mut self,
        destination: &mut [u16],
        width: u32,
        height: u32,
        rect: Rect,
        color: u16,
    ) -> bool {
        let Some(value) = self.value else {
            return false;
        };
        let Some(callback) = value.fill_rgb565 else {
            return false;
        };
        unsafe {
            callback(
                value.user_data,
                destination.as_mut_ptr(),
                destination.len(),
                width,
                height,
                native_rect(rect),
                color,
            )
        }
    }

    fn blend_a8_rgb565(
        &mut self,
        destination: &mut [u16],
        width: u32,
        height: u32,
        mask: &[u8],
        rect: Rect,
        color: [u8; 3],
        global_alpha: u8,
    ) -> bool {
        let Some(value) = self.value else {
            return false;
        };
        let Some(callback) = value.blend_a8_rgb565 else {
            return false;
        };
        unsafe {
            callback(
                value.user_data,
                destination.as_mut_ptr(),
                destination.len(),
                width,
                height,
                mask.as_ptr(),
                mask.len(),
                native_rect(rect),
                color[0],
                color[1],
                color[2],
                global_alpha,
            )
        }
    }

    fn srm_psm5650_to_rgb565(
        &mut self,
        destination: &mut [u16],
        width: u32,
        height: u32,
        source: &[u8],
        source_width: u32,
        source_height: u32,
        source_rect: Rect,
        destination_rect: Rect,
        transform: SrmTransform,
    ) -> bool {
        let Some(value) = self.value else {
            return false;
        };
        let Some(callback) = value.srm_psm5650_rgb565 else {
            return false;
        };
        let rotation = match transform.rotation {
            QuarterTurn::None => 0,
            QuarterTurn::Ccw90 => 1,
            QuarterTurn::Ccw180 => 2,
            QuarterTurn::Ccw270 => 3,
        };
        unsafe {
            callback(
                value.user_data,
                destination.as_mut_ptr(),
                destination.len(),
                width,
                height,
                source.as_ptr(),
                source.len(),
                source_width,
                source_height,
                native_rect(source_rect),
                native_rect(destination_rect),
                rotation,
                transform.mirror_x,
                transform.mirror_y,
            )
        }
    }
}

fn native_rect(value: Rect) -> NativeRect {
    NativeRect {
        x: value.x,
        y: value.y,
        width: value.w,
        height: value.h,
    }
}

fn render_rect(value: NativeRect) -> Rect {
    Rect {
        x: value.x,
        y: value.y,
        w: value.width,
        h: value.height,
    }
}

unsafe extern "C" {
    fn pocketjs_native_ui_frame_validate(frame: *const NativeFrameView) -> i32;
    fn pocketjs_native_ui_texture(
        core: *mut c_void,
        handle: i32,
        out: *mut NativeTextureView,
    ) -> i32;
    fn pocketjs_native_ui_font(core: *mut c_void, slot: u32, out: *mut NativeFontView) -> i32;
}
struct FrameResources<'a>(&'a NativeFrameView);
impl RenderResources for FrameResources<'_> {
    fn viewport(&self) -> (f32, f32) {
        (self.0.logical_width as f32, self.0.logical_height as f32)
    }
    fn raster_revision(&self) -> u64 {
        self.0.raster_revision
    }
    fn texture(&self, handle: i32) -> Option<TexView<'_>> {
        unsafe {
            let mut view: NativeTextureView = core::mem::zeroed();
            view.struct_size = core::mem::size_of::<NativeTextureView>();
            if pocketjs_native_ui_texture(self.0.private_core, handle, &mut view) != 0 {
                return None;
            }
            Some(TexView {
                pixels: slice::from_raw_parts(view.pixels, view.pixel_bytes),
                w: view.width,
                h: view.height,
                psm: view.psm,
                linear: view.linear,
                palette: if view.palette_bytes == 0 {
                    None
                } else {
                    Some(slice::from_raw_parts(view.palette, view.palette_bytes))
                },
            })
        }
    }
    fn font_atlas(&self, slot: u8) -> Option<FontView<'_>> {
        unsafe {
            let mut view: NativeFontView = core::mem::zeroed();
            view.struct_size = core::mem::size_of::<NativeFontView>();
            if pocketjs_native_ui_font(self.0.private_core, slot as u32, &mut view) != 0 {
                return None;
            }
            Some(FontView {
                bitmap: slice::from_raw_parts(view.bitmap, view.bitmap_bytes),
                cell_w: view.cell_width,
                cell_h: view.cell_height,
                raster_density: view.raster_density as u8,
                glyph_count: view.glyph_count as u16,
            })
        }
    }
}
unsafe fn frame_parts<'a>(
    frame: *const NativeFrameView,
) -> Option<(FrameResources<'a>, &'a [u32])> {
    let frame = frame.as_ref()?;
    if frame.struct_size < core::mem::size_of::<NativeFrameView>()
        || pocketjs_native_ui_frame_validate(frame) != 0
    {
        return None;
    }
    let words = if frame.draw_word_count == 0 {
        &[]
    } else {
        if frame.draw_words.is_null() {
            return None;
        }
        slice::from_raw_parts(frame.draw_words, frame.draw_word_count)
    };
    Some((FrameResources(frame), words))
}

fn copy_stats(output: &mut NativeRenderStats, stats: pocketjs_render_rgb565::RenderStats) {
    let output_size = output.struct_size;
    *output = NativeRenderStats {
        struct_size: output_size,
        ppa_fills: stats.ppa_fills,
        ppa_blends: stats.ppa_blends,
        ppa_srm: stats.ppa_srm,
        software_ops: stats.software_ops,
        software_words: stats.software_words,
        damage_regions: stats.damage_regions,
        damage_pixels: stats.damage_pixels,
        damage_bounds: native_rect(stats.damage_bounds),
        full_redraw: stats.full_redraw,
    };
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_renderer_create(
    config: *const NativeRendererConfig,
    output: *mut *mut NativeRenderer,
) -> i32 {
    if config.is_null()
        || output.is_null()
        || (*config).struct_size < core::mem::size_of::<NativeRendererConfig>()
    {
        return -1;
    }
    *output = ptr::null_mut();
    let config = &*config;
    let Some(renderer) = Renderer::new(RendererConfig {
        scale: config.scale,
        min_fill_pixels: config.min_fill_pixels,
        min_blend_pixels: config.min_blend_pixels,
        min_srm_pixels: config.min_srm_pixels,
    }) else {
        return -1;
    };
    *output = Box::into_raw(Box::new(NativeRenderer { renderer }));
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_renderer_destroy(renderer: *mut NativeRenderer) {
    if !renderer.is_null() {
        drop(Box::from_raw(renderer));
    }
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_render_target_create(
    output: *mut *mut NativeRenderTarget,
) -> i32 {
    if output.is_null() {
        return -1;
    }
    *output = ptr::null_mut();
    *output = Box::into_raw(Box::new(NativeRenderTarget {
        state: RenderTargetState::new(),
        prepared_epoch: 0,
        prepared_core: ptr::null_mut(),
        prepared: false,
    }));
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_render_target_destroy(target: *mut NativeRenderTarget) {
    if !target.is_null() {
        drop(Box::from_raw(target));
    }
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_render_target_invalidate(target: *mut NativeRenderTarget) {
    if let Some(target) = target.as_mut() {
        target.state.invalidate();
        target.prepared = false;
    }
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_renderer_prepare(
    renderer: *mut NativeRenderer,
    target: *mut NativeRenderTarget,
    frame: *const NativeFrameView,
    output: *mut NativeDamagePlan,
) -> i32 {
    let (Some(renderer), Some(target), Some(output)) =
        (renderer.as_mut(), target.as_mut(), output.as_mut())
    else {
        return -1;
    };
    target.prepared = false;
    if output.struct_size < core::mem::size_of::<NativeDamagePlan>() {
        return -1;
    }
    let Some((core, words)) = frame_parts(frame) else {
        return -1;
    };
    if renderer.renderer.config().scale != (*frame).raster_density {
        return -1;
    }
    let Some(plan) = renderer
        .renderer
        .prepare_damage(&target.state, &core, words)
    else {
        return -1;
    };
    let output_size = output.struct_size;
    *output = NativeDamagePlan {
        struct_size: output_size,
        region_count: plan.region_count() as u32,
        full_redraw: plan.is_full_redraw(),
        regions: [NativeRect::default(); MAX_DAMAGE_REGIONS],
    };
    for (index, region) in plan.regions().iter().enumerate() {
        output.regions[index] = NativeRect {
            x: region.x0.max(0) as u32,
            y: region.y0.max(0) as u32,
            width: region.x1.saturating_sub(region.x0) as u32,
            height: region.y1.saturating_sub(region.y0) as u32,
        };
    }
    target.prepared_epoch = (*frame).epoch;
    target.prepared_core = (*frame).private_core;
    target.prepared = true;
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_renderer_render_strip(
    renderer: *mut NativeRenderer,
    frame: *const NativeFrameView,
    destination: *mut u16,
    destination_pixels: usize,
    region: NativeRect,
    accelerator: *const NativeAccelerator,
    output: *mut NativeRenderStats,
) -> i32 {
    let (Some(renderer), Some(output)) = (renderer.as_mut(), output.as_mut()) else {
        return -1;
    };
    if output.struct_size < core::mem::size_of::<NativeRenderStats>() || destination.is_null() {
        return -1;
    }
    let Some((core, words)) = frame_parts(frame) else {
        return -1;
    };
    if renderer.renderer.config().scale != (*frame).raster_density {
        return -1;
    }
    let destination = slice::from_raw_parts_mut(destination, destination_pixels);
    let accelerator = accelerator
        .as_ref()
        .filter(|value| value.struct_size >= core::mem::size_of::<NativeAccelerator>());
    let mut accelerator = CAccelerator { value: accelerator };
    let Some(stats) = renderer.renderer.render_strip(
        &core,
        words,
        destination,
        render_rect(region),
        &mut accelerator,
    ) else {
        return -1;
    };
    copy_stats(output, stats);
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_renderer_commit(
    renderer: *mut NativeRenderer,
    target: *mut NativeRenderTarget,
    frame: *const NativeFrameView,
) -> i32 {
    let (Some(renderer), Some(target)) = (renderer.as_mut(), target.as_mut()) else {
        return -1;
    };
    if !target.prepared {
        target.state.invalidate();
        return -1;
    }
    let Some((core, words)) = frame_parts(frame) else {
        target.state.invalidate();
        target.prepared = false;
        return -1;
    };
    if target.prepared_epoch != (*frame).epoch || target.prepared_core != (*frame).private_core {
        target.state.invalidate();
        target.prepared = false;
        return -1;
    }
    let committed = renderer
        .renderer
        .commit_damage(&mut target.state, &core, words);
    target.prepared = false;
    committed as i32
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_renderer_abort(
    renderer: *mut NativeRenderer,
    target: *mut NativeRenderTarget,
) {
    if let (Some(renderer), Some(target)) = (renderer.as_mut(), target.as_mut()) {
        renderer.renderer.abort_damage(&mut target.state);
        target.prepared = false;
    }
}


#[cfg(test)]
mod tests {
    use super::*;
    use pocketjs_rtthread_ui_core::*;

    #[test]
    fn real_ui_pixels_damage_and_borrow_contract() {
        unsafe {
            let cfg = NativeUiConfig { struct_size: core::mem::size_of::<NativeUiConfig>(),
                logical_width: 64, logical_height: 32, raster_density: 1, tick_hz: 60 };
            let mut ui = ptr::null_mut();
            assert_eq!(pocketjs_native_ui_create(&cfg, &mut ui), 0);
            pocketjs_native_ui_set_prop(ui, 1, 17, 0.0); // root defaults to column
            for (index, color) in [0xff0000ffu32, 0xff00ff00, 0xffff0000, 0xffffffff].iter().enumerate() {
                let node = pocketjs_native_ui_create_node(ui, 0);
                assert!(node > 1, "node {index}");
                pocketjs_native_ui_set_prop(ui, node, 1, 16.0);
                pocketjs_native_ui_set_prop(ui, node, 2, 32.0);
                pocketjs_native_ui_set_prop(ui, node, 64, *color as f64);
                pocketjs_native_ui_insert_before(ui, 1, node, 0);
            }
            let rcfg = NativeRendererConfig { struct_size: core::mem::size_of::<NativeRendererConfig>(),
                scale: 1, min_fill_pixels: 1024, min_blend_pixels: 256, min_srm_pixels: 256 };
            let mut renderer = ptr::null_mut();
            let mut target = ptr::null_mut();
            assert_eq!(pocketjs_native_renderer_create(&rcfg, &mut renderer), 0);
            assert_eq!(pocketjs_native_render_target_create(&mut target), 0);
            let mut frame = NativeFrameView { struct_size: core::mem::size_of::<NativeFrameView>(), ..Default::default() };
            let mut plan = NativeDamagePlan { struct_size: core::mem::size_of::<NativeDamagePlan>(), ..Default::default() };
            let mut stats = NativeRenderStats { struct_size: core::mem::size_of::<NativeRenderStats>(), ..Default::default() };
            assert_eq!(pocketjs_native_ui_draw(ui, &mut frame), 0);
            assert_eq!(pocketjs_native_renderer_prepare(renderer, target, &frame, &mut plan), 0);
            assert!(plan.full_redraw);
            let mut row = [0x5a5au16; 66];
            let mut region = NativeRect { x: 0, y: 0, width: 64, height: 1 };
            assert_ne!(pocketjs_native_renderer_render_strip(renderer, &frame, row.as_mut_ptr().add(1), 63, region, ptr::null(), &mut stats), 0);
            assert_eq!(row, [0x5a5a; 66]);
            for y in 0..32 {
                region.y = y;
                assert_eq!(pocketjs_native_renderer_render_strip(renderer, &frame, row.as_mut_ptr().add(1), 64, region, ptr::null(), &mut stats), 0);
                assert_eq!(row[0], 0x5a5a);
                assert_eq!(row[65], 0x5a5a);
                for x in 0..64 { assert_eq!(row[x + 1], [0xf800, 0x07e0, 0x001f, 0xffff][x / 16], "x={x} y={y}"); }
            }
            assert_eq!(pocketjs_native_renderer_commit(renderer, target, &frame), 1);
            assert_eq!(pocketjs_native_renderer_prepare(renderer, target, &frame, &mut plan), 0);
            assert_eq!(plan.region_count, 0);
            pocketjs_native_renderer_abort(renderer, target);
            assert_eq!(pocketjs_native_renderer_prepare(renderer, target, &frame, &mut plan), 0);
            assert!(plan.full_redraw);
            pocketjs_native_ui_tick(ui);
            assert_ne!(pocketjs_native_renderer_render_strip(renderer, &frame, row.as_mut_ptr().add(1), 64, region, ptr::null(), &mut stats), 0);
            assert_ne!(pocketjs_native_renderer_commit(renderer, target, &frame), 1);
            pocketjs_native_render_target_destroy(target);
            pocketjs_native_renderer_destroy(renderer);
            pocketjs_native_ui_destroy(ui);
        }
    }
}
