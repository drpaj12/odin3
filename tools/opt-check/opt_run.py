"""opt-run: Odin III's Phase 2 script on one design, with the corpus checks (2F spec 20.4, plan
Task 18).

Usage::

    opt-run --odin3 O [--top T] [--techlib L] [--nolatches] [--mem-cap C] [--work DIR] FILE...

Runs ``[read_techlib L;] read_verilog [--nolatches] FILE...; hierarchy; <the Phase 2 script of
tools/opt-check/phase2.script>; check`` through ``odin3 --check`` (the IR is also checked before
and after every pass), under memcap, and requires:

- every command to return ``ODIN3_OK`` (the child exits 0) and the final ``check`` (FULL) to pass;
- no ``opt: ... no fixed point`` warning, and every ``opt: fixed point after K iterations`` line to
  have ``K`` below ``ODIN3_OPT_MAX_ITERATIONS`` (10,000; ``src/passes/opt/opt.h``).

On success it prints ``opt-run: ok: iterations max=K opt-runs=N`` (the largest K of the two ``opt``
runs of the script, and how many ``opt`` runs reported one). Exit codes: 0 ok; 1 a check failed;
2 usage, or Odin III failed to start; 3 the child crashed (a signal or
a memcap kill) or timed out (elab-sweep marks it ``hard``: never an expected failure); 77
skipped (an ASan child and no systemd-run).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

import memcap
import opt_equiv

TOOL = "opt-run"
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_CRASH, EXIT_SKIP = 0, 1, 2, 3, 77
MAX_ITERATIONS = 10000  # ODIN3_OPT_MAX_ITERATIONS (test_opt_run checks it against opt.h)
DEFAULT_CAP = 2 << 30
TIMEOUT_SECONDS = 3600.0
FIXED_POINT = re.compile(r"opt: fixed point after (\d+) iterations")
NO_FIXED_POINT = re.compile(r"opt: module '[^']*': no fixed point after (\d+) iterations")


def script_for(files: Sequence[str], top: str | None, nolatches: bool, techlib: str | None
               ) -> str:
    """The ``-p`` script: the read, ``hierarchy`` and the Phase 2 commands, then ``check``."""
    front = opt_equiv.read_commands(files, top, nolatches, techlib)
    return "; ".join([*front, *opt_equiv.phase2_commands(), "check"])


def judge(log: str) -> tuple[int, str]:
    """(exit code, one line) of a successful child's log."""
    stuck = NO_FIXED_POINT.search(log)
    if stuck is not None:
        return EXIT_FAIL, f"no fixed point after {stuck.group(1)} iterations (the cap)"
    counts = [int(k) for k in FIXED_POINT.findall(log)]
    if not counts:
        return EXIT_FAIL, "no `opt: fixed point` line in the log"
    if max(counts) >= MAX_ITERATIONS:
        return EXIT_FAIL, f"{max(counts)} iterations reach the cap {MAX_ITERATIONS}"
    return EXIT_OK, f"ok: iterations max={max(counts)} opt-runs={len(counts)}"


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--top", help="the top module")
    ap.add_argument("--techlib", help="read this tech library (.o3lib) first: the hard blocks")
    ap.add_argument("--nolatches", action="store_true", help="read_verilog --nolatches")
    ap.add_argument("--mem-cap", type=opt_equiv.parse_size, default=DEFAULT_CAP, metavar="SIZE",
                    help="the child's cap (default 2G)")
    ap.add_argument("--work", type=Path, help="the child's working directory (default: a new one)")
    ap.add_argument("files", nargs="+", help="the Verilog files of one design")
    return ap.parse_args(argv)


def execute(args: argparse.Namespace, work: Path) -> int:
    files = [str(Path(f).resolve()) for f in args.files]
    techlib = str(Path(args.techlib).resolve()) if args.techlib else None
    top = ["--top", args.top] if args.top else []
    script = script_for(files, args.top, args.nolatches, techlib)
    done = memcap.run([opt_equiv.exe(args.odin3), "--check", *top, "-p", script], args.mem_cap,
                      capture_output=True, cwd=work, timeout=TIMEOUT_SECONDS)
    log = (done.stdout or "") + (done.stderr or "")
    if done.returncode < 0 or done.returncode == 137:  # 137: a cap kill seen as exit
        print(f"{TOOL}: CRASH: odin3 killed by signal {abs(done.returncode) % 128}: "
              f"{opt_equiv.why(log)}")
        return EXIT_CRASH
    if done.returncode != 0:
        print(f"{TOOL}: FAIL: odin3 exit {done.returncode}: {opt_equiv.why(log)}")
        return EXIT_FAIL
    code, line = judge(log)
    print(f"{TOOL}: {'FAIL: ' if code else ''}{line}")
    return code


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    made = args.work is None
    try:
        with tempfile.TemporaryDirectory(prefix="opt-run-") as tmp:
            work = Path(tmp) if made else args.work
            return execute(args, work)
    except subprocess.TimeoutExpired:
        print(f"{TOOL}: CRASH: odin3 timed out after {TIMEOUT_SECONDS:.0f} s")
        return EXIT_CRASH
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP
    except (OSError, opt_equiv.ToolError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
