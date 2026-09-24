#!/usr/bin/env python3
"""Verify the ArtInChip Luban-Lite checkout matches the validated baseline.

The point (spec section 39): if the SDK silently differs from the baseline that
Gate 0 was proven against, every downstream result is void. So a mismatch is an
error by default, not a warning.

    python tools/check-sdk.py                 # strict
    python tools/check-sdk.py --override-sdk  # proceed anyway, but say so loudly

Checks:
  * git HEAD equals versions.toml [luban_lite].commit
  * working tree is clean (uncommitted SDK edits must be captured as patches)
  * CPUNAME / -march / -mabi / -mcmodel read from d13x/rtconfig.py
  * the memheap API the allocator will bind to exists
  * the MPP framebuffer API the renderer will bind to exists

Exit code 0 = PASS, 1 = FAIL.
"""

from __future__ import annotations

import argparse
import re
import sys
import tomllib
from pathlib import Path

import portenv as pe

FAILURES: list[str] = []
WARNINGS: list[str] = []

# Symbols the port will bind to later. Gate 0 only needs to know they are there
# and correctly named, so that Phase 1/2 are not built on a guess.
MEMHEAP_SYMBOLS = ("aic_memheap_malloc", "aic_memheap_free")
MEMHEAP_REGIONS = ("MEM_PSRAM_SW", "MEM_CMA")
MPP_SYMBOLS = ("mpp_fb_open", "AICFB_GET_SCREENINFO")

SEARCH_GLOBS = ("*.h", "*.c")


def check(cond: bool, label: str, detail: str = "") -> bool:
    if cond:
        pe.ok(label)
    else:
        pe.fail(label + (f" - {detail}" if detail else ""))
        FAILURES.append(label)
    return cond


def load_pin() -> dict:
    with open(pe.versions_toml(), "rb") as fh:
        return tomllib.load(fh)


def grep_first(root: Path, needle: str) -> tuple[Path, int, str] | None:
    """First (file, lineno, text) containing `needle`, headers searched first."""
    for glob in SEARCH_GLOBS:
        for path in sorted(root.rglob(glob)):
            try:
                with path.open("r", encoding="utf-8", errors="ignore") as fh:
                    for i, line in enumerate(fh, 1):
                        if needle in line:
                            return path, i, line.strip()
            except OSError:
                continue
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--override-sdk", action="store_true",
                    help="continue despite a baseline mismatch (records a warning)")
    args = ap.parse_args()

    pin = load_pin()
    expected_sha = pin["luban_lite"]["commit"]
    root = pe.sdk_root()

    print(f"sdk root: {root}")
    print(f"pinned  : {expected_sha}")
    print()

    # --- identity ---------------------------------------------------------
    actual_sha = pe.sdk_head_sha()
    print(f"actual  : {actual_sha}")
    if not check(actual_sha == expected_sha,
                 "SDK HEAD matches versions.toml pin",
                 f"expected {expected_sha}, got {actual_sha}"):
        if not args.override_sdk:
            print()
            print("Refusing to continue: the SDK is not the validated baseline.")
            print("Either check out the pinned commit, or re-run with --override-sdk")
            print("and record why in the gate report.")
            print()
            print(f"RESULT: FAIL ({len(FAILURES)} check(s) failed)")
            return 1
        WARNINGS.append("SDK SHA mismatch accepted via --override-sdk")

    dirty = pe.sdk_is_dirty()
    if dirty and not args.override_sdk:
        check(False, "SDK working tree is clean",
              "uncommitted SDK edits must be captured as patches/overlay")
        print()
        print("Refusing to continue: the SDK has uncommitted changes.")
        print("Regenerate sdk/patches + sdk/overlay, or re-run with --override-sdk.")
        print()
        print(f"RESULT: FAIL ({len(FAILURES)} check(s) failed)")
        return 1
    if dirty:
        WARNINGS.append("SDK working tree is dirty (accepted via --override-sdk)")
    else:
        pe.ok("SDK working tree is clean")

    # --- build flags ------------------------------------------------------
    rtconfig = root / "bsp" / "artinchip" / "sys" / "d13x" / "rtconfig.py"
    if not check(rtconfig.is_file(), "d13x rtconfig.py present", str(rtconfig)):
        print()
        print(f"RESULT: FAIL ({len(FAILURES)} check(s) failed)")
        return 1

    text = rtconfig.read_text(encoding="utf-8", errors="ignore")

    m = re.search(r"CPUNAME\s*=\s*'([^']+)'", text)
    cpu = m.group(1) if m else "(not found)"
    check(cpu == pin["target"]["cpu"], f"CPUNAME is {pin['target']['cpu']}", f"got {cpu}")

    for key, needle in (("march", "rv32imafdc"), ("mabi", "ilp32d"), ("mcmodel", "medlow")):
        want = pin["firmware_flags"][key]
        present = want in text
        check(present, f"rtconfig.py contains {key}={want}", "not found in rtconfig.py")
        if present:
            for line in text.splitlines():
                if want in line:
                    print(f"       {line.strip()[:110]}")
                    break

    # The SDK's own module build uses the plain rv32imafdc/ilp32d subset - the
    # precedent that makes the Rust target's ISA choice legitimate.
    check(pin["firmware_flags"]["m_device_march"] in text,
          "SDK itself uses the plain rv32imafdc subset (M_DEVICE)")

    # --- memory API -------------------------------------------------------
    print()
    for sym in MEMHEAP_SYMBOLS + MEMHEAP_REGIONS:
        hit = grep_first(root / "bsp", sym) or grep_first(root / "kernel", sym)
        if hit:
            path, lineno, line = hit
            pe.ok(f"{sym} found")
            print(f"       {path.relative_to(root)}:{lineno}: {line[:90]}")
        else:
            check(False, f"{sym} found", "allocator cannot bind to a missing API")

    # --- display API ------------------------------------------------------
    print()
    for sym in MPP_SYMBOLS:
        hit = grep_first(root / "bsp", sym) or grep_first(root / "packages", sym)
        if hit:
            path, lineno, line = hit
            pe.ok(f"{sym} found")
            print(f"       {path.relative_to(root)}:{lineno}: {line[:90]}")
        else:
            check(False, f"{sym} found", "renderer cannot bind to a missing API")

    print()
    for w in WARNINGS:
        print(f"WARN: {w}")
    if FAILURES:
        print(f"RESULT: FAIL ({len(FAILURES)} check(s) failed)")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
