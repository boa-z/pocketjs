#!/usr/bin/env python3
"""Build the PocketJS D13x Gate 0 firmware end to end.

Order matters and is deliberate (spec section 33): the Rust staticlib is built
first, on its own, so that when the link fails it is unambiguous whether the
Rust compile or the Luban-Lite link is at fault.

  1. cargo build      -> libpocketjs_abi_probe.a   (release, ilp32d)
  2. stage            -> <sdk>/application/rt-thread/pocketjs-smoke/third_party/pocketjs/lib/
  3. scons --apply-def-> .config for the pocketjs-smoke scheme
  4. scons -jN        -> firmware ELF + flashable images
  5. collect          -> images + re-run the checks into the validation tree

Step 5 also re-runs the ABI / SDK / overlay verifications and the ELF
inspections into the run directory, so the gate's evidence is reproduced by one
command instead of being remembered from a session.

The Rust archive is staged into the SDK and never committed; the package's
``lib/.gitignore`` exists precisely so that stays true.

Usage:
    python tools/build-firmware.py
    python tools/build-firmware.py --skip-rust      # reuse the staged archive
    python tools/build-firmware.py --defconfig-only # just switch the scheme
    python tools/build-firmware.py -j16
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import portenv as pe

# Must match the [lib] name in rust/abi-probe/Cargo.toml and the LIB_NAME in
# the package SConscript.
LIB_STEM = "pocketjs_abi_probe"

# The runtime package inside the SDK. Must match PKG_REL in apply-sdk.py.
PKG_REL = Path("application") / "rt-thread" / "pocketjs-smoke" / "third_party" / "pocketjs"
DEFCONFIG = "d13x_d50t-2-lite_rt-thread_pocketjs-smoke_defconfig"


def _run(cmd: list[str], cwd: Path, label: str) -> int:
    print(f"\n=== {label} ===")
    print("$ " + " ".join(str(c) for c in cmd))
    print(f"  cwd: {cwd}")
    proc = subprocess.run([str(c) for c in cmd], cwd=str(cwd))
    if proc.returncode != 0:
        print(f"\nFAIL ({label}): exit {proc.returncode}", file=sys.stderr)
    return proc.returncode


def rust_artifact(profile: str = "release") -> Path:
    return pe.rust_target_dir() / "d13x-e907-ilp32d" / profile / f"lib{LIB_STEM}.a"


def build_rust() -> int:
    return _run([sys.executable, str(pe.TOOLS_DIR / "build-native.py")],
                cwd=pe.PORT_ROOT, label="1/5 rust staticlib")


def stage_rust() -> Path:
    src = rust_artifact()
    if not src.is_file():
        raise SystemExit(
            f"Rust archive missing: {src}\n"
            "Run without --skip-rust, or run tools/build-native.py first."
        )
    dst = pe.sdk_root() / PKG_REL / "lib" / f"lib{LIB_STEM}.a"
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    print(f"\n=== 2/5 stage ===")
    print(f"{src}")
    print(f"  -> {dst}  ({dst.stat().st_size} bytes)")
    return dst


def scons(*extra: str, jobs: int | None = None) -> int:
    # SCons is not stdlib, so it must run under an interpreter that has it -
    # not necessarily the one that launched this script.
    cmd = [pe.scons_python(), "-m", "SCons", *extra]
    if jobs:
        cmd.append(f"-j{jobs}")
    return _run(cmd, cwd=pe.sdk_root(), label="scons " + " ".join(extra))


def out_dir() -> Path:
    return pe.sdk_root() / "output" / f"d13x_d50t-2-lite_rt-thread_pocketjs-smoke"


def collect(run_dir: Path) -> list[Path]:
    """Copy the flashable artifacts out of the SDK so evidence survives a clean."""
    src = out_dir() / "images"
    if not src.is_dir():
        print(f"\nwarning: no images directory at {src}", file=sys.stderr)
        return []

    dst = run_dir / "images"
    dst.mkdir(parents=True, exist_ok=True)
    wanted = ["*.elf", "*.bin", "*.map", "*.pbp", "*.aic", "bootcfg.txt"]
    copied: list[Path] = []
    for pat in wanted:
        for f in sorted(src.glob(pat)):
            target = dst / f.name
            shutil.copyfile(f, target)
            copied.append(target)
    return copied


def capture(cmd: list[str], dest: Path) -> int:
    """Run `cmd`, write stdout+stderr and the exit code to `dest`, return the code."""
    proc = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    body = proc.stdout
    if proc.stderr:
        body += "\n--- stderr ---\n" + proc.stderr
    body += f"\n--- exit: {proc.returncode} ---\n"
    dest.write_text(body, encoding="utf-8")
    return proc.returncode


def capture_pjs_symbols(elf: Path, dest: Path) -> int:
    """Record every `pjs_*` symbol in the linked image (the port's fingerprint)."""
    proc = subprocess.run([pe.tool("nm"), str(elf)], capture_output=True, text=True)
    lines = [ln for ln in proc.stdout.splitlines() if "pjs_" in ln]
    dest.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return proc.returncode


def evidence(run_dir: Path) -> list[tuple[str, int]]:
    """Re-run the verifications into the run directory.

    The gate's evidence must be reproducible from one command, so the check
    outputs and ELF inspections are written next to the images rather than
    remembered from the session that produced them. A non-zero checker result is
    recorded, not swallowed - and not fatal, because check-sdk.py legitimately
    reports FAIL when the product tree carries changes that are not the port's.
    """
    elf = out_dir() / "images" / "d13x.elf"
    py = sys.executable
    t = pe.TOOLS_DIR
    plan: list[tuple[str, list[str]]] = [
        ("check-abi.txt", [py, str(t / "check-abi.py")]),
        ("check-sdk.txt", [py, str(t / "check-sdk.py"), "--override-sdk"]),
        ("apply-sdk-check.txt", [py, str(t / "apply-sdk.py"), "--check"]),
        # The allocator's arithmetic, exercised on the host. Exit 2 means no
        # host compiler was available, which is recorded rather than hidden.
        ("host-alloc-test.txt", [py, str(t / "test-alloc-host.py")]),
    ]
    if elf.is_file():
        plan += [
            ("elf-header.txt", [pe.tool("readelf"), "-h", str(elf)]),
            ("elf-attributes.txt", [pe.tool("readelf"), "-A", str(elf)]),
            ("elf-size.txt", [pe.tool("size"), str(elf)]),
        ]

    results: list[tuple[str, int]] = []
    for name, cmd in plan:
        results.append((name, capture(cmd, run_dir / name)))
    if elf.is_file():
        results.append(("elf-pjs-symbols.txt", capture_pjs_symbols(elf, run_dir / "elf-pjs-symbols.txt")))
    return results


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--skip-rust", action="store_true",
                    help="reuse the already staged archive")
    ap.add_argument("--defconfig-only", action="store_true",
                    help="switch the scheme and stop")
    ap.add_argument("--run-dir", type=Path, default=None,
                    help="where to collect evidence "
                         "(default .pocket-build/d13x/validation/gate0/<stamp>)")
    args = ap.parse_args()

    started = time.time()
    sdk = pe.sdk_root()
    branch = pe.sdk_branch()
    want = pe.port_branch()

    print("PocketJS D13x Gate 0 firmware build")
    print(f"  SDK        : {sdk}")
    print(f"  branch     : {branch}")
    print(f"  defconfig  : {DEFCONFIG}")

    if branch != want:
        print(f"\nFAIL: SDK is on '{branch}', not the port branch '{want}'.",
              file=sys.stderr)
        print(f"      git -C \"{sdk}\" checkout {want}", file=sys.stderr)
        return 2

    if not args.skip_rust:
        rc = build_rust()
        if rc != 0:
            return rc
    staged = stage_rust()

    rc = scons(f"--apply-def={DEFCONFIG}")
    if rc != 0:
        return rc

    if args.defconfig_only:
        print("\ndefconfig applied; stopping as requested.")
        return 0

    rc = scons(jobs=args.jobs)
    if rc != 0:
        return rc

    run_dir = args.run_dir or (
        pe.build_root() / "validation" / "gate0" / time.strftime("%Y%m%dT%H%M%S")
    )
    run_dir.mkdir(parents=True, exist_ok=True)
    copied = collect(run_dir)
    checks = evidence(run_dir)

    print("\n=== 5/5 collect + verify ===")
    print(f"staged archive : {staged}")
    print(f"evidence dir   : {run_dir}")
    for f in copied:
        print(f"  images/{f.name}  ({f.stat().st_size} bytes)")
    print()
    for name, rc in checks:
        print(f"  {name:<24} exit {rc}")

    elf = out_dir() / "images" / "d13x.elf"
    print(f"\nelapsed: {time.time() - started:.1f}s")
    if elf.is_file():
        print(f"ELF: {elf}")
        print("\nNext: flash it on the D50T-2-Lite and capture the console.")
    else:
        print(f"warning: ELF not found at {elf}", file=sys.stderr)
        return 1
    return 1 if any(rc != 0 for _, rc in checks) else 0


if __name__ == "__main__":
    sys.exit(main())
