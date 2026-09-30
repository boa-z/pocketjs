# PocketJS RT-Thread host

The RT-Thread host layer for PocketJS. It follows the same architectural split
the ESP-IDF host established: **the product firmware owns the platform, PocketJS
owns the runtime.**

```
product firmware owns            PocketJS components own
-------------------------        ---------------------------------
threads and scheduling           package parser
input devices and queues         QuickJS guest
display buffers                  UI C ABI
presentation and vsync           retained UI core
package storage                  RGB565 renderer
```

Nothing in this directory may reach into `engine/core` semantics. The host adapts
the platform to PocketJS's existing contract; it does not change the contract.

## Touch turns

The UI bridge consumes complete touch snapshots through `pocketjs_ui_turn`.
If a device queue loses contact edges, the host calls
`pocketjs_ui_cancel_touches` on the owning thread. This performs one UI turn
with terminal CANCEL words for all contacts previously delivered by the binding
and clears native capture. The host suppresses affected contacts until lift;
an empty release snapshot cannot substitute for cancellation because it can
activate a captured button. Guest failures still require session teardown.

`pocketjs_ui_qjs_get_turn_stats` reports elapsed OS ticks for hit testing,
the guest frame including Promise jobs, the retained core tick, and draw-list
generation. Counters reset on each turn; stages not reached stay zero. Read
these counters on the owning thread. They include preemption and blocking.

## Layout

```
hosts/rt-thread/
├── components/                 # platform-independent RT-Thread components
│   ├── pocketjs_package/       #   .pocket package parsing
│   ├── pocketjs_guest/         #   QuickJS-ng guest lifecycle
│   ├── pocketjs_ui_core/       #   retained UI core host binding
│   ├── pocketjs_ui_qjs/        #   globalThis.ui -> UI C ABI bridge
│   └── pocketjs_render_rgb565/ #   RGB565 software renderer
└── ports/
    └── artinchip-d13x/         # ArtInChip D13x / D133ECS specifics
```

`components/` holds code that would work on any RT-Thread target.
`ports/artinchip-d13x/` holds everything ArtInChip-specific: memory map,
framebuffer access, GE, pinmux assumptions, and the SDK build glue.

Platform conditionals (`#ifdef ARTINCHIP`, `#ifdef RTTHREAD`, `#ifdef D133`) are
**not** permitted in `engine/core`. If a port appears to need one, that is a
signal the existing PocketJS contract is insufficient, and the contract change
is the discussion to have - not an `#ifdef`.

## Reference implementation

`hosts/esp-idf` is the reference for module boundaries, ownership, the
caller-driven model, the allocator model and the renderer contract. Its APIs are
not copied. The substitutions are:

| ESP-IDF | RT-Thread / ArtInChip |
|---------|----------------------|
| FreeRTOS tasks | RT-Thread threads |
| `heap_caps_malloc` | `aic_memheap_malloc` (SRAM / PSRAM_SW / CMA) |
| ESP display + PPA | ArtInChip MPP framebuffer + GE |
| ESP logging | RT-Thread ulog / `aic_log` |

## Ports

| Port | SoC | CPU | ABI | Status |
|------|-----|-----|-----|--------|
| [artinchip-d13x](ports/artinchip-d13x/README.md) | D133ECS | Xuantie E907FDP | RV32IMAFDC / ILP32D | Gate 2 board accepted; Gate 3 guest/UI under validation |
