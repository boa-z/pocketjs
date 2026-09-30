#!/usr/bin/env python3
"""Build the PocketJS D13x firmware end to end.

Order matters and is deliberate (spec section 33): the Rust staticlibs are built
first, on their own, so that when the link fails it is unambiguous whether a
Rust compile or the Luban-Lite link is at fault.

  1. cargo build      -> libpocketjs_abi_probe.a          (release, ilp32d)
                         libpocketjs_rtthread_ui_core.a
  2. stage            -> <sdk>/application/rt-thread/pocketjs-smoke/third_party/pocketjs/lib/
                         <sdk>/application/rt-thread/pocketjs-smoke/third_party/pocketjs_ui_core/lib/
  3. scons --apply-def-> .config for the scheme below
  4. scons -jN        -> firmware ELF + flashable images
  5. collect          -> images + re-run the checks into the validation tree

Step 5 also re-runs the ABI / SDK / overlay verifications and the ELF
inspections into the run directory, so the gate's evidence is reproduced by one
command instead of being remembered from a session. The ABI check is handed the
linked ELF, because the link-level assertions - above all that `pjs_host_log`
survived `-Wl,-gc-sections` - cannot be made against an archive.

The Rust archives are staged into the SDK and never committed; each package's
``lib/.gitignore`` exists precisely so that stays true.

Usage:
    python tools/build-firmware.py
    python tools/build-firmware.py --skip-rust       # reuse the staged archives
    python tools/build-firmware.py --defconfig-only  # just switch the scheme
    python tools/build-firmware.py --no-apply-def    # build the current .config
    python tools/build-firmware.py --gate gate1b     # where evidence lands
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

# The Rust archives the firmware links, paired with the SDK package each is
# staged into.
#
# Each stem must match the [lib] name in that crate's Cargo.toml, the LIB_NAME
# in the matching package SConscript, and the Crate.lib_stem in
# tools/build-native.py, which is what actually produces them.
ARCHIVES = (
    ("pocketjs_rtthread_render_rgb565",
     Path("application") / "rt-thread" / "pocketjs-smoke" / "third_party" / "pocketjs_render_rgb565"),
    ("pocketjs_abi_probe",
     Path("application") / "rt-thread" / "pocketjs-smoke" / "third_party" / "pocketjs"),
    ("pocketjs_rtthread_ui_core",
     Path("application") / "rt-thread" / "pocketjs-smoke" / "third_party" / "pocketjs_ui_core"),
)

# The scheme `--apply-def` applies when it is not overridden.
#
# Note that `scons` re-applies the scheme named in `.config` on *every* run, so
# this is only the default for switching; the effective scheme is always the one
# `active_defconfig()` reads back.
DEFCONFIG = "d13x_d50t-2-lite_rt-thread_pocketjs-smoke_defconfig"


def _run(cmd: list[str], cwd: Path, label: str) -> int:
    print(f"\n=== {label} ===")
    print("$ " + " ".join(str(c) for c in cmd))
    print(f"  cwd: {cwd}")
    proc = subprocess.run([str(c) for c in cmd], cwd=str(cwd))
    if proc.returncode != 0:
        print(f"\nFAIL ({label}): exit {proc.returncode}", file=sys.stderr)
    return proc.returncode


def rust_artifact(stem: str, profile: str = "release") -> Path:
    return pe.rust_target_dir() / "d13x-e907-ilp32d" / profile / f"lib{stem}.a"


def build_rust() -> int:
    return _run([sys.executable, str(pe.TOOLS_DIR / "build-native.py")],
                cwd=pe.PORT_ROOT, label="1/5 rust staticlibs")


def stage_rust() -> list[Path]:
    staged: list[Path] = []
    print("\n=== 2/5 stage ===")
    for stem, pkg_rel in ARCHIVES:
        src = rust_artifact(stem)
        if not src.is_file():
            raise SystemExit(
                f"Rust archive missing: {src}\n"
                "Run without --skip-rust, or run tools/build-native.py first."
            )
        if pe.sdk_submodule_mode():
            print(f"{src} (linked directly from the source dependency)")
            staged.append(src)
            continue
        dst = pe.sdk_root() / pkg_rel / "lib" / f"lib{stem}.a"
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        print(f"{src}")
        print(f"  -> {dst}  ({dst.stat().st_size} bytes)")
        staged.append(dst)
    return staged


def scons(*extra: str, jobs: int | None = None) -> int:
    # SCons is not stdlib, so it must run under an interpreter that has it -
    # not necessarily the one that launched this script.
    cmd = [pe.scons_python(), "-m", "SCons", *extra]
    if jobs:
        cmd.append(f"-j{jobs}")
    return _run(cmd, cwd=pe.sdk_root(), label="scons " + " ".join(extra))


def active_defconfig() -> str:
    """The scheme ``.config`` currently pins.

    SCons writes to ``output/<scheme>/`` and ``--apply-def`` rewrites
    ``.config``, so which directory gets populated is decided by ``.config``,
    not by the constant above. Reading the active pin means an already
    configured tree is collected from where it actually built.

    This checkout depends on that: the Gate 1 rebuild uses a locally excluded
    ``...-g1b`` alias (see .git/info/exclude) purely to make SCons populate a
    fresh output directory, because the full rebuild needs more bulk deletions
    than the build environment will grant. A fresh clone has no such file and
    builds the canonical name; the resulting ``rtconfig.h`` is identical.
    """
    cfg = pe.sdk_root() / ".config"
    if cfg.is_file():
        for line in cfg.read_text(encoding="utf-8", errors="ignore").splitlines():
            if line.startswith("CONFIG_PRJ_DEFCONFIG_FILENAME="):
                return line.split("=", 1)[1].strip().strip('"')
    return DEFCONFIG


def scheme_of(defconfig: str) -> str:
    """`d13x_..._pocketjs-smoke_defconfig` -> `d13x_..._pocketjs-smoke`."""
    suffix = "_defconfig"
    return defconfig[: -len(suffix)] if defconfig.endswith(suffix) else defconfig


def out_dir() -> Path:
    return pe.sdk_root() / "output" / scheme_of(active_defconfig())


def collect(run_dir: Path) -> list[Path]:
    """Copy the flashable artifacts out of the SDK so evidence survives a clean."""
    src = out_dir() / "images"
    if not src.is_dir():
        print(f"\nwarning: no images directory at {src}", file=sys.stderr)
        return []

    dst = run_dir / "images"
    dst.mkdir(parents=True, exist_ok=True)
    wanted = ["*.elf", "*.bin", "*.map", "*.pbp", "*.aic", "*.itb", "*.img", "bootcfg.txt"]
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


def capture_port_symbols(elf: Path, dest: Path) -> int:
    """Record every port-owned symbol in the linked image (its fingerprint).

    Both prefixes, because Gate 1 added a second archive: `pjs_*` is the port's
    own seam and probe, `pocketjs_*` is the retained UI core's entry points.
    """
    proc = subprocess.run([pe.tool("nm"), str(elf)], capture_output=True, text=True)
    lines = [
        ln for ln in proc.stdout.splitlines()
        if "pjs_" in ln or "pocketjs_" in ln
    ]
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

    # The linked ELF is what makes the Gate 0 section 8.4 assertion possible at
    # all, so hand it over whenever it exists.
    abi_cmd = [py, str(t / "check-abi.py")]
    if elf.is_file():
        abi_cmd += ["--elf", str(elf)]

    plan: list[tuple[str, list[str]]] = [
        ("check-abi.txt", abi_cmd),
        ("check-sdk.txt", [py, str(t / "check-sdk.py")]),
        # The allocator's arithmetic, exercised on the host. Exit 2 means no
        # host compiler was available, which is recorded rather than hidden.
        ("host-alloc-test.txt", [py, str(t / "test-alloc-host.py")]),
    ]
    if not pe.sdk_submodule_mode():
        plan.append(("apply-sdk-check.txt", [py, str(t / "apply-sdk.py"), "--check"]))
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
        results.append(("elf-port-symbols.txt",
                        capture_port_symbols(elf, run_dir / "elf-port-symbols.txt")))
    return results


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--skip-rust", action="store_true",
                    help="reuse the already staged archives")
    ap.add_argument("--defconfig-only", action="store_true",
                    help="switch the scheme and stop")
    ap.add_argument("--no-apply-def", action="store_true",
                    help="do not rewrite .config; build whatever scheme is active")
    ap.add_argument("--defconfig", default=DEFCONFIG, metavar="NAME",
                    help=f"scheme to apply with --apply-def (default: {DEFCONFIG})")
    ap.add_argument("--gate", default="gate0", metavar="NAME",
                    help="gate name used in the default run directory")
    ap.add_argument("--run-dir", type=Path, default=None,
                    help="where to collect evidence "
                         "(default .pocket-build/d13x/validation/<gate>/<stamp>)")
    args = ap.parse_args()

    started = time.time()
    sdk = pe.sdk_root()
    branch = pe.sdk_branch()
    want = pe.port_branch()

    print("PocketJS D13x firmware build")
    print(f"  SDK        : {sdk}")
    print(f"  branch     : {branch}")
    if args.no_apply_def:
        print(f"  defconfig  : {active_defconfig()} (active, not applied)")
    else:
        print(f"  defconfig  : {args.defconfig}")

    if branch != want:
        print(f"\nFAIL: SDK is on '{branch}', not the port branch '{want}'.",
              file=sys.stderr)
        print(f"      git -C \"{sdk}\" checkout {want}", file=sys.stderr)
        return 2

    if args.defconfig != DEFCONFIG and not args.no_apply_def:
        print("FAIL: this build entry only accepts the PocketJS smoke defconfig.", file=sys.stderr)
        return 2
    if args.no_apply_def and active_defconfig() != DEFCONFIG:
        print("FAIL: active SDK configuration is not PocketJS smoke.", file=sys.stderr)
        return 2
    dependency = sdk / "application/rt-thread/pocketjs-smoke/third_party/pocketjs"
    if (dependency / ".git").exists() and not pe.sdk_submodule_mode():
        print("FAIL: build from the SDK's pinned PocketJS dependency, not another checkout.",
              file=sys.stderr)
        return 2
    if pe.sdk_submodule_mode():
        header = sdk / "application/rt-thread/pocketjs-smoke/pocketjs_build.h"
        header.write_text(
            "/* Generated by PocketJS build-firmware.py. */\n"
            '#define PJS_BUILD_REV "' + pe.port_revision() + '"\n', encoding="utf-8")

    if not args.skip_rust:
        rc = build_rust()
        if rc != 0:
            return rc
    staged = stage_rust()

    if not args.no_apply_def:
        rc = scons(f"--apply-def={args.defconfig}")
        if rc != 0:
            return rc

    if args.defconfig_only:
        print("\ndefconfig applied; stopping as requested.")
        return 0

    rc = _run([sys.executable, str(pe.TOOLS_DIR / "prepare-quickjs.py")],
              cwd=pe.repo_root(), label="prepare pinned QuickJS-ng")
    if rc != 0:
        return rc
    if 'CONFIG_LPKG_USING_POCKETJS_PACKAGE=y' in (sdk / '.config').read_text():
        rc = _run([sys.executable, str(pe.TOOLS_DIR / 'build-app.py')],
                  cwd=pe.repo_root(), label='build and embed TSX package')
        if rc != 0:
            return rc
    rc = scons(jobs=args.jobs)
    if rc != 0:
        return rc

    run_dir = args.run_dir or (
        pe.build_root() / "validation" / args.gate / time.strftime("%Y%m%dT%H%M%S")
    )
    run_dir.mkdir(parents=True, exist_ok=True)
    copied = collect(run_dir)
    checks = evidence(run_dir)

    print("\n=== 5/5 collect + verify ===")
    print(f"scheme         : {active_defconfig()}")
    for dst in staged:
        print(f"staged archive : {dst}  ({dst.stat().st_size} bytes)")
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
