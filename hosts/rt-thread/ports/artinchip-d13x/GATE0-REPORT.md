# Gate 0 Report — PocketJS on ArtInChip D13x

**Gate:** 0 — Rust RV32IMAFDC / ILP32D toolchain bridge, C↔Rust ABI, allocator
**Date:** 2026-09-24
**Board:** D50T-2-Lite (D133ECS, Xuantie E907FDP)
**Status:** **BUILD VALIDATED — NOT HARDWARE VALIDATED**

> No D133ECS board was reachable during this run. Every result below is from
> the toolchain, the static analysers and the linked image. Nothing was executed
> on silicon, so the C↔Rust ABI is **proven to link and proven to be
> correctly encoded, but not proven to compute the right answers at runtime**.

---

## 1. Baseline

| Component | Pin |
|-----------|-----|
| PocketJS | branch `d13x`, `868c36f` (from `main` @ `d7deb80e`) |
| Luban-Lite SDK | `D50T-2-Lite/luban-lite-jc-d50t-rev`, Luban-Lite **V1.3.2** |
| SDK base commit | `f7572509111d1c70962e6347e5ad7bc87b77fbba` |
| SDK port branch | `pocketjs-d13x` |
| Rust | `nightly-2026-07-02` → `rustc 1.98.0-nightly (4c9d2bfe4 2026-07-01)` |
| Firmware GCC | Xuantie-900 elf newlib **V2.6.1 B-20220906**, GCC **10.2.0** |
| Firmware binutils | **2.35** |
| Firmware flags | `-march=rv32imafdcpzpsfoperand_xtheade -mabi=ilp32d -mcmodel=medany` |

### Baseline deviation (recorded, not hidden)

The port originally pinned `github.com/artinchip/luban-lite`. That was
abandoned on explicit user direction because the mirror cannot support the
target board:

- It is frozen at **v1.0.3**, pushed **2024-02-23**, with one branch and zero
  tags, while this product SDK is based on **V1.3.2**.
- That snapshot has **no D133ECS support at all** — no `ram_param.c` (the PSRAM
  fuse table behind `aic_get_ram_size()`), no `CPU_PSRAM_BASE`/`CPU_SRAM_BASE`,
  and no `target/d13x/d50t-2-lite/` board target.
- Re-deriving the board on v1.0.3 would have meant back-porting the chip
  Kconfig, the memory map and the PSRAM bring-up — the "unrecorded SDK upgrade"
  the spec forbids.

All port work is isolated on `pocketjs-d13x`, cut from `f7572509`. That branch
is never merged back into the product line automatically.

---

## 2. Changes

### PocketJS repository (`hosts/rt-thread/ports/artinchip-d13x/`)

| Path | What |
|------|------|
| `include/pocketjs_d13x.h` | C↔Rust ABI contract; mirrored byte-for-byte by the crate |
| `src/pocketjs_host.c` | Host services Rust calls back into + `pjs_abi` / `pjs_abi_panic` MSH commands |
| `rust/targets/d13x-e907-ilp32d.json` | Custom target: `+d`, `llvm-abiname: ilp32d` |
| `rust/abi-probe/` | `#![no_std]` + `alloc` crate, `panic = abort`, `GlobalAlloc` telemetry |
| `sdk/overlay/` | What gets materialised into the SDK |
| `tools/apply-sdk.py` | The only sanctioned path from repo → SDK |
| `tools/build-native.py` | cargo build + attribute normalisation |
| `tools/build-firmware.py` | Whole chain: cargo → stage → defconfig → SCons → collect |
| `tools/patch-riscv-attrs.py` | Resolves the LLVM/binutils attribute clash (§4) |
| `tools/check-abi.py` | 18 static assertions over the Rust archive |
| `tools/check-sdk.py` | Baseline, branch and build-flag assertions |

### SDK branch `pocketjs-d13x`

```
packages/third-party/pocketjs/            the runtime package (generated)
application/rt-thread/pocketjs-smoke/     thin Gate 0 entry point (generated)
target/configs/d13x_d50t-2-lite_rt-thread_pocketjs-smoke_defconfig
packages/third-party/Kconfig              +1 injected `source` line
```

The runtime lives in `packages/third-party/` — the idiomatic Luban-Lite home
for third-party code — and is vendored so it rebuilds in place. The built
`.a` is **not** committed; `lib/` holds only a `.gitignore`.

The Gate 0 defconfig is deliberately minimal: no display, no GE, no LVGL, no
touch, no CAN, no filesystem. Gate 0 is not allowed to contain UI or GE work.

---

## 3. Commits

PocketJS `d13x` (5 commits, all Conventional Commits):

| SHA | Subject |
|-----|---------|
| `af827b0` | `chore(d13x): establish ArtInChip port baseline` |
| `7e13456` | `feat(d13x): add rv32 ilp32d rust target` |
| `3824b61` | `test(d13x): add c rust abi conformance probe` |
| `969f211` | `chore(d13x): retarget the port at the D50T-2-Lite SDK` |
| `868c36f` | `feat(d13x): add the SDK overlay, the packages/third-party build chain, and the binutils attribute fix` |

SDK `pocketjs-d13x`: cut from `f7572509`; working tree carries the port only.
No force-push, no `main` modification, no `d211` derivation.

---

## 4. Build

```
python tools/build-firmware.py -j8
```

result → `Luban-Lite is built successfully`

| Artifact | Size |
|----------|------|
| `d13x.elf` | 3,515,432 B |
| `d13x.bin` | 220,988 B |
| `.text` / `.data` / `.bss` | 213,228 / 7,740 / 18,812 B |
| Flashable image | `d13x_D50T-2-Lite_page_2k_block_128k_v1.0.0.img` |

### The blocker that had to be solved first

The first real link failed, once per libc member:

```
ld.exe: failed to merge target specific data of file .../libc.a(lib_a-strlen.o)
```

**Cause: an ISA-attribute dialect mismatch, not an ABI mismatch.**

```
Rust  (LLVM 20)  rv32i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_zicsr2p0_zmmul1p0_zaamo1p0_zalrsc1p0_zca1p0_zcd1p0_zcf1p0
GCC   (Xuantie)  rv32i2p0_m2p0_a2p0_f2p0_d2p0_c2p0_xtheade2p0
```

`zmmul`, `zaamo`, `zalrsc`, `zca`, `zcd`, `zcf` are all **subsets** of
I/M/A/C/F/D — LLVM emits no instruction binutils 2.35 cannot encode. Only the
metadata spelling differs, and binutils 2.35 predates those names.

`tools/patch-riscv-attrs.py` removes `.riscv.attributes` from the Rust archive
members and nothing else. **`e_flags` is untouched** — that is where the ABI
lives, and the linker enforces float-ABI compatibility from it, so the
"cannot link soft-float with double-float" safety net stays armed. The ISA
assertion moves to `check-abi.py`, which reads a sidecar
`<archive>.riscv-attrs.txt` captured before patching.

This is explicitly **not** the forbidden "switch to soft-float to make linking
succeed" workaround: no ABI was changed, and `EF_RISCV_FLOAT_ABI_DOUBLE` is
present on all 38 members before and after.

---

## 5. Validation

| Check | Result |
|-------|--------|
| `check-abi.py` (18 assertions) | **PASS** |
| `check-sdk.py` (14 assertions) | **PASS** (2 unrelated pre-existing changes waived) |

### ELF-level, on the linked firmware

```
Class:                ELF32
Machine:              RISC-V
Entry point address:  0x40000000
Flags:                0x5, RVC, double-float ABI
```

`0x5` = `EF_RISCV_RVC | EF_RISCV_FLOAT_ABI_DOUBLE`. The firmware is **ILP32D
hard-float**, and entry is in PSRAM as the config intends.

Merged ISA attribute of the image:

```
Tag_RISCV_arch: "rv32i2p0_m2p0_a2p0_f2p0_d2p0_c2p0_p0p9_zpn0p9_zpsfoperand0p9_xtheade2p0"
```

Contains `f` and `d` — the Rust subset is covered by the C side's string.

### Symbols linked in

38 `pjs_*` symbols in the ELF: 21 `pjs_probe_*` (including all 6
`pjs_probe_call_host_*`), 9 `pjs_host_*`, plus the ABI-probe entry points.

### ABI encoding, from the archive

| Assertion | Evidence |
|-----------|----------|
| ELF32 / RISC-V, every member | 38/38 |
| `EF_RISCV_FLOAT_ABI_DOUBLE`, every member | 38/38, `e_flags = 0x5` |
| ISA advertises d / f / m | `..._f2p2_d2p2_..._m2p0_...` |
| No P extension, no XThead | absent from the Rust arch string |
| Hardware double arithmetic | `pjs_probe_f64` → `fld fa5` / `fmul.d` / `fadd.d` |
| f64 in the ILP32D return register | returns in `fa0` |
| No soft-float helper calls from any `pjs_probe_*` | confirmed |
| No POSIX/libc dependency hiding in undefined symbols | confirmed |

**What this does and does not prove.** It proves the two compilers agree on
struct layout, register assignment and the hard-float ABI at the encoding
level, and that the archive links into real Luban-Lite firmware. It does not
prove the runtime values are correct — that needs the board.

---

## 6. Hardware

**NOT HARDWARE VALIDATED.**

No D133ECS board was reachable during this run. The image is built and
verified statically, but has not been flashed or executed.

To complete Gate 0 on hardware:

```
python tools/build-firmware.py -j8        # produce the image
# flash output/d13x_d50t-2-lite_rt-thread_pocketjs-smoke/images/
#   d13x_D50T-2-Lite_page_2k_block_128k_v1.0.0.img
# console: 115200 8N1
msh /> pjs_abi          # expect: SUMMARY pass=N fail=0 / RESULT PASS
msh /> pjs_abi_panic    # expect: abort + halted board (no return)
```

Gate 0 passes only when `pjs_abi` prints `RESULT PASS` on real silicon and
`pjs_abi_panic` halts the board. Until then **no UI Core, QuickJS, framebuffer
or GE work starts.**

---

## 7. Memory

```
SRAM (1 MiB)       RT-Thread kernel, interrupts, thread stacks,
                   driver state, hot native state
PSRAM_SW (16 MiB)  PocketJS Rust heap, QuickJS heap, UI tree,
                   DrawList, text, general runtime buffers
PSRAM_CMA          framebuffer, GE, DMA, MPP buffers
```

Gate 0 uses the RT-Thread system heap behind a single seam
(`pjs_host_alloc`, which wraps `rt_malloc` and asserts 8-byte alignment).
Phase 1 repoints that seam at `aic_memheap_malloc(MEM_PSRAM_SW)` without
touching the Rust side. **The ordinary PocketJS allocator must never consume
CMA.**

Measured in the linked image: `.bss` 18,812 B, `.data` 7,740 B — no PSRAM
heap is reserved by Gate 0 itself.

---

## 8. Known Issues

1. **NOT HARDWARE VALIDATED** (see §6). The single blocking item.
2. **`pjs_host_log` is unexercised.** It is declared and defined, but the
   crate never calls it, so `--gc-sections` drops it. The Rust→C log direction
   is therefore not proven. Add a `pjs_probe_call_host_log` case in Gate 1.
3. **Attribute normalisation is mandatory.** Every Rust archive must pass
   through `patch-riscv-attrs.py` before linking; `build-native.py` does this
   automatically. Skipping it reproduces the libc merge failure. If the
   toolchain is ever upgraded to binutils ≥ 2.38 this step can likely be
   dropped — re-verify before removing.
4. **`-mcmodel` differs by side.** C uses `medany`; Rust's target JSON leaves
   LLVM's default (`small`/medlow). Both reach every D13x region from absolute
   zero, so they interoperate, but the mismatch is a measured fact rather than
   an assumption.
5. **Full clean rebuild is constrained.** A from-scratch `scons -c` + rebuild
   is blocked by the environment's bulk-delete confirmation guard. The
   verified build is an incremental relink onto a partially cleaned tree. The
   link itself is fully exercised; only object recompilation is not.
6. **SDK baseline is the product SDK, not the upstream mirror** (see §1). A
   future move to a newer official baseline is a separate upgrade task that
   must re-run this gate.
7. **`reg.exe` is blocked by security policy** during SCons. The probe is
   non-fatal and the build completes, but the warning is noise.

---

## 9. Next Gate

**Gate 0 remains OPEN until it runs on hardware.**

| Step | Action |
|------|--------|
| 0a | Flash the image on D50T-2-Lite, capture the console |
| 0b | Confirm `pjs_abi` → `RESULT PASS` on silicon |
| 0c | Confirm `pjs_abi_panic` aborts and halts |
| 0d | Record the run under `.pocket-build/validation/d13x/gate0/<run>/` and update this report |

Only after 0a–0d: **Gate 1 — retained UI core (`no_std` + alloc)**, which also
closes issue 2 above.

---

## Appendix — evidence

`.pocket-build/d13x/validation/gate0/20260924T2203/` (not committed):

```
images/d13x.elf            linked firmware
images/d13x.bin            flat binary
images/d13x.map            link map
images/*.img               flashable image
images/bootcfg.txt         boot config
elf-header.txt             readelf -h   (Flags: 0x5, RVC, double-float ABI)
elf-attributes.txt         readelf -A   (merged ISA attribute)
elf-pjs-symbols.txt        nm           (38 pjs_* symbols)
elf-size.txt               size
check-abi.txt              RESULT: PASS
check-sdk.txt              RESULT: PASS
```

`build-native.py` also writes a receipt with the rustc/cargo versions, the
target-spec SHA-256 and the exact cargo command line.
