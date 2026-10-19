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

A written case then runs the round trip of §16.3 step 1 (step 5: CI on the fixtures), as the
3E-1 final review's erratum amends it: ``W2 = write_blif --vtr (read_blif W1)``, and

- when the write of W1 legalised no name (no ``names legalised`` info line, ``renamed == 0``),
  W1 == W2 byte for byte;
- otherwise W1 and W2 are compared under the VB-10 decode (``vtr_normalized``: continuations
  joined, comment lines dropped, every name decoded to a fixed point), and the byte comparison
  is not applied: VB-10 escapes the ``%`` of an escape again (``\\`` -> ``%5C`` -> ``%255C``), so
  W2 differs from W1 by design. A ``note`` line names such a case.

Then ``blif-vtr-check --strict`` runs on every expected ``.vtr.blif`` (skipped with a note
without ``--checker``). A case in ``STRICT_EXCEPTIONS`` runs the checker without ``--strict``
(the VPR rules C1-C9 still apply) and must still fail ``--strict``, so a stale exception fails.
An expected file that no case owns fails.

A case may have ``<name>.args``: extra ``write_blif --vtr`` options (3E-6: ``--name-style
provenance``). Its round trip is not applied (W2 would wrap each name with the location in W1).

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
ARGS_SUFFIX = ".args"
DEBUG_ERR_SUFFIX = ".debug.err"
RUN_TIMEOUT = 300  # seconds per odin3 run; the fixtures take milliseconds
HEADER = re.compile(r"^# Odin III \S+ write_blif --vtr \(K=(\d+), names (short|provenance)\)$")
ERROR_PREFIX = "odin3: error: "
# VB-10 item 9's info line; its count is the report's `renamed` (§16.3 step 1, amended).
RENAMED = re.compile(r"^odin3: info: write_blif: --vtr: (\d+) names legalised ", re.MULTILINE)
# A VB-10 escape: `%` and two upper-case hex digits (plan 3E point (o)).
ESCAPE = re.compile(rb"%([0-9A-F]{2})")
TRAILER = re.compile(r"^odin3: error: (pass \S+: |-p: command \d+: )")
# Expected files that break an Odin III convention (C-S rule) by design, with the reason; the
# checker runs on them without --strict. shiftx.parmys is a Parmys k6 golden read back: its
# class-U `unconn` stays on .names pins (spec §9.2, the writer only warns), which C-S2 flags.
STRICT_EXCEPTIONS = {
    "shiftx.parmys": "C-S2: the Parmys golden's class-U 'unconn' on .names pins (spec 9.2)",
}


class UsageError(Exception):
    """A bad argument or an unreadable fixture directory (exit 2)."""


@dataclass(frozen=True)
class Case:
    """One fixture: its name (the expected files' stem) and its input."""

    name: str
    source: Path
    args: str = ""  # extra write_blif options, from <name>.args


@dataclass
class Outcome:
    """What odin3 did with a case: the file it wrote, or its error lines."""

    written: str | None
    errors: str
    detail: str  # why the run itself is wrong (empty when it is consistent)
    renamed: int = 0  # the names the write legalised (VB-10 item 9's count)


def read_text(path: Path) -> str:
    return path.read_bytes().decode("utf-8", errors="surrogateescape")


def write_text(path: Path, text: str) -> None:
    path.write_bytes(text.encode("utf-8", errors="surrogateescape"))


def args_of(fixtures: Path, name: str) -> str:
    """The extra write_blif options of case name: the text of <name>.args, "" without the file."""
    path = fixtures / (name + ARGS_SUFFIX)
    return read_text(path).strip() if path.is_file() else ""


def find_cases(fixtures: Path) -> list[Case]:
    """The cases of the fixture directory, smallest input first (then by name)."""
    cases = [
        Case(p.name[: -len(IN_SUFFIX)], p, args_of(fixtures, p.name[: -len(IN_SUFFIX)]))
        for p in fixtures.glob("*" + IN_SUFFIX)
    ]
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


def rewrite(
    odin3: Path, source: Path, out: Path, args: str = ""
) -> subprocess.CompletedProcess[bytes]:
    """odin3 reads source (from its directory, by its basename) and writes it --vtr (with the extra
    options args, such as ``--name-style provenance``) to out."""
    extra = f" {args}" if args else ""
    script = (
        f'read_techlib "{LIBRARY}"; read_blif "{source.name}"; write_blif --vtr{extra} "{out}"'
    )
    return subprocess.run(
        [str(odin3), "-p", script],
        cwd=source.parent,
        capture_output=True,
        timeout=RUN_TIMEOUT,
        check=False,
    )


def renamed_of(stderr: str) -> int:
    """The `names legalised` count of a write's log, 0 when it has no such line."""
    match = RENAMED.search(stderr)
    return int(match[1]) if match is not None else 0


def run_case(odin3: Path, case: Case, work: Path) -> Outcome:
    out = work / (case.name + OUT_SUFFIX)
    proc = rewrite(odin3, case.source, out, case.args)
    stderr = proc.stderr.decode("utf-8", errors="surrogateescape")
    written = read_text(out) if out.is_file() else None
    detail = ""
    if proc.returncode == 0 and written is None:
        detail = "odin3 exited 0 but wrote no file"
    elif proc.returncode != 0 and written is not None:
        detail = f"odin3 exited {proc.returncode} but left a file"
    elif proc.returncode != 0 and error_lines(stderr) == "":
        detail = f"odin3 exited {proc.returncode} with no error line:\n{stderr}"
    errors = error_lines(stderr) if written is None else ""
    return Outcome(written, errors, detail, renamed_of(stderr))


def vb10_decode(name: bytes) -> bytes:
    """name with VB-10 undone to a fixed point: rule 3's `%` prefix (a `%` not followed by two
    upper-case hex digits) dropped, then every `%XX` replaced by its byte, until nothing changes.
    One step inverts one write exactly (point (o)); the fixed point makes a name written twice
    (`%255C`) decode like one written once (`%5C`), both to `\\`."""
    while True:
        step = name[1:] if name.startswith(b"%") and ESCAPE.match(name) is None else name
        step = ESCAPE.sub(lambda m: bytes([int(m[1], 16)]), step)
        if step == name:
            return name
        name = step


def vtr_normalized(text: bytes) -> list[list[bytes]]:
    """A --vtr file as token lines: ` \\` continuations joined, `#` comment lines and blank lines
    dropped, and every name (each side of a `formal=actual`) VB-10-decoded (vb10_decode)."""
    lines = []
    for line in text.replace(b"\\\n", b" ").split(b"\n"):
        tokens = line.split()
        if not tokens or tokens[0].startswith(b"#"):
            continue
        lines.append([b"=".join(vb10_decode(part) for part in tok.split(b"=")) for tok in tokens])
    return lines


def round_trip_verdict(w1: bytes, w2: bytes, renamed: int) -> tuple[str | None, str]:
    """§16.3 step 1 as amended (3E-1 final review I1): (why it fails or None, the comparison
    applied, "bytes" or "decoded"). renamed == 0 needs W1 == W2 byte for byte; renamed > 0 needs
    W1 and W2 equal under vtr_normalized, and the byte comparison is not applied."""
    if renamed == 0:
        return (None if w1 == w2 else "W1 != W2 byte for byte (no name legalised)"), "bytes"
    same = vtr_normalized(w1) == vtr_normalized(w2)
    return (None if same else "W1 and W2 differ under the VB-10 decode"), "decoded"


def round_trip(odin3: Path, name: str, work: Path, renamed: int) -> str | None:
    """W2 from work/<name>.vtr.blif (W1), then round_trip_verdict; None when it holds."""
    w1 = work / (name + OUT_SUFFIX)
    w2 = work / (name + ".w2" + OUT_SUFFIX)
    proc = rewrite(odin3, w1, w2)
    if proc.returncode != 0 or not w2.is_file():
        sys.stdout.write(proc.stderr.decode("utf-8", errors="surrogateescape"))
        return f"round trip: odin3 exited {proc.returncode} on W1"
    why, mode = round_trip_verdict(w1.read_bytes(), w2.read_bytes(), renamed)
    if why is not None:
        show_diff(read_text(w1), read_text(w2), "W1", "W2")
        return f"round trip: {why}"
    if mode == "decoded":
        print(
            f"note {name}: round trip: {renamed} names legalised, so W1 and W2 are compared "
            "under the VB-10 decode (byte comparison not applied)"
        )
    return None


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
        print(f"note {case.name}: matched {debug.name}, not {err.name} (refused before the writer)")
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
        why = compare(case, fixtures, outcome)
        if why is None and outcome.written is not None and case.args:
            # a provenance name wraps the name it reads with W1's location: W2 differs by design
            print(f"note {case.name}: round trip not applied (options: {case.args})")
        elif why is None and outcome.written is not None:
            why = round_trip(odin3, case.name, Path(tmp), outcome.renamed)
        return why


def orphans(fixtures: Path, cases: list[Case]) -> list[str]:
    """The expected files in fixtures that no case owns."""
    names = {case.name for case in cases}
    found = []
    for path in sorted(fixtures.iterdir()):
        for suffix in (DEBUG_ERR_SUFFIX, OUT_SUFFIX, ERR_SUFFIX, ARGS_SUFFIX):
            if path.name.endswith(suffix):
                if path.name[: -len(suffix)] not in names:
                    found.append(path.name)
                break
    return found


def check_file(checker: Path, blif: Path, strict: bool) -> subprocess.CompletedProcess[str]:
    """blif-vtr-check (with --strict when strict) on blif."""
    return subprocess.run(
        [str(checker), *(["--strict"] if strict else []), str(blif)],
        capture_output=True,
        text=True,
        timeout=RUN_TIMEOUT,
        check=False,
    )


def check_expected(checker: Path, name: str, blif: Path) -> str | None:
    """None when blif passes the checker (--strict unless an exception), else why not."""
    reason = STRICT_EXCEPTIONS.get(name)
    proc = check_file(checker, blif, reason is None)
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout + proc.stderr)
        flag = "" if reason is not None else " --strict"
        return f"blif-vtr-check{flag} exited {proc.returncode}"
    if reason is None:
        return None
    if check_file(checker, blif, True).returncode != 1:
        return "in STRICT_EXCEPTIONS but passes blif-vtr-check --strict (drop the exception)"
    print(f"note {name}: blif-vtr-check without --strict ({reason})")
    return None


def run_checker(checker: Path | None, fixtures: Path, cases: list[Case]) -> int:
    """blif-vtr-check --strict on every expected .vtr.blif (STRICT_EXCEPTIONS without it);
    the number that fail."""
    if checker is None:
        print("vtr-fixtures: note: no --checker given; the blif-vtr-check step is skipped")
        return 0
    failed = 0
    for case in cases:
        blif = fixtures / (case.name + OUT_SUFFIX)
        if not blif.is_file():
            continue
        why = check_expected(checker, case.name, blif)
        if why is not None:
            failed += 1
            print(f"FAIL {case.name}: {why}")
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
