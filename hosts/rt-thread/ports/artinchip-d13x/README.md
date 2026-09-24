# PocketJS - ArtInChip D13x port

PocketJS on **D133ECS** (Xuantie E907FDP, RV32IMAFDC / ILP32D) under
**Luban-Lite + RT-Thread**.

Target data path this port exists to enable:

```
TypeScript / TSX
  -> PocketJS guest bundle
  -> QuickJS-ng
  -> PocketJS Rust retained UI core
  -> RGB565 renderer
  -> ArtInChip framebuffer / Display Engine
  -> 800x480 LCD
```

Gate 0 does not touch any of that. Gate 0 answers one question:

> Can a Rust RV32IMAFDC / ILP32D staticlib be linked by the ArtInChip Xuantie GCC
> toolchain into an official Luban-Lite firmware and exchange values with C
> correctly on real D133ECS silicon?

Until that is proven on hardware, no UI core, QuickJS, framebuffer or GE work
starts.

## Pinned baseline

See [`versions.toml`](versions.toml) for the machine-readable pin. Summary:

| Component | Value |
|-----------|-------|
| PocketJS baseline | `d7deb80e5ddbebf50180ab90f309455758a65c21` (`main` at branch time) |
| Port branch | `d13x` |
| Luban-Lite | `77fbe30dd2de366fae702ece84e1694153d94591` (`main`) |
| Rust | `nightly-2026-07-02` -> `rustc 1.98.0-nightly (4c9d2bfe4 2026-07-01)` |
| Firmware GCC | Xuantie-900 elf newlib gcc Toolchain V2.6.1 B-20220906 (GCC 10.2.0) |

Two deviations from the original plan were forced by reality and are recorded
rather than papered over:

1. **gitee is not reachable.** `https://gitee.com/artinchip/luban-lite.git`
   returns HTTP 401 to anonymous git access, so it cannot be cloned without
   credentials. The official GitHub release channel
   (`artinchip/luban-lite`, self-described as "the official release channel for
   ArtInchip's Luban SDK") was used instead.
2. **There is no `master` branch.** The official repository has a single branch,
   `main`.

The SDK is a **2024-02-23 snapshot**, not the live gitee tip. If a newer
baseline is required, that is a separate upgrade task that re-runs every gate.

The SDK checkout is sparse: `doc/` and `docs/` are excluded (about 800 images
with no bearing on a firmware build). Everything a D13x build needs is present.

## Why a custom Rust target

`riscv32imafc-unknown-none-elf` cannot be used. Inspecting the real spec from
the pinned rustc:

```console
$ rustc +nightly-2026-07-02 -Z unstable-options --print target-spec-json \
    --target riscv32imafc-unknown-none-elf
  "features": "+m,+a,+c,+f"        <- no +d
  "llvm-abiname": "ilp32f"         <- ILP32F, not ILP32D
```

Linking that against the firmware's `-mabi=ilp32d` is exactly the double-float
ABI mismatch that must never be "fixed" by switching to soft-float. So
`rust/targets/d13x-e907-ilp32d.json` sets:

```json
"features": "+m,+a,+f,+d,+c",
"llvm-abiname": "ilp32d",
"panic-strategy": "abort",
"os": "none",
```

The firmware compiles with
`-march=rv32imafdcpzpsfoperand_xtheade -mabi=ilp32d` (from
`bsp/artinchip/sys/d13x/rtconfig.py`, `CPUNAME=e907fdp`). The SDK's own module
build already uses the plain `-march=rv32imafdc -mabi=ilp32d` subset, which is
the precedent for this choice: Rust emits a strict ISA subset with an identical
ABI. The P extension, XThead and vendor instructions are deliberately unused
(spec 10.1).

## Reproducible commands

### 0. Environment

```bash
export POCKETJS_AIC_TOOLCHAIN=/path/to/toolchain   # dir containing bin/
export POCKETJS_AIC_SDK_ROOT=/path/to/luban-lite
```

The official Luban-Lite repo does **not** vendor a toolchain. Any Xuantie
`riscv64-unknown-elf-` GCC install works; the version actually used is recorded
in `versions.toml`.

### 1. Fetch the SDK (blobless + sparse; the full pack transfer does not survive
###    this network path)

```bash
git clone --filter=blob:none --depth 1 --single-branch --branch main --sparse \
  https://github.com/artinchip/luban-lite.git luban-lite
cd luban-lite
for p in tools kernel bsp target application packages; do
  git sparse-checkout add "$p"
done
git rev-parse HEAD   # must equal versions.toml [luban_lite].commit
```

### 2. Build the Rust staticlib

```bash
python hosts/rt-thread/ports/artinchip-d13x/tools/build-native.py
```

which runs, from `rust/abi-probe/`:

```bash
cargo +nightly-2026-07-02 build --release \
  --target ../targets/d13x-e907-ilp32d.json \
  -Zjson-target-spec \
  -Z build-std=core,alloc,compiler_builtins
```

Output: `.pocket-build/d13x/rust-target/d13x-e907-ilp32d/release/libpocketjs_abi_probe.a`

### 3. Static ABI verification (not "it links")

```bash
python hosts/rt-thread/ports/artinchip-d13x/tools/check-abi.py
python hosts/rt-thread/ports/artinchip-d13x/tools/check-sdk.py
```

`check-abi.py` asserts ELF32, RISC-V, `EF_RISCV_FLOAT_ABI_DOUBLE`, an ISA string
with `d` and without `p`/`xthead`, hardware `.d` arithmetic in the probe
functions, `fa0` as the f64 argument/return register, and no POSIX/libc
dependency hiding in the undefined symbols.

### 4. Firmware

```bash
python hosts/rt-thread/ports/artinchip-d13x/tools/apply-sdk.py     # overlay into the SDK
cd "$POCKETJS_AIC_SDK_ROOT"
scons --apply-def=<defconfig>
scons -j8
```

### 5. On target

Serial 115200 8N1. Then:

```
msh /> pjs_abi          # full conformance run, prints RESULT PASS/FAIL
msh /> pjs_abi_panic    # deliberate panic; must abort and halt
```

## Memory plan

```
SRAM (1 MiB)          RT-Thread kernel, interrupts, thread stacks,
                      driver state, hot native state
PSRAM_SW (16 MiB)     PocketJS Rust heap, QuickJS heap, UI tree,
                      DrawList, text, general runtime buffers
PSRAM_CMA             framebuffer, GE, DMA, MPP buffers
```

The ordinary PocketJS allocator must never consume CMA. Gate 0 uses the
RT-Thread system heap behind a single seam (`pjs_host_alloc`); Phase 1 repoints
that seam at `aic_memheap_malloc(MEM_PSRAM_SW)` without touching the Rust side.

## Gate status

| Gate | Scope | Status |
|------|-------|--------|
| 0 | Rust ILP32D toolchain bridge, ABI, allocator | see Gate 0 report |
| 1 | Retained UI core (`no_std` + alloc) | not started |
| 2 | RGB565 software renderer -> AIC framebuffer | not started |
| 3 | QuickJS-ng guest | not started |
| 4 | First real PocketJS app (TSX counter) | not started |
| 5 | `.pocket` package | not started |
| 6 | Touch input | not started |
| 7 | GE acceleration | not started |

## Directory map

```
ports/artinchip-d13x/
├── include/pocketjs_d13x.h    C <-> Rust ABI contract (single source of truth)
├── src/                       port runtime (Phase 1+: memory, display, input, ge)
├── rust/
│   ├── rust-toolchain.toml    pinned toolchain
│   ├── targets/               d13x-e907-ilp32d.json
│   └── abi-probe/             Gate 0 Rust crate
├── tools/                     build-native.py, check-abi.py, check-sdk.py,
│                              apply-sdk.py, portenv.py
├── sdk/
│   ├── patches/               git patches against the pinned SDK
│   └── overlay/               files copied into the SDK tree
└── examples/abi-probe/        Gate 0 C harness
```

Validation artifacts (board logs, objdump, readelf dumps, receipts) go to
`.pocket-build/validation/d13x/<gate>/<run>/` and are never committed.
