"""memcap: run one child under a memory cap (2D plan point (n); ruling I12, C1).

``run(argv, cap_bytes, capture_output=False)`` starts ``argv`` and returns the finished
``subprocess.CompletedProcess`` (stdout and stderr as text when ``capture_output`` is true). The
cap is in bytes. Every sweep and bridge driver starts its children through it (Odin III, Yosys,
equiv-check, ``bench_elab``), so a runaway child cannot take the machine down (the WSL VM dies
with the host's memory).

How a child is capped:

- ``systemd-run`` on ``PATH``: ``systemd-run --user --scope -q [--slice=$ODIN3_HEAVY_SLICE] -p
  MemoryMax=<cap> -p MemorySwapMax=0 <argv>``. Inside a heavy run (``tools/heavy-run`` exports the
  slice) the scope is created under that slice, so the slice bounds the job and all its children
  together; a bare nested ``--scope`` would land in ``app.slice``, outside the cap. This holds for
  ASan binaries too.
- no ``systemd-run``: a Release child runs under ``prlimit --as=<cap>`` (``RLIMIT_AS`` in the child
  when ``prlimit`` is missing too). An ASan child cannot run under an address-space limit (its
  shadow memory is terabytes), so ``run`` raises ``CapUnavailable`` and the driver exits 77 (CTest's
  skip). A binary is an ASan child when it contains the bytes ``__asan_init``.

``run`` refuses a cap that is not positive or above 60% of ``MemTotal`` (``ValueError``). Keyword
``cwd`` and ``env`` go to the child. When file descriptor 9 is open (the lock ``tools/heavy-run``
and ``tools/build-locked`` hold), it is passed on to the child (``pass_fds=(9,)``); otherwise a
nested ``tools/heavy-run`` would exit 2.
"""

from __future__ import annotations

import os
import resource
import shutil
import subprocess
from collections.abc import Callable, Mapping, Sequence
from typing import Any

MEMINFO = "/proc/meminfo"  # tests point this at a fake
SYSTEMD_RUN = "systemd-run"
PRLIMIT = "prlimit"
SLICE_ENV = "ODIN3_HEAVY_SLICE"
CAP_FRACTION_PERCENT = 60
LOCK_FD = 9  # the heavy lock (tools/heavy-run, tools/build-locked)
ASAN_MARK = b"__asan_init"
SCAN_CHUNK = 1 << 20

_ASAN_CACHE: dict[tuple[str, int, int], bool] = {}


class CapUnavailable(RuntimeError):
    """The child cannot be capped here (an ASan binary and no systemd-run): the driver skips."""


def mem_total() -> int:
    """MemTotal in bytes from ``MEMINFO``."""
    try:
        with open(MEMINFO, encoding="ascii", errors="replace") as handle:
            for line in handle:
                fields = line.split()
                if len(fields) >= 2 and fields[0] == "MemTotal:" and fields[1].isdigit():
                    return int(fields[1]) * 1024
    except OSError as exc:
        raise RuntimeError(f"cannot read {MEMINFO}: {exc}") from exc
    raise RuntimeError(f"no MemTotal in {MEMINFO}")


def check_cap(cap_bytes: int) -> None:
    """ValueError for a cap that is not positive or is above 60% of MemTotal."""
    if cap_bytes <= 0:
        raise ValueError(f"cap {cap_bytes} is not positive")
    limit = mem_total() * CAP_FRACTION_PERCENT // 100
    if cap_bytes > limit:
        raise ValueError(
            f"cap {cap_bytes} bytes exceeds {CAP_FRACTION_PERCENT}% of MemTotal ({limit} bytes)")


def is_asan(path: str) -> bool:
    """True when the executable at ``path`` holds the ASan runtime's marker (cached per file)."""
    try:
        stat = os.stat(path)
    except OSError:
        return False
    key = (path, stat.st_size, stat.st_mtime_ns)
    if key not in _ASAN_CACHE:
        _ASAN_CACHE[key] = _contains(path, ASAN_MARK)
    return _ASAN_CACHE[key]


def _contains(path: str, needle: bytes) -> bool:
    tail = b""
    try:
        with open(path, "rb") as handle:
            while chunk := handle.read(SCAN_CHUNK):
                if needle in tail + chunk:
                    return True
                tail = chunk[-(len(needle) - 1):]
    except OSError:
        return False
    return False


def wrap(argv: Sequence[str], cap_bytes: int) -> tuple[list[str], Callable[[], None] | None]:
    """The command that runs ``argv`` capped, and a ``preexec_fn`` (RLIMIT_AS) when that is the
    only way; ``CapUnavailable`` for an ASan child without systemd-run."""
    if shutil.which(SYSTEMD_RUN) is not None:
        slice_name = os.environ.get(SLICE_ENV)
        cmd = [SYSTEMD_RUN, "--user", "--scope", "-q"]
        if slice_name:
            cmd.append(f"--slice={slice_name}")
        return [*cmd, "-p", f"MemoryMax={cap_bytes}", "-p", "MemorySwapMax=0", *argv], None
    exe = shutil.which(argv[0]) or argv[0]
    if is_asan(exe):
        raise CapUnavailable(
            f"{argv[0]} is an ASan binary and there is no {SYSTEMD_RUN}; it cannot be capped")
    if shutil.which(PRLIMIT) is not None:
        return [PRLIMIT, f"--as={cap_bytes}", *argv], None

    def limit() -> None:
        resource.setrlimit(resource.RLIMIT_AS, (cap_bytes, cap_bytes))

    return list(argv), limit


TRIO_PROBE = "opt -nodffe -nosdff"  # the trio's own flags; fails until `opt` exists (2F-5)


def lock_fds() -> tuple[int, ...]:
    """The heavy lock's descriptor when open (tools/heavy-run holds it), to pass on to a child."""
    try:
        os.fstat(LOCK_FD)
    except OSError:
        return ()
    return (LOCK_FD,)


def run(argv: Sequence[str], cap_bytes: int, capture_output: bool = False, *,
        cwd: str | os.PathLike[str] | None = None, env: Mapping[str, str] | None = None,
        timeout: float | None = None) -> subprocess.CompletedProcess[str]:
    """Runs ``argv`` under ``cap_bytes`` (see the module text); never raises for a child's own
    failure. ``ValueError`` for a bad cap, ``CapUnavailable`` for an uncappable ASan child,
    ``OSError`` when the command cannot start."""
    check_cap(cap_bytes)
    cmd, preexec = wrap(argv, cap_bytes)
    kwargs: dict[str, Any] = {"capture_output": capture_output, "text": True, "errors": "replace",
                              "check": False, "cwd": cwd, "env": env, "timeout": timeout,
                              "pass_fds": lock_fds()}
    if preexec is not None:
        kwargs["preexec_fn"] = preexec
    return subprocess.run(cmd, **kwargs)  # noqa: S603
