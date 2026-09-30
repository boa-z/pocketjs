#!/usr/bin/env python3
"""Static ABI verification for the PocketJS D13x Rust staticlibs.

"it links" is not evidence. This script inspects the actual ELF objects with
the Xuantie binutils and refuses to pass unless the archives really are
RV32IMAFDC / ILP32D:

  * every member is ELF32 RISC-V
  * every member carries EF_RISCV_FLOAT_ABI_DOUBLE
  * Tag_RISCV_arch advertises d, and does not advertise p or xthead
  * the probe entry points use hardware double, not soft-float helpers
  * the f64 argument/return registers are the FP register file
  * no POSIX/libc dependency smuggled in as an undefined symbol

Two archives are checked, because the Gate 1 firmware links two:

  libpocketjs_abi_probe.a         -> pjs_probe_*           (Gate 0 probe)
  libpocketjs_rtthread_ui_core.a  -> pocketjs_native_ui_*  (retained UI core)

The ELF-class, ISA and float-ABI checks are identical for both, so they run
over each archive in turn. The disassembly checks that prove hardware double
arithmetic are probe-specific and only run against the probe.

``--elf PATH`` adds the link-level assertions, and those are the ones that
actually close Gate 0 section 8.4. ``pjs_host_log`` is defined in C, so nothing
about any archive can prove it is reachable: only the linked image can show
that the Rust side references it, that the linker therefore pulled the member
that needs it, and that it survived ``-Wl,-gc-sections``.

Usage:
    python tools/check-abi.py                       # both archives
    python tools/check-abi.py --elf <d13x.elf>      # + the link-level checks
    python tools/check-abi.py <probe.a> --ui-core <ui_core.a>

Exit code 0 = PASS, 1 = FAIL.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
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

# Host callbacks the probe is expected to reference. `pjs_host_log` is
# deliberately absent from this list: the probe's own code never calls it. That
# gap is Gate 0 section 8.4, and it is closed by the UI core and asserted
# against the linked ELF.
#
# The probe archive does now carry an undefined `pjs_host_log` all the same,
# because it bundles the runtime crate, which defines `host_log` on top of it.
# That is a property of the dependency graph rather than of the probe: nothing
# in the probe calls it, so `--gc-sections` would still drop it from a probe-only
# image. Only the ELF check can show the UI core keeping it alive.
PROBE_UNDEFINED = (
    "pjs_host_u32", "pjs_host_f32", "pjs_host_f64", "pjs_host_value",
    "pjs_host_nested", "pjs_host_mixed", "pjs_host_alloc", "pjs_host_free",
    "pjs_host_abort",
)

# Probe entry points that must be present and exported.
PROBE_EXPORTED = (
    "pjs_probe_u32", "pjs_probe_i32", "pjs_probe_usize", "pjs_probe_f32",
    "pjs_probe_f64", "pjs_probe_ptr_sum", "pjs_probe_value", "pjs_probe_nested",
    "pjs_probe_mixed", "pjs_probe_layout", "pjs_probe_alloc", "pjs_probe_mem_stats",
    "pjs_probe_panic", "pjs_probe_call_host_u32", "pjs_probe_call_host_f32",
    "pjs_probe_call_host_f64", "pjs_probe_call_host_value",
    "pjs_probe_call_host_nested", "pjs_probe_call_host_mixed",
    "pjs_probe_roundtrip_ptr", "pjs_probe_log_len",
)

# Host seam the retained UI core resolves against. `pjs_host_log` is the one
# that matters - it is the symbol Gate 0 section 8.4 is about, and it is why the
# RT-Thread runtime crate exists separately from the ESP-IDF one.
UI_CORE_UNDEFINED = (
    "pjs_host_alloc", "pjs_host_free", "pjs_host_abort", "pjs_host_log",
)

# Raw entry points the UI core archive must export.
#
# Spelled out rather than read back from the generated `native_ui.h`: the header
# is produced by parsing this same crate, so comparing the archive against it
# would only be the crate agreeing with itself. A literal list is what catches a
# rename or a silently dropped entry point.
UI_CORE_EXPORTED = (
    "pocketjs_native_ui_animate",
    "pocketjs_native_ui_cancel_animation",
    "pocketjs_native_ui_create",
    "pocketjs_native_ui_create_node",
    "pocketjs_native_ui_destroy",
    "pocketjs_native_ui_destroy_node",
    "pocketjs_native_ui_draw",
    "pocketjs_native_ui_font",
    "pocketjs_native_ui_frame_validate",
    "pocketjs_native_ui_free_texture",
    "pocketjs_native_ui_get_config",
    "pocketjs_native_ui_hit_test",
    "pocketjs_native_ui_hit_test_bounds",
    "pocketjs_native_ui_insert_before",
    "pocketjs_native_ui_load_assets",
    "pocketjs_native_ui_load_font",
    "pocketjs_native_ui_load_styles",
    "pocketjs_native_ui_measure_text",
    "pocketjs_native_ui_remove_child",
    "pocketjs_native_ui_replace_text",
    "pocketjs_native_ui_set_active",
    "pocketjs_native_ui_set_cursor",
    "pocketjs_native_ui_set_cursor_position",
    "pocketjs_native_ui_set_focus",
    "pocketjs_native_ui_set_image",
    "pocketjs_native_ui_set_prop",
    "pocketjs_native_ui_set_sprite",
    "pocketjs_native_ui_set_style",
    "pocketjs_native_ui_set_text",
    "pocketjs_native_ui_texture",
    "pocketjs_native_ui_tick",
    "pocketjs_native_ui_touch_hits",
    "pocketjs_native_ui_upload_img_entry",
    "pocketjs_native_ui_upload_texture",
    "pocketjs_native_ui_wrap_text",
)

# Symbols the linked image must define for Gate 0 section 8.4 to be closed.
# `pjs_host_log` is the point: it is defined in C, and while nothing on the Rust
# side called it the linker had no reason to keep it. Its presence in the final
# ELF is the only proof that the Rust -> C log direction is really wired.
RENDER_EXPORTED = (
    "pocketjs_native_render_target_create", "pocketjs_native_render_target_destroy",
    "pocketjs_native_render_target_invalidate", "pocketjs_native_renderer_create",
    "pocketjs_native_renderer_destroy", "pocketjs_native_renderer_prepare",
    "pocketjs_native_renderer_render_strip", "pocketjs_native_renderer_commit",
    "pocketjs_native_renderer_abort",
)

ELF_REQUIRED_DEFINED = (
    "pocketjs_native_renderer_prepare", "pocketjs_native_renderer_render_strip",
    "pocketjs_native_renderer_commit", "pocketjs_native_renderer_abort",
    "pjs_host_log",
    "pjs_host_alloc",
    "pjs_host_free",
    "pjs_host_abort",
    # The UI core member must have been pulled, which only happens because the
    # thunk layer references it. These are the two call sites of `host_log`.
    "pocketjs_native_ui_create",
    "pocketjs_native_ui_destroy",
    # ...and the thunk layer itself must have survived.
    "pocketjs_ui_core_create",
    "pocketjs_ui_core_destroy",
)

FAILURES: list[str] = []


def check(cond: bool, label: str, detail: str = "") -> bool:
    if cond:
        pe.ok(label)
    else:
        pe.fail(label + (f" - {detail}" if detail else ""))
        FAILURES.append(label)
    return cond


@dataclass(frozen=True)
class ArchiveSpec:
    label: str
    path: Path
    exported: tuple[str, ...]
    undefined: tuple[str, ...] = ()
    probe: bool = False  # run the double-arithmetic disassembly checks


def default_probe_archive() -> Path:
    return (pe.rust_target_dir() / "d13x-e907-ilp32d" / "release"
            / "libpocketjs_abi_probe.a")


def default_ui_core_archive() -> Path:
    return (pe.rust_target_dir() / "d13x-e907-ilp32d" / "release"
            / "libpocketjs_rtthread_ui_core.a")


def nm_symbols(archive: str, mode: str) -> set[str]:
    """Symbol names from `nm`, dropping the `member.a:` header lines."""
    out: set[str] = set()
    for line in pe.run([pe.tool("nm"), mode, archive]).stdout.splitlines():
        parts = line.split()
        if parts:
            out.add(parts[-1])
    return {s for s in out if not s.endswith(":")}


# Archive member names carry cargo's `-Cmetadata` hash:
#   pocketjs_rtthread_runtime-207c131b3b3f7893.pocketjs_rtthread_runtime.<hash>-cgu.0.rcgu.o
# The hash after the crate name is what rustc folds into every mangled symbol in
# that crate. Reading it back from the built archive is what turns "the two
# archives must agree" from a convention into a check.
#
# compiler-rt C members use the same hash as a *prefix* (`<hash>-int_util.o`) and
# so do not match: the pattern anchors on an identifier at the start.
MEMBER_METADATA = re.compile(r"^(?P<crate>[A-Za-z_][A-Za-z0-9_]*)-(?P<meta>[0-9a-f]{16})\.")


def archive_member_metadata(path: Path) -> dict[str, set[str]]:
    """crate name -> the set of `-Cmetadata` hashes its archive members carry."""
    found: dict[str, set[str]] = {}
    for member in pe.run([pe.tool("ar"), "t", str(path)]).stdout.split():
        m = MEMBER_METADATA.match(member)
        if m:
            found.setdefault(m.group("crate"), set()).add(m.group("meta"))
    return found


def check_shared_metadata(archives: list[tuple[str, Path]]) -> None:
    """Every crate present in more than one archive must carry one metadata hash.

    A Rust `staticlib` bundles its entire dependency graph, so both archives
    carry `core`, `alloc`, `compiler_builtins` and `pocketjs-rtthread-runtime`.
    That is safe only while the copies are interchangeable: the linker then
    extracts one copy to satisfy every reference and never needs the second.

    If the same crate is built with different metadata in the two archives, the
    linker sees two differently named copies, cannot satisfy the second archive's
    references from the first, and extracts both. The compiler-owned `__rustc`
    allocator/panic shims keep their *fixed* names through all of this, so they
    are what finally collides:

        multiple definition of `_RNvCs4iuDAxO633X_7___rustc12___rust_alloc'

    This check exists because that error names `__rust_alloc` and says nothing
    about why. It catches both ways the metadata drifts - a differing
    `[profile.release]`, and a differing feature set on a shared dependency.
    The second is easy to miss: `default-features = false` on one side only is
    enough, because cargo emits `--cfg feature="default"` for one build and not
    the other, and those cfgs are folded into the metadata.
    """
    print()
    print("=== cross-archive metadata (the shared dependency graph) ===")
    per_archive = {label: archive_member_metadata(path) for label, path in archives}

    every_crate: set[str] = set()
    for found in per_archive.values():
        every_crate |= set(found)

    shared = 0
    for crate in sorted(every_crate):
        present = {label: by_crate[crate] for label, by_crate in per_archive.items()
                   if crate in by_crate}
        if len(present) < 2:
            continue
        shared += 1
        merged: set[str] = set()
        for hashes in present.values():
            merged |= hashes
        detail = "; ".join(f"{label} -> {sorted(h)}" for label, h in present.items())
        check(len(merged) == 1,
              f"shared crate `{crate}` carries one metadata hash in every archive",
              detail)

    check(shared > 0, "at least one crate is shared by both archives",
          "expected core/alloc/compiler_builtins/runtime - is an archive empty?")
    if shared:
        print(f"       shared crates checked: {shared}")


def check_archive(spec: ArchiveSpec) -> None:
    arc = str(spec.path)
    readelf = pe.tool("readelf")
    objdump = pe.tool("objdump")
    ar = pe.tool("ar")

    print()
    print(f"=== {spec.label} ===")
    print(f"archive: {arc}")

    if not spec.path.is_file():
        check(False, f"{spec.label}: archive present",
              f"{arc} is missing - run tools/build-native.py first")
        return

    # --- ELF header checks -------------------------------------------------
    hdr = pe.run([readelf, "-h", arc]).stdout
    classes = set(re.findall(r"Class:\s+(\S+)", hdr))
    machines = set(re.findall(r"Machine:\s+(.+)", hdr))
    flags = re.findall(r"Flags:\s+(.+)", hdr)

    check(classes == {"ELF32"}, "ELF class is ELF32 on every member",
          f"got {sorted(classes)}")
    check(machines == {"RISC-V"}, "machine is RISC-V on every member",
          f"got {sorted(machines)}")
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

    # --- probe-only disassembly checks -------------------------------------
    if spec.probe:
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
    undef = nm_symbols(arc, "-u")
    forbidden = sorted(s for s in undef if s in FORBIDDEN_UNDEFINED)
    check(not forbidden, "no forbidden POSIX/libc undefined symbols", f"{forbidden}")

    missing_host = sorted(s for s in spec.undefined if s not in undef)
    check(
        not missing_host,
        "all expected pjs_host_* callbacks are referenced",
        f"missing {missing_host}",
    )

    defined: set[str] = set()
    for line in pe.run([pe.tool("nm"), "--defined-only", arc]).stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            defined.add(parts[-1])
    missing_exported = sorted(s for s in spec.exported if s not in defined)
    check(
        not missing_exported,
        "all entry points are exported",
        f"missing {missing_exported}",
    )

    members = pe.run([pe.tool("ar"), "t", arc]).stdout.split()
    print(f"       archive members: {len(members)}")
    print(f"       undefined symbols: {len(undef)}")
    print(f"       exported entry points checked: {len(spec.exported)}")


def check_linked_elf(elf: Path) -> None:
    """Link-level assertions. This is what actually closes section 8.4."""
    print()
    print("=== linked image ===")
    print(f"elf: {elf}")

    if not elf.is_file():
        check(False, "linked ELF present", f"{elf} is missing")
        return

    symbols: dict[str, str] = {}
    for line in pe.run([pe.tool("nm"), str(elf)]).stdout.splitlines():
        parts = line.split()
        # "<addr> <type> <name>"; undefined entries have no address column.
        if len(parts) == 3:
            symbols[parts[2]] = parts[1]
        elif len(parts) == 2:
            symbols[parts[1]] = parts[0]

    missing = sorted(s for s in ELF_REQUIRED_DEFINED if s not in symbols)
    check(
        not missing,
        "every required symbol survived --gc-sections into the linked image",
        f"missing {missing}",
    )

    # A symbol can be present as an *undefined* reference, which would mean the
    # archive was never pulled in. Distinguish the two rather than trusting the
    # name alone.
    undefined_here = sorted(
        s for s in ELF_REQUIRED_DEFINED if symbols.get(s) in ("U", "w", "v")
    )
    check(
        not undefined_here,
        "no required symbol is only an undefined reference",
        f"{undefined_here}",
    )

    for name in ELF_REQUIRED_DEFINED:
        kind = symbols.get(name)
        if kind is not None:
            print(f"       {kind} {name}")

    # The UI core archive contributes many more entry points than the two the
    # log path needs; a link that pulled only those would be suspicious.
    native = sorted(s for s in symbols if s.startswith("pocketjs_native_ui_"))
    check(
        len(native) >= 10,
        "the retained UI core is substantially linked in, not just the log path",
        f"only {len(native)} pocketjs_native_ui_* symbol(s) in the image",
    )
    print(f"       pocketjs_native_ui_* symbols in image: {len(native)}")

    # Gate 0's headline result must not regress: the probe is still linked.
    probes = sorted(s for s in symbols if s.startswith("pjs_probe_"))
    check(
        len(probes) >= 10,
        "the Gate 0 probe is still linked into the same image",
        f"only {len(probes)} pjs_probe_* symbol(s) in the image",
    )
    print(f"       pjs_probe_* symbols in image: {len(probes)}")

    eflags = re.findall(r"Flags:\s+(.+)", pe.run([pe.tool("readelf"), "-h", str(elf)]).stdout)
    bad = [f for f in eflags if "double-float ABI" not in f]
    check(not bad, "linked ELF keeps the double-float ABI flag",
          f"got {eflags}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive", nargs="?", default=None,
                    help="probe archive (default: the built one under .pocket-build)")
    ap.add_argument("--ui-core", type=Path, default=None,
                    help="UI core archive (default: the built one under .pocket-build)")
    ap.add_argument("--elf", type=Path, default=None,
                    help="linked firmware ELF; enables the link-level checks")
    args = ap.parse_args()

    probe = Path(args.archive) if args.archive else default_probe_archive()
    ui_core = args.ui_core if args.ui_core else default_ui_core_archive()

    print(f"toolchain binutils: {pe.tool('readelf')}")

    specs = [
        ArchiveSpec(label="probe archive (Gate 0)", path=probe,
                    exported=PROBE_EXPORTED, undefined=PROBE_UNDEFINED, probe=True),
        ArchiveSpec(label="UI core archive (Gate 1)", path=ui_core,
                    exported=UI_CORE_EXPORTED, undefined=UI_CORE_UNDEFINED),
    ]

    renderer = pe.rust_target_dir() / "d13x-e907-ilp32d/release/libpocketjs_rtthread_render_rgb565.a"
    specs.append(ArchiveSpec(label="RGB565 renderer (Gate 2)", path=renderer,
                             exported=RENDER_EXPORTED,
                             undefined=("pjs_host_alloc", "pjs_host_free", "pjs_host_abort",
                                        "pocketjs_native_ui_frame_validate")))
    for spec in specs:
        check_archive(spec)

    check_shared_metadata([(s.label, s.path) for s in specs if s.path.is_file()])

    if args.elf is not None:
        check_linked_elf(args.elf)
    else:
        print()
        print("note: no --elf given, so the link-level checks were skipped.")
        print("      Gate 0 section 8.4 (pjs_host_log surviving --gc-sections)")
        print("      is only closed by those checks; pass the built d13x.elf.")

    print()
    if FAILURES:
        print(f"RESULT: FAIL ({len(FAILURES)} check(s) failed)")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
