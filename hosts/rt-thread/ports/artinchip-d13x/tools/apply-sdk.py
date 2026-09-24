#!/usr/bin/env python3
"""Materialise the PocketJS D13x overlay into the Luban-Lite checkout.

The PocketJS repo is the single source of truth for the port. This script is
the only sanctioned way those sources reach the SDK tree, which is what keeps
the ``pocketjs-d13x`` branch a *derived*, reviewable snapshot instead of a
second copy that silently drifts.

What it writes into the SDK:

  packages/third-party/pocketjs/       from sdk/overlay/, plus the vendored port
                                       sources (ABI header, host glue, Rust
                                       crate) so the package rebuilds in place
  application/rt-thread/pocketjs-smoke/ the thin Gate 0 entry point
  target/configs/…_pocketjs-smoke_defconfig
  packages/third-party/Kconfig         one injected `source` line

It never touches the Rust archive: ``lib/`` receives only a ``.gitignore`` so a
built ``.a`` can be dropped there without ever being committable.

Usage:
    python tools/apply-sdk.py              # apply
    python tools/apply-sdk.py --check      # report drift, change nothing
    python tools/apply-sdk.py --force      # apply even off the port branch
"""

from __future__ import annotations

import argparse
import filecmp
import shutil
import sys
from pathlib import Path

import portenv as env

OVERLAY_DIR = env.PORT_ROOT / "sdk" / "overlay"

PKG_REL = Path("packages") / "third-party" / "pocketjs"
APP_REL = Path("application") / "rt-thread" / "pocketjs-smoke"

# Port sources vendored into the SDK package so it is self-contained: someone
# with only the SDK branch can rebuild the Rust half without cloning PocketJS.
VENDORED = {
    PKG_REL / "include" / "pocketjs_d13x.h": env.PORT_ROOT / "include" / "pocketjs_d13x.h",
    PKG_REL / "src" / "pocketjs_host.c": env.PORT_ROOT / "src" / "pocketjs_host.c",
    PKG_REL / "rust" / "rust-toolchain.toml": env.PORT_ROOT / "rust" / "rust-toolchain.toml",
    PKG_REL / "rust" / "targets" / "d13x-e907-ilp32d.json":
        env.PORT_ROOT / "rust" / "targets" / "d13x-e907-ilp32d.json",
    PKG_REL / "rust" / "abi-probe" / "Cargo.toml":
        env.PORT_ROOT / "rust" / "abi-probe" / "Cargo.toml",
    PKG_REL / "rust" / "abi-probe" / "src" / "lib.rs":
        env.PORT_ROOT / "rust" / "abi-probe" / "src" / "lib.rs",
}

# Idempotent one-line injections into existing SDK files. Replacing whole SDK
# files would make this overlay fight every upstream change; inserting a single
# marked line does not.
INJECTIONS = [
    (
        Path("packages") / "third-party" / "Kconfig",
        'source "packages/third-party/pocketjs/Kconfig"',
        'source "packages/third-party/at24cxx/Kconfig"',
    ),
]

# Paths produced by earlier revisions of this overlay. Removed if present so a
# stale copy can never be silently compiled alongside the current one.
STALE = [
    APP_REL / "abi_probe_host.c",
    APP_REL / "pocketjs_d13x.h",
    APP_REL / "lib",
]

LIB_GITIGNORE = """\
# Rust staticlib produced by the PocketJS tooling. Never commit a build
# artifact: Gate 0 must stay reproducible from source.
*.a
*.a.tmp
"""


def _rel(p: Path) -> str:
    return str(p).replace("\\", "/")


def _sync_file(src: Path, dst: Path, check: bool) -> str:
    """Copy src -> dst. Returns 'create' | 'update' | 'same' | 'drift'."""
    if not src.is_file():
        raise SystemExit(f"overlay source missing: {src}")

    if dst.is_file() and filecmp.cmp(src, dst, shallow=False):
        return "same"
    existed = dst.is_file()

    if check:
        return "drift"

    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    return "update" if existed else "create"


def _overlay_files() -> list[tuple[Path, Path]]:
    """(source, destination-relative-to-SDK) pairs from the overlay tree."""
    pairs: list[tuple[Path, Path]] = []
    for src in sorted(OVERLAY_DIR.rglob("*")):
        if not src.is_file():
            continue
        # .gitkeep only keeps empty overlay directories in git; it is
        # scaffolding for this repo and must never reach the SDK.
        if src.name == ".gitkeep":
            continue
        pairs.append((src, src.relative_to(OVERLAY_DIR)))
    return pairs


def _apply_injection(sdk: Path, rel: Path, line: str, anchor: str, check: bool) -> str:
    """Ensure `line` appears after `anchor` in sdk/rel. Returns a status."""
    path = sdk / rel
    if not path.is_file():
        raise SystemExit(f"injection target missing: {path}")

    text = path.read_text(encoding="utf-8")
    if line in text.splitlines():
        return "same"
    if check:
        return "drift"

    lines = text.splitlines(keepends=True)
    for i, existing in enumerate(lines):
        if existing.strip() == anchor:
            indent = existing[: len(existing) - len(existing.lstrip())]
            lines.insert(i + 1, f"{indent}{line}\n")
            break
    else:
        raise SystemExit(
            f"anchor not found in {rel}: {anchor!r}\n"
            "The SDK layout changed; update INJECTIONS in apply-sdk.py."
        )

    path.write_text("".join(lines), encoding="utf-8")
    return "update"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="report drift only; do not write anything")
    ap.add_argument("--force", action="store_true",
                    help="apply even when the SDK is not on the port branch")
    args = ap.parse_args()

    sdk = env.sdk_root()
    branch = env.sdk_branch()
    want = env.port_branch()

    print(f"SDK        : {sdk}")
    print(f"branch     : {branch} (port branch: {want})")
    print(f"overlay    : {OVERLAY_DIR}")
    print()

    if branch != want and not args.force:
        print(f"FAIL: SDK is on '{branch}', not the port branch '{want}'.", file=sys.stderr)
        print(f"      Switch with: git -C \"{sdk}\" checkout {want}", file=sys.stderr)
        print("      Or pass --force if you deliberately want to write here.", file=sys.stderr)
        return 2

    actions: list[tuple[str, str]] = []

    # 1. overlay tree
    for src, rel in _overlay_files():
        actions.append((_sync_file(src, sdk / rel, args.check), _rel(rel)))

    # 2. vendored port sources
    for rel, src in sorted(VENDORED.items()):
        actions.append((_sync_file(src, sdk / rel, args.check), _rel(rel)))

    # 3. the lib drop-box guard
    gi = sdk / PKG_REL / "lib" / ".gitignore"
    if gi.is_file() and gi.read_text(encoding="utf-8") == LIB_GITIGNORE:
        actions.append(("same", _rel(PKG_REL / "lib" / ".gitignore")))
    elif args.check:
        actions.append(("drift", _rel(PKG_REL / "lib" / ".gitignore")))
    else:
        existed = gi.is_file()
        gi.parent.mkdir(parents=True, exist_ok=True)
        gi.write_text(LIB_GITIGNORE, encoding="utf-8")
        actions.append(("update" if existed else "create",
                        _rel(PKG_REL / "lib" / ".gitignore")))

    # 4. injections into existing SDK files
    for rel, line, anchor in INJECTIONS:
        actions.append((_apply_injection(sdk, rel, line, anchor, args.check), _rel(rel)))

    # 5. prune anything left over from an earlier overlay layout
    for rel in STALE:
        path = sdk / rel
        if not path.exists():
            continue
        if args.check:
            actions.append(("stale", _rel(rel)))
        else:
            shutil.rmtree(path) if path.is_dir() else path.unlink()
            actions.append(("removed", _rel(rel)))

    # 6. report
    order = {"create": 0, "update": 1, "removed": 2, "drift": 3, "stale": 4, "same": 5}
    actions.sort(key=lambda a: (order[a[0]], a[1]))
    for status, rel in actions:
        print(f"  {status:8s} {rel}")

    changed = sum(1 for s, _ in actions if s in ("create", "update", "removed"))
    bad = sum(1 for s, _ in actions if s in ("drift", "stale"))

    print()
    if args.check:
        if bad:
            print(f"DRIFT: {bad} path(s) differ from the overlay.")
            return 1
        print("clean: the SDK matches the overlay.")
        return 0

    print(f"applied: {changed} path(s) written, "
          f"{sum(1 for s, _ in actions if s == 'same')} already current.")
    print()
    print("Next:")
    print("  python tools/build-firmware.py          # rust lib + scons build")
    print(f"  git -C \"{sdk}\" status --short           # review the branch diff")
    return 0


if __name__ == "__main__":
    sys.exit(main())
