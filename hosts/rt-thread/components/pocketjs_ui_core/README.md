# pocketjs_ui_core (RT-Thread)

The retained UI core, bound to RT-Thread. `engine/core` owns the tree, style,
taffy layout, text, animation and draw-list emission; this component owns the
instance ABI that a host calls into.

- Rust crate: `hosts/rt-thread/native/ui-core`
- Public headers: `pocketjs/ui_core.h` (hand-written), `pocketjs/native_ui.h` and
  `pocketjs/ui_types.h` (generated)
- QuickJS dependency: none
- Renderer dependency: none

## What is generated and what is not

`contracts/spec/idf-native.ts` is the single source of truth for the ABI.
`tools/esp-idf-contracts.ts` emits it for **both** hosts:

| Output | ESP-IDF | RT-Thread |
|--------|---------|-----------|
| Rust `#[repr(C)]` types | `hosts/esp-idf/native/abi/src/lib.rs` | `hosts/rt-thread/native/abi/src/lib.rs` |
| C types | `.../pocketjs_ui_core/include/pocketjs/ui_types.h` | `.../pocketjs_ui_core/include/pocketjs/ui_types.h` |
| C entry points | `.../include/pocketjs/native_ui.h` | `.../include/pocketjs/native_ui.h` |

The two Rust type files and the two `ui_types.h` files are byte-identical,
because the ABI types carry nothing host-specific. That is the point: the port
cannot drift from the reference, and there is one place to change a field.

`native_ui.h` is parsed from each host's own `ui-core` crate, so it reflects
what that crate actually exports rather than what someone remembered to declare.

Regenerate with `node --experimental-strip-types tools/esp-idf-contracts.ts`
(or `bun tools/esp-idf-contracts.ts`), and verify with `--check`, which fails on
any stale output. The generator writes LF; this checkout has `core.autocrlf=true`,
so compare with `git hash-object`, not `sha256sum`, when checking for drift.

## What is hand-written

- `include/pocketjs/ui_core.h` — the RT-Thread-flavoured surface. Identical
  function set to the ESP-IDF component, with errors as `rt_err_t`
  (`RT_EOK`, `-RT_EINVAL`, `-RT_ENOMEM`) instead of `esp_err_t`.
- `src/ui_core.c` — argument checking and error translation only.

## The runtime seam is not implemented here

The ESP-IDF component's `ui_core.c` also implements the Rust runtime symbols
(`pocketjs_idf_rust_alloc` / `_dealloc` / `_panic`). This component deliberately
does not, because the port already owns that seam and validated it on hardware:

```
pocketjs_alloc.c -> pjs_host_alloc / pjs_host_free   (PSRAM_SW, Gate 1A)
pocketjs_host.c  -> pjs_host_abort / pjs_host_log
```

`pocketjs-rtthread-runtime` binds Rust's `GlobalAlloc` and `#[panic_handler]` to
those. A second implementation here would put two allocators on one heap.

## Ownership

One caller-selected thread owns each core instance. Pointers in frame, texture
and font views stay valid until the next mutation or tick on that core.

A frame view is a **borrow, not a snapshot**. `pocketjs_ui_core_draw()` hands
back a pointer to the core's draw words, and any subsequent mutation, tick or
further draw replaces them. `pocketjs_ui_core_frame_validate()` returns 0 only
while the view still matches the core's current epoch and buffer, which is how a
caller finds out its view went stale before it reads freed words.

## Logging

`pocketjs-rtthread-runtime` calls `pjs_host_log` on instance create and destroy.
Gate 0's report (section 8.4) recorded that `pjs_host_log` was declared and
defined but never called from Rust, so `--gc-sections` dropped it and the
Rust-to-C log direction was unproven. This component exercises it on every run.
