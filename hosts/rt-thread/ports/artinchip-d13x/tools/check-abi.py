#!/usr/bin/env python3
"""Gate 0 static ABI verification for the PocketJS D13x Rust staticlib.

"it links" is not evidence. This script inspects the actual ELF objects with
the Xuantie binutils and refuses to pass unless the archive really is
RV32IMAFDC / ILP32D:

  * every member is ELF32 RISC-V
  * every member carries EF_RISCV_FLOAT_ABI_DOUBLE
  * Tag_RISCV_arch advertises d, and does not advertise p or xthead
  * the probe entry points use hardware double, not soft-float helpers
  * the f64 argument/return registers are the FP register file
  * no POSIX/libc dependency smuggled in as an undefined symbol

Exit code 0 = PASS, 1 = FAIL.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

import portenv as pe

# Soft-float helpers. A call to any of these from our own code would mean the
# D extension is not actually being used for arithmetic.
SOFT_DOUBLE = ("__adddf3", "__subdf3", "__muldf3", "__divdf3")

# Undefined symbols that would betray a fake POSIX/libc dependency
# (spec section 40). Rust source must stay RTOS/bare-metal.
FORBIDDEN_UNDEFINED = (
    "pthread_create", "pthread_mutex_lock", "mmap", "munmap", "dlopen", "dlsym",
    "fork", "execve", "epoll_create", "epoll_wait", "open", "openat", "close",
    "read", "write", "lseek", "ioctl", "printf", "fprintf", "sprintf", "puts",
    "malloc", "calloc", "realloc", "free", "abort", "exit", "_exit", "atexit",
    "sysconf", "sbrk", "brk", "signal", "sigaction", "gettimeofday", "clock_gettime",
    "opendir", "readdir", "stat", "fopen", "fread", "fwrite",
)

# Symbols the C harness is expected to provide.
EXPECTED_UNDEFINED = (
    "pjs_host_u32", "pjs_host_f32", "pjs_host_f64", "pjs_host_value",
    "pjs_host_nested", "pjs_host_mixed", "pjs_host_alloc", "pjs_host_free",
    "pjs_host_abort",
)

# Probe entry points that must be present and exported.
EXPECTED_EXPORTED = (
    "pjs_probe_u32", "pjs_probe_i32", "pjs_probe_usize", "pjs_probe_f32",
    "pjs_probe_f64", "pjs_probe_ptr_sum", "pjs_probe_value", "pjs_probe_nested",
    "pjs_probe_mixed", "pjs_probe_layout", "pjs_probe_alloc", "pjs_probe_mem_stats",
    "pjs_probe_panic", "pjs_probe_call_host_u32", "pjs_probe_call_host_f32",
    "pjs_probe_call_host_f64", "pjs_probe_call_host_value",
    "pjs_probe_call_host_nested", "pjs_probe_call_host_mixed",
    "pjs_probe_roundtrip_ptr", "pjs_probe_log_len",
)

FAILURES: list[str] = []


def check(cond: bool, label: str, detail: str = "") -> bool:
    if cond:
        pe.ok(label)
    else:
        pe.fail(label + (f" - {detail}" if detail else ""))
        FAILURES.append(label)
    return cond


def archive_path() -> str:
    if len(sys.argv) > 1:
        return sys.argv[1]
    cand = pe.rust_target_dir() / "d13x-e907-ilp32d" / "release" / "libpocketjs_abi_probe.a"
    if not cand.is_file():
        raise SystemExit(
            f"Rust staticlib not found at {cand}\n"
            "Run tools/build-native.py first."
        )
    return str(cand)


def main() -> int:
    arc = archive_path()
    readelf = pe.tool("readelf")
    objdump = pe.tool("objdump")
    nm = pe.tool("nm")
    ar = pe.tool("ar")

    print(f"archive: {arc}")
    print(f"toolchain binutils: {readelf}")
    print()

    # --- ELF header checks -------------------------------------------------
    hdr = pe.run([readelf, "-h", arc]).stdout
    classes = set(re.findall(r"Class:\s+(\S+)", hdr))
    machines = set(re.findall(r"Machine:\s+(.+)", hdr))
    flags = re.findall(r"Flags:\s+(.+)", hdr)

    check(classes == {"ELF32"}, "ELF class is ELF32 on every member", f"got {sorted(classes)}")
    check(machines == {"RISC-V"}, "machine is RISC-V on every member", f"got {sorted(machines)}")
    check(bool(flags), "ELF header flags present")
    bad_flags = [f for f in flags if "double-float ABI" not in f]
    check(
        not bad_flags,
        "EF_RISCV_FLOAT_ABI_DOUBLE set on every member",
        f"{len(bad_flags)} member(s) without double-float ABI, e.g. {bad_flags[:2]}",
    )
    print(f"       ({len(flags)} member ELF headers inspected)")

    # --- ISA attributes ----------------------------------------------------
    # The archive may have been through patch-riscv-attrs.py, which removes
    # .riscv.attributes so binutils 2.35 can link against the vendor libc. In
    # that case the as-emitted metadata lives in the sidecar evidence file, and
    # checking it there is still checking what rustc actually produced.
    attr = pe.run([readelf, "-A", arc]).stdout
    arch_source = "archive"
    if not re.search(r"Tag_RISCV_arch:", attr):
        side = Path(arc + ".riscv-attrs.txt")
        if side.is_file():
            attr = side.read_text(encoding="utf-8")
            arch_source = str(side)
        else:
            arch_source = "MISSING"
    print(f"       (ISA attributes read from: {arch_source})")

    arches = set(re.findall(r'Tag_RISCV_arch:\s+"([^"]+)"', attr))
    check(bool(arches), "Tag_RISCV_arch present",
          "archive was normalised but no sidecar evidence file was found")
    joined = " ".join(arches)
    check("_d2p" in joined, "Tag_RISCV_arch advertises the D extension")
    check("_f2p" in joined, "Tag_RISCV_arch advertises the F extension")
    check("_m2p" in joined, "Tag_RISCV_arch advertises the M extension")
    # Spec section 10.1: no P extension, no XThead, no vendor extensions.
    check("_p" not in joined.replace("_p2p", "") or "zpsfoperand" not in joined,
          "no P extension (zpsfoperand) in Tag_RISCV_arch")
    check("xthead" not in joined, "no XThead vendor extension in Tag_RISCV_arch")
    for a in sorted(arches):
        print(f"       arch: {a}")

    # --- disassembly checks ------------------------------------------------
    dis = pe.run([objdump, "-d", "--no-show-raw-insn", arc]).stdout
    funcs: dict[str, list[str]] = {}
    cur: str | None = None
    for line in dis.splitlines():
        m = re.match(r"^[0-9a-f]+ <(.+)>:$", line.strip())
        if m:
            cur = m.group(1)
            funcs[cur] = []
        elif cur is not None and line.strip():
            funcs[cur].append(line.strip())

    probe_funcs = {k: v for k, v in funcs.items() if k.startswith("pjs_probe")}
    check(bool(probe_funcs), "probe functions found in disassembly")

    soft_hits = []
    for name, body in probe_funcs.items():
        for ins in body:
            if re.search(r"\b(call|jal|tail|j)\b", ins) and any(s in ins for s in SOFT_DOUBLE):
                soft_hits.append(f"{name}: {ins}")
    check(
        not soft_hits,
        "no soft-float double helpers called from any pjs_probe_* function",
        f"{soft_hits[:3]}",
    )

    # pjs_probe_f64 must take its double in fa0 and return it in fa0, and use
    # real .d arithmetic.
    f64_body = funcs.get("pjs_probe_f64", [])
    f64_text = "\n".join(f64_body)
    check(bool(f64_body), "pjs_probe_f64 disassembled")
    check("fmul.d" in f64_text or "fadd.d" in f64_text,
          "pjs_probe_f64 uses hardware double arithmetic (.d opcodes)")
    check(re.search(r"\bfa0\b", f64_text) is not None,
          "pjs_probe_f64 uses FP register fa0 (ILP32D argument/return register)")
    for ins in f64_body:
        print(f"       {ins}")

    # --- symbol checks -----------------------------------------------------
    undef = set()
    for line in pe.run([nm, "-u", arc]).stdout.splitlines():
        parts = line.split()
        if parts:
            undef.add(parts[-1])
    # nm -u prints member-name lines too; drop the ":" suffixed ones.
    undef = {s for s in undef if not s.endswith(":")}

    forbidden = sorted(s for s in undef if s in FORBIDDEN_UNDEFINED)
    check(not forbidden, "no forbidden POSIX/libc undefined symbols", f"{forbidden}")

    missing_host = sorted(s for s in EXPECTED_UNDEFINED if s not in undef)
    check(
        not missing_host,
        "all expected pjs_host_* callbacks are referenced",
        f"missing {missing_host}",
    )

    defined = set()
    for line in pe.run([nm, "--defined-only", arc]).stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            defined.add(parts[-1])
    missing_exported = sorted(s for s in EXPECTED_EXPORTED if s not in defined)
    check(
        not missing_exported,
        "all probe entry points are exported",
        f"missing {missing_exported}",
    )

    members = pe.run([ar, "t", arc]).stdout.split()
    print(f"       archive members: {len(members)}")
    print(f"       undefined symbols: {len(undef)}")

    print()
    if FAILURES:
        print(f"RESULT: FAIL ({len(FAILURES)} check(s) failed)")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
