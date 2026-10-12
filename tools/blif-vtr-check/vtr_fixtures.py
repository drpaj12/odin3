"""vtr-fixtures: the ``write_blif --vtr`` fixture gate (spec 3E §16.2, CTest blif_vtr_fixtures).

Usage::

    vtr-fixtures --odin3 PATH --dir DIR [--checker PATH] [--write]

The cases are every ``DIR/<name>.in.blif`` and every repo-relative path listed in
``DIR/sample.txt`` (plan 3E point (r); ``<name>`` is then the basename without ``.blif``), run
smallest input first. Each case runs, from the input's directory under its basename::

    odin3 -p "read_techlib <repo>/lib/vtr.o3lib; read_blif <in>;
              write_blif --vtr <tmp>/<name>.vtr.blif"

and expects either ``DIR/<name>.vtr.blif`` or ``DIR/<name>.err``:

- a written file is compared with ``<name>.vtr.blif`` byte for byte, except that the header's
  first line is matched by the pattern of point (s), so a version bump does not churn the
  fixtures;
- a refusal must leave no file, and its ``odin3: error:`` lines on stderr (the pass manager's own
  trailers ``pass <p>: …`` and ``-p: command …`` left out) must equal ``<name>.err``, or
  ``<name>.debug.err`` when that exists: a Debug build checks the IR after every pass, so it can
  refuse an input at ``read_blif`` before the writer sees it (a multi-driver net, E12).

Then ``blif-vtr-check --strict`` runs on every expected ``.vtr.blif`` (skipped with a note
without ``--checker``: the checker lands in 3E-2). An expected file that no case owns fails.

``--write`` writes the missing expected file of a case (``.vtr.blif`` or ``.err``); it never
overwrites one.

Exit 0 when every case passes, 1 on any difference (a unified diff is printed), 2 for usage.
"""

from __future__ import annotations

import argparse
import difflib
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LIBRARY = REPO / "lib" / "vtr.o3lib"
SAMPLE = "sample.txt"
IN_SUFFIX = ".in.blif"
OUT_SUFFIX = ".vtr.blif"
ERR_SUFFIX = ".err"
DEBUG_ERR_SUFFIX = ".debug.err"
RUN_TIMEOUT = 300  # seconds per odin3 run; the fixtures take milliseconds
HEADER = re.compile(r"^# Odin III \S+ write_blif --vtr \(K=(\d+), names (short|provenance)\)$")
ERROR_PREFIX = "odin3: error: "
TRAILER = re.compile(r"^odin3: error: (pass \S+: |-p: command \d+: )")


class UsageError(Exception):
    """A bad argument or an unreadable fixture directory (exit 2)."""


@dataclass(frozen=True)
class Case:
    """One fixture: its name (the expected files' stem) and its input."""

    name: str
    source: Path


@dataclass
class Outcome:
    """What odin3 did with a case: the file it wrote, or its error lines."""

    written: str | None
    errors: str
    detail: str  # why the run itself is wrong (empty when it is consistent)


def read_text(path: Path) -> str:
    return path.read_bytes().decode("utf-8", errors="surrogateescape")


def write_text(path: Path, text: str) -> None:
    path.write_bytes(text.encode("utf-8", errors="surrogateescape"))


def find_cases(fixtures: Path) -> list[Case]:
    """The cases of the fixture directory, smallest input first (then by name)."""
    cases = [Case(p.name[: -len(IN_SUFFIX)], p) for p in fixtures.glob("*" + IN_SUFFIX)]
    sample = fixtures / SAMPLE
    if sample.is_file():
        for line in read_text(sample).splitlines():
            entry = line.strip()
            if entry == "" or entry.startswith("#"):
                continue
            source = REPO / entry
            if not source.is_file():
                raise UsageError(f"{sample}: no such input '{entry}'")
            cases.append(Case(source.name[: -len(".blif")], source))
    names = [case.name for case in cases]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        raise UsageError(f"{fixtures}: two inputs for the case(s) {', '.join(duplicates)}")
    return sorted(cases, key=lambda case: (case.source.stat().st_size, case.name))


def error_lines(stderr: str) -> str:
    """The odin3 error lines of stderr, the pass manager's trailers left out."""
    kept = [
        line
        for line in stderr.splitlines()
        if line.startswith(ERROR_PREFIX) and not TRAILER.match(line)
    ]
    return "".join(line + "\n" for line in kept)


def run_case(odin3: Path, case: Case, work: Path) -> Outcome:
    out = work / (case.name + OUT_SUFFIX)
    script = (
        f'read_techlib "{LIBRARY}"; read_blif "{case.source.name}"; write_blif --vtr "{out}"'
    )
    proc = subprocess.run(
        [str(odin3), "-p", script],
        cwd=case.source.parent,
        capture_output=True,
        timeout=RUN_TIMEOUT,
        check=False,
    )
    stderr = proc.stderr.decode("utf-8", errors="surrogateescape")
    written = read_text(out) if out.is_file() else None
    detail = ""
    if proc.returncode == 0 and written is None:
        detail = "odin3 exited 0 but wrote no file"
    elif proc.returncode != 0 and written is not None:
        detail = f"odin3 exited {proc.returncode} but left a file"
    elif proc.returncode != 0 and error_lines(stderr) == "":
        detail = f"odin3 exited {proc.returncode} with no error line:\n{stderr}"
    return Outcome(written, error_lines(stderr) if written is None else "", detail)


def normalize_header(text: str) -> str:
    """text with the version on its first line replaced, when that line is the header."""
    first, sep, rest = text.partition("\n")
    match = HEADER.match(first)
    if match is None:
        return text
    return f"# Odin III <version> write_blif --vtr (K={match[1]}, names {match[2]}){sep}{rest}"


def show_diff(want: str, have: str, want_name: str, have_name: str) -> None:
    diff = difflib.unified_diff(
        want.splitlines(keepends=True),
        have.splitlines(keepends=True),
        fromfile=want_name,
        tofile=have_name,
    )
    sys.stdout.writelines(diff)
    if not have.endswith("\n") or not want.endswith("\n"):
        sys.stdout.write("\n")


def compare(case: Case, fixtures: Path, outcome: Outcome) -> str | None:
    """None when outcome is what the case expects, else why not (a diff is printed)."""
    blif = fixtures / (case.name + OUT_SUFFIX)
    err = fixtures / (case.name + ERR_SUFFIX)
    debug = fixtures / (case.name + DEBUG_ERR_SUFFIX)
    if blif.is_file() and err.is_file():
        return f"both {blif.name} and {err.name} exist"
    if outcome.written is not None:
        if not blif.is_file():
            if not err.is_file():
                return f"no expected file {blif.name} (--write writes it)"
            show_diff(read_text(err), "", err.name, "(written, no error)")
            return f"written, but {err.name} expects a refusal"
        want = normalize_header(read_text(blif))
        have = normalize_header(outcome.written)
        if want != have:
            show_diff(want, have, blif.name, "written")
            return f"differs from {blif.name}"
        return None
    if not err.is_file():
        if not blif.is_file():
            return f"no expected file {err.name} (--write writes it)"
        show_diff("", outcome.errors, blif.name, "errors")
        return f"refused, but {blif.name} expects a file"
    if outcome.errors == read_text(err):
        return None
    if debug.is_file() and outcome.errors == read_text(debug):
        return None
    show_diff(read_text(err), outcome.errors, err.name, "errors")
    return f"error lines differ from {err.name}"


def write_missing(case: Case, fixtures: Path, outcome: Outcome) -> bool:
    """--write: the expected file of a case that has none; True when one was written."""
    blif = fixtures / (case.name + OUT_SUFFIX)
    err = fixtures / (case.name + ERR_SUFFIX)
    if blif.exists() or err.exists():
        return False
    target, text = (blif, outcome.written) if outcome.written is not None else (err, outcome.errors)
    write_text(target, text)
    print(f"vtr-fixtures: wrote {target}")
    return True


def check_case(odin3: Path, case: Case, fixtures: Path, write: bool) -> str | None:
    with tempfile.TemporaryDirectory(prefix="vtr-fixtures.") as tmp:
        outcome = run_case(odin3, case, Path(tmp))
    if outcome.detail:
        return outcome.detail
    if write and write_missing(case, fixtures, outcome):
        return None
    return compare(case, fixtures, outcome)


def orphans(fixtures: Path, cases: list[Case]) -> list[str]:
    """The expected files in fixtures that no case owns."""
    names = {case.name for case in cases}
    found = []
    for path in sorted(fixtures.iterdir()):
        for suffix in (DEBUG_ERR_SUFFIX, OUT_SUFFIX, ERR_SUFFIX):
            if path.name.endswith(suffix):
                if path.name[: -len(suffix)] not in names:
                    found.append(path.name)
                break
    return found


def run_checker(checker: Path | None, fixtures: Path, cases: list[Case]) -> int:
    """blif-vtr-check --strict on every expected .vtr.blif; the number that fail."""
    if checker is None:
        print("vtr-fixtures: note: no --checker given; the blif-vtr-check step is skipped")
        return 0
    failed = 0
    for case in cases:
        blif = fixtures / (case.name + OUT_SUFFIX)
        if not blif.is_file():
            continue
        proc = subprocess.run(
            [str(checker), "--strict", str(blif)],
            capture_output=True,
            text=True,
            timeout=RUN_TIMEOUT,
            check=False,
        )
        if proc.returncode != 0:
            failed += 1
            print(f"FAIL {case.name}: blif-vtr-check --strict exited {proc.returncode}")
            sys.stdout.write(proc.stdout + proc.stderr)
    return failed


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="vtr-fixtures", description=__doc__.split("\n")[0])
    parser.add_argument("--odin3", required=True, type=Path, help="the odin3 binary")
    parser.add_argument("--dir", required=True, type=Path, help="the fixture directory")
    parser.add_argument("--checker", type=Path, help="tools/blif-vtr-check/blif-vtr-check")
    parser.add_argument("--write", action="store_true", help="write missing expected files")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        if not args.odin3.is_file():
            raise UsageError(f"no odin3 binary '{args.odin3}'")
        if not args.dir.is_dir():
            raise UsageError(f"no fixture directory '{args.dir}'")
        if args.checker is not None and not args.checker.is_file():
            raise UsageError(f"no checker '{args.checker}'")
        fixtures = args.dir.resolve()
        cases = find_cases(fixtures)
    except UsageError as err:
        print(f"vtr-fixtures: {err}", file=sys.stderr)
        return 2
    failed = 0
    for case in cases:
        why = check_case(args.odin3.resolve(), case, fixtures, args.write)
        print(f"ok   {case.name}" if why is None else f"FAIL {case.name}: {why}")
        failed += why is not None
    for name in orphans(fixtures, cases):
        print(f"FAIL {name}: no case has this expected file")
        failed += 1
    failed += run_checker(args.checker, fixtures, cases)
    print(f"vtr-fixtures: {len(cases)} cases, {failed} failures")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
