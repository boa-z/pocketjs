#!/usr/bin/env python3
"""Neutralise the LLVM/binutils RISC-V attribute dialect clash.

The problem
-----------
The Xuantie toolchain ships **binutils 2.35**, whose RISC-V attribute merger
cannot parse the ISA string that LLVM 20 writes. Linking the Rust archive
against the vendor libc therefore fails with, once per libc member:

    ld.exe: failed to merge target specific data of file .../libc.a(lib_a-*.o)

The two strings are dialects of the *same* ISA:

    Rust  (LLVM 20)  rv32i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_zicsr2p0_zmmul1p0_
                     zaamo1p0_zalrsc1p0_zca1p0_zcd1p0_zcf1p0
    GCC   (Xuantie)  rv32i2p0_m2p0_a2p0_f2p0_d2p0_c2p0_xtheade2p0

LLVM names the modern decompositions explicitly (`zmmul` = multiply-only,
`zaamo`/`zalrsc` = the two halves of A, `zca`/`zcd`/`zcf` = compressed
encodings of C/D/F). Every one of those is a **subset** of I/M/A/C/F/D. LLVM
emits no instruction the vendor toolchain cannot encode; only the *metadata
spelling* differs. binutils 2.35 predates those names and bails out.

What this does
--------------
Removes the `.riscv.attributes` section from the Rust archive members, and
nothing else. Specifically it does **not** touch `e_flags`, which is where the
ABI actually lives:

    EF_RISCV_RVC            (0x1)
    EF_RISCV_FLOAT_ABI_DOUBLE (0x4)   -> e_flags 0x5 for ILP32D

That matters because the linker enforces float-ABI compatibility from
`e_flags`, so the "can't link soft-float with double-float" safety net stays
armed. The ISA-attribute check is preserved on the *unpatched* archive by
`check-abi.py`, which reads the arch string before this step runs.

Losing the section costs nothing in the final image: the C objects still carry
the vendor ISA string, and the firmware's merged attribute is
`rv32i2p0_m2p0_a2p0_f2p0_d2p0_c2p0_p0p9_zpn0p9_zpsfoperand0p9_xtheade2p0`,
which already contains `f` and `d`.

Usage:
    python tools/patch-riscv-attrs.py                 # patch the built archive
    python tools/patch-riscv-attrs.py --check         # report, change nothing
    python tools/patch-riscv-attrs.py --archive X --out Y
"""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import portenv as pe

# ELF32 header: e_flags sits at a fixed offset.
ELF32_E_FLAGS_OFF = 36
EF_RISCV_RVC = 0x1
EF_RISCV_FLOAT_ABI = 0x6
EF_RISCV_FLOAT_ABI_DOUBLE = 0x4

ATTR_SECTION = ".riscv.attributes"


def _run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def _e_flags(obj: Path) -> int | None:
    """Read e_flags straight out of the ELF header, without shelling out."""
    with obj.open("rb") as fh:
        head = fh.read(ELF32_E_FLAGS_OFF + 4)
    if len(head) < ELF32_E_FLAGS_OFF + 4 or head[:4] != b"\x7fELF":
        return None
    if head[4] != 1:  # EI_CLASS: 1 = ELF32
        return None
    return struct.unpack_from("<I", head, ELF32_E_FLAGS_OFF)[0]


def _members(archive: Path) -> list[str]:
    proc = _run([pe.tool("ar"), "t", archive])
    if proc.returncode != 0:
        raise SystemExit(f"ar t failed:\n{proc.stderr}")
    return [m for m in proc.stdout.splitlines() if m.strip()]


def _arch_strings(archive: Path, members: list[str]) -> dict[str, str]:
    """Map member -> Tag_RISCV_arch string, via readelf."""
    out: dict[str, str] = {}
    with tempfile.TemporaryDirectory() as tmp:
        tmpd = Path(tmp)
        for name in members:
            proc = _run([pe.tool("ar"), "x", archive, name], cwd=tmpd)
            if proc.returncode != 0:
                continue
            obj = tmpd / name
            if not obj.is_file():
                continue
            attrs = _run([pe.tool("readelf"), "-A", obj]).stdout
            for line in attrs.splitlines():
                line = line.strip()
                if line.startswith("Tag_RISCV_arch:"):
                    out[name] = line.split(":", 1)[1].strip().strip('"')
                    break
    return out


def _raw_attrs(archive: Path) -> str:
    """readelf -A over the whole archive, as emitted (before any patching)."""
    return _run([pe.tool("readelf"), "-A", archive]).stdout


def _write_evidence(path: Path, archive: Path, raw: str, flags: dict[str, int]) -> None:
    """Persist the as-emitted ISA metadata.

    This is what makes the pipeline order-independent: `check-abi.py` asserts
    the ISA string, but patching removes the section that carries it. Keeping
    the original here means the assertion still has something true to read, and
    the Gate 0 report has the raw evidence rather than a summary of it.
    """
    lines = [
        "# RISC-V attributes as emitted by rustc/LLVM, before normalisation.",
        "#",
        "# Written by tools/patch-riscv-attrs.py. The archive this describes no",
        "# longer carries .riscv.attributes: binutils 2.35 cannot parse LLVM 20's",
        "# extension names, so the section is removed to let the vendor linker",
        "# merge the objects. e_flags is untouched and is what the linker uses",
        "# to enforce the float ABI.",
        "#",
        f"archive: {archive}",
        f"members: {len(flags)}",
        "",
        "## e_flags per member",
        "",
    ]
    for name in sorted(flags):
        f = flags[name]
        abi = "double-float" if (f & EF_RISCV_FLOAT_ABI) == EF_RISCV_FLOAT_ABI_DOUBLE else "NOT-DOUBLE"
        rvc = "RVC" if f & EF_RISCV_RVC else "-"
        lines.append(f"  0x{f:02x}  {abi:12s} {rvc:3s}  {name}")
    lines += [
        "",
        "## readelf -A (original)",
        "",
        raw.rstrip(),
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--archive", type=Path, default=None,
                    help="archive to patch (default: the built Rust staticlib)")
    ap.add_argument("--out", type=Path, default=None,
                    help="output path (default: patch in place)")
    ap.add_argument("--check", action="store_true",
                    help="report the dialect clash without changing anything")
    ap.add_argument("--evidence", type=Path, default=None,
                    help="where to save the as-emitted attributes "
                         "(default: <out>.riscv-attrs.txt)")
    args = ap.parse_args()

    src = args.archive or (
        pe.rust_target_dir() / "d13x-e907-ilp32d" / "release" / "libpocketjs_abi_probe.a"
    )
    if not src.is_file():
        raise SystemExit(f"archive not found: {src}")

    dst = args.out or src
    evidence = args.evidence or Path(str(dst) + ".riscv-attrs.txt")

    members = _members(src)
    print(f"archive : {src}")
    print(f"members : {len(members)}")

    before = _arch_strings(src, members)
    raw_before = _raw_attrs(src)
    flags_before = {}
    with tempfile.TemporaryDirectory() as tmp:
        tmpd = Path(tmp)
        for name in members:
            if _run([pe.tool("ar"), "x", src, name], cwd=tmpd).returncode == 0:
                f = _e_flags(tmpd / name)
                if f is not None:
                    flags_before[name] = f

    dialects = sorted(set(before.values()))
    print(f"arch tags found: {len(dialects)} distinct")
    for d in dialects:
        n = sum(1 for v in before.values() if v == d)
        print(f"  [{n:2d}] {d}")

    bad_abi = {k: v for k, v in flags_before.items()
               if (v & EF_RISCV_FLOAT_ABI) != EF_RISCV_FLOAT_ABI_DOUBLE}
    rvc = sum(1 for v in flags_before.values() if v & EF_RISCV_RVC)
    print(f"e_flags : double-float ABI on {len(flags_before) - len(bad_abi)}/"
          f"{len(flags_before)} members, RVC on {rvc}")

    if not before:
        print("\nnothing to do: no Tag_RISCV_arch in this archive.")
        return 0

    if bad_abi:
        print(f"\nFAIL: {len(bad_abi)} member(s) are not double-float ABI. "
              "Refusing to patch a mis-built archive.", file=sys.stderr)
        for k, v in list(bad_abi.items())[:5]:
            print(f"  {k}: e_flags=0x{v:x}", file=sys.stderr)
        return 1

    if args.check:
        print(f"\nwould strip {ATTR_SECTION} from {len(members)} member(s).")
        return 0

    # Persist the as-emitted metadata before it is removed.
    _write_evidence(evidence, src, raw_before, flags_before)
    print(f"evidence: {evidence}")

    # objcopy handles archives: it applies the section removal to every member.
    # Note it *adds* members when rewriting an archive, so the output can grow;
    # that is harmless (duplicate members are never pulled in by the linker).
    proc = _run([pe.tool("objcopy"), "-R", ATTR_SECTION, src, dst])
    if proc.returncode != 0:
        raise SystemExit(f"objcopy failed:\n{proc.stderr}")

    after_members = _members(dst)
    after = _arch_strings(dst, after_members)
    with tempfile.TemporaryDirectory() as tmp:
        tmpd = Path(tmp)
        flags_after = {}
        for name in after_members:
            if _run([pe.tool("ar"), "x", dst, name], cwd=tmpd).returncode == 0:
                f = _e_flags(tmpd / name)
                if f is not None:
                    flags_after[name] = f

    bad_after = {k: v for k, v in flags_after.items()
                 if (v & EF_RISCV_FLOAT_ABI) != EF_RISCV_FLOAT_ABI_DOUBLE}

    print()
    print(f"patched : {dst}  ({dst.stat().st_size} bytes)")
    print(f"members : {len(after_members)}")
    print(f"arch tags remaining: {len(after)}")
    print(f"e_flags : double-float ABI on {len(flags_after) - len(bad_after)}/"
          f"{len(flags_after)} members")

    problems = []
    if after:
        problems.append(f"{len(after)} member(s) still carry {ATTR_SECTION}")
    if bad_after:
        problems.append(f"{len(bad_after)} member(s) lost the double-float ABI flag")
    if len(flags_after) < len(flags_before):
        problems.append(
            f"member count dropped from {len(flags_before)} to {len(flags_after)}")

    if problems:
        print("\nFAIL:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    print("\nok: ABI flags intact, ISA metadata dialect clash removed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
