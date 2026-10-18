"""lower-run: the Phase 3 script up to ``compact`` on one design, with the corpus checks (3B spec
14.5 ``lower_micro`` / ``lower_vtr``; plan Task 9 step 4).

Usage::

    lower-run --odin3 O [--top T] [--techlib L] [--lower-args A] [--mem-cap C] [--work DIR] FILE...

Runs ``[read_techlib L;] read_verilog --nolatches FILE...; hierarchy; proc; opt; lower [A];
opt -fast; compact; check; write_blif OUT`` through ``odin3 --check`` (the IR is also checked
before and after every pass), under memcap, and requires:

- every command to return ``ODIN3_OK`` (the child exits 0), the final ``check`` (FULL) to pass,
  and ``write_blif`` to write the design (it refuses a word cell, so a written BLIF is the
  netlist view of IR-9);
- no cap error: a ``lower: ... would create N nodes (cap C)`` line is the outcome ``cap`` (exit 5,
  the one the VTR list ``tests/lower/vtr_expect.txt`` records before 3C lands, spec 14.5).

On success it prints ``lower-run: ok: <lower's INFO line>`` (modules, cells, nodes) and
``lower-run: compared N of 1 designs`` (N is 1 when `lower` lowered a word cell, 0 for "nothing to
lower"; the corpus sweeps sum it and fail below a floor, 3B-4 review H3). A log without any
`lower:` line is a failure (the pass did not run), never ok. This is a smoke check: it sees an
error, a failed `check` or a cap, not a wrong lowering (that is lower-equiv's job). Exit codes:
0 ok; 1 a command or check failed; 2 usage, or Odin III failed to start; 3 the child crashed (a
signal or a memcap kill) or timed out (elab-sweep marks it ``hard``: never an expected failure);
5 a cap (the design needs more than a cap of `lower` allows); 77 skipped (an ASan child and no
systemd-run).
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

TOOL = "lower-run"
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_CRASH, EXIT_CAP, EXIT_SKIP = 0, 1, 2, 3, 5, 77
DEFAULT_CAP = 2 << 30
TIMEOUT_SECONDS = 3600.0
CAP_LINE = re.compile(r"lower: module '[^']*': .* would create \d+ nodes \(cap \d+\)")
TOTALS_LINE = re.compile(r"lower: (?:module '[^']*': lowered|nothing to lower).*$", re.M)
SCRIPT = ("proc", "opt", "lower {args}", "opt -fast", "compact", "check", "write_blif {out}")


def script_for(files: Sequence[str], top: str | None, techlib: str | None, lower_args: str,
               out: Path) -> str:
    """The ``-p`` script of the corpus run."""
    front = opt_equiv.read_commands(files, top, True, techlib)
    body = [step.format(args=lower_args, out=opt_equiv.quote(str(out))).replace("lower ;", "lower;")
            for step in SCRIPT]
    return "; ".join([*front, *(b.rstrip() for b in body)])


def judge(log: str) -> tuple[int, str]:
    """(exit code, one line) of a successful child's log: a log with no ``lower:`` line (module
    totals or "nothing to lower") means the pass never ran, which is a failure."""
    totals = TOTALS_LINE.findall(log)
    if not totals:
        return EXIT_FAIL, "no lower line in the log (the lower pass did not run)"
    return EXIT_OK, "ok: " + "; ".join(totals)


def compared(log: str) -> int:
    """1 when the log shows `lower` lowering at least one word cell, else 0."""
    return int(any("lowered" in line for line in TOTALS_LINE.findall(log)))


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--top", help="the top module")
    ap.add_argument("--techlib", help="read this tech library (.o3lib) first: the hard blocks")
    ap.add_argument("--lower-args", default="", metavar="A", help="arguments of the lower pass")
    ap.add_argument("--mem-cap", type=opt_equiv.parse_size, default=DEFAULT_CAP, metavar="SIZE",
                    help="the child's cap (default 2G)")
    ap.add_argument("--work", type=Path, help="the child's working directory (default: a new one)")
    ap.add_argument("files", nargs="+", help="the Verilog files of one design")
    return ap.parse_args(argv)


def execute(args: argparse.Namespace, work: Path) -> int:
    files = [str(Path(f).resolve()) for f in args.files]
    techlib = str(Path(args.techlib).resolve()) if args.techlib else None
    top = ["--top", args.top] if args.top else []
    script = script_for(files, args.top, techlib, args.lower_args, work / "out.blif")
    done = memcap.run([opt_equiv.exe(args.odin3), "--check", *top, "-p", script], args.mem_cap,
                      capture_output=True, cwd=work, timeout=TIMEOUT_SECONDS)
    log = (done.stdout or "") + (done.stderr or "")
    if done.returncode < 0 or done.returncode == 137:  # 137: a cap kill seen as exit
        print(f"{TOOL}: CRASH: odin3 killed by signal {abs(done.returncode) % 128}: "
              f"{opt_equiv.why(log)}")
        return EXIT_CRASH
    if done.returncode != 0:
        cap = CAP_LINE.search(log)
        if cap is not None:
            print(f"{TOOL}: cap: {cap.group(0)}")
            return EXIT_CAP
        print(f"{TOOL}: FAIL: odin3 exit {done.returncode}: {opt_equiv.why(log)}")
        return EXIT_FAIL
    code, line = judge(log)
    print(f"{TOOL}: {'FAIL: ' if code else ''}{line}")
    if code == EXIT_OK:
        print(f"{TOOL}: compared {compared(log)} of 1 designs")
    return code


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    try:
        with tempfile.TemporaryDirectory(prefix="lower-run-") as tmp:
            work = Path(tmp) if args.work is None else args.work
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
