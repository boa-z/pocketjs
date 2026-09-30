#!/usr/bin/env python3
"""Build the PocketJS D13x Rust staticlibs.

Deliberately a separate step from the SCons firmware build (spec section 33).
When an ABI or link problem shows up, it must be obvious whether the Rust
compile or the Luban-Lite link is at fault, so the two are never entangled
until Gate 2 is stable.

Three archives are produced for the Gate 2 firmware:

  abi-probe   libpocketjs_abi_probe.a          the Gate 0 conformance probe
  ui-core     libpocketjs_rtthread_ui_core.a   the retained UI core
  render-rgb565 libpocketjs_rtthread_render_rgb565.a the software renderer

They are independent archives that bundle the same dependency graph: `core`,
`alloc`, `compiler_builtins`, and `pocketjs-rtthread-runtime`. A Rust `staticlib`
always bundles its dependencies, so the same symbols reach the link line twice.

That is safe *provided* the shared crates are built identically in both archives.
The linker then extracts one copy to satisfy every reference and never needs the
second, so nothing is defined twice. Two things are therefore required, and both
are properties of the crates rather than flags passed here:

  * No `-Cmetadata` namespacing. An earlier revision gave each crate its own
    `-Cmetadata` to stop the shared symbols colliding. That is the wrong
    instrument: it makes the two copies of `core`/`alloc` *different*, which
    converts a loud "multiple definition" error into two silently incompatible
    allocators in one image. Cargo already gives each root crate a distinct
    disambiguator derived from its package id, which is all that is needed.

  * Exactly one owner of the runtime lang items. `#[global_allocator]` and
    `#[panic_handler]` are declared only in `pocketjs-rtthread-runtime`. rustc
    lowers them into `#[rustc_std_internal_symbol]` shims whose names are fixed
    (`__rustc` plus a constant disambiguator) so that one copy can serve a whole
    link - and a fixed name is precisely what `-Cmetadata` cannot change. Two
    crates declaring them therefore collide whatever flags are passed. Feature
    crates depend on the runtime; they never provide one. This mirrors the
    ESP-IDF host, where `pocketjs-idf-runtime` is the only declarer and both
    `ui-core` and `render-rgb565` consume it.

All three archives use an identical release profile and shared runtime features.
The build checks those profiles before invoking Cargo.

Usage:
    python tools/build-native.py [--crate NAME ...] [--receipt PATH] [--debug]

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
from dataclasses import dataclass
from pathlib import Path

import portenv as pe


@dataclass(frozen=True)
class Crate:
    """One Rust staticlib the port stages into the SDK."""

    name: str
    manifest_dir: Path
    lib_stem: str
    extra_args: tuple[str, ...] = ()
    note: str = ""


def crates() -> list[Crate]:
    """The crates this port builds, in link order.

    The probe is listed first because it is the one whose failure must be
    reported first: a broken ABI makes anything built on top of it meaningless.

    Neither crate is namespaced with `-Cmetadata`; see the module docstring for
    why that instrument is actively harmful here. Cargo's own per-package
    disambiguator already keeps the two root crates' symbols apart.
    """
    return [
        Crate(
            name="abi-probe",
            manifest_dir=pe.abi_probe_dir(),
            lib_stem="pocketjs_abi_probe",
            note="Gate 0 conformance probe (port-local crate)",
        ),
        Crate(
            name="ui-core",
            manifest_dir=pe.ui_core_dir(),
            lib_stem="pocketjs_rtthread_ui_core",
            # The crate's default feature is `std`, which a bare-metal target
            # cannot build. Turning it off is what selects the `no_std` path
            # that binds GlobalAlloc and the panic handler to the host seam.
            extra_args=("--no-default-features",),
            note="retained UI core (host-level crate, staged by this port)",
        ),
        Crate(
            name="render-rgb565",
            manifest_dir=pe.ui_core_dir().parent / "render-rgb565",
            lib_stem="pocketjs_rtthread_render_rgb565",
            extra_args=("--no-default-features",),
            note="RGB565 software renderer",
        ),
    ]


def profile_of(debug: bool) -> str:
    return "debug" if debug else "release"


# Cargo's release-profile defaults, so two manifests that *mean* the same thing
# compare equal whether or not they spell every key out.
RELEASE_DEFAULTS: dict[str, object] = {
    "opt-level": 3,
    "codegen-units": 16,
    "lto": False,
    "panic": "unwind",
    "debug": False,
    "debug-assertions": False,
    "overflow-checks": False,
    "incremental": False,
    "rpath": False,
    "strip": "none",
}


def release_profile(manifest_dir: Path) -> dict[str, object]:
    """A crate's effective `[profile.release]`, with cargo's defaults filled in."""
    try:
        import tomllib
    except ImportError:  # pragma: no cover - Python < 3.11
        raise SystemExit(
            "build-native.py needs Python 3.11+ for `tomllib`, because it verifies\n"
            "that every crate agrees on [profile.release]. Building without that\n"
            "check is how the two archives ended up with incompatible copies of\n"
            "their shared dependencies."
        )
    with (manifest_dir / "Cargo.toml").open("rb") as fh:
        doc = tomllib.load(fh)
    raw = doc.get("profile", {}).get("release", {})
    return {k: raw.get(k, default) for k, default in RELEASE_DEFAULTS.items()}


def check_profiles() -> None:
    """Refuse to build when the crates disagree about the release profile.

    See the module docstring. Cargo folds the profile into the metadata hash it
    turns into `-Cmetadata`, so a profile mismatch means the two archives carry
    two *differently named* copies of every shared crate. The linker cannot
    absorb that: the compiler-owned `__rustc` allocator/panic shims keep their
    fixed names regardless, so both copies enter the link and collide with
    "multiple definition".

    Checked here rather than left to the link, because the failure the link
    reports names `__rust_alloc` and says nothing at all about profiles.
    """
    available = crates()
    grouped: dict[tuple, list[str]] = {}
    for crate in available:
        key = tuple(sorted(release_profile(crate.manifest_dir).items()))
        grouped.setdefault(key, []).append(crate.name)

    if len(grouped) == 1:
        pe.ok(f"release profile identical across all {len(available)} crates")
        return

    lines = ["the crates disagree about [profile.release]:"]
    for key, names in grouped.items():
        lines.append(f"  {', '.join(names)}:")
        lines += [f"    {k} = {v!r}" for k, v in key]
    lines += [
        "",
        "They must match key for key. Cargo folds the profile into `-Cmetadata`,",
        "so a mismatch gives the shared crates two different sets of mangled",
        "names and the link fails on the fixed-name `__rustc` shims.",
    ]
    raise SystemExit("\n".join(lines))


def cargo_cmd(crate: Crate, debug: bool) -> list[str]:
    target = pe.target_json()
    if not target.is_file():
        raise SystemExit(f"target spec missing: {target}")
    return [
        pe.cargo(),
        "build",
        f"--{profile_of(debug)}",
        "--target", str(target),
        "-Zjson-target-spec",
        "-Z", "build-std=core,alloc,compiler_builtins",
        *crate.extra_args,
    ]


def artifact(crate: Crate, debug: bool) -> Path:
    return (pe.rust_target_dir() / "d13x-e907-ilp32d" / profile_of(debug)
            / f"lib{crate.lib_stem}.a")


def build_crate(crate: Crate, debug: bool) -> tuple[int, list[str]]:
    if not crate.manifest_dir.is_dir():
        raise SystemExit(f"crate directory missing: {crate.manifest_dir}")
    if not (crate.manifest_dir / "Cargo.toml").is_file():
        raise SystemExit(f"Cargo.toml missing in {crate.manifest_dir}")

    cmd = cargo_cmd(crate, debug)

    env = dict(os.environ)
    # Keep every byte of build output inside the ignored .pocket-build tree.
    env["CARGO_TARGET_DIR"] = str(pe.rust_target_dir())
    # No `-Cmetadata` here, deliberately. See the module docstring: namespacing
    # the shared crates makes the two archives' copies of `core`/`alloc`
    # incompatible instead of identical, which is worse than the link error it
    # was meant to silence. Cargo's own per-package metadata already separates
    # the two root crates.
    env.pop("RUSTFLAGS", None)
    # The host-level crates sit outside rust/, where rust-toolchain.toml lives,
    # so the pin has to be handed over explicitly or cargo would quietly use
    # whatever rustup defaults to.
    channel = pe.toolchain_channel()
    if channel:
        env["RUSTUP_TOOLCHAIN"] = channel

    print()
    print(f"=== {crate.name}: {crate.note} ===")
    print("$ " + " ".join(cmd))
    print(f"  cwd: {crate.manifest_dir}")
    print(f"  CARGO_TARGET_DIR={env['CARGO_TARGET_DIR']}")
    if channel:
        print(f"  RUSTUP_TOOLCHAIN={channel}")

    proc = subprocess.run(cmd, cwd=str(crate.manifest_dir), env=env, text=True)
    return proc.returncode, cmd


def patch_attributes(crate: Crate, debug: bool) -> int:
    """Remove the LLVM/binutils RISC-V attribute dialect clash.

    LLVM 20 writes a `Tag_RISCV_arch` string using the modern extension names
    (`zmmul`, `zaamo`, `zalrsc`, `zca`, ...). The Xuantie toolchain ships
    binutils 2.35, which cannot parse them, so the link dies with "failed to
    merge target specific data" once per libc member. See
    patch-riscv-attrs.py for the full explanation and for what is preserved.

    Runs on every build because cargo rewrites the archive whenever the crate
    changes, which would otherwise silently reintroduce the clash. Every archive
    the firmware links needs this, not just the probe: the UI core archive
    carries the same LLVM 20 attribute string and would fail the same way.
    """
    art = artifact(crate, debug)
    if not art.is_file():
        print(f"cannot patch {crate.name}: {art} is missing", file=sys.stderr)
        return 1
    proc = subprocess.run(
        [sys.executable, str(pe.TOOLS_DIR / "patch-riscv-attrs.py"), "--archive", str(art)],
        cwd=str(pe.PORT_ROOT), text=True,
    )
    return proc.returncode


def write_receipt(path: Path, built: list[tuple[Crate, list[str], int]], debug: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# PocketJS D13x native build receipt",
        "",
        f"pocketjs_commit: {_git(pe.repo_root(), 'rev-parse', 'HEAD')}",
        f"rustc: {pe.rustc_version()}",
        f"cargo: {pe.run([pe.cargo(), '--version']).stdout.strip()}",
        f"rustup_toolchain: {pe.toolchain_channel() or '(unpinned)'}",
        f"target_spec: {pe.target_json()}",
        f"target_spec_sha256: {_sha256(pe.target_json())}",
        f"profile: {profile_of(debug)}",
        "",
    ]
    for crate, cmd, rc in built:
        art = artifact(crate, debug)
        lines += [
            f"## {crate.name}",
            f"manifest: {crate.manifest_dir}",
            "rust_symbol_namespacing: none (cargo default per-package metadata)",
            f"exit_code: {rc}",
            f"artifact: {art}",
            f"artifact_bytes: {art.stat().st_size if art.is_file() else 0}",
            "command:",
            "  " + " ".join(cmd),
            "",
        ]
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"\nreceipt: {path}")


def _git(cwd: Path, *args: str) -> str:
    p = subprocess.run(["git", *args], cwd=str(cwd), capture_output=True, text=True)
    return p.stdout.strip() or "(unknown)"


def _sha256(path: Path) -> str:
    import hashlib

    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--debug", action="store_true", help="build the debug profile")
    ap.add_argument("--crate", action="append", default=None, metavar="NAME",
                    help="build only this crate (repeatable); default: all")
    ap.add_argument(
        "--receipt",
        type=Path,
        default=None,
        help="write a build receipt "
             "(default: .pocket-build/d13x/validation/native/<stamp>/native-build.txt)",
    )
    ap.add_argument("--no-patch-attrs", action="store_true",
                    help="skip the RISC-V attribute normalisation (the link will fail)")
    args = ap.parse_args()

    # Global invariant, checked before anything is compiled: a profile mismatch
    # is what makes two archives unable to share their dependency graph.
    check_profiles()

    available = crates()
    if args.crate:
        wanted = set(args.crate)
        unknown = sorted(wanted - {c.name for c in available})
        if unknown:
            raise SystemExit(
                f"unknown crate(s): {unknown}\n"
                f"known: {[c.name for c in available]}"
            )
        selected = [c for c in available if c.name in wanted]
    else:
        selected = available

    started = time.time()
    built: list[tuple[Crate, list[str], int]] = []
    rc = 0

    for crate in selected:
        crate_rc, cmd = build_crate(crate, args.debug)
        built.append((crate, cmd, crate_rc))
        if crate_rc != 0:
            # Stop at the first failure: a later archive built against a broken
            # toolchain or target would only add noise.
            rc = crate_rc
            break

    elapsed = time.time() - started
    print()
    print(f"exit: {rc}  elapsed: {elapsed:.1f}s", flush=True)

    if rc == 0 and not args.no_patch_attrs:
        for crate, _, _ in built:
            prc = patch_attributes(crate, args.debug)
            if prc != 0:
                print(f"FAIL: attribute normalisation failed for {crate.name}",
                      file=sys.stderr)
                rc = prc
                break

    # Reported after patching so the sizes are the ones that actually get staged.
    for crate, _, crate_rc in built:
        art = artifact(crate, args.debug)
        status = "ok" if crate_rc == 0 else f"FAILED (exit {crate_rc})"
        if art.is_file():
            print(f"artifact: {crate.name:<10} {art.name}  "
                  f"({art.stat().st_size} bytes)  [{status}]")
        else:
            print(f"artifact: {crate.name:<10} MISSING ({art})", file=sys.stderr)

    receipt = args.receipt
    if receipt is None:
        stamp = time.strftime("%Y%m%dT%H%M%S")
        receipt = pe.build_root() / "validation" / "native" / stamp / "native-build.txt"
    write_receipt(receipt, built, args.debug)

    return rc


if __name__ == "__main__":
    sys.exit(main())
