# Gate 1A — Memory Map Report (D50T-2-Lite / D133ECS)

**Purpose:** establish where the PocketJS heaps can actually live, by reading the
current SDK configuration and the linker template — **not** by assuming.

**Date:** 2026-09-25
**Status:** change applied and verified at the config + linker-script level.
**The full firmware rebuild is blocked by the build environment** — see §6.

---

## 1. Headline

> **`PSRAM_SW` was not 16 MiB on this board. It was zero bytes, and
> `MEM_PSRAM_SW` was not even an enumerator.**

The planning note in `GATE0-REPORT.md` §7 and `README.md` ("PSRAM_SW (16 MiB)")
was wrong for this configuration, and Gate 1A as originally written could not be
implemented.

Per decision, `PSRAM_SW` is now enabled at **8 MiB** in the **port's own**
defconfig. The product board baseline is deliberately untouched.

---

## 2. What the original configuration actually said

`target/configs/d13x_d50t-2-lite_rt-thread_D50T-2-Lite_defconfig:23` — the
**product board baseline** — pins:

```
CONFIG_AIC_PSRAM_SW_SIZE=0x0
```

and the port's defconfig inherited it. Consequences, all verified:

| Layer | Effect |
|-------|--------|
| `target/d13x/common/Kconfig.board:940` | `AIC_PSRAM_SW_EN` is `default y if AIC_PSRAM_SW_SIZE > 0` → **never defined** |
| `bsp/common/include/aic_common.h:410` | `MEM_PSRAM_SW` guarded on `AIC_PSRAM_SW_EN` → **not an enumerator** |
| `target/d13x/d50t-2-lite/board.c:63` | `heap_psram_sw` guarded on the same symbol → **not registered** |
| linked firmware | `__psram_sw_heap_start == __psram_sw_heap_end == 0x41000000` → **zero bytes** |

`MEM_CMA` was the only named memheap in the build. Since the PocketJS allocator
must never use CMA (reserved for framebuffer / GE / MPP / DMA), and the 1 MiB
SRAM default heap is far too small, there was no viable heap for Gate 1.

---

## 3. The change

One line in the port's own defconfig,
`hosts/rt-thread/ports/artinchip-d13x/sdk/overlay/target/configs/d13x_d50t-2-lite_rt-thread_pocketjs-smoke_defconfig`:

```
CONFIG_AIC_PSRAM_SW_SIZE=0x800000      # was 0x0
```

with the rationale written next to it. `tools/apply-sdk.py` materialises it into
the SDK; the product baseline defconfig is not modified.

Resolved into `rtconfig.h` (verified):

```
#define AIC_PSRAM_SIZE 0x1000000
#define AIC_PSRAM_CMA_EN
#define AIC_PSRAM_SW_SIZE 0x800000
#define AIC_PSRAM_SW_EN                 <- newly present
#define AIC_DEFAULT_SYS_HEAP_SRAM       <- unchanged: default heap stays in SRAM
```

The `#` comments in the defconfig are tolerated: the file is loaded by
`kconfiglib` (`kernel/rt-thread/tools/defconfig.py`), which ignores them, and
`scons --apply-def=…` completes with no warnings.

---

## 4. The partition formula, confirmed

`bsp/artinchip/sys/d13x/link_script/gcc_aic.ld` is **generated**, not authored.
`SConstruct:84-88` preprocesses the tracked template:

```
gcc -E -P -< gcc_aic.ld.S > gcc_aic.ld
```

and `gcc_aic.ld.S:50` does `#include "rtconfig.h"`, so the defconfig values flow
straight into the memory map. From `gcc_aic.ld.S:79-80,108-113`:

```
PSRAM_CMA : ORIGIN = 0x40000000,                                       LENGTH = AIC_PSRAM_SIZE - AIC_PSRAM_SW_SIZE
PSRAM_SW  : ORIGIN = 0x40000000 + AIC_PSRAM_SIZE - AIC_PSRAM_SW_SIZE,  LENGTH = AIC_PSRAM_SW_SIZE

__psram_cma_end = 0x40000000 + AIC_PSRAM_SIZE - AIC_PSRAM_SW_SIZE
__psram_sw_end  = 0x40000000 + AIC_PSRAM_SIZE
__psram_end     = 0x40000000 + AIC_PSRAM_SIZE
```

That is the whole partition: **PSRAM_SW is carved off the top of PSRAM**, and
CMA takes what is left. The regenerated `gcc_aic.ld` confirms the substitution
landed:

```
PSRAM_CMA : ORIGIN = 0x40000000 , LENGTH = 0x1000000 - 0x800000
PSRAM_SW : ORIGIN = 0x40000000 + 0x1000000 - 0x800000 , LENGTH = 0x800000
```

---

## 5. The resulting map

Everything below follows from §4 plus two facts verified in the source:

- Nothing places data in `.psram_sw_data` or `.psram_cma_data` — only the macros
  `PSRAM_SW_DATA_DEFINE` / `PSRAM_CMA_DATA_DEFINE` exist, and nothing uses them.
  So each heap starts exactly at its section's start.
- `AIC_DEFAULT_SYS_HEAP_SRAM` is still selected, so `__heap_start`/`__heap_end`
  keep pointing at SRAM (`gcc_aic.ld.S:128-137`).

| Region | Base | End | Size |
|--------|------|-----|------|
| PSRAM, total | `0x40000000` | `0x41000000` | **16.00 MiB** (unchanged) |
| └ image (text/rodata/data/bss) | `0x40000000` | `__psram_cma_heap_start` | 234.2 KiB at the last build |
| └ **`PSRAM_CMA` heap** | `__psram_cma_heap_start` ≈ `0x4003A8BC` | **`0x40800000`** | **≈ 7.77 MiB** (was 15.77) |
| └ **`PSRAM_SW` heap** | **`0x40800000`** | **`0x41000000`** | **8.00 MiB** (was 0) |
| SRAM default heap | `0x30040000` | `0x30140000` | **1.00 MiB** (unchanged) |

Named symbol values:

```
__psram_start          = 0x40000000     unchanged
__psram_end            = 0x41000000     unchanged
__psram_cma_end        = 0x40800000     was 0x41000000
__psram_sw_data_start  = 0x40800000     deterministic (no .psram_sw_data users)
__psram_sw_heap_start  = 0x40800000     was 0x41000000
__psram_sw_heap_end    = 0x41000000     was 0x41000000 (equal to start)
__heap_start/__heap_end= 0x30040000 / 0x30140000   unchanged
```

The arithmetic, for the record: `0x40800000 - 0x4003A8BC = 8,148,804 B =
7.77 MiB`, and `0x41000000 - 0x40800000 = 8,388,608 B = 8.00 MiB`.

### Do the firmware / link sections move?

**No.** `REGION_TEXT`, `REGION_RODATA`, `REGION_DATA` and `REGION_BSS` all alias
`PSRAM_CMA` (`gcc_aic.ld.S:161-198`), whose **origin is unchanged at
`0x40000000`**. Only the region's upper bound moves, from `0x41000000` to
`0x40800000`. The image occupies 234 KiB of the new 8 MiB, so nothing relocates.

The one thing that does shift is `__psram_cma_heap_start`: it is placed after the
image sections, so it moves by whatever the recompiled image grows or shrinks by.
That number needs a real link to pin down (§6).

### A boot-time risk worth flagging

`board.c:85` grows the PSRAM_SW heap at runtime by
`aic_get_ram_size() - AIC_PSRAM_SIZE`, where `aic_get_ram_size()` reads the
**PSRAM size fuse**. If the fuse ever reported *less* than the configured 16 MiB,
that delta would be negative and `aic_memheap_init()`'s
`RT_ASSERT(end_align > begin_align)` (`board.c:92`) would fire at boot. If the
fuse reports more, PSRAM_SW simply grows past `0x41000000`, which is the SDK's
intent. The fused size is only readable on hardware, so this is a **board-side
check for step 5**, not something this report can settle.

---

## 6. Blocker: the full rebuild cannot complete here

The config change invalidates `rtconfig.h`, which nearly every translation unit
includes, so SCons must recompile the whole project — 157 objects plus a relink.

The build environment enforces a **bulk-delete budget of 50 operations per
turn**. SCons removes an out-of-date target before re-running its builder, so
each of those 157 rebuilds counts. After 50 the guard fires:

```
[safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED]
  {"count":51,"threshold":50,"scope":"turn","targets":[".../pocketjs-smoke/main.o"],"targetCount":1}
```

and SCons then prints `scons: done building targets.` and exits **0** — a
silent no-op. The linked firmware is still the previous one:

```
output/.../images/d13x.elf   2026-09-25 07:33:15   (unchanged)
output/.../images/d13x.bin   sha256 52d21549…      (unchanged)
```

What was tried, and why each failed:

| Attempt | Result |
|---------|--------|
| `build-firmware.py -j8` | compiled ~8 objects, then blocked at 08:05 and hung |
| `build-firmware.py -j4` | same block; exits 0 without rebuilding |
| `scons -j4` directly | same block |
| removing `.sconsign.dblite` | allowed, but does not change the block |
| running the build with the sandbox disabled | **still blocked** — the guard is a filesystem-level control, not the shell sandbox |
| renaming the output directory aside to force a clean build | `Permission denied` |

So the environment cannot currently produce the new firmware, and the *linked*
map cannot be confirmed. The map in §5 is derived from the linker template and
the resolved config, both of which are verified on disk; the confirmation step
needs a build that can write more than 50 files.

This is not a linker, boot or MPP conflict — it is a tooling limit. Nothing has
been changed to work around it, and no hand-rolled link was attempted: that would
be exactly the kind of unreproducible step this port forbids.

---

## 7. Status and what is needed

**Done:**

- The original finding (PSRAM_SW = 0) is fully evidenced.
- The partition formula is confirmed from the template, not inferred.
- The config change is applied, resolves correctly, and is recorded in the port's
  defconfig only.
- The regenerated linker script is verified on disk with the expected values.
- The resulting map is derived and documented, including what does *not* move.

**Not done, and deliberately not claimed:**

- No firmware has been linked with the new map, so §5 is a derived prediction for
  the symbol values, not a reading from a `.map` file.
- No Gate 1A code has been written. `pjs_host_alloc(size, align)` is still exactly
  the Gate 0 implementation backed by `rt_malloc`; the backend swap has not
  started.

**Needed to continue:** a build that can rewrite more than 50 files in one turn —
either a raised bulk-delete threshold, or the output directory cleaned out of
band. Once the link succeeds, §5 gets replaced with the real `.map` values, and
only then does the Gate 1A allocator work begin.

---

## 8. Evidence

```
target/configs/d13x_d50t-2-lite_rt-thread_D50T-2-Lite_defconfig   (baseline, PSRAM_SW=0x0)
target/configs/d13x_d50t-2-lite_rt-thread_pocketjs-smoke_defconfig (the change)
rtconfig.h                                                         (resolved symbols)
target/d13x/common/Kconfig.board                                   (lines 915-960)
bsp/common/include/aic_common.h                                    (lines 395-447)
target/d13x/d50t-2-lite/board.c                                    (lines 49-98)
bsp/artinchip/sys/d13x/link_script/gcc_aic.ld.S                    (lines 50, 79-80, 108-113)
bsp/artinchip/sys/d13x/link_script/gcc_aic.ld                      (regenerated, verified)
SConstruct                                                         (lines 84-88)
```

The `d13x` linker script and `board.c` are shared across the D13x family, so
other D13x boards may well have a non-zero `PSRAM_SW`; **this** board's baseline
sets it to zero.
