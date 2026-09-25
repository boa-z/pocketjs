# Gate 0 Report — PocketJS on ArtInChip D13x

**Gate:** 0 — Rust RV32IMAFDC / ILP32D toolchain bridge, C↔Rust ABI, allocator
**Date:** 2026-09-24 (build) · 2026-09-25 (hardware runs 1 and 2)
**Board:** D50T-2-Lite (D133ECS, Xuantie E907FDP)
**Status:** **HARDWARE VALIDATED — GATE 0 PASS**

> Two runs on real silicon, both kept in this report.
>
> **Run 1** executed the whole C↔Rust ABI and **passed every case** — scalars,
> f32/f64 in registers, pointers, structs by value, nested structs, mixed
> scalar+pointer, layout agreement between the two compilers, and Rust→C
> callbacks — then aborted in the allocator. This board's `rt_malloc` returns
> 4-byte-aligned memory while both sides assumed 8. A genuine defect, so **run 1
> did not pass**. It is analysed in §5.1 and was fixed by making the allocator
> honour the caller's alignment.
>
> **Run 2** flashed the corrected image and printed `SUMMARY pass=47 fail=0` /
> `RESULT PASS`, with `alloc.align8`, `host.alloc_align4/8/64` all green and
> `live=0 allocs=5 frees=5 fails=0`. **Gate 0 passes.**
>
> Run 1 is deliberately not deleted. It is the only evidence that this class of
> defect existed, and it is the reason the allocator contract was redesigned
> instead of patched.
>
> Both items that were carried as **uncaptured** — the deliberate
> `pjs_abi_panic` abort and the post-reset recovery re-run — were captured on
> 2026-09-25 and both pass. Gate 0 has no open evidence gaps. See §9.1 and §9.2.

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

### What was actually validated

The pin above names the tree the Gate 0 image was built from. Both hardware runs
tested the same *code*: `a926701`, the allocator fix. The commits after it are
documentation plus the banner-identity fix (§2), which changes what the firmware
prints but not what it computes.

| | Revision |
|---|---|
| Validated code | PocketJS `a926701` / SDK `b9664c44` |
| Run 2 firmware | build `20260925T072651`, `d13x.bin` `c01f51bf…` |
| Banner-fixed build (not flashed) | build `20260925T074256`, `d13x.bin` `52d21549…` |

The two `d13x.bin` hashes differ **only** in the banner string. Both contain
`alloc.align8` and `host.alloc_align4/8/64`. The run-2 image still prints
`__DATE__`/`__TIME__`; the banner-fixed image prints the revision instead. No
computation differs between them.

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

### The banner now names a revision, not a compile time

Run 1 and run 2 printed the *same* banner (`built Sep 24 2026 21:36:42`) because
`main.c` uses `__DATE__`/`__TIME__`, which are **per-translation-unit** compile
times. `main.c` was not recompiled between the two builds, so its stamp froze,
while the rest of the firmware moved. The binary was provably the corrected one
(§6), but the banner was actively misleading evidence — and it was briefly the
only way to tell the two candidate builds apart.

`tools/apply-sdk.py` now generates
`application/rt-thread/pocketjs-smoke/pocketjs_build.h` from
`portenv.port_revision()` (`git rev-parse --short HEAD`, plus `-dirty` when the
tree has uncommitted changes), and the banner prints that. It cannot go stale
independently of the tree it names, and because the header is generated,
`apply-sdk.py --check` reports it as drift the moment the revision moves.

### SDK branch `pocketjs-d13x`

```
packages/third-party/pocketjs/            the runtime package (generated)
application/rt-thread/pocketjs-smoke/     thin Gate 0 entry point (generated)
application/rt-thread/pocketjs-smoke/pocketjs_build.h
                                          generated banner identity (see above)
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

PocketJS `d13x` (12 commits, all Conventional Commits):

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
| `d79c34c` | `docs(d13x): record hardware run 1 and the allocator fix` |
| `0fb442e` | `docs(d13x): record the SDK commit for the allocator fix` |
| `f7a871a` | `fix(d13x): stamp the firmware banner with a revision, not a compile time` |

`a926701` is the validated code revision — the one both hardware runs exercised.

SDK `pocketjs-d13x` (3 commits on top of the fork point):

| SHA | Subject |
|-----|---------|
| `16837b2d` | `feat(pocketjs): add the PocketJS runtime package and the Gate 0 application` |
| `b9664c44` | `fix(pocketjs): honour the caller's alignment in the host allocator` |
| `052da248` | `fix(pocketjs): stamp the firmware banner with a revision, not a compile time` |

This report is itself the commit `docs(d13x): close Gate 0 hardware validation`
on the PocketJS branch, so it carries no SHA of its own.

Cut from `f7572509`; the working tree carries the port only. No force-push, no
`main` modification, no `d211` derivation.

---

## 4. Build

```
python tools/build-firmware.py -j8
```

result → `Luban-Lite is built successfully`

| Artifact | Run 2 firmware (`20260925T072651`) | Current, banner-fixed (`20260925T074256`) |
|----------|-----------------------------------|-------------------------------------------|
| `d13x.elf` | 3,518,988 B | 3,518,976 B |
| `d13x.bin` | 221,500 B | 221,500 B |
| `.text` / `.data` / `.bss` | 213,708 / 7,740 / 18,812 B | 213,724 / 7,740 / 18,812 B |
| `d13x.bin` SHA-256 | `c01f51bf…` | `52d21549…` |
| Flashable image | `d13x_D50T-2-Lite_page_2k_block_128k_v1.0.0.img` | same name, rebuilt |

The two differ by 16 bytes of `.text`: the banner now formats one revision
string instead of two compile-time strings. Nothing else changed, and the
banner-fixed build has **not** been flashed — it exists to prove the banner
change compiles and links, and all eight evidence checks exit 0 on it.

`.text` has read 213,228 / 213,708 / 213,724 at different points in this work.
The figure moves with the code, so the report is pinned to the two builds named
above rather than to a single number.

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

Two runs on the D50T-2-Lite, 115200 8N1. **Both are kept**, because run 1 is the
only evidence that the allocator defect existed.

| | Run 1 | Run 2 |
|---|---|---|
| Firmware | pre-fix build | build `20260925T072651` (allocator fix) |
| Banner | `built Sep 24 2026 21:36:42` | `built Sep 24 2026 21:36:42` — **stale, see below** |
| `d13x.bin` SHA-256 | not archived | `c01f51bfda442583a34ffcda5ffe9d5a4953ad9c58fd8d9729d95f8e6f7081a4` |
| ABI cases | 34 / 34 PASS | 47 / 47 PASS |
| Allocator | **FAIL** — aborted | PASS — `live=0 fails=0` |
| Result | ABI proven, gate open | **`RESULT PASS`** |

Both runs printed the *same* banner. That is not the same build — `main.c` was
not recompiled between them, so its `__DATE__`/`__TIME__` stamp froze. Run 2's
binary is provably the corrected one (it contains `alloc.align8` and
`host.alloc_align4/8/64`, which do not exist before the fix), but the banner was
misleading and was briefly the only way to tell the two candidate builds apart.
The banner now prints a revision — see §2.

### Run 1 — ABI PASS, allocator FAIL

Runtime `packages/third-party/pocketjs`. Raw console in the appendix
(`20260925T070648-hw-run1/board-console.txt`).

#### What passed on silicon

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

#### What failed

```
[pjs-abi] -- allocator: Box / Vec / String / over-alignment --
[pjs-abi] FATAL: rt_malloc returned 30045804, not 8-byte aligned
[pjs-abi] ABORT: Rust panic reached the host (panic=abort).
[pjs-abi] ABORT: firmware halted by design; reset the board.
```

`30045804` = `0x01CA766C` — 4-byte aligned, not 8. Root cause and fix in §5.1.

Note the abort path behaved exactly as designed: the panic reached the host and
the board halted rather than continuing in a corrupt state. That is the
`panic = abort` half of step 0c, observed for real — albeit via an unplanned
panic rather than the deliberate `pjs_abi_panic` command.

### Run 2 — `RESULT PASS`

The corrected image was flashed and the probe re-run.

```
[pjs-abi] SUMMARY pass=47 fail=0
[pjs-abi] RESULT PASS
```

The 13 cases that run 1 never reached all passed, including the ones added with
the fix:

| Case | What it proves |
|------|----------------|
| `alloc.align8` | a Rust `Box<f64>` allocation really is 8-byte aligned |
| `host.alloc_align4` | the host service returns 4-aligned memory for align 4 |
| `host.alloc_align8` | …for align 8 |
| `host.alloc_align64` | …for align 64, i.e. the over-alignment path |

Allocator telemetry at the end of the run:

```
live=0  peak=256  allocs=5  frees=5  fails=0
```

`live=0` with `allocs == frees` is the meaningful part: every block the Rust
`GlobalAlloc` handed out came back, and nothing failed. `peak=256` is the
high-water mark of live bytes the probe reached — small, because the probe
allocates a handful of objects, not because the heap is capped.

`host.alloc_*` is deliberately checked **with no Rust in the loop**, so a
regression in the host contract cannot hide behind the `GlobalAlloc` wrapper.

#### Evidence provenance — read this before quoting run 2

Run 2's console was **not captured to disk**. The operator pasted the result into
the review session; the full log was never written out. The record in
`.pocket-build/d13x/validation/gate0/20260925T072651-hw-run2/board-console.txt`
therefore separates `[reported]` (stated by the operator) from `[observed]`
(independently confirmed from the archived build artifacts), and marks
unreported details as unknown rather than filling them in.

What *is* independently confirmed:

- `20260925T072651/images/d13x.bin` contains `alloc.align8` and
  `host.alloc_align4`, neither of which exists in the pre-fix source, so the
  binary under test really was the corrected one.
- The banner `Sep 24 2026 21:36:42` matches `20260925T072651` exactly and cannot
  be produced by the later banner-fixed build, which pins which image was
  flashed.

#### Not captured in run 2 — both since closed

Two items from the Gate 0 closeout checklist were **not** performed in run 2 and
are flagged as open rather than assumed:

1. **The deliberate `pjs_abi_panic`.** The abort path was observed in run 1, but
   as a side effect of the allocator defect, not by invoking the command that
   exists to exercise it. This matters because `pjs_abi_panic` is the only thing
   that proves the abort path works *on demand* rather than by accident.
2. **The post-reset recovery re-run.** "Panic → board halts → reset → `pjs_abi`
   passes again" has not been demonstrated end to end.

Neither affects the allocator verdict, and neither is a defect. They are
unverified claims that this report declines to make. To close them:

```
aic /> pjs_abi_panic    # expect: abort, board halts, no return
# press reset
aic /> pjs_abi          # expect: SUMMARY pass=47 fail=0 / RESULT PASS
```

> **Both were closed on 2026-09-25** by the capture recorded in §9.1 and §9.2:
> the deliberate panic aborted and halted the board, and `pjs_abi` printed
> `pass=47 fail=0` after a reset. The text above is left as the run-2 record.

### Why Gate 0 passes

The gate's stated criterion is that `pjs_abi` prints `RESULT PASS` on real
silicon. Run 2 does exactly that: **47 of 47 checks pass**, including the
allocator cases that aborted in run 1.

The `panic = abort` half was also observed for real in run 1 — the Rust panic
reached the host and the board halted instead of continuing in a corrupt state.
That is the behaviour the gate asks for. What was *not* yet demonstrated at run 2
was the same path triggered **on demand** by `pjs_abi_panic`, and recovery after
a reset; **both were captured on 2026-09-25 and pass (§9.1, §9.2)**.

So: **GATE 0 PASS**, with two claims that were left explicitly unverified at run
2 rather than silently assumed, and have since been verified.

---

## 7. Memory

**These are planning figures, not measurements** — kept as written, because they
are what Gate 0 was designed against.

> **Superseded.** Gate 1A read the real numbers from the SDK config and the
> linked image, and the `PSRAM_SW` guess below was wrong: it was **zero bytes**
> on this board, not 16 MiB, and `MEM_PSRAM_SW` was not even an enumerator. It is
> now 8 MiB, enabled in the port's own defconfig. The measured map is in
> [GATE1A-MEMORY-MAP.md](GATE1A-MEMORY-MAP.md); do not plan against the block
> below.

```
SRAM (1 MiB)       RT-Thread kernel, interrupts, thread stacks,
                   driver state, hot native state
PSRAM_SW (16 MiB?) PocketJS Rust heap, QuickJS heap, UI tree,
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

1. ~~**Two Gate 0 evidence gaps remain.**~~ **Resolved 2026-09-25.** At run 2,
   the deliberate `pjs_abi_panic` and the post-reset recovery re-run had not been
   performed. The abort path *was* observed for real in run 1, as the side effect
   of the allocator defect — so the mechanism was proven, but not its on-demand
   invocation. Both were subsequently captured and both pass; see §9.1 and §9.2.
2. **The allocator alignment defect (found on hardware, fixed, host-tested).**
   Full analysis in §5.1. Worth keeping visible because of what it says about
   the process: every static check in this report passed while both sides
   carried a hardcoded 8-byte assumption that was false for this build. Only
   execution caught it — which is the entire reason Gate 0 exists.
3. **The banner was stale and nearly made run 2 unattributable.** Both runs
   printed the same `__DATE__`/`__TIME__` stamp. Fixed by generating a revision
   header (§2). Keep this in mind for any future build-vs-flash comparison: a
   banner is only evidence if it is derived from the tree, not from when one
   object file happened to be compiled.
4. **`pjs_host_log` is unexercised.** It is declared and defined, but the
   crate never calls it, so `--gc-sections` drops it. The Rust→C log direction
   is therefore not proven. Add a `pjs_probe_call_host_log` case in Gate 1.
5. **Attribute normalisation is mandatory.** Every Rust archive must pass
   through `patch-riscv-attrs.py` before linking; `build-native.py` does this
   automatically. Skipping it reproduces the libc merge failure. If the
   toolchain is ever upgraded to binutils ≥ 2.38 this step can likely be
   dropped — re-verify before removing.
6. **`-mcmodel` differs by side.** C uses `medany`; Rust's target JSON leaves
   LLVM's default (`small`/medlow). Both reach every D13x region from absolute
   zero, so they interoperate, but the mismatch is a measured fact rather than
   an assumption.
7. **Full clean rebuild is constrained.** A from-scratch `scons -c` + rebuild
   is blocked by the environment's bulk-delete confirmation guard. The
   verified build is an incremental relink onto a partially cleaned tree. The
   link itself is fully exercised; only object recompilation is not.
8. **SDK baseline is the product SDK, not the upstream mirror** (see §1). A
   future move to a newer official baseline is a separate upgrade task that
   must re-run this gate.
9. **`reg.exe` is blocked by security policy** during SCons. The probe is
   non-fatal and the build completes, but the warning is noise.
10. **SCons is not stdlib and must be discovered.** Luban-Lite is built by
   SCons, which is installed into one specific interpreter (here: SCons 4.11.1
   in the tooling venv) - not necessarily the one that launches the build
   script. A bare `python tools/build-firmware.py` under a different
   interpreter used to fail late and confusingly with `No module named SCons`.
   `build-firmware.py` now probes for an interpreter that can `import SCons`
   (env override → current → tooling venv → `PATH`) and stops with the exact
   `pip install scons` line if none qualifies; override with
   `POCKETJS_SCONS_PYTHON`.

---

## 9. Gate Status

**Gate 0: PASS — closed.** Every step below is now captured on real silicon,
including the two that were carried as open evidence gaps.

| Step | Action | Status |
|------|--------|--------|
| 0a | Flash the image on D50T-2-Lite, capture the console | **done** — run 1, run 2, then run 3 |
| 0b | Confirm `pjs_abi` → `RESULT PASS` on silicon | **done** — run 2 and run 3: `pass=47 fail=0` |
| 0c | Confirm the `panic = abort` path halts the board | **done** — deliberate `pjs_abi_panic`, 2026-09-25 |
| 0d | Record the runs and update this report | **done** — runs 1–3, none deleted |
| 0e | Post-reset recovery re-run | **done** — 2026-09-25: `pass=47 fail=0` after a reset |

### 9.1 Step 0c — the deliberate abort

Captured at the prompt:

```
aic /> pjs_abi_panic
[pjs-abi] invoking Rust panic; expect an abort and a halted board
[pjs-abi] ABORT: Rust panic reached the host (panic=abort).
[pjs-abi] ABORT: firmware halted by design; reset the board.
```

Both `ABORT:` lines appear, `FAIL  panic.returned` does not, and no prompt
returns. The halt is structural rather than incidental: `pjs_host_abort()`
(`src/pocketjs_host.c:95-107`) prints those two lines, calls
`rt_enter_critical()` and spins in a bare `for (;;)`. With interrupts off no
further console output is possible, so the absent prompt cannot be an artifact
of a truncated capture.

The first line is the stronger of the two: it reports the panic arriving at the
*host* hook, so what is observed is the FFI handoff under `panic=abort`, not
merely a message being printed.

### 9.2 Step 0e — recovery after the reset

```
aic /> pjs_abi
[pjs-abi] SUMMARY pass=47 fail=0
[pjs-abi] RESULT PASS
aic />
```

The returning `aic />` prompt is the positive evidence: the board is reachable
and executing again after the halt, rather than wedged.

Two readings differ from run 3's autorun. Both are expected, and both reconcile
exactly:

- **`allocs=4615`, where run 3's `pjs_abi` printed `allocs=5`.** The allocator
  counters are cumulative for the life of a boot and are never reset in firmware
  — `pjs_host_alloc_stats_reset()` has no caller on target, only in the host
  test. This `pjs_abi` was typed *after* the boot autorun had already run both
  probes, so it reports what those left behind plus its own. The arithmetic
  closes: the autorun's `pjs_mem_test` printed `allocs=4610`, which is the
  autorun's `pjs_abi` (5) plus its own stress (4605), and 5 + 4605 + 5 = 4615.
- **`heap before: used=14196 max_used=22556`, where run 3 read `used=22556
  max_used=22556`.** This is a one-instant sample of the RT-Thread system heap,
  and the two captures sample at different points: run 3 read it during boot
  (the SDMC driver had not finished — its message printed at `[0.906]`), this
  one after boot had settled. That `max_used` is `22556` in **both** is the
  proof they share the same peak; only the sampling instant moved.

Neither reading bears on the Gate 1A claim, which rests on the *delta* across
the stress run (`sram used 22556 -> 22556`) and not on any absolute heap figure.

### 9.3 Why these needed an operator

`pjs_abi_panic` is the one probe deliberately **not** auto-run at boot: it halts
the board, so autorunning it would turn every boot into a halt. Neither item
could therefore be closed by a boot capture — both needed an operator at the
prompt. The prompt on this board is `aic />`, not `msh />`; earlier revisions of
this report and of the port README printed `msh />`, which never appears on
target. Corrected.

### Next: Gate 1 — retained UI core

Gate 1 is the `no_std` + `alloc` retained UI core, and it closes issue 4 above
(`pjs_host_log`) on the way. **No RGB565 renderer, no QuickJS, no framebuffer
and no GE work starts until Gate 1 is reviewed and passed.**

**Gate 1A (the memory map and the allocator backend) is done — PASS on hardware,
2026-09-25.** Its first task, the memory-map report read from the current SDK
config rather than from assumption, is
[GATE1A-MEMORY-MAP.md](GATE1A-MEMORY-MAP.md). §7's `PSRAM_SW (16 MiB)` was indeed
wrong: it was zero bytes, and is now 8 MiB in the port's own defconfig.

**Gate 1B–1C is what remains of Gate 1**, and has not started.

---

## Appendix — evidence

Everything below lives under `.pocket-build/d13x/validation/gate0/` and is
**not committed**.

### Hardware runs

```
20260925T070648-hw-run1/
  board-console.txt        raw console from the board, with the failing
                           pointer decoded

20260925T072651-hw-run2/
  board-console.txt        run-2 result. NOT a raw capture - the full console
                           was never written to disk, so this file separates
                           [reported] from [observed] and marks the rest unknown
  firmware.txt             which image this was and why, with SHA-256s
```

Run 1 is a verbatim capture. Run 2 is a provenance record, and says so — the
distinction is deliberate rather than cosmetic.

### Builds

**`20260925T072651/` — the flashed (run 2) image.**

```
images/d13x.elf            linked firmware       (3,518,988 B, sha ce1b64a2…)
images/d13x.bin            flat binary           (221,500 B, sha c01f51bf…)
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

**`20260925T074256/` — the banner-fixed image (not flashed).** Same eight checks,
all exit 0; `d13x.bin` sha `52d21549…`, `.text` 213,724. It differs from the
flashed image only in the banner string.

Both are reproduced by the one documented command,
`python tools/build-firmware.py -j8`; nothing is transcribed by hand.

`build-native.py` also writes a receipt with the rustc/cargo versions, the
target-spec SHA-256 and the exact cargo command line.

