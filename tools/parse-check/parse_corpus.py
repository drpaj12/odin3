"""parse-corpus: a corpus of Verilog files through odin3-parse-check (2C spec §7.4, plan Task 10).

    parse-corpus --check <odin3-parse-check> [--expect-fail F] [--writer <odin3-write>]
                 [--root R] [--tmp D] <dir or file>...

Collects every ``.v`` under the given directories (and the given files), sorts them by size,
smallest first (the memory rule), and runs ``odin3-parse-check <file>`` on each, one at a time:
read, check FULL, print, re-read, equal, and the location oracle (no ``.ast`` goldens). One line
per file and a summary go to stdout.

With --writer the inputs are ``.blif`` files instead: each is first written as structural
Verilog with ``odin3-write --verilog <tmp>.v <blif>`` (Review Focus 1: the escaped names our own
writer emits must read back), and the temporary file is checked. The temporary files go under
--tmp (default /tmp/claude-<uid>/parse-corpus, never the bare /tmp: written netlists can be GBs);
one over 2B's 2^28-byte source-file cap is that BLIF's failure without running the check.

The expect-fail list F holds ``<relative path> <one-line reason>`` lines (blank lines and ``#``
comments ignored); a path is relative to --root when the file is under it, else to the directory
argument it was found under, else the file's name. A failure is accepted only when the file is
listed; an unlisted failure, a listed file that passes, or a listed path that names no collected
file fails the run.

It has no heavy-run logic of its own: a large corpus runs under ``tools/heavy-run`` (the sweep
CTests register it through ``odin3_add_sweep_test()``, which does so), and heavy-run alone decides
the lock, the slice and nesting (2E plan C1; Task 10 re-review R1/R2).

A file whose check runs past an hour is that file's failure.

Exit codes: 0 pass, 1 an unexpected result, 2 usage or input error.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

EXIT_OK, EXIT_FAIL, EXIT_USAGE = 0, 1, 2
EXIT_TIMEOUT = 124
TIMEOUT_S = 3600
TAIL_LINES = 4
SOURCE_CAP = 1 << 28  # 2B's per-file source cap (ODIN3_PP_MAX_SOURCE_BYTES)


@dataclass(frozen=True)
class Input:
    path: Path
    rel: str
    size: int


@dataclass(frozen=True)
class Outcome:
    ok: bool
    secs: float
    why: str


class UsageError(Exception):
    pass


def read_expect(path: Path) -> dict[str, str]:
    """The expect-fail list: relative path -> reason. Every entry needs a reason."""
    expect: dict[str, str] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) < 2 or not parts[1].strip():
            raise UsageError(f"{path}:{number}: '{parts[0]}' has no reason")
        if parts[0] in expect:
            raise UsageError(f"{path}:{number}: '{parts[0]}' is listed twice")
        expect[parts[0]] = parts[1].strip()
    return expect


def relative(path: Path, base: Path, root: Path | None) -> str:
    if root is not None and path.is_relative_to(root):
        return path.relative_to(root).as_posix()
    if path.is_relative_to(base):
        return path.relative_to(base).as_posix()
    return path.name


def collect(args: Sequence[str], suffix: str, root: Path | None) -> list[Input]:
    """Every file with suffix under the directory arguments, and the file arguments, smallest
    first (ties by relative path)."""
    found: dict[Path, Input] = {}
    for arg in args:
        given = Path(arg).resolve()
        if given.is_dir():
            paths = sorted(p for p in given.rglob(f"*{suffix}") if p.is_file())
            base = given
        elif given.is_file():
            paths, base = [given], given.parent
        else:
            raise UsageError(f"{arg}: no such file or directory")
        for path in paths:
            found.setdefault(path, Input(path, relative(path, base, root), path.stat().st_size))
    return sorted(found.values(), key=lambda item: (item.size, item.rel))


def first_error(text: str) -> str:
    """The first error line of a run's output, else its last few lines."""
    lines = [line for line in text.splitlines() if line.strip()]
    for line in lines:
        if "error:" in line:
            return line.removeprefix("odin3: error: ")
    return " | ".join(lines[-TAIL_LINES:])


def run(cmd: Sequence[str]) -> subprocess.CompletedProcess[str]:
    """cmd's result; a run past TIMEOUT_S is killed and reported as an error, not raised."""
    try:
        return subprocess.run(
            list(cmd), capture_output=True, text=True, errors="replace", timeout=TIMEOUT_S,
            check=False,
        )  # fmt: skip
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(
            list(cmd), EXIT_TIMEOUT, "", f"error: timed out after {TIMEOUT_S} s"
        )


def check_one(check: str, writer: str | None, item: Input, tmp: Path) -> Outcome:
    start = time.monotonic()
    target = item.path
    if writer is not None:
        target = tmp / (item.path.stem + ".v")
        wrote = run([writer, "--verilog", str(target), str(item.path)])
        if wrote.returncode != 0:
            why = f"odin3-write exit {wrote.returncode}: {first_error(wrote.stdout + wrote.stderr)}"
            target.unlink(missing_ok=True)
            return Outcome(False, time.monotonic() - start, why)
        size = target.stat().st_size
        if size > SOURCE_CAP:
            target.unlink()
            why = f"the written Verilog is {size} bytes, over 2B's source-file cap of {SOURCE_CAP}"
            return Outcome(False, time.monotonic() - start, why)
    try:
        proc = run([check, str(target)])
    finally:
        if writer is not None:
            target.unlink(missing_ok=True)
    why = "" if proc.returncode == 0 else f"exit {proc.returncode}: {first_error(proc.stderr)}"
    return Outcome(proc.returncode == 0, time.monotonic() - start, why)


def verdict(item: Input, outcome: Outcome, expect: dict[str, str]) -> tuple[str, bool]:
    """The file's line and whether it is an unexpected result."""
    head = f"{item.rel} ({item.size} B, {outcome.secs:.3f} s)"
    reason = expect.get(item.rel)
    if outcome.ok and reason is None:
        return f"ok    {head}", False
    if outcome.ok:
        return f"XPASS {head}: listed as expected to fail ({reason}) but passes", True
    if reason is not None:
        return f"xfail {head}: {reason}", False
    return f"FAIL  {head}: {outcome.why}", True


def sweep(opts: argparse.Namespace, items: list[Input], expect: dict[str, str]) -> int:
    counts = {"ok": 0, "xfail": 0, "fail": 0, "xpass": 0}
    start = time.monotonic()
    opts.tmp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="parse-corpus-", dir=opts.tmp) as tmp:
        for item in items:
            outcome = check_one(opts.check, opts.writer, item, Path(tmp))
            line, bad = verdict(item, outcome, expect)
            print(line, flush=True)
            key = ("xpass" if outcome.ok else "fail") if bad else ("ok" if outcome.ok else "xfail")
            counts[key] += 1
    seen = {item.rel for item in items}
    stale = sorted(rel for rel in expect if rel not in seen)
    for rel in stale:
        print(f"STALE {rel}: listed as expected to fail but not in the corpus")
    print(
        f"parse-corpus: {len(items)} files, {counts['ok']} passed, {counts['xfail']} expected "
        f"failures, {counts['fail']} unexpected failures, {counts['xpass']} listed but passing, "
        f"{len(stale)} stale entries, {time.monotonic() - start:.1f} s"
    )
    return EXIT_FAIL if counts["fail"] or counts["xpass"] or stale else EXIT_OK


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="parse-corpus", description=__doc__.splitlines()[0])
    parser.add_argument("--check", required=True, help="the odin3-parse-check binary")
    parser.add_argument("--expect-fail", type=Path, help="<relative path> <reason> lines")
    parser.add_argument("--writer", help="odin3-write: check .blif files through write_verilog")
    parser.add_argument("--root", type=Path, help="make listed paths relative to this directory")
    parser.add_argument(
        "--tmp", type=Path, default=Path(f"/tmp/claude-{os.getuid()}/parse-corpus"),
        help="where --writer's temporary Verilog goes",
    )  # fmt: skip
    parser.add_argument("inputs", nargs="+", help="directories or files")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    opts = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        expect = read_expect(opts.expect_fail) if opts.expect_fail is not None else {}
        root = opts.root.resolve() if opts.root is not None else None
        items = collect(opts.inputs, ".blif" if opts.writer else ".v", root)
    except (UsageError, OSError) as exc:
        print(f"parse-corpus: {exc}", file=sys.stderr)
        return EXIT_USAGE
    if not items:
        print("parse-corpus: no input files", file=sys.stderr)
        return EXIT_USAGE
    return sweep(opts, items, expect)


if __name__ == "__main__":
    sys.exit(main())
