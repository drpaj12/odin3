"""qor_heavy: the heavy-run convention for vtr-qor and the heavy tools of 3D and 3E.

The 3H spec QH-7.1, as made exact by the 3H plan's point (a), (b), (d) and (m):

- ``inside_heavy_run`` checks ``tools/heavy-run``'s own nesting evidence: ``ODIN3_HEAVY_SLICE``
  is set, this process's cgroup lies under that slice, and descriptor 9 (inherited from
  ``heavy-run``) is the heavy lock. The variable alone is not evidence.
- ``reexec_under_lock`` re-runs a tool through ``tools/heavy-run`` (never a copy of it) with the
  caller's environment unchanged; inside a heavy run it only verifies the cap.
- ``verify_scope`` walks the cgroup ancestors: ``heavy-run`` caps the slice, so the scope's own
  ``memory.max`` reads ``max``.
- The machine readers (``disk_free_gib``, ``mem_available_gib``) feed admission (spec §7.3).
- ``safe_rmtree`` is the only deletion (spec §7.5): strictly under the run's root, through no
  symlink, on one file system, and only under ``~/odin3-ws/work`` or ``/tmp/claude-1000``.

Command line (3E's shell tool ``blif-roundtrip --vtr``)::

    python3 -I tools/vtr-qor/qor_heavy.py require-heavy [--xl]

exits 0 inside a heavy run with the cap verified, 3 when not inside, and 2 (with
``vtr-qor: <message>`` on stderr) when inside but the cap check fails.

No safety check reads a test hook from the environment (controller ruling, fix round 1): the
production paths read only ``/proc/self/cgroup``, ``/sys/fs/cgroup``, ``/proc/self/fd/9``,
``HEAVY_LOCK`` and the real ``df`` on ``/mnt/c``. Tests replace the module-level seams
(``SELF_CGROUP``, ``CGROUP_ROOT``, ``FD9``, ``HEAVY_LOCK``, ``HEAVY_RUN``, ``DF_CMD``) or pass the
keyword parameters that ``inside_heavy_run``, ``verify_scope`` and ``reexec_under_lock`` take.
"""

from __future__ import annotations

import argparse
import contextlib
import fcntl
import os
import pwd
import shutil
import stat
import subprocess
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

HEAVY_DIR = Path("/tmp/claude-1000")
HEAVY_LOCK = HEAVY_DIR / "odin3-heavy.lock"
CAP_BYTES = 6 * 1024**3
CAP_XL_BYTES = 9 * 1024**3
REPO_ROOT = Path(__file__).resolve().parents[2]
HEAVY_RUN = REPO_ROOT / "tools" / "heavy-run"
XL_SLICE = "odin3-heavyxl.slice"
WORK_SUBDIR = Path("odin3-ws") / "work"
SELF_CGROUP = Path("/proc/self/cgroup")
CGROUP_ROOT = Path("/sys/fs/cgroup")
FD9 = Path("/proc/self/fd/9")
WIN_C = "/mnt/c"
# df by absolute path (never PATH): /usr/bin/df, or /bin/df without the merged /usr.
DF_BIN = "/usr/bin/df" if Path("/usr/bin/df").exists() else "/bin/df"
DF_CMD: tuple[str, ...] = (DF_BIN, "-B1", "--output=target,avail", WIN_C)
# The longest a df of /mnt/c may take (a stalled drvfs mount can block statfs).
DF_TIMEOUT_S = 30.0
# How long a killed df is waited for before it is let go (it may be stuck in the kernel).
DF_REAP_S = 1.0

PREFIX = "vtr-qor: "
EXIT_USAGE = 2
EXIT_NOT_INSIDE = 3
EXIT_LOCK_BUSY = 4
EXIT_DISK = 5
HEAVY_RUN_SKIPPED = 77
SIGNAL_EXIT_BASE = 128
MIB = 1024**2
KIB_PER_GIB = 1024**2
GIB = 1024**3

MSG_NOT_CONTROLLER = "heavy runs are the controller's: set ODIN3_HEAVY_OK=1"
MSG_XL_NOT_OK = "--xl needs ODIN3_HEAVY_XL_OK=1 (an idle machine; the controller starts it)"
MSG_NOT_INSIDE = "not inside a heavy run: the controller starts it through tools/heavy-run"
MSG_CAP_77 = "cannot apply the memory cap (tools/heavy-run exited 77; see its message above)"
MSG_DF = "cannot read free space of /mnt/c"


class HeavyError(Exception):
    """A heavy-run refusal: ``code`` is the exit code (plan, Global Constraints), ``message``
    the text without the ``vtr-qor: `` prefix, which the CLI adds."""

    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


@dataclass(frozen=True)
class ScopeInfo:
    """The verified cap: ``path`` is the capping cgroup directory (whose ``memory.events``
    counts OOM kills), ``leaf`` this process's own cgroup (whose ``memory.peak`` is the job's)."""

    path: Path
    leaf: Path
    memory_max: int
    swap_zero: bool


def _environ(env: Mapping[str, str] | None) -> Mapping[str, str]:
    return os.environ if env is None else env


def _cgroup_path(self_cgroup: Path) -> str | None:
    """The ``0::<path>`` entry of ``/proc/self/cgroup`` (cgroup v2), or None."""
    try:
        text = self_cgroup.read_text(encoding="utf-8")
    except OSError:
        return None
    for line in text.splitlines():
        if line.startswith("0::"):
            return line[3:].strip()
    return None


def inside_heavy_run(
    env: Mapping[str, str] | None = None,
    *,
    self_cgroup: Path = SELF_CGROUP,
    fd_link: Path = FD9,
    lock: Path = HEAVY_LOCK,
) -> bool:
    """True when this process runs inside ``tools/heavy-run``: ``ODIN3_HEAVY_SLICE`` is set, the
    cgroup path lies under that slice, and ``fd_link`` (descriptor 9) resolves to ``lock``."""
    heavy_slice = _environ(env).get("ODIN3_HEAVY_SLICE", "")
    if not heavy_slice or "/" in heavy_slice:
        return False
    cgroup = _cgroup_path(self_cgroup)
    if cgroup is None or f"/{heavy_slice}/" not in cgroup:
        return False
    try:
        target = os.readlink(fd_link)
    except OSError:
        return False
    return os.path.realpath(target) == os.path.realpath(lock)


def _read_cg(directory: Path, name: str) -> str | None:
    try:
        return (directory / name).read_text(encoding="utf-8").strip()
    except OSError:
        return None


def verify_scope(
    limit: int = CAP_BYTES,
    *,
    self_cgroup: Path = SELF_CGROUP,
    cgroup_root: Path = CGROUP_ROOT,
) -> ScopeInfo:
    """Verify this process's cap (plan point (b)): some cgroup from the leaf up to the root has a
    numeric ``memory.max`` <= ``limit``, and some has ``memory.swap.max`` = 0. The capping
    directory is the one with the tightest ``memory.max`` (the nearest on a tie)."""
    cgroup = _cgroup_path(self_cgroup)
    rel = (cgroup or "").strip("/")
    leaf = cgroup_root / rel if rel else cgroup_root
    tightest: tuple[int, Path] | None = None
    swap_zero = False
    current = leaf
    while True:
        value = _read_cg(current, "memory.max")
        if value is not None and value.isdigit() and (tightest is None or int(value) < tightest[0]):
            tightest = (int(value), current)
        swap_zero = swap_zero or _read_cg(current, "memory.swap.max") == "0"
        if current == cgroup_root or current.parent == current:
            break
        current = current.parent
    if cgroup is not None and tightest is not None and tightest[0] <= limit and swap_zero:
        return ScopeInfo(path=tightest[1], leaf=leaf, memory_max=tightest[0], swap_zero=True)
    shown_max = "max" if tightest is None else str(tightest[0])
    shown_swap = _read_cg(leaf, "memory.swap.max") or "max"
    raise HeavyError(
        EXIT_USAGE,
        f"not inside a capped scope (memory.max {shown_max}, memory.swap.max {shown_swap}); "
        "run through the heavy wrapper",
    )


def _verify_inside(env: Mapping[str, str], xl: bool) -> ScopeInfo:
    """Inside a heavy run: ``--xl`` needs the xl slice (as ``tools/heavy-run`` refuses ``--xl``
    nested in the 6G slice), then the cap is verified from the real cgroup tree."""
    heavy_slice = env.get("ODIN3_HEAVY_SLICE", "")
    if xl and heavy_slice != XL_SLICE:
        raise HeavyError(EXIT_USAGE, f"--xl inside {heavy_slice} (6G): run the xl job on its own")
    limit = CAP_XL_BYTES if xl else CAP_BYTES
    return verify_scope(limit, self_cgroup=SELF_CGROUP, cgroup_root=CGROUP_ROOT)


def _inside(env: Mapping[str, str], lock: Path) -> bool:
    return inside_heavy_run(env, self_cgroup=SELF_CGROUP, fd_link=FD9, lock=lock)


def require_heavy(*, xl: bool = False) -> ScopeInfo:
    """For tools that must already run inside a heavy run (3D's golden paths): the nesting
    evidence and the cap, else ``HeavyError(2, …)``."""
    env = os.environ
    if not _inside(env, HEAVY_LOCK):
        raise HeavyError(EXIT_USAGE, MSG_NOT_INSIDE)
    return _verify_inside(env, xl)


def reexec_argv(argv: Sequence[str], script: Path, *, xl: bool = False) -> list[str]:
    """``[<repo>/tools/heavy-run, (--xl), <python>, -I, <script>, *argv]``."""
    head = [str(HEAVY_RUN)] + (["--xl"] if xl else [])
    return [*head, sys.executable, "-I", str(script), *argv]


def _lock_busy(lock: Path) -> bool:
    """Probe the heavy lock as ``flock -n`` would (exclusive, non-blocking), then release it at
    once: ``heavy-run`` takes it for real."""
    lock.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(lock, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_CLOEXEC, 0o666)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return True
    finally:
        os.close(fd)  # closing the only descriptor releases a lock taken on it
    return False


def reexec_under_lock(
    argv: Sequence[str],
    *,
    script: Path | None = None,
    xl: bool = False,
    no_wait: bool = False,
    lock: Path = HEAVY_LOCK,
    env: Mapping[str, str] | None = None,
) -> ScopeInfo:
    """Make the caller a heavy run (plan point (a)). Inside one, verify the cap and return it.
    Outside, with ``ODIN3_HEAVY_OK=1`` (and ``ODIN3_HEAVY_XL_OK=1`` for ``xl``) in the caller's
    environment, re-run ``script argv`` through ``tools/heavy-run`` and ``sys.exit`` with its
    status; ``heavy-run``'s 77 (skipped) becomes ``HeavyError(2, …)``.

    ``no_wait`` is best effort: the lock is probed, then released for ``heavy-run`` to take, so a
    job that takes it in between makes ``heavy-run`` wait (up to its ``ODIN3_HEAVY_WAIT``) with no
    message. A race-free ``--no-wait`` needs ``heavy-run`` to support it."""
    environ = _environ(env)
    if _inside(environ, lock):
        return _verify_inside(environ, xl)
    if environ.get("ODIN3_HEAVY_OK") != "1":
        raise HeavyError(EXIT_USAGE, MSG_NOT_CONTROLLER)
    if xl and environ.get("ODIN3_HEAVY_XL_OK") != "1":
        raise HeavyError(EXIT_USAGE, MSG_XL_NOT_OK)
    if _lock_busy(lock):
        if no_wait:
            raise HeavyError(EXIT_LOCK_BUSY, f"heavy lock {lock} is busy (--no-wait)")
        print(f"{PREFIX}waiting for the heavy lock {lock}", file=sys.stderr, flush=True)
    target = Path(sys.argv[0]).resolve() if script is None else script
    child_env = None if env is None else dict(env)
    try:
        proc = subprocess.run(reexec_argv(argv, target, xl=xl), env=child_env, check=False)
    except OSError as exc:  # no tools/heavy-run (2E-1 not merged): no fallback, plan point (g)
        raise HeavyError(
            EXIT_USAGE, f"cannot apply the memory cap (cannot run {HEAVY_RUN}: {exc.strerror})"
        ) from exc
    if proc.returncode == HEAVY_RUN_SKIPPED:
        raise HeavyError(EXIT_USAGE, MSG_CAP_77)
    sys.exit(proc.returncode if proc.returncode >= 0 else SIGNAL_EXIT_BASE - proc.returncode)


def oom_kills(scope: ScopeInfo) -> int:
    """The capping directory's ``memory.events`` ``oom_kill`` counter, 0 when absent. The slice
    persists across heavy runs, so the counter is cumulative: callers take the delta between a
    read before and a read after each job (plan point (b)), never the absolute value."""
    text = _read_cg(scope.path, "memory.events")
    for line in (text or "").splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] == "oom_kill" and fields[1].isdigit():
            return int(fields[1])
    return 0


def scope_peak_mib(scope: ScopeInfo) -> int | None:
    """The leaf cgroup's ``memory.peak`` in MiB (the job's own peak, plan point (b)), or None."""
    value = _read_cg(scope.leaf, "memory.peak")
    if value is None or not value.isdigit():
        return None
    return int(value) // MIB


def parse_df(returncode: int, stdout: str) -> int:
    """Free whole GiB on ``C:`` (floored) from ``DF_CMD``'s output. The reported mount point must
    be ``/mnt/c``: with ``C:`` unmounted, ``df`` would silently report the file system holding the
    directory (the VHDX), so that is E-QH11 too."""
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if returncode != 0 or len(lines) < 2:
        raise HeavyError(EXIT_DISK, MSG_DF)
    fields = lines[-1].rsplit(None, 1)
    if len(fields) != 2 or fields[0] != WIN_C or not fields[1].isdigit():
        raise HeavyError(EXIT_DISK, MSG_DF)
    return int(fields[1]) // GIB


def disk_free_gib() -> int:
    """Free whole GiB on ``C:`` from ``df -B1 --output=target,avail /mnt/c`` (``DF_CMD``; never an
    environment override, so a leaked variable cannot defeat admission), within ``DF_TIMEOUT_S``:
    ``statfs`` on WSL's ``/mnt/c`` can hang while Windows stalls, and a hung reader must not hold
    the heavy lock. Any failure is E-QH11."""
    try:
        proc = subprocess.Popen(list(DF_CMD), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True)
    except OSError as exc:
        raise HeavyError(EXIT_DISK, MSG_DF) from exc
    try:
        stdout, _ = proc.communicate(timeout=DF_TIMEOUT_S)
    except subprocess.TimeoutExpired as exc:
        # Kill it and reap it within DF_REAP_S; a df stuck in the kernel is let go, never
        # waited for without a bound (subprocess.run's timeout would wait on it).
        proc.kill()
        with contextlib.suppress(subprocess.TimeoutExpired):
            proc.wait(timeout=DF_REAP_S)
        if proc.stdout is not None:
            proc.stdout.close()
        raise HeavyError(EXIT_DISK, MSG_DF) from exc
    return parse_df(proc.returncode, stdout)


def mem_available_gib(meminfo: Path = Path("/proc/meminfo")) -> float:
    """``MemAvailable`` in GiB."""
    try:
        text = meminfo.read_text(encoding="utf-8")
    except OSError as exc:
        raise HeavyError(EXIT_USAGE, f"cannot read MemAvailable from {meminfo}") from exc
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] == "MemAvailable:" and fields[1].isdigit():
            return int(fields[1]) / KIB_PER_GIB
    raise HeavyError(EXIT_USAGE, f"cannot read MemAvailable from {meminfo}")


def _allowed_roots(home: Path) -> tuple[Path, Path]:
    return (Path(os.path.realpath(home / WORK_SUBDIR)), Path(os.path.realpath(HEAVY_DIR)))


def _strictly_under(path: Path, root: Path) -> bool:
    return path != root and root in path.parents


def _account_home() -> Path:
    """The account's home from the password database (not ``$HOME``, which can leak)."""
    return Path(pwd.getpwuid(os.getuid()).pw_dir)


def check_out_dir(out: Path, home: Path) -> Path:
    """Plan point (m): ``--out`` must resolve strictly under ``~/odin3-ws/work`` or
    ``/tmp/claude-1000``. Returns the resolved path."""
    resolved = Path(os.path.realpath(out))
    if any(_strictly_under(resolved, root) for root in _allowed_roots(home)):
        return resolved
    raise HeavyError(
        EXIT_USAGE, f"refusing --out {resolved}: not under ~/odin3-ws/work or /tmp/claude-1000"
    )


def _other_device(real: Path, device: int) -> Path | None:
    """The first entry at or below ``real`` (symlinks not followed) whose ``st_dev`` differs from
    ``device``, or None. An entry that vanishes during the walk raises its ``OSError``."""
    info = os.lstat(real)
    if info.st_dev != device:
        return real
    if not stat.S_ISDIR(info.st_mode):  # a file: nothing below it to walk
        return None
    for top, dirs, files in os.walk(real, followlinks=False, onerror=_raise):
        for name in (*dirs, *files):
            entry = Path(top) / name
            if os.lstat(entry).st_dev != device:
                return entry
    return None


def _raise(exc: OSError) -> None:
    raise exc


def safe_rmtree(path: Path, root: Path) -> None:
    """Delete ``path`` (a tree or a file; absent is a no-op) only when it lies strictly under
    ``root`` both as spelled (no ``..``) and after ``realpath``, no component from ``root`` down to
    ``path`` is a symlink, no entry to delete has another ``st_dev`` than ``root`` (a mount of
    another file system, such as a bind mount from another disk; a bind mount of a directory on
    the same file system has the same ``st_dev`` and is not detected), and ``root`` itself is a
    permitted ``--out`` (E-QH12). The checks run before the
    deletion, so a component swapped for a symlink in between is not caught (one user; accepted);
    ``shutil.rmtree`` itself never follows a symlink inside the tree."""
    not_under = HeavyError(EXIT_USAGE, f"refusing to delete {path}: not under {root}")
    spelled, spelled_root = Path(os.path.abspath(path)), Path(os.path.abspath(root))
    if ".." in Path(path).parts or not _strictly_under(spelled, spelled_root):
        raise not_under
    try:
        check_out_dir(root, _account_home())
    except HeavyError:
        raise not_under from None
    current = spelled_root
    for part in ("", *spelled.relative_to(spelled_root).parts):
        current = current / part if part else current
        if current.is_symlink():
            raise HeavyError(EXIT_USAGE, f"refusing to delete {path}: {current} is a symlink")
    real, real_root = Path(os.path.realpath(path)), Path(os.path.realpath(root))
    if not _strictly_under(real, real_root):
        raise not_under
    if not real.exists():
        return
    try:
        foreign = _other_device(real, os.stat(real_root).st_dev)
    except OSError as exc:  # the tree changed under the check: refuse, never a bare OSError
        where = exc.filename if isinstance(exc.filename, str) else real
        raise HeavyError(
            EXIT_USAGE, f"refusing to delete {path}: {where} changed during the check"
        ) from exc
    if foreign is not None:
        raise HeavyError(
            EXIT_USAGE, f"refusing to delete {path}: {foreign} is on another file system"
        )
    if real.is_dir():
        shutil.rmtree(real)
    else:
        real.unlink()


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="qor_heavy.py", description="the heavy-run convention for vtr-qor and its peers"
    )
    sub = parser.add_subparsers(dest="command", required=True)
    req = sub.add_parser("require-heavy", help="exit 0 inside a capped heavy run, 3 outside")
    req.add_argument("--xl", action="store_true", help="require the 9 GiB xl slice")
    args = parser.parse_args(argv)
    if not _inside(os.environ, HEAVY_LOCK):
        return EXIT_NOT_INSIDE
    try:
        _verify_inside(os.environ, bool(args.xl))
    except HeavyError as exc:
        print(f"{PREFIX}{exc.message}", file=sys.stderr)
        return exc.code
    return 0


if __name__ == "__main__":
    sys.exit(main())
