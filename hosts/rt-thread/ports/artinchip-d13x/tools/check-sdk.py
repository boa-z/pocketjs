#!/usr/bin/env python3
"""Verify the Luban-Lite checkout matches the validated baseline.

The point (spec section 39): if the SDK silently differs from the baseline that
Gate 0 was proven against, every downstream result is void. So a mismatch is an
error by default, not a warning.

    python tools/check-sdk.py                 # strict
    python tools/check-sdk.py --override-sdk  # proceed anyway, but say so loudly

Checks:
  * git branch is versions.toml [luban_lite].port_branch
  * git HEAD descends from versions.toml [luban_lite].base_commit (the fork
    point stays in history; HEAD is expected to be ahead of it on the port
    branch, so equality is not required)
  * every uncommitted change is confined to the port's own paths - the product
    line must not be carrying stray edits
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

# Everything the port is allowed to touch on its branch. Anything else showing
# up as modified means the product line is drifting under the port's feet.
PORT_PATHS = (
    "application/rt-thread/pocketjs-smoke/",
    "packages/third-party/pocketjs/",
    "target/configs/d13x_d50t-2-lite_rt-thread_pocketjs-smoke_defconfig",
    "packages/third-party/Kconfig",
)


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


def squash_concatenations(text: str) -> str:
    """Fold Python string concatenation so flag literals become searchable.

    rtconfig.py builds the ISA flag rather than spelling it out:

        DEVICE = ' -march=rv32imafdcpzpsfoperand' + ISA_TAG + '_xtheade -mabi=ilp32d'

    A literal substring search for the assembled flag therefore fails even
    though the SDK really does compile with it. Removing quotes and `+` (and
    resolving ISA_TAG, which is empty for the V2.6.1 toolchain) recovers the
    assembled form.
    """
    out = re.sub(r"['\"]\s*\+\s*", "", text)
    out = re.sub(r"\s*\+\s*['\"]", "", out)
    return out.replace("ISA_TAG", "")


def find_flag(text: str, squashed: str, needle: str) -> str | None:
    """Line containing `needle`, preferring the squashed (assembled) view."""
    for blob in (squashed, text):
        for line in blob.splitlines():
            if needle in line:
                return line.strip()
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--override-sdk", action="store_true",
                    help="continue despite a baseline mismatch (records a warning)")
    args = ap.parse_args()

    pin = load_pin()
    expected_sha = pin["luban_lite"]["base_commit"]
    want_branch = pin["luban_lite"]["port_branch"]
    root = pe.sdk_root()

    print(f"sdk root: {root}")
    print(f"pinned  : {expected_sha}  (branch {want_branch})")
    print()

    # --- identity ---------------------------------------------------------
    branch = pe.sdk_branch()
    check(branch == want_branch,
          f"SDK is on the port branch '{want_branch}'",
          f"on '{branch}' - port work must not land on the product line")

    actual_sha = pe.sdk_head_sha()
    print(f"actual  : {actual_sha}  (branch {branch})")

    # base_commit is the fork point, not the tip. On the port branch HEAD is
    # *expected* to be ahead of it, so requiring equality would fail the moment
    # the port's own work is committed. What must hold is that the pinned
    # baseline is still in this branch's history - otherwise the port sits on
    # the wrong tree, or the history was rewritten (both forbidden).
    if actual_sha == expected_sha:
        pe.ok("SDK HEAD is the pinned baseline commit")
    elif pe.sdk_head_descends_from(expected_sha):
        ahead = pe.sdk_commits_ahead(expected_sha)
        pe.ok("SDK HEAD descends from the pinned baseline")
        print(f"       ({ahead} port commit(s) on top of {expected_sha[:12]})")
    else:
        detail = (
            f"base_commit {expected_sha[:12]} is not an ancestor of HEAD "
            f"{actual_sha[:12]} - wrong baseline, or rewritten history"
        )
        if args.override_sdk:
            WARNINGS.append(f"baseline mismatch accepted via --override-sdk: {detail}")
        else:
            check(False, "SDK HEAD descends from versions.toml base_commit", detail)
            print()
            print("Refusing to continue: the SDK is not the validated baseline.")
            print("Either check out the pinned commit, or re-run with --override-sdk")
            print("and record why in the gate report.")
            print()
            print(f"RESULT: FAIL ({len(FAILURES)} check(s) failed)")
            return 1

    # The port branch is expected to differ from base_commit - that is the whole
    # point of it. What must not happen is a change outside the port's own paths.
    entries = pe.sdk_dirty_entries()
    stray = [
        e for e in entries
        if not any(e[3:].strip().strip('"').startswith(p) for p in PORT_PATHS)
    ]
    if stray:
        if args.override_sdk:
            WARNINGS.append(
                f"{len(stray)} change(s) outside the port's paths, accepted via "
                f"--override-sdk: {stray[:3]}")
            print(f"WARN: {len(stray)} change(s) outside the port's paths:")
            for e in stray[:5]:
                print(f"       {e}")
        else:
            check(False, "all SDK changes are confined to the port's paths",
                  f"{len(stray)} stray change(s): {stray[:3]}")
    else:
        pe.ok("all SDK changes are confined to the port's paths")
        print(f"       ({len(entries)} tracked change(s), all within the port)")

    # --- build flags ------------------------------------------------------
    rtconfig = root / "bsp" / "artinchip" / "sys" / "d13x" / "rtconfig.py"
    if not check(rtconfig.is_file(), "d13x rtconfig.py present", str(rtconfig)):
        print()
        print(f"RESULT: FAIL ({len(FAILURES)} check(s) failed)")
        return 1

    text = rtconfig.read_text(encoding="utf-8", errors="ignore")
    squashed = squash_concatenations(text)

    m = re.search(r"CPUNAME\s*=\s*'([^']+)'", text)
    cpu = m.group(1) if m else "(not found)"
    check(cpu == pin["target"]["cpu"], f"CPUNAME is {pin['target']['cpu']}", f"got {cpu}")

    for key in ("march", "mabi", "mcmodel"):
        want = pin["firmware_flags"][key]
        line = find_flag(text, squashed, want)
        check(line is not None, f"rtconfig.py contains {key}={want}",
              "not found in rtconfig.py")
        if line:
            print(f"       {line[:110]}")

    # The SDK's own module build uses the plain rv32imafdc/ilp32d subset - the
    # precedent that makes the Rust target's ISA choice legitimate.
    check(pin["firmware_flags"]["m_device_march"] in squashed,
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
