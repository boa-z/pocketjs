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

## Two archives, one runtime, and how the duplicate is made harmless

A firmware that links both the Gate 0 probe and this component puts two Rust
`staticlib`s on the link line, and a Rust `staticlib` always bundles its
dependency graph. So both archives carry `core`, `alloc`, `compiler_builtins`
and `pocketjs-rtthread-runtime`.

That is fine, for a specific reason: **the shared crates are built identically in
both archives**, so the linker extracts one copy to satisfy every reference and
never needs the second. Nothing is defined twice. Two properties make that true,
and both are required.

**1. Exactly one crate declares the runtime lang items.**
`#[global_allocator]` and `#[panic_handler]` are declared only in
`pocketjs-rtthread-runtime`. rustc lowers them into
`#[rustc_std_internal_symbol]` shims whose names are *fixed* — `__rustc` plus a
constant disambiguator — precisely so one copy can serve a whole link. Because
the name is fixed, `-Cmetadata` cannot rename it, and two crates declaring the
lang items collide whatever flags are passed:

```
multiple definition of `_RNvCs4iuDAxO633X_7___rustc12___rust_alloc'
```

This matches the ESP-IDF host, where `pocketjs-idf-runtime` is the only
declarer and both `ui-core` and `render-rgb565` consume it. A feature crate
depends on the runtime; it never provides one.

**2. Every crate in the image shares one release profile.**
Cargo folds the release profile into the metadata hash it turns into
`-Cmetadata`, so crates built with different profiles get *differently named*
copies of the shared dependencies. The linker then cannot satisfy this
component's references from the probe's copy, extracts both, and the fixed-name
shims collide. `tools/build-native.py` therefore reads every crate's
`[profile.release]` and refuses to build unless they match key for key.

### What does not work

* **`-Wl,--no-whole-archive` alone.** It resolves member by member, but a member
  is pulled as a whole: the runtime shim's codegen unit carries `host_log`, which
  only this component needs, so pulling it for `host_log` drags in that archive's
  `__rust_alloc` too.
* **Per-crate `-Cmetadata` namespacing.** An earlier revision did this; it is the
  wrong instrument. It makes the two copies of `core`/`alloc` *differently named*
  rather than identical, which is exactly what forces the linker to extract both.
  Left unchecked it would turn a loud "multiple definition" error into two
  silently incompatible allocators in one image — the hazard the ESP-IDF
  wrapper's own comment warns about. Cargo's per-package metadata already keeps
  the two root crates' symbols apart, so nothing extra is needed.

## Self-test

`src/pocketjs_ui_core_demo.c` registers two MSH commands, gated by
`LPKG_POCKETJS_UI_CORE_DEMO`:

| Command | What it does |
|---------|--------------|
| `pjs_ui [w] [h]` | One full cycle: create, build a tree, style/prop/text, tick, draw, hit-test, destroy — plus the leak check. |
| `pjs_ui_stress [n]` | The same cycle n times, asserting the host allocator's live count returns to baseline every round. |

The frame checks are the point. `frame_validate()` must reject a saved view
after a redraw, after a mutation and after a tick, and must accept the current
one — that is the borrow contract in executable form, not a convenience.

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
