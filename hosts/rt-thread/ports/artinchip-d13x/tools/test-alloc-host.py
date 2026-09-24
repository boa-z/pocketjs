#!/usr/bin/env python3
"""Compile and run the host-side test for the D13x host allocator.

The first Gate 0 hardware run passed every C<->Rust ABI case and then aborted in
the allocator: this board's `rt_malloc` returns 4-byte-aligned memory, while both
sides assumed 8. That failure is arithmetic, not silicon - so it is reproduced
here against a stubbed RT-Thread heap and checked on the build host, where it can
be run without flashing anything.

    python tools/test-alloc-host.py

Needs a host C compiler (gcc/cc/clang). Override with PJS_HOST_CC.
Exit code 0 = PASS, 1 = FAIL, 2 = no compiler.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import portenv as pe

HOST_TEST_DIR = pe.TOOLS_DIR / "host-test"
STUB_DIR = HOST_TEST_DIR / "stub"

SOURCES = (
    pe.PORT_ROOT / "src" / "pocketjs_alloc.c",   # the code under test
    STUB_DIR / "rtthread.c",                     # the stubbed heap
    HOST_TEST_DIR / "test_alloc.c",              # the assertions
)


def find_compiler() -> str | None:
    env = os.environ.get("PJS_HOST_CC")
    if env:
        return env
    for name in ("gcc", "cc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    return None


def main() -> int:
    cc = find_compiler()
    if cc is None:
        print("No host C compiler found (gcc/cc/clang).", file=sys.stderr)
        print("Set PJS_HOST_CC to one, or skip this check.", file=sys.stderr)
        return 2

    out_dir = pe.build_root() / "host-test"
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = out_dir / ("pjs_alloc_test.exe" if os.name == "nt" else "pjs_alloc_test")

    cmd = [
        cc,
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-O1",
        "-g",
        # <rtthread.h> resolves to the stub, never to the SDK's real header.
        f"-I{STUB_DIR}",
        f"-I{pe.PORT_ROOT / 'include'}",
        *[str(s) for s in SOURCES],
        "-o",
        str(exe),
    ]

    print(f"compiler : {cc}")
    print(f"stub     : {STUB_DIR}")
    print(f"binary   : {exe}")
    print()
    print("$ " + " ".join(cmd))
    print()

    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.stdout.strip():
        print(build.stdout)
    if build.stderr.strip():
        print(build.stderr, file=sys.stderr)
    if build.returncode != 0:
        print(f"RESULT: FAIL (compile failed, exit {build.returncode})")
        return 1

    run = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    if run.stderr.strip():
        sys.stderr.write(run.stderr)
    return run.returncode


if __name__ == "__main__":
    sys.exit(main())
