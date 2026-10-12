"""run-reject: the Verilog negative cases (2C spec §7.3, plan Task 9 points (i), (j)).

    run-reject [--elaborate] <odin3> <dir>

Every ``<case>.v`` in <dir> (smallest first) runs in <dir> under its basename, with the default
given paths (plan point (j)): ``odin3 --parse-only <case>.v``, or with --elaborate
``odin3 -p "read_verilog <case>.v"`` (no --parse-only: the 2D handover cases of
tests/verilog/reject-2d and ignore-2d). Its expectation is ``<case>.err`` or ``<case>.warn``:

    line 1  the located prefix, e.g. ``case.v:3:14: error:`` (``warning:`` in a .warn)
    line 2  a substring of that same diagnostic line

The logger's ``odin3: <level>: `` prefix is removed from each output line before matching. A
case passes when a line starts with line 1 and contains line 2 — for an .err without
--elaborate, the first error line (the error a 2C case pins; cascades may follow), otherwise
any line (a 2D handover file may raise another row's error first) — the exit status is odin3's
failure status 1 for an .err (0 for a .warn; a signal or any other status is a crash), and no
line reports ODIN3_ERR_INVALID_ARG, ODIN3_ERR_CHECK, a check failure (a builder or check failure
on user input is a 2C bug) or a sanitizer report (AddressSanitizer, LeakSanitizer, UBSan's
``runtime error:``).

With --elaborate each case is judged on its own (2D plan point (l), review I4): **pass** as
above; else **fail** on a forbidden line, a signal or a status other than 0 and 1 (2D Task 10
review I2: a crash never hides behind a pending line); else **pending** when a line contains
``elaboration is not available yet (2D)`` — 2D's located line for a construct whose elaboration
has not landed (``f.v:3:5: error: elaboration is not available yet (2D): 'ALWAYS'``) or stage 2's
unlocated stub text — listed, not failed; else **fail**. The summary line is ``run-reject: pass
N, pending P, fail F``. Exit codes: 0 every case passes or is pending, 1 any case fails, 2 usage
or input error, 77 (--elaborate) every case is pending.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_SKIP = 0, 1, 2, 77
STUB_TEXT = "elaboration is not available yet (2D)"
ERROR = "odin3: error: "
LOGGER_PREFIX = re.compile(r"^odin3: (?:error|warning|info|debug): ")
FORBIDDEN = re.compile(
    r"ODIN3_ERR_INVALID_ARG|ODIN3_ERR_CHECK|^(?:ast check|check|pp_check): "
    r"|AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|MemorySanitizer|ThreadSanitizer"
    r"|runtime error: "
)
EXIT_ODIN3_FAIL = 1  # odin3's exit status for a failed pass (0 success, 2 usage)
TIMEOUT_S = 120
# A sanitizer report is printed unsymbolized: symbolizing one can take about two minutes on a
# machine without an unversioned llvm-symbolizer, which the timeout would misread (2D Task 10
# review, n1). The report's banner is what FORBIDDEN judges; addresses are enough to see a fail.
SANITIZER_QUIET = "symbolize=0"


def case_env() -> dict[str, str]:
    """The environment of each case: ours, with symbolize=0 added to ASAN_OPTIONS."""
    env = dict(os.environ)
    given = env.get("ASAN_OPTIONS", "")
    env["ASAN_OPTIONS"] = f"{given}:{SANITIZER_QUIET}" if given else SANITIZER_QUIET
    return env


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


def candidates(expect: Expect, output: str, elaborate: bool) -> list[str]:
    """The lines the expectation may match: a 2C .err case pins the first error line (cascades
    may follow); otherwise any line."""
    lines = log_lines(output)
    if not expect.is_error or elaborate:
        return lines
    raws = output.splitlines()
    return [line for raw, line in zip(raws, lines, strict=True) if raw.startswith(ERROR)][:1]


def judge(expect: Expect, code: int, output: str, elaborate: bool) -> Verdict:
    """Pass, pending (--elaborate and a not-yet-available line of 2D) or fail, with the reason."""
    lines = log_lines(output)
    found = any(
        line.startswith(expect.prefix) and expect.text in line
        for line in candidates(expect, output, elaborate)
    )
    forbidden = [line for line in lines if FORBIDDEN.search(line)]
    exit_ok = code == EXIT_ODIN3_FAIL if expect.is_error else code == 0
    if found and exit_ok and not forbidden:
        return Verdict("pass", "")
    if forbidden:  # a sanitizer report, a check failure, misuse: never pending
        return Verdict("fail", f"forbidden line: {forbidden[0]}")
    if code not in (0, EXIT_ODIN3_FAIL):  # a signal (< 0) or a crash's status: never pending
        return Verdict("fail", f"exit status {code}")
    if elaborate and any(STUB_TEXT in line for line in lines):
        return Verdict("pending", "2D: not available yet")
    if not found:
        return Verdict("fail", f"no line '{expect.prefix} ...{expect.text}...'")
    return Verdict("fail", f"exit status {code}")


def command(odin3: str, name: str, elaborate: bool) -> list[str]:
    if elaborate:
        return [odin3, "-p", f"read_verilog {name}"]
    return [odin3, "--parse-only", name]


def run_case(odin3: str, case: Path, elaborate: bool) -> tuple[int, str]:
    proc = subprocess.run(
        command(odin3, case.name, elaborate),
        cwd=case.parent,
        env=case_env(),
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
