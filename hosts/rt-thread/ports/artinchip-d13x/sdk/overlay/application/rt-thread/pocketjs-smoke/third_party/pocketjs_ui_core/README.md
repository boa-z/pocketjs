# PocketJS UI core — ArtInChip D13x / Luban-Lite

> **Generated package.** Do not edit this directory inside the SDK tree.
> Source of truth: `hosts/rt-thread/ports/artinchip-d13x/sdk/overlay/` in the
> PocketJS repository. Regenerate with `python tools/apply-sdk.py`.

The retained UI core for the D13x port: `engine/core` owns the node tree, style,
taffy layout, text, animation and draw-list emission; this package owns the
instance ABI a host calls into.

## Layout

| Path | Purpose |
|------|---------|
| `include/pocketjs/ui_core.h` | The RT-Thread surface. Hand-written; identical function set to the ESP-IDF component, with errors as `rt_err_t`. |
| `include/pocketjs/native_ui.h` | The raw `pocketjs_native_ui_*` entry points. Generated from the crate. |
| `include/pocketjs/ui_types.h` | The `#[repr(C)]` ABI types. Generated; byte-identical to the ESP-IDF copy. |
| `src/ui_core.c` | Thunk layer: argument checking and error translation only. |
| `lib/` | Drop-box for `libpocketjs_rtthread_ui_core.a`. Git-ignored — never commit it. |

## What is generated

`contracts/spec/idf-native.ts` is the single source of truth for the ABI, and
`tools/esp-idf-contracts.ts` emits it for both hosts. The two Rust type files and
the two `ui_types.h` files are byte-identical, because the ABI types carry
nothing host-specific. `native_ui.h` is parsed from each host's own crate, so it
reflects what that crate actually exports.

Regenerate from the PocketJS repository:

```sh
node --experimental-strip-types tools/esp-idf-contracts.ts   # write
node --experimental-strip-types tools/esp-idf-contracts.ts --check   # fail on drift
```

## Why the archive is not committed

`libpocketjs_rtthread_ui_core.a` is built from `hosts/rt-thread/native/ui-core`,
which depends on `engine/core` and its crate graph. Unlike the `pocketjs`
package — whose crate is small enough to vendor so the SDK branch rebuilds in
place — this one is staged as a prebuilt. The C half above *is* vendored, so the
SDK branch stays self-contained for everything except recompiling the engine.

```sh
python hosts/rt-thread/ports/artinchip-d13x/tools/apply-sdk.py      # sync this package
python hosts/rt-thread/ports/artinchip-d13x/tools/build-firmware.py # build + stage + link
```

## The runtime seam is not implemented here

Unlike the ESP-IDF component, `src/ui_core.c` does **not** implement the Rust
runtime symbols (`alloc` / `dealloc` / `panic`). The port already owns that seam
and validated it on hardware:

```
pocketjs_alloc.c -> pjs_host_alloc / pjs_host_free   (PSRAM_SW, Gate 1A)
pocketjs_host.c  -> pjs_host_abort / pjs_host_log
```

`pocketjs-rtthread-runtime` binds Rust's `GlobalAlloc` and `#[panic_handler]` to
those. A second implementation here would put two allocators on one heap.

## Why both archives can carry that runtime

Both Rust archives bundle the same dependency graph — `core`, `alloc`,
`compiler_builtins`, and the runtime shim. A Rust `staticlib` always bundles its
dependencies, so linking two of them puts the same symbols on the link line
twice.

`-Wl,--no-whole-archive` is **not** enough to make that safe, and it was worth
finding out the hard way: a member is pulled as a whole, and the runtime shim's
codegen unit also carries `host_log`, which only the UI core needs. Pulling it
for `host_log` drags in a second `__rust_alloc`, and the link dies with
`multiple definition of _RNvCs4iuDAxO633X_7___rustc12___rust_alloc`.

Each crate is therefore built with its own `-Cmetadata` namespace, which changes
the crate disambiguator Rust bakes into mangled symbol names, so each archive
gets its own copy of the implementation. This is the mechanism the ESP-IDF host
already uses (`hosts/esp-idf/components/pocketjs_ui_core/rustc_wrapper.rs`); see
`Crate.namespace` in the port's `tools/build-native.py` for why this port
namespaces every crate rather than only the non-standard ones.

## Ownership

One caller-selected thread owns each core instance. Pointers in frame, texture
and font views stay valid until the next mutation or tick on that core.

A frame view is a **borrow, not a snapshot**. `pocketjs_ui_core_draw()` hands
back a pointer to the core's draw words, and any later mutation, tick or further
draw replaces them. `pocketjs_ui_core_frame_validate()` returns 0 only while the
view still matches the core's current epoch and buffer, which is how a caller
finds out its view went stale before it reads freed words.

## Verifying without hardware

```sh
python hosts/rt-thread/ports/artinchip-d13x/tools/check-abi.py --elf <d13x.elf>
```

Checks both Rust archives for ELF class, ISA attributes and float ABI, and — with
`--elf` — that `pjs_host_log` actually survived `-Wl,-gc-sections` into the
linked image. That last one is what closes Gate 0 report section 8.4.
