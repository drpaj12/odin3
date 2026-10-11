"""run-reject: the Verilog negative cases (2C spec §7.3, plan Task 9 points (i), (j)).

    run-reject [--elaborate] <odin3> <dir>

Every ``<case>.v`` in <dir> (smallest first) runs in <dir> under its basename, with the default
given paths (plan point (j)): ``odin3 --parse-only <case>.v``, or with --elaborate
``odin3 -p "read_verilog <case>.v"`` (no --parse-only: the 2D handover cases of
tests/verilog/reject-2d and ignore-2d). Its expectation is ``<case>.err`` or ``<case>.warn``:

    line 1  the located prefix, e.g. ``case.v:3:14: error:`` (``warning:`` in a .warn)
    line 2  a substring of that same diagnostic line

The logger's ``odin3: <level>: `` prefix is removed from each output line before matching. A
case passes when some line starts with line 1 and contains line 2, the exit status is non-zero
for an .err (zero for a .warn), and no line reports ODIN3_ERR_INVALID_ARG, ODIN3_ERR_CHECK or a
check failure (a builder or check failure on user input is a 2C bug).

With --elaborate a case that does not pass but prints the 2D stub's ``elaboration is not
available yet (2D)`` is pending, not failed: the handover cases run as soon as 2D replaces the
stub. Exit codes: 0 every case passes (or passes or is pending), 1 any case fails, 2 usage or
input error, 77 (--elaborate) every case is pending.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_SKIP = 0, 1, 2, 77
STUB_TEXT = "elaboration is not available yet (2D)"
LOGGER_PREFIX = re.compile(r"^odin3: (?:error|warning|info|debug): ")
FORBIDDEN = re.compile(r"ODIN3_ERR_INVALID_ARG|ODIN3_ERR_CHECK|^(?:ast check|check|pp_check): ")
TIMEOUT_S = 120


@dataclass(frozen=True)
class Expect:
    prefix: str
    text: str
    is_error: bool


@dataclass(frozen=True)
class Verdict:
    status: str  # "pass", "pending" or "fail"
    why: str


def read_expect(case: Path) -> Expect | None:
    """The case's .err (or .warn) expectation; None when it has neither or it is malformed."""
    for suffix, is_error in ((".err", True), (".warn", False)):
        path = case.with_suffix(suffix)
        if path.exists():
            lines = path.read_text(encoding="utf-8").splitlines()
            level = "error:" if is_error else "warning:"
            if len(lines) < 2 or not lines[0].rstrip().endswith(level) or not lines[1]:
                return None
            return Expect(lines[0].rstrip(), lines[1], is_error)
    return None


def log_lines(output: str) -> list[str]:
    return [LOGGER_PREFIX.sub("", line) for line in output.splitlines()]


def judge(expect: Expect, code: int, output: str, elaborate: bool) -> Verdict:
    """Pass, pending (--elaborate and the 2D stub) or fail, with the reason."""
    lines = log_lines(output)
    found = any(line.startswith(expect.prefix) and expect.text in line for line in lines)
    forbidden = [line for line in lines if FORBIDDEN.search(line)]
    exit_ok = code != 0 if expect.is_error else code == 0
    if found and exit_ok and not forbidden:
        return Verdict("pass", "")
    if elaborate and any(STUB_TEXT in line for line in lines):
        return Verdict("pending", "2D stub")
    if not found:
        why = f"no line '{expect.prefix} ...{expect.text}...'"
    elif not exit_ok:
        why = f"exit status {code}"
    else:
        why = f"forbidden line: {forbidden[0]}"
    return Verdict("fail", why)


def command(odin3: str, name: str, elaborate: bool) -> list[str]:
    if elaborate:
        return [odin3, "-p", f"read_verilog {name}"]
    return [odin3, "--parse-only", name]


def run_case(odin3: str, case: Path, elaborate: bool) -> tuple[int, str]:
    proc = subprocess.run(
        command(odin3, case.name, elaborate),
        cwd=case.parent,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=TIMEOUT_S,
        check=False,
    )
    return proc.returncode, proc.stdout + proc.stderr


def cases_of(directory: Path) -> list[Path]:
    """The directory's .v cases, smallest first (then by name)."""
    return sorted(directory.glob("*.v"), key=lambda p: (p.stat().st_size, p.name))


def summarize(verdicts: dict[str, Verdict], elaborate: bool) -> int:
    counts = {s: sum(v.status == s for v in verdicts.values()) for s in ("pass", "pending", "fail")}
    for name, verdict in verdicts.items():
        if verdict.status != "pass":
            print(f"{verdict.status.upper()} {name}: {verdict.why}", file=sys.stderr)
    print(f"run-reject: pass {counts['pass']}, pending {counts['pending']}, fail {counts['fail']}")
    if counts["fail"] > 0:
        return EXIT_FAIL
    if elaborate and counts["pending"] == len(verdicts):
        return EXIT_SKIP
    return EXIT_OK


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="run-reject", description="the Verilog negative cases")
    parser.add_argument(
        "--elaborate", action="store_true", help="read_verilog without --parse-only"
    )
    parser.add_argument("odin3", help="the odin3 executable")
    parser.add_argument("dir", type=Path, help="the case directory")
    args = parser.parse_args(argv)
    odin3 = str(Path(args.odin3).resolve())
    cases = cases_of(args.dir) if args.dir.is_dir() else []
    if not cases:
        print(f"run-reject: no cases in {args.dir}", file=sys.stderr)
        return EXIT_USAGE
    verdicts: dict[str, Verdict] = {}
    for case in cases:
        expect = read_expect(case)
        if expect is None:
            print(f"run-reject: {case}: missing or malformed .err/.warn", file=sys.stderr)
            return EXIT_USAGE
        try:
            code, output = run_case(odin3, case, args.elaborate)
        except (OSError, subprocess.TimeoutExpired) as exc:
            print(f"run-reject: {case}: {exc}", file=sys.stderr)
            return EXIT_USAGE
        verdicts[case.name] = judge(expect, code, output, args.elaborate)
    return summarize(verdicts, args.elaborate)


if __name__ == "__main__":
    sys.exit(main())
