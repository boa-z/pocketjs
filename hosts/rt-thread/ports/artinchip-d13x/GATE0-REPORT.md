# Gate 0 Report — PocketJS on ArtInChip D13x

**Gate:** 0 — Rust RV32IMAFDC / ILP32D toolchain bridge, C↔Rust ABI, allocator
**Date:** 2026-09-24 (build) · 2026-09-25 (first hardware run)
**Board:** D50T-2-Lite (D133ECS, Xuantie E907FDP)
**Status:** **HARDWARE RUN 1 — ABI PASSED ON SILICON, 1 ALLOCATOR DEFECT FOUND, FIXED, AWAITING RE-TEST**

> The board was flashed and the probe executed on real silicon. **Every C↔Rust
> ABI case passed** — scalars, f32/f64 in registers, pointers, structs by value,
> nested structs, mixed scalar+pointer, layout agreement between the two
> compilers, and Rust→C callbacks.
>
> The run then aborted in the allocator: this board's `rt_malloc` returns
> 4-byte-aligned memory, while both the C and Rust sides assumed 8. That is a
> genuine defect, so **Gate 0 did not pass on this run**. It is fixed and
> verified on the host (§5.1) but the corrected image has not been flashed yet.
> One more run is needed.

---

## 1. Baseline

| Component | Pin |
|-----------|-----|
| PocketJS | branch `d13x`, `a926701` (from `main` @ `d7deb80e`) |
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
| `src/pocketjs_alloc.c` | Host allocator — split out so it is host-testable (§5.1) |
| `rust/targets/d13x-e907-ilp32d.json` | Custom target: `+d`, `llvm-abiname: ilp32d` |
| `rust/abi-probe/` | `#![no_std]` + `alloc` crate, `panic = abort`, `GlobalAlloc` telemetry |
| `sdk/overlay/` | What gets materialised into the SDK |
| `tools/apply-sdk.py` | The only sanctioned path from repo → SDK |
| `tools/build-native.py` | cargo build + attribute normalisation |
| `tools/build-firmware.py` | Whole chain: cargo → stage → defconfig → SCons → collect + verify |
| `tools/patch-riscv-attrs.py` | Resolves the LLVM/binutils attribute clash (§4) |
| `tools/check-abi.py` | 18 static assertions over the Rust archive |
| `tools/check-sdk.py` | Baseline, branch and build-flag assertions |
| `tools/test-alloc-host.py` | 19 checks over the allocator, on a stubbed heap (§5.1) |

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

PocketJS `d13x` (9 commits, all Conventional Commits):

| SHA | Subject |
|-----|---------|
| `af827b0` | `chore(d13x): establish ArtInChip port baseline` |
| `7e13456` | `feat(d13x): add rv32 ilp32d rust target` |
| `3824b61` | `test(d13x): add c rust abi conformance probe` |
| `969f211` | `chore(d13x): retarget the port at the D50T-2-Lite SDK` |
| `868c36f` | `feat(d13x): add the SDK overlay, the packages/third-party build chain, and the binutils attribute fix` |
| `bb29c61` | `docs(d13x): add the Gate 0 report and align check-sdk with the new baseline` |
| `dad3c35` | `chore(d13x): drop unused SDK patch scaffolding` |
| `70c4cca` | `fix(d13x): make the port tooling report true state and rebuild reproducibly` |
| `a926701` | `fix(d13x): honour the caller's alignment in the host allocator` |

SDK `pocketjs-d13x` (2 commits on top of the fork point):

| SHA | Subject |
|-----|---------|
| `16837b2d` | `feat(pocketjs): add the PocketJS runtime package and the Gate 0 application` |
| `b9664c44` | `fix(pocketjs): honour the caller's alignment in the host allocator` |

Cut from `f7572509`; the working tree carries the port only. No force-push, no
`main` modification, no `d211` derivation.

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
| `check-sdk.py` (14 assertions) | **PASS** with `--override-sdk` (2 unrelated pre-existing changes waived) |
| `apply-sdk.py --check` | **PASS** — "the SDK matches the overlay" |
| `test-alloc-host.py` (19 checks) | **PASS** — the host allocator, on a stubbed heap |

### 5.1 The allocator defect the hardware found

The first hardware run aborted here:

```
[pjs-abi] FATAL: rt_malloc returned 30045804, not 8-byte aligned
```

`30045804` is `0x01CA766C` — 4-byte aligned, not 8. The cause is a build
setting, not a bug in the probe:

```
CONFIG_RT_ALIGN_SIZE=4                  (.config:467, rtconfig.h:261)
CONFIG_RT_USING_MEMHEAP_AS_HEAP=y       (.config:515)
```

so `rt_malloc` → `rt_memheap_alloc` returns **`RT_ALIGN_SIZE`-aligned** memory,
which is 4 here. Both sides had hardcoded the opposite belief:

| Where | Wrong assumption |
|-------|------------------|
| `src/pocketjs_host.c` | `#define PJS_HOST_ALIGN 8` — asserted 8 on **every** allocation |
| `rust/abi-probe/src/lib.rs` | `const HOST_GUARANTEED_ALIGN: usize = 8` — returned the host pointer raw for any `align <= 8` |

Two compounding errors. The Rust side handed a 4-aligned pointer to code that
required 8 (memory corruption waiting to happen — it was caught only because the
C side asserted). And the C assertion was over-strict: it fired on the very first
allocation, `Box<u32>`, which asks for 4 bytes of alignment, not 8.

**The fix removes the class of bug rather than the instance.** The host
allocator now takes the alignment the caller needs and is responsible for
honouring it, so nothing has to be assumed about `RT_ALIGN_SIZE`:

```c
void *pjs_host_alloc(uint32_t size, uint32_t align);   /* was (uint32_t size) */
void  pjs_host_free(void *ptr, uint32_t align);        /* was (void *ptr)     */
```

- `align <= RT_ALIGN_SIZE` takes the fast path (`rt_malloc` already satisfies it).
- `align >  RT_ALIGN_SIZE` over-allocates and stashes the raw pointer in the word
  before the payload, so `pjs_host_free` can hand the original block back.
- `HOST_GUARANTEED_ALIGN`, `ALIGN_HEADER` and the duplicated over-alignment
  bookkeeping are **deleted** from the Rust side; `layout.align()` is passed
  straight through. One place now owns alignment instead of two.
- The C assertion is kept but now checks the *real* contract.

The allocator moved to `src/pocketjs_alloc.c` for one reason: it is arithmetic,
so it can be tested on the build host instead of argued about. `test-alloc-host.py`
compiles it against a stubbed RT-Thread heap whose base is **deterministically
4-but-not-8 aligned** — reproducing the board — and checks 19 properties,
including the exact failing case (`Box<f64>`, align 8), adjacent over-aligned
blocks written to the brim, and that every `free` returns a block `rt_malloc`
actually issued. The test first guards that the stub really does reproduce the
board, so it cannot pass vacuously.

Writing that test immediately paid for itself: it exposed that an early clamp
in my own fix (`align < sizeof(void*)` → `sizeof(void*)`) behaves **differently
on a 64-bit build host than on the 32-bit target**, which would have made the
test silently unrepresentative. The clamp turned out to be unnecessary in both
branches and was deleted — see the comment in `src/pocketjs_alloc.c`.

Also verified: the C harness now exercises the host service **directly**, with no
Rust in the loop (`host.alloc_align4/8/64`), so a regression here cannot hide
behind the GlobalAlloc wrapper.

`check-sdk.py` asserts that the SDK is on `pocketjs-d13x` and that the pinned
`base_commit` (`f7572509`) is still an **ancestor** of HEAD — not that HEAD
equals it. `base_commit` is the fork point, so the port branch is expected to
sit ahead of it; requiring equality would fail the moment the port's own work
was committed. At the time the Gate 0 image was built the branch had no port
commits, so HEAD and the fork point coincided; the SDK commit that landed
afterwards (`16837b2d`) is one commit on top, and the ancestry assertion still
holds.

In **strict** mode (no `--override-sdk`) the run reports FAIL, because the
product tree carries two uncommitted changes that are **not** the port's:
`M .vscode/settings.json` and an untracked
`packages/third-party/mbedtls/ports/src/tls_certificate.c`. Both pre-date this
work and were deliberately left untouched; the port touches only
`packages/third-party/pocketjs/`, `application/rt-thread/pocketjs-smoke/`,
`target/configs/d13x_d50t-2-lite_rt-thread_pocketjs-smoke_defconfig` and one
added `source` line in `packages/third-party/Kconfig`. The strict FAIL is the
gate doing its job — it is telling us the product tree is dirty, not that the
port is broken.

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

**Run 1 — 2026-09-25, D50T-2-Lite, 115200 8N1. Result: ABI PASS, allocator FAIL.**

Firmware banner: `built Sep 24 2026 21:36:42`, runtime
`packages/third-party/pocketjs`. Raw console in the appendix
(`board-console.txt`).

### What passed on silicon

All 34 ABI cases, in both directions:

| Group | Cases | Result |
|-------|-------|--------|
| C→Rust scalars (`u32`/`i32`/`usize`/`f32`/`f64`) | 5 | PASS |
| C→Rust pointers (`ptr_sum`, `ptr_null`, `ptr_roundtrip`, `log_len`) | 4 | PASS |
| C→Rust structs by value (`value.*`, `nested.*`) | 9 | PASS |
| C→Rust mixed scalar + pointer | 1 | PASS |
| Layout agreement (Rust vs C compiler) | 2 | PASS |
| Rust→C callbacks (`u32`/`f32`/`f64`, `value.*`, `nested.*`, `mixed`) | 13 | PASS |

The layout agreement line is the important one — both compilers independently
report the same numbers:

```
[pjs-abi]       rust: usize=4/4 f64=8/8 value=16/8 nested=32/8 ptr=4
[pjs-abi]       c:    usize=4/4 f64=8/8 value=16/8 nested=32/8 ptr=4
```

So **RV32IMAFDC / ILP32D hard-float is proven end to end on this silicon**: f64
crosses the boundary in registers and in the return slot, structs by value
follow the psABI, and both compilers agree on every size and offset.

### What failed

```
[pjs-abi] -- allocator: Box / Vec / String / over-alignment --
[pjs-abi] FATAL: rt_malloc returned 30045804, not 8-byte aligned
[pjs-abi] ABORT: Rust panic reached the host (panic=abort).
[pjs-abi] ABORT: firmware halted by design; reset the board.
```

`30045804` = `0x01CA766C` — 4-byte aligned, not 8. Root cause and fix in §5.1.

Note the abort path behaved exactly as designed: the panic reached the host and
the board halted rather than continuing in a corrupt state. That is step 0c of
the gate checklist, observed for real (albeit via an unplanned panic rather than
`pjs_abi_panic`).

### To finish Gate 0

```
python tools/build-firmware.py -j8        # produce the corrected image
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
(`pjs_host_alloc(size, align)`, which wraps `rt_malloc` and honours the
caller's alignment — see §5.1). Phase 1 repoints that seam at
`aic_memheap_malloc(MEM_PSRAM_SW)` without touching the Rust side. **The
ordinary PocketJS allocator must never consume CMA.**

Measured in the linked image: `.bss` 18,812 B, `.data` 7,740 B — no PSRAM
heap is reserved by Gate 0 itself.

---

## 8. Known Issues

1. **Gate 0 has not passed on hardware yet.** Run 1 (§6) proved the ABI on
   silicon and found one allocator defect, now fixed. The corrected image has
   **not been flashed**. This is the single blocking item.
2. **The allocator alignment defect (found on hardware, fixed, host-tested).**
   Full analysis in §5.1. Worth keeping visible because of what it says about
   the process: every static check in this report passed while both sides
   carried a hardcoded 8-byte assumption that was false for this build. Only
   execution caught it — which is the entire reason Gate 0 exists.
3. **`pjs_host_log` is unexercised.** It is declared and defined, but the
   crate never calls it, so `--gc-sections` drops it. The Rust→C log direction
   is therefore not proven. Add a `pjs_probe_call_host_log` case in Gate 1.
4. **Attribute normalisation is mandatory.** Every Rust archive must pass
   through `patch-riscv-attrs.py` before linking; `build-native.py` does this
   automatically. Skipping it reproduces the libc merge failure. If the
   toolchain is ever upgraded to binutils ≥ 2.38 this step can likely be
   dropped — re-verify before removing.
5. **`-mcmodel` differs by side.** C uses `medany`; Rust's target JSON leaves
   LLVM's default (`small`/medlow). Both reach every D13x region from absolute
   zero, so they interoperate, but the mismatch is a measured fact rather than
   an assumption.
6. **Full clean rebuild is constrained.** A from-scratch `scons -c` + rebuild
   is blocked by the environment's bulk-delete confirmation guard. The
   verified build is an incremental relink onto a partially cleaned tree. The
   link itself is fully exercised; only object recompilation is not.
7. **SDK baseline is the product SDK, not the upstream mirror** (see §1). A
   future move to a newer official baseline is a separate upgrade task that
   must re-run this gate.
8. **`reg.exe` is blocked by security policy** during SCons. The probe is
   non-fatal and the build completes, but the warning is noise.
9. **SCons is not stdlib and must be discovered.** Luban-Lite is built by
   SCons, which is installed into one specific interpreter (here: SCons 4.11.1
   in the tooling venv) - not necessarily the one that launches the build
   script. A bare `python tools/build-firmware.py` under a different
   interpreter used to fail late and confusingly with `No module named SCons`.
   `build-firmware.py` now probes for an interpreter that can `import SCons`
   (env override → current → tooling venv → `PATH`) and stops with the exact
   `pip install scons` line if none qualifies; override with
   `POCKETJS_SCONS_PYTHON`.

---

## 9. Next Gate

**Gate 0 remains OPEN.** Run 1 (§6) executed on silicon and passed the whole ABI,
but the allocator aborted, so the gate did not pass. The defect is fixed and
host-tested; what remains is one more flash.

| Step | Action | Status |
|------|--------|--------|
| 0a | Flash the image on D50T-2-Lite, capture the console | done — run 1 |
| 0b | Confirm `pjs_abi` → `RESULT PASS` on silicon | **not yet** — run 1 aborted in the allocator |
| 0c | Confirm `pjs_abi_panic` aborts and halts | partially — the abort path was observed for real, but via an unplanned panic; still needs the deliberate command |
| 0d | Record the run and update this report | done for run 1; **run 2 pending** |

Next action: flash the corrected image and re-run `pjs_abi` and `pjs_abi_panic`.

Only after 0a–0d: **Gate 1 — retained UI core (`no_std` + alloc)**, which also
closes issue 3 above.

---

## Appendix — evidence

Two runs, both under `.pocket-build/d13x/validation/gate0/` (not committed):

**Run 1 — hardware (2026-09-25)**

```
20260925T070648-hw-run1/board-console.txt   raw console from the board,
                                            with the failing pointer decoded
```

**Build — `20260925T072651/`** (the corrected image). Reproduced by the one
documented command, `python tools/build-firmware.py -j8`; nothing is
transcribed by hand.

```
images/d13x.elf            linked firmware
images/d13x.bin            flat binary            (221,500 B)
images/d13x.map            link map
images/*.img               flashable image
images/bootcfg.txt         boot config
elf-header.txt             readelf -h   (Flags: 0x5, RVC, double-float ABI)
elf-attributes.txt         readelf -A   (merged ISA attribute)
elf-pjs-symbols.txt        nm           (38 pjs_* symbols)
elf-size.txt               size         (.text 213,708 / .data 7,740 / .bss 18,812)
check-abi.txt              RESULT: PASS  (18 assertions)
check-sdk.txt              RESULT: PASS  (14 assertions, --override-sdk)
apply-sdk-check.txt        clean: the SDK matches the overlay
host-alloc-test.txt        RESULT: PASS  (19 checks, stubbed heap)
```

`build-native.py` also writes a receipt with the rustc/cargo versions, the
target-spec SHA-256 and the exact cargo command line.
