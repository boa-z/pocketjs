"""Shared discovery helpers for the PocketJS ArtInChip D13x port tooling.

Everything that needs to know "where is the SDK" or "where is the Xuantie
toolchain" goes through here, so the three tools in this directory agree on one
answer and a mismatch is reported once, loudly, instead of silently diverging.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

# .../hosts/rt-thread/ports/artinchip-d13x/tools -> this file's directory
TOOLS_DIR = Path(__file__).resolve().parent
PORT_ROOT = TOOLS_DIR.parent


def repo_root() -> Path:
    """PocketJS checkout root."""
    return PORT_ROOT.parents[3]


def rust_dir() -> Path:
    return PORT_ROOT / "rust"


def target_json() -> Path:
    return rust_dir() / "targets" / "d13x-e907-ilp32d.json"


def abi_probe_dir() -> Path:
    return rust_dir() / "abi-probe"


def build_root() -> Path:
    """Ignored build/validation root. Never committed (see .gitignore)."""
    return repo_root() / ".pocket-build" / "d13x"


def rust_target_dir() -> Path:
    return build_root() / "rust-target"


def versions_toml() -> Path:
    return PORT_ROOT / "versions.toml"


# ---------------------------------------------------------------------------
# SDK
# ---------------------------------------------------------------------------

def sdk_root() -> Path:
    """Locate the Luban-Lite checkout this port builds against.

    Order: explicit env override, then the path pinned in ``versions.toml``,
    then the sibling layout (``<workspace>/luban-lite`` beside ``pocketjs``).
    """
    env = os.environ.get("POCKETJS_AIC_SDK_ROOT")
    if env:
        return Path(env).expanduser().resolve()

    pinned = _toml_str("luban_lite", "local_path")
    if pinned:
        cand = Path(pinned).expanduser()
        if (cand / "SConstruct").is_file():
            return cand.resolve()

    sibling = repo_root().parent / "luban-lite"
    if (sibling / "SConstruct").is_file():
        return sibling.resolve()

    raise SystemExit(
        "Cannot locate the Luban-Lite checkout.\n"
        "Set POCKETJS_AIC_SDK_ROOT, or make sure it exists at:\n"
        f"  {pinned or sibling}"
    )


def sdk_head_sha() -> str:
    return _git(sdk_root(), "rev-parse", "HEAD").strip()


def sdk_head_descends_from(sha: str) -> bool:
    """True when `sha` is an ancestor of the SDK's HEAD (equal counts).

    ``base_commit`` is the *fork point*, so on the port branch HEAD is expected
    to sit ahead of it. The invariant worth enforcing is that the pinned
    baseline is still in this branch's history - a failure means the port was
    built on the wrong tree, or the history was rewritten.
    """
    if not sha:
        return False
    proc = subprocess.run(
        ["git", "merge-base", "--is-ancestor", sha, "HEAD"],
        cwd=str(sdk_root()),
        capture_output=True,
        text=True,
    )
    # 0 = ancestor, 1 = not an ancestor, other = bad object / not a repo
    return proc.returncode == 0


def sdk_commits_ahead(sha: str) -> int:
    """How many commits HEAD is ahead of `sha` (0 when equal)."""
    if not sha:
        return 0
    try:
        return int(_git(sdk_root(), "rev-list", "--count", f"{sha}..HEAD").strip() or 0)
    except (ValueError, SystemExit):
        return 0


def sdk_branch() -> str:
    """Current branch name, or a detached-HEAD description."""
    out = _git(sdk_root(), "rev-parse", "--abbrev-ref", "HEAD").strip()
    return out


def sdk_dirty_entries() -> list[str]:
    """Tracked modifications in the SDK tree, excluding submodule churn.

    Submodule pointer noise (`` m <path>`` / `` M <path>``) is filtered out
    because the product SDK legitimately carries submodules that the port never
    touches; only real source edits should be able to fail the gate.
    """
    raw = _git(sdk_root(), "status", "--porcelain").strip()
    if not raw:
        return []
    entries = []
    for line in raw.splitlines():
        if len(line) > 2 and line[1] == "M" and line[2] == " ":
            continue  # submodule worktree change
        entries.append(line)
    return entries


def sdk_is_dirty() -> bool:
    return bool(sdk_dirty_entries())


def port_branch() -> str:
    return _toml_str("luban_lite", "port_branch") or "pocketjs-d13x"


def sdk_base_commit() -> str:
    return _toml_str("luban_lite", "base_commit") or ""


def _toml_str(section: str, key: str) -> str | None:
    """Minimal reader for the handful of flat string keys versions.toml holds."""
    path = versions_toml()
    if not path.is_file():
        return None
    current = None
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1].strip()
            continue
        if current != section or "=" not in line:
            continue
        k, _, v = line.partition("=")
        if k.strip() != key:
            continue
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
            return v[1:-1]
        return v
    return None


def _git(cwd: Path, *args: str) -> str:
    proc = subprocess.run(
        ["git", *args],
        cwd=str(cwd),
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise SystemExit(f"git {' '.join(args)} failed in {cwd}:\n{proc.stderr}")
    return proc.stdout


# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------

PREFIX = "riscv64-unknown-elf-"


def toolchain_bin() -> Path | None:
    """Directory holding the Xuantie `riscv64-unknown-elf-*` binaries.

    The official Luban-Lite repo does not vendor the toolchain, so this may
    legitimately be an external install. Order: explicit override, the SDK's
    own ``toolchain/``, then ``toolchain/`` beside the PocketJS checkout.
    """
    candidates = []
    for var in ("POCKETJS_AIC_TOOLCHAIN", "RTT_EXEC_PATH"):
        val = os.environ.get(var)
        if val:
            candidates.append(Path(val).expanduser())

    try:
        candidates.append(sdk_root() / "toolchain")
    except SystemExit:
        pass
    candidates.append(repo_root().parent / "toolchain")

    for cand in candidates:
        exe = cand / "bin" / f"{PREFIX}gcc.exe"
        exe_unix = cand / "bin" / f"{PREFIX}gcc"
        if exe.is_file() or exe_unix.is_file():
            return cand / "bin"
    return None


def tool(name: str) -> str:
    """Resolve a Xuantie binutil, preferring the discovered toolchain."""
    binned = toolchain_bin()
    if binned is not None:
        for suffix in (".exe", ""):
            cand = binned / f"{PREFIX}{name}{suffix}"
            if cand.is_file():
                return str(cand)
    found = shutil.which(f"{PREFIX}{name}")
    if found:
        return found
    raise SystemExit(
        f"Cannot find {PREFIX}{name}.\n"
        "Set POCKETJS_AIC_TOOLCHAIN to the Xuantie toolchain root "
        "(the directory containing bin/)."
    )


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kwargs)


def cargo() -> str:
    found = shutil.which("cargo")
    if not found:
        raise SystemExit("cargo not found on PATH.")
    return found


def rustc_version() -> str:
    proc = run([rustc_path(), "--version"])
    return proc.stdout.strip() or proc.stderr.strip()


def rustc_path() -> str:
    found = shutil.which("rustc")
    if not found:
        raise SystemExit("rustc not found on PATH.")
    return found


# ---------------------------------------------------------------------------
# SCons interpreter
# ---------------------------------------------------------------------------

# Where this port's tooling is normally provisioned (see the repo README).
_VENV_CANDIDATES = (
    Path.home() / ".workbuddy-ai" / "binaries" / "python" / "envs" / "default"
    / "Scripts" / "python.exe",
    Path.home() / ".workbuddy-ai" / "binaries" / "python" / "envs" / "default"
    / "bin" / "python",
)


def scons_python() -> str:
    """An interpreter that can ``import SCons``.

    Luban-Lite is built with SCons, but SCons is not stdlib: whichever Python
    runs the build tools must be the one it was installed into. Assuming
    ``sys.executable`` fails confusingly (``No module named SCons``) when the
    tools are launched with a bare interpreter that never had it. So probe the
    candidates instead, and on failure print the one-line fix.
    """
    candidates: list[str] = []
    env = os.environ.get("POCKETJS_SCONS_PYTHON")
    if env:
        candidates.append(env)
    candidates.append(sys.executable)
    candidates.extend(str(p) for p in _VENV_CANDIDATES)
    for name in ("python3", "python"):
        found = shutil.which(name)
        if found:
            candidates.append(found)

    tried: list[str] = []
    for cand in candidates:
        if not cand or cand in tried:
            continue
        tried.append(cand)
        if not Path(cand).is_file():
            continue
        proc = subprocess.run([cand, "-c", "import SCons"],
                              capture_output=True, text=True)
        if proc.returncode == 0:
            return cand

    raise SystemExit(
        "No Python with SCons found - Luban-Lite cannot be built.\n"
        "Install it into the interpreter you build with:\n"
        f"  {sys.executable} -m pip install scons\n"
        "or point POCKETJS_SCONS_PYTHON at an interpreter that already has it.\n"
        "Tried:\n" + "".join(f"  {t}\n" for t in tried)
    )


def info(msg: str) -> None:
    print(msg, file=sys.stderr)


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)


def ok(msg: str) -> None:
    print(f"ok:   {msg}", file=sys.stderr)
