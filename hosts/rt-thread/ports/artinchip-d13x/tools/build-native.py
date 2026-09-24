#!/usr/bin/env python3
"""Build the PocketJS D13x Rust staticlib.

Deliberately a separate step from the SCons firmware build (spec section 33).
When an ABI or link problem shows up, it must be obvious whether the Rust
compile or the Luban-Lite link is at fault, so the two are never entangled
until Gate 2 is stable.

Usage:
    python tools/build-native.py [--receipt PATH] [--debug]

Environment:
    POCKETJS_AIC_TOOLCHAIN  Xuantie toolchain root (only needed by check-abi.py)
    CARGO_TARGET_DIR        overridden to <repo>/.pocket-build/d13x/rust-target
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

import portenv as pe


def cargo_build(debug: bool) -> tuple[int, str, list[str]]:
    target = pe.target_json()
    if not target.is_file():
        raise SystemExit(f"target spec missing: {target}")

    manifest_dir = pe.abi_probe_dir()
    if not (manifest_dir / "Cargo.toml").is_file():
        raise SystemExit(f"Cargo.toml missing in {manifest_dir}")

    profile = "debug" if debug else "release"

    cmd = [
        pe.cargo(),
        "build",
        f"--{profile}",
        "--target", str(target),
        "-Zjson-target-spec",
        "-Z", "build-std=core,alloc,compiler_builtins",
    ]

    env = dict(os.environ)
    # Keep every byte of build output inside the ignored .pocket-build tree.
    env["CARGO_TARGET_DIR"] = str(pe.rust_target_dir())

    print("$ " + " ".join(cmd))
    print(f"  cwd: {manifest_dir}")
    print(f"  CARGO_TARGET_DIR={env['CARGO_TARGET_DIR']}")

    proc = subprocess.run(cmd, cwd=str(manifest_dir), env=env, text=True)
    return proc.returncode, profile, cmd


def artifact(profile: str) -> Path:
    return pe.rust_target_dir() / "d13x-e907-ilp32d" / profile / "libpocketjs_abi_probe.a"


def patch_attributes(profile: str) -> int:
    """Remove the LLVM/binutils RISC-V attribute dialect clash.

    LLVM 20 writes a `Tag_RISCV_arch` string using the modern extension names
    (`zmmul`, `zaamo`, `zalrsc`, `zca`, ...). The Xuantie toolchain ships
    binutils 2.35, which cannot parse them, so the link dies with "failed to
    merge target specific data" once per libc member. See
    patch-riscv-attrs.py for the full explanation and for what is preserved.

    Runs on every build because cargo rewrites the archive whenever the crate
    changes, which would otherwise silently reintroduce the clash.
    """
    art = artifact(profile)
    if not art.is_file():
        print(f"cannot patch: {art} is missing", file=sys.stderr)
        return 1
    proc = subprocess.run(
        [sys.executable, str(pe.TOOLS_DIR / "patch-riscv-attrs.py"), "--archive", str(art)],
        cwd=str(pe.PORT_ROOT), text=True,
    )
    return proc.returncode


def write_receipt(path: Path, profile: str, cmd: list[str], rc: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    art = artifact(profile)
    lines = [
        "# Gate 0 native build receipt",
        "",
        f"pocketjs_commit: {_git(pe.repo_root(), 'rev-parse', 'HEAD')}",
        f"rustc: {pe.rustc_version()}",
        f"cargo: {pe.run([pe.cargo(), '--version']).stdout.strip()}",
        f"target_spec: {pe.target_json()}",
        f"target_spec_sha256: {_sha256(pe.target_json())}",
        f"profile: {profile}",
        f"exit_code: {rc}",
        f"artifact: {art}",
        f"artifact_bytes: {art.stat().st_size if art.is_file() else 0}",
        "",
        "command:",
        "  " + " ".join(cmd),
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"receipt: {path}")


def _git(cwd: Path, *args: str) -> str:
    p = subprocess.run(["git", *args], cwd=str(cwd), capture_output=True, text=True)
    return p.stdout.strip() or "(unknown)"


def _sha256(path: Path) -> str:
    import hashlib

    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--debug", action="store_true", help="build the debug profile")
    ap.add_argument(
        "--receipt",
        type=Path,
        default=None,
        help="write a build receipt (default: .pocket-build/validation/d13x/gate0/<run>/native-build.txt)",
    )
    ap.add_argument("--no-patch-attrs", action="store_true",
                    help="skip the RISC-V attribute normalisation (the link will fail)")
    args = ap.parse_args()

    started = time.time()
    rc, profile, cmd = cargo_build(args.debug)
    elapsed = time.time() - started

    art = artifact(profile)
    print()
    print(f"exit: {rc}  elapsed: {elapsed:.1f}s", flush=True)

    if rc == 0 and not args.no_patch_attrs:
        rc = patch_attributes(profile)
        if rc != 0:
            print("FAIL: attribute normalisation failed", file=sys.stderr)

    # Reported after patching so the size is the one that actually gets staged.
    if art.is_file():
        print(f"artifact: {art}  ({art.stat().st_size} bytes)")
    else:
        print(f"artifact: MISSING ({art})", file=sys.stderr)

    receipt = args.receipt
    if receipt is None:
        stamp = time.strftime("%Y%m%dT%H%M%S")
        receipt = pe.build_root() / "validation" / "gate0" / stamp / "native-build.txt"
    write_receipt(receipt, profile, cmd, rc)

    return rc


if __name__ == "__main__":
    sys.exit(main())
