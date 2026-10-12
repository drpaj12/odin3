#!/usr/bin/env python3
"""compare.py — the generated modules through odin3 and Yosys (2C spec §7.5).

Usage::

    compare.py --odin3 BIN --parse-check BIN --yosys BIN --seeds A..B [--mutate]
               [--work DIR] [--jobs J]

For each seed in A..B (inclusive) ``verilog_gen.py`` writes ``gen_<seed>.v`` (and its expected
tree ``gen_<seed>.ast``) into the work directory, then ``odin3 --parse-only gen_<seed>.v`` and
``yosys -q -p "read_verilog gen_<seed>.v"`` run there.  Each seed is classified:

* both accept: the **tree oracle** -- ``odin3-parse-check --dump gen_<seed>.v`` (read, check,
  print, re-read, equal, the location oracle, and its own comparison with ``gen_<seed>.ast``)
  must pass and print exactly the generator's tree;
* odin3 rejects while Yosys accepts: a **failure** (a 2C bug);
* odin3 accepts while Yosys rejects: **reported** with the file, not a failure (Yosys's
  ``read_verilog`` also elaborates and is stricter on a few legal forms); the tree oracle still
  runs, since odin3 accepted;
* both reject a generated (not mutated) file: a **generator bug**, a failure.

With ``--mutate`` each file has one token dropped, duplicated or swapped: whenever Yosys
rejects, odin3 must reject with a located error (``gen_<seed>.v:L:C: error:``); an accepted
mutant Yosys rejects is reported, as is a mutant odin3 rejects (located) while Yosys accepts
(2C is stricter by decision on a few forms, e.g. ``- -a``).  A sanitizer report, a signal or a
timeout from odin3 is a failure in every mode.

The files of every failed or reported seed stay in the work directory (a fresh temporary one
unless ``--work`` names one), whose path the summary prints.  Exit 0 no failure, 1 a failure,
2 usage error or a tool that cannot run.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import verilog_gen

TIMEOUT_S = 120
SANITIZER_RE = re.compile(r"ERROR: (Address|Leak|Memory|Thread)Sanitizer|runtime error:")


@dataclass
class Run:
    rc: int
    out: str
    err: str
    timed_out: bool = False


def run(cmd: Sequence[str], cwd: Path) -> Run:
    try:
        proc = subprocess.run(list(cmd), cwd=cwd, capture_output=True, text=True,
                              errors="replace", timeout=TIMEOUT_S, check=False)
    except subprocess.TimeoutExpired:
        return Run(-1, "", "", True)
    return Run(proc.returncode, proc.stdout, proc.stderr)


@dataclass
class Tools:
    odin3: str
    parse_check: str
    yosys: str


@dataclass
class Outcome:
    """One seed: failed (with why), reported (with why), or neither."""

    seed: int
    failure: str = ""
    report: str = ""
    files: list[str] = field(default_factory=list)


def crashed(result: Run) -> str:
    if result.timed_out:
        return "timed out"
    if result.rc < 0:
        return f"killed by signal {-result.rc}"
    if SANITIZER_RE.search(result.err):
        return "sanitizer report"
    return ""


def located(err: str, name: str) -> bool:
    """A diagnostic located in the file: NAME:line:col: error: with line, col >= 1."""
    pattern = re.compile(re.escape(name) + r":([1-9][0-9]*):([1-9][0-9]*): error:")
    return pattern.search(err) is not None


def tree_oracle(tools: Tools, work: Path, name: str, expected: str) -> str:
    """'' when odin3-parse-check passes and dumps exactly the generator's tree, else why."""
    result = run([tools.parse_check, "--dump", name], work)
    why = crashed(result)
    if why:
        return f"odin3-parse-check {why}"
    if result.rc != 0:
        return f"odin3-parse-check exit {result.rc}: {last_lines(result.err)}"
    if result.out != expected:
        return "the dump differs from the generator's tree (diff gen_N.ast)"
    return ""


def last_lines(text: str, count: int = 3) -> str:
    lines = [line for line in text.splitlines() if line.strip()]
    return " | ".join(lines[-count:])


def check_plain(tools: Tools, work: Path, seed: int) -> Outcome:
    name = f"gen_{seed}.v"
    made = verilog_gen.render(seed, name)
    (work / name).write_text(made.text, encoding="utf-8")
    header = f"# generated: verilog_gen.py --seed {seed}\n"
    (work / f"gen_{seed}.ast").write_text(header + made.ast, encoding="utf-8")
    outcome = Outcome(seed, files=[name, f"gen_{seed}.ast"])
    odin3 = run([tools.odin3, "--parse-only", name], work)
    yosys = run([tools.yosys, "-q", "-p", f"read_verilog {name}"], work)
    why = crashed(odin3)
    if why:
        outcome.failure = f"odin3 {why}"
    elif odin3.rc != 0 and yosys.rc == 0:
        outcome.failure = f"odin3 rejects, Yosys accepts: {last_lines(odin3.err)}"
    elif odin3.rc != 0:
        outcome.failure = f"both reject (generator bug): {last_lines(yosys.err, 1)}"
    else:
        outcome.failure = tree_oracle(tools, work, name, made.ast)
        if yosys.rc != 0:
            outcome.report = f"Yosys rejects: {last_lines(yosys.err + yosys.out, 1)}"
    return outcome


def check_mutant(tools: Tools, work: Path, seed: int) -> Outcome:
    name = f"gen_{seed}.v"
    made = verilog_gen.render(seed, name, mutated=True)
    (work / name).write_text(made.text, encoding="utf-8")
    outcome = Outcome(seed, files=[name])
    odin3 = run([tools.odin3, "--parse-only", name], work)
    yosys = run([tools.yosys, "-q", "-p", f"read_verilog {name}"], work)
    why = crashed(odin3)
    if why:
        outcome.failure = f"odin3 {why} ({made.note})"
    elif odin3.rc != 0 and not located(odin3.err, name):
        outcome.failure = f"odin3 rejects without a located error ({made.note})"
    elif odin3.rc == 0 and yosys.rc != 0:
        outcome.report = f"accepted mutant Yosys rejects ({made.note})"
    elif odin3.rc != 0 and yosys.rc == 0:
        outcome.report = f"mutant odin3 rejects, Yosys accepts ({made.note}): " \
                         f"{last_lines(odin3.err, 2)}"
    return outcome


def one_seed(tools: Tools, work: Path, seed: int, mutated: bool) -> Outcome:
    outcome = check_mutant(tools, work, seed) if mutated else check_plain(tools, work, seed)
    if not outcome.failure and not outcome.report:
        for name in outcome.files:
            (work / name).unlink(missing_ok=True)
    return outcome


def parse_seeds(text: str) -> range:
    match = re.fullmatch(r"(\d+)(?:\.\.(\d+))?", text)
    if match is None:
        raise argparse.ArgumentTypeError(f"--seeds wants A..B, not {text!r}")
    first = int(match.group(1))
    last = int(match.group(2)) if match.group(2) else first
    if last < first:
        raise argparse.ArgumentTypeError(f"--seeds {text}: B is below A")
    return range(first, last + 1)


def tools_missing(tools: Tools) -> list[str]:
    return [path for path in (tools.odin3, tools.parse_check, tools.yosys)
            if shutil.which(path) is None]


def summarize(outcomes: Sequence[Outcome], work: Path, mutated: bool) -> int:
    failed = [o for o in outcomes if o.failure]
    reported = [o for o in outcomes if o.report and not o.failure]
    for outcome in failed:
        print(f"FAIL seed {outcome.seed}: {outcome.failure} [{work / outcome.files[0]}]")
    for outcome in reported:
        print(f"REPORT seed {outcome.seed}: {outcome.report} [{work / outcome.files[0]}]")
    mode = "mutants" if mutated else "generated files"
    passed = len(outcomes) - len(failed) - len(reported)
    print(f"verilog-gen compare: {len(outcomes)} {mode}: {passed} passed, "
          f"{len(reported)} reported, {len(failed)} failed (work directory {work})")
    return 1 if failed else 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--odin3", required=True)
    parser.add_argument("--parse-check", required=True)
    parser.add_argument("--yosys", required=True)
    parser.add_argument("--seeds", required=True, type=parse_seeds, help="A..B, inclusive")
    parser.add_argument("--mutate", action="store_true")
    parser.add_argument("--work", help="directory for the files (default: a new temporary one)")
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args(argv)
    tools = Tools(args.odin3, args.parse_check, args.yosys)
    missing = tools_missing(tools)
    if missing:
        print(f"compare.py: cannot run {', '.join(missing)}", file=sys.stderr)
        return 2
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="verilog-gen-"))
    work.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        outcomes = list(pool.map(lambda s: one_seed(tools, work, s, args.mutate), args.seeds))
    code = summarize(outcomes, work, args.mutate)
    if not args.work and not any(o.failure or o.report for o in outcomes):
        shutil.rmtree(work, ignore_errors=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
