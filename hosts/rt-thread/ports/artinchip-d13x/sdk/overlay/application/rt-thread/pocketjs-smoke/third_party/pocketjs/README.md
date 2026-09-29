# PocketJS — ArtInChip D13x / Luban-Lite port

> **Generated package.** Do not edit this directory inside the SDK tree.
> Source of truth: `hosts/rt-thread/ports/artinchip-d13x/sdk/overlay/` in the
> PocketJS repository. Regenerate with `python tools/apply-sdk.py`.

PocketJS on ArtInChip D13x (`D133ECS`, Xuantie **E907FDP**, RV32IMAFDC /
**ILP32D** hard-float), running on Luban-Lite + RT-Thread.

## Gate 0 scope

This package currently ships **only** the C↔Rust ABI conformance probe. That is
intentional: Gate 0 exists to prove, on real silicon, that a Rust
`RV32IMAFDC`/`ILP32D` staticlib can be linked by the Xuantie GCC toolchain into
official Luban-Lite firmware and exchange values with C correctly.

Until that is proven, no UI Core, no QuickJS, no framebuffer and no GE work is
allowed to land here.

## Layout

| Path | Purpose |
|------|---------|
| `include/pocketjs_d13x.h` | The C↔Rust ABI contract. Mirrored byte-for-byte by `rust/abi-probe/src/lib.rs`. |
| `src/pocketjs_host.c` | Host services Rust calls back into, plus the `pjs_abi` / `pjs_abi_panic` MSH commands. |
| `rust/` | The crate and its JSON target spec, so the package can be rebuilt in place. |
| `lib/` | Drop-box for `libpocketjs_abi_probe.a`. Git-ignored — never commit it. |

## Why a custom target spec

The stock Rust target `riscv32imafc-unknown-none-elf` declares
`"features": "+m,+a,+c,+f"` (no `+d`) and `"llvm-abiname": "ilp32f"`. Linking it
against Luban-Lite, which compiles with `-mabi=ilp32d`, produces exactly the
double-float ABI mismatch Gate 0 is meant to catch — so this port uses
`rust/targets/d13x-e907-ilp32d.json` instead, which sets `+d` and `ilp32d`.

## Building

From the PocketJS repository:

```sh
python hosts/rt-thread/ports/artinchip-d13x/tools/apply-sdk.py   # sync this package
python hosts/rt-thread/ports/artinchip-d13x/tools/build-firmware.py
```

`build-firmware.py` builds the Rust staticlib, stages it into `lib/`, applies
`d13x_d50t-2-lite_rt-thread_pocketjs-smoke_defconfig` and runs the SCons build.

## Verifying without hardware

```sh
python hosts/rt-thread/ports/artinchip-d13x/tools/check-abi.py   # static ELF checks
python hosts/rt-thread/ports/artinchip-d13x/tools/check-sdk.py   # baseline checks
```

On the board, `pjs_abi` re-runs the probe and `pjs_abi_panic` exercises the
`panic = abort` path.
