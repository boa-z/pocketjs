# Gate 1A — Memory Map Report (D50T-2-Lite / D133ECS)

**Purpose:** establish where the PocketJS heaps can actually live, by reading the
current SDK configuration and the linked firmware — **not** by assuming.

**Date:** 2026-09-25
**Config measured:** `d13x_d50t-2-lite_rt-thread_pocketjs-smoke_defconfig`
**Firmware measured:** build `20260925T074256` (`d13x.map`)

---

## 1. Headline

> **`PSRAM_SW` is not 16 MiB on this board. It is zero bytes, and
> `MEM_PSRAM_SW` does not exist as an enumerator in this build.**

The planning note in `GATE0-REPORT.md` §7 and `README.md` ("PSRAM_SW (16 MiB)")
is wrong for this configuration. Gate 1A as originally specified — repoint
`pjs_host_alloc` at `aic_memheap_malloc(MEM_PSRAM_SW, …)` — **cannot be
implemented as written** and needs a decision (§5).

---

## 2. Configuration, as read

`target/configs/d13x_d50t-2-lite_rt-thread_pocketjs-smoke_defconfig`:

```
CONFIG_AIC_PSRAM_SW_SIZE=0x0        # <-- the whole finding, in one line
CONFIG_AIC_PSRAM_SIZE=0x1000000
CONFIG_AIC_SEC_TEXT_PSRAM=y
CONFIG_AIC_SEC_RODATA_PSRAM=y
CONFIG_AIC_SEC_DATA_PSRAM=y
CONFIG_AIC_SEC_BSS_PSRAM=y
CONFIG_RT_USING_MEMHEAP_AS_HEAP=y
CONFIG_RT_ALIGN_SIZE=4
```

The same `CONFIG_AIC_PSRAM_SW_SIZE=0x0` is inherited from the **product board
baseline**, `target/configs/d13x_d50t-2-lite_rt-thread_D50T-2-Lite_defconfig:23`.
The port did not introduce it, and the port did not change it.

Resolved into `rtconfig.h`:

```
#define AIC_PSRAM_SIZE 0x1000000
#define AIC_PSRAM_CMA_EN
#define AIC_PSRAM_SW_SIZE 0x0
#define AIC_DEFAULT_SYS_HEAP_SRAM
#define AIC_SRAM0_SW_EN
```

Note what is **absent**: there is no `AIC_PSRAM_SW_EN`. It is a derived symbol
(`target/d13x/common/Kconfig.board:940`):

```kconfig
config AIC_PSRAM_SW_EN
    bool
    default y if AIC_PSRAM_SW_SIZE > 0
    depends on !AIC_NO_PSRAM
```

`AIC_PSRAM_SW_SIZE` is 0, so the symbol is never defined. That matters, because
`MEM_PSRAM_SW` is guarded on it in **both** places that matter:

| File | Guard | Effect when undefined |
|------|-------|-----------------------|
| `bsp/common/include/aic_common.h:410` | `#if defined(AIC_PSRAM_SW_EN) && !defined(AIC_DEFAULT_SYS_HEAP_PSRAM)` | `MEM_PSRAM_SW` is **not an enumerator** |
| `target/d13x/d50t-2-lite/board.c:63` | same | `heap_psram_sw` is **not registered** |

So `aic_memheap_malloc(MEM_PSRAM_SW, size)` would not even compile against this
configuration, and if it did it would find no heap.

---

## 3. The map, as linked

Symbols read from `d13x.map` of the flashed configuration:

```
__psram_start          = 0x40000000
__psram_end            = 0x41000000
__psram_cma_heap_start = 0x4003A8BC
__psram_cma_heap_end   = 0x41000000
__psram_sw_heap_start  = 0x41000000
__psram_sw_heap_end    = 0x41000000      <-- equal: zero size
__heap_start           = 0x30040000      (default system heap = SRAM)
__heap_end             = 0x30140000
__cma_heap_start       = 0x4003A8BC
__cma_heap_end         = 0x41000000
```

| Region | Base | End | Size | What it is |
|--------|------|-----|------|------------|
| PSRAM, total | `0x40000000` | `0x41000000` | **16.00 MiB** | the whole PSRAM |
| └ image (text/rodata/data/bss) | `0x40000000` | `0x4003A8BC` | 234.2 KiB | `SEC_*_PSRAM=y`, so code lives in PSRAM |
| └ `PSRAM_CMA` heap | `0x4003A8BC` | `0x41000000` | **15.77 MiB** | `MEM_CMA` — framebuffer / GE / DMA / MPP |
| └ `PSRAM_SW` heap | `0x41000000` | `0x41000000` | **0 bytes** | **does not exist** |
| SRAM default heap | `0x30040000` | `0x30140000` | **1.00 MiB** | `__heap_start`/`__heap_end`, what `rt_malloc` uses |

Cross-check against hardware: run 1's console printed
`heap before: total=1048576`. That is exactly the 1.00 MiB SRAM default heap
above, so the map reading is consistent with what the board actually reported.

### Which named heaps actually exist

`target/d13x/d50t-2-lite/board.c:49` builds `aic_memheaps[]`. Applying the
guards to this configuration:

| Entry | Registered? | Why |
|-------|-------------|-----|
| `MEM_ITCM` / `MEM_DTCM` | no | `AIC_TCM_EN` undefined |
| `MEM_SRAM0_SW` | **no** | excluded by `!defined(AIC_DEFAULT_SYS_HEAP_SRAM)` — it *is* the default heap instead |
| `MEM_SRAM1_SW` | no | `AIC_SRAM1_SW_EN` undefined (`AIC_SRAM1_SW_SIZE=0x0`) |
| `MEM_PSRAM_SW` | **no** | `AIC_PSRAM_SW_EN` undefined |
| `MEM_CMA` | **yes** | `AIC_PSRAM_CMA_EN` defined → `0x4003A8BC .. 0x41000000` |

**`MEM_CMA` is the only named memheap in this build.** Everything else is the
1 MiB default heap behind `rt_malloc`, or absent.

---

## 4. Why this blocks Gate 1A as specified

Gate 1A asked for two things that this configuration cannot satisfy:

1. **"Replace `rt_malloc`/`rt_free` with `aic_memheap_malloc(MEM_PSRAM_SW, …)`."**
   `MEM_PSRAM_SW` is not an enumerator here, and there is no PSRAM_SW heap to
   allocate from. Not a runtime failure — a compile-time one.

2. **"Ordinary PocketJS allocations must never enter CMA."**
   With PSRAM_SW at zero, the *only* PSRAM heap available is `MEM_CMA`. So the
   constraint and the available memory are mutually exclusive as things stand.

The one thing that does work today is the 1 MiB SRAM default heap — which is
what Gate 0 already uses, and which is far too small to carry the retained UI
tree, DrawList and (later) QuickJS.

---

## 5. Options

### Option A — give `PSRAM_SW` a real size (recommended)

Set `CONFIG_AIC_PSRAM_SW_SIZE` to a non-zero value in the **port's** defconfig.
The SDK already supports the whole path: the linker script carves `PSRAM_SW` out
of the top of PSRAM (`bsp/artinchip/sys/d13x/link_script/gcc_aic.ld:8`), `board.c`
registers `heap_psram_sw`, and `MEM_PSRAM_SW` becomes a real enumerator.

| | |
|---|---|
| Change | one line: `CONFIG_AIC_PSRAM_SW_SIZE=0x800000` (8 MiB) in the port's defconfig |
| Effect | `PSRAM_SW` = `0x40800000 .. 0x41000000` (8 MiB); `MEM_CMA` shrinks to ≈ 7.77 MiB |
| CMA impact | a single 800×480 RGB565 framebuffer is 768,000 B ≈ 0.73 MiB — 7.77 MiB is ample |
| Scope | board memory-map change; must be recorded, and the **base board defconfig is not touched** |

One detail worth knowing: `board.c:85` extends the PSRAM_SW heap by
`aic_get_ram_size() - AIC_PSRAM_SIZE`. If the chip is fused for more than the
configured 16 MiB, PSRAM_SW would grow beyond the configured end automatically.
The fused size is only readable on hardware, so this report does not claim it.

**Cost:** the board's memory map changes, so Gate 0's validated image is no
longer the same configuration. That is a real, recordable step — not a silent
one — and it is the reason this is being raised rather than just done.

### Option B — put the PocketJS heap in `MEM_CMA`

Works today with no config change, but **violates the explicit constraint** that
ordinary PocketJS allocations must never consume CMA. Not recommended; listed
only for completeness.

### Option C — keep the 1 MiB SRAM heap

No change, but it cannot carry Gate 1's targets, let alone QuickJS. This is the
status quo, not an option.

---

## 6. Recommendation

**Option A**, with `CONFIG_AIC_PSRAM_SW_SIZE=0x800000` as a starting point and
the exact split to be confirmed. It is the only option that satisfies both
constraints at once — a dedicated PSRAM heap for PocketJS *and* no ordinary
allocation in CMA — and it uses the SDK's own supported mechanism rather than
working around it.

Two things this needs before it is implemented:

1. **Your decision on the split.** 8 MiB SW / 7.77 MiB CMA is a suggestion; if
   the eventual framebuffer set or GE buffers need more CMA, the split should
   change now rather than later.
2. **A recorded note that the board memory map changed**, and that Gate 0's
   validated image was built under the previous map. Gate 0's ABI/allocator
   conclusions are unaffected — it never touched PSRAM — but the firmware
   configuration is not byte-identical any more.

Until this is decided, **no Gate 1A code has been written**, and the
`pjs_host_alloc(size, align)` contract from Gate 0 is left exactly as validated.

---

## 7. Evidence

Everything above is derived from two files, both reproducible:

```
target/configs/d13x_d50t-2-lite_rt-thread_pocketjs-smoke_defconfig
target/configs/d13x_d50t-2-lite_rt-thread_D50T-2-Lite_defconfig
rtconfig.h
target/d13x/common/Kconfig.board                       (lines 915-960)
bsp/common/include/aic_common.h                        (lines 395-447)
target/d13x/d50t-2-lite/board.c                        (lines 49-98)
bsp/artinchip/sys/d13x/link_script/gcc_aic.ld          (lines 1-43)

.pocket-build/d13x/validation/gate0/20260925T074256/images/d13x.map
```

Nothing here is inferred from documentation or from a sibling board. The
`d13x` linker script and `board.c` are shared across the D13x family, so other
D13x boards may well have a non-zero `PSRAM_SW`; **this** board's defconfig sets
it to zero.
