"""fsm-diff: Odin III's ``fsm`` against Yosys 0.55's on the same input (4.8 spec T-3).

Usage::

    fsm-diff [--odin PATH] [--yosys PATH] [--techlib FILE] [--corpus FILE] [--expect FILE]
             [--only NAME...] [--micros] [--keep DIR] [--design NAME=PATH...]

Per design of the corpus (``tests/fsm/facts_corpus.txt``, plus ``--design``): Odin III reads the
source, runs ``proc`` and writes ``in.il``; then both run ``fsm -nomap`` on it (Odin III on the
design in memory, Yosys on ``in.il``) and the facts are compared: the registers marked and
rejected (stage ``detect``), the extraction failures (``failures``) and, per FSM, the state
count, state bits before and after, encoding, reset state, numbers of control inputs, outputs
and transitions, the state codes and the shape of the transition table (``facts``). Then both run
the full ``fsm; opt_clean; opt -fast`` and the change of the cell-type histogram of every
module, against the same run without ``fsm``, is compared (``map``).

``tests/fsm/diff_expect.txt`` lists the differences that are accepted: ``NAME STAGE REASON``.
Lines whose stage starts with ``native-`` belong to other tests. A listed difference is absorbed;
a listed difference that did not occur is a stale line and fails.

Exit codes: 0 no unexpected difference; 1 a difference or a stale expect line; 2 usage or tool
error; 3 a tool crashed or timed out.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import fsm_facts
import fsm_probe
import odin_log
import yosys_log

EXIT_OK = 0
EXIT_DIFFER = 1
EXIT_ERROR = 2
EXIT_HARD = 3

TIMEOUT_SECONDS = 1800
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EXPECT = REPO_ROOT / "tests/fsm/diff_expect.txt"
RAM_MODELS = ("single_port_ram", "dual_port_ram")


class DiffError(Exception):
    """A usage or tool error (exit 2)."""


class HardError(Exception):
    """A tool crashed or timed out (exit 3)."""


@dataclass(frozen=True)
class Diff:
    """One difference: the stage it belongs to and what differs."""

    stage: str
    text: str


# --- comparing facts --------------------------------------------------------------------------

REASON_CODE = {
    "Register is connected to module port": "port",
    "Users of register don't seem to benefit from recoding": "users",
    "Register has an initialization value": "init",
    "Circuit seems to be self-resetting": "selfreset",
}


def _marks(mods: dict[str, yosys_log.ModuleFacts]) -> set[str]:
    return {f"{m}.{w}" for m, facts in mods.items() for w in facts.marked}


def _rejects(mods: dict[str, yosys_log.ModuleFacts]) -> set[str]:
    out: set[str] = set()
    for mod, facts in mods.items():
        for wire, reasons in facts.rejected.items():
            codes = "+".join(sorted(REASON_CODE.get(r, r) for r in reasons))
            out.add(f"{mod}.{wire}:{codes}")
    return out


def _failures(mods: dict[str, yosys_log.ModuleFacts]) -> set[str]:
    return {f"{m}.{w}: {r}" for m, facts in mods.items() for w, r in facts.failures}


def _popcount(bits: str) -> int:
    return bits.split("'", 1)[-1].count("1")


def _dont_cares(bits: str) -> int:
    return bits.split("'", 1)[-1].count("-")


def shape(fsm: yosys_log.Fsm) -> tuple[object, ...]:
    """What a table has in common whatever the order of its control signals: the facts and, per
    transition, the states, the number of 1s in the outputs and of don't cares in the inputs."""
    rows = sorted((t[0], t[2], _popcount(t[3]), _dont_cares(t[1])) for t in fsm.table)
    return (fsm.states, fsm.bits_before, fsm.bits_after, fsm.encoding, fsm.reset_index,
            fsm.inputs, fsm.outputs, fsm.transitions, fsm.codes, tuple(rows))


def _fsm_map(mods: dict[str, yosys_log.ModuleFacts]) -> dict[str, yosys_log.Fsm]:
    return {f"{m}.{f.wire}": f for m, facts in mods.items() for f in facts.fsms}


def compare_facts(yosys: dict[str, yosys_log.ModuleFacts],
                  odin: dict[str, yosys_log.ModuleFacts]) -> list[Diff]:
    """The differences between the two logs' facts (stages detect, failures, facts)."""
    diffs: list[Diff] = []
    for what, lhs, rhs in (("marked", _marks(yosys), _marks(odin)),
                           ("rejected", _rejects(yosys), _rejects(odin))):
        for item in sorted(lhs - rhs):
            diffs.append(Diff("detect", f"{what} by Yosys only: {item}"))
        for item in sorted(rhs - lhs):
            diffs.append(Diff("detect", f"{what} by Odin III only: {item}"))
    for item in sorted(_failures(yosys) ^ _failures(odin)):
        diffs.append(Diff("failures", f"extraction failure on one side only: {item}"))
    y_fsms, o_fsms = _fsm_map(yosys), _fsm_map(odin)
    for key in sorted(y_fsms.keys() | o_fsms.keys()):
        if key not in o_fsms:
            diffs.append(Diff("facts", f"FSM {key} found by Yosys only"))
        elif key not in y_fsms:
            diffs.append(Diff("facts", f"FSM {key} found by Odin III only"))
        elif shape(y_fsms[key]) != shape(o_fsms[key]):
            diffs.append(Diff("facts", f"FSM {key}: {_describe(y_fsms[key], o_fsms[key])}"))
    return diffs


def _describe(yos: yosys_log.Fsm, odi: yosys_log.Fsm) -> str:
    fields = ("states", "bits_before", "bits_after", "encoding", "reset_index", "inputs",
              "outputs", "transitions", "codes")
    parts = [f"{f} Yosys {getattr(yos, f)} Odin III {getattr(odi, f)}"
             for f in fields if getattr(yos, f) != getattr(odi, f)]
    return "; ".join(parts) or "transition tables differ in shape"


# --- histograms -------------------------------------------------------------------------------

MODULE_LINE = re.compile(r"^module (\S+)")
CELL_LINE = re.compile(r"^\s+cell (\S+) ")


def histograms(rtlil: str) -> dict[str, Counter[str]]:
    """Per module, the number of cells of each type in an RTLIL text."""
    out: dict[str, Counter[str]] = {}
    current: Counter[str] | None = None
    for line in rtlil.splitlines():
        if (m := MODULE_LINE.match(line)) is not None:
            current = out.setdefault(m.group(1).lstrip("\\"), Counter())
        elif current is not None and (c := CELL_LINE.match(line)) is not None:
            current[c.group(1)] += 1
    return out


Hist = dict[str, Counter[str]]


def hist_delta(after: Hist, before: Hist) -> Hist:
    """What a step added or removed, per module and cell type (zero entries dropped)."""
    out: Hist = {}
    for mod in after.keys() | before.keys():
        counts = Counter(after.get(mod, Counter()))
        counts.subtract(before.get(mod, Counter()))
        out[mod] = Counter({t: n for t, n in counts.items() if n != 0})
    return out


def compare_histograms(yosys: Hist, odin: Hist) -> list[Diff]:
    diffs: list[Diff] = []
    for mod in sorted(yosys.keys() | odin.keys()):
        lhs, rhs = yosys.get(mod, Counter()), odin.get(mod, Counter())
        if lhs != rhs:
            types = sorted(lhs.keys() | rhs.keys())
            delta = {t: (lhs[t], rhs[t]) for t in types if lhs[t] != rhs[t]}
            diffs.append(Diff("map", f"module {mod}: cells (Yosys, Odin III) {delta}"))
    return diffs


# --- the expect list ----------------------------------------------------------------------------

def read_expect(path: Path) -> list[tuple[str, str, str]]:
    rows: list[tuple[str, str, str]] = []
    for raw in path.read_text().splitlines():
        if not raw.strip() or raw.startswith("#"):
            continue
        parts = raw.split(None, 2)
        if len(parts) != 3:
            raise DiffError(f"{path}: a line needs NAME STAGE REASON: {raw!r}")
        rows.append((parts[0], parts[1], parts[2]))
    return rows


def apply_expect(name: str, diffs: list[Diff], expect: Sequence[tuple[str, str, str]]
                 ) -> tuple[list[Diff], set[tuple[str, str]]]:
    """The differences nothing absorbs, and the (name, stage) pairs of the expect list used."""
    stages = {stage for n, stage, _ in expect if n == name}
    left = [d for d in diffs if d.stage not in stages]
    used = {(name, d.stage) for d in diffs if d.stage in stages}
    return left, used


def stale_lines(expect: Sequence[tuple[str, str, str]], used: set[tuple[str, str]],
                names: Sequence[str]) -> list[str]:
    """Expect lines of a design that ran whose difference did not happen."""
    return [f"{n} {stage}" for n, stage, _ in expect
            if n in names and not stage.startswith("native-") and (n, stage) not in used]


# --- running the tools ----------------------------------------------------------------------------

def _run(cmd: Sequence[str], log: Path, limit: bool) -> str:
    prefix = ["prlimit", f"--as={fsm_probe.ADDRESS_LIMIT_KIB * 1024}"] if (
        limit and shutil.which("prlimit")) else []
    try:
        proc = subprocess.run(list(prefix) + list(cmd), capture_output=True, text=True,
                              timeout=TIMEOUT_SECONDS, check=False)
    except subprocess.TimeoutExpired as exc:
        raise HardError(f"timeout: {' '.join(cmd[:3])}") from exc
    text = proc.stdout + proc.stderr
    log.write_text(text)
    if proc.returncode != 0:
        raise HardError(f"{cmd[0]} exited {proc.returncode}: {text[-400:]}")
    return text


def needs_vtr(path: str) -> bool:
    return any(model in Path(path).read_text(errors="replace") for model in RAM_MODELS)


@dataclass
class Tools:
    odin: str
    yosys: str
    techlib: str


def odin_read(tools: Tools, kind: str, path: str) -> str:
    vtr = kind == "vtr" or needs_vtr(path)
    flags = "--nolatches --vtr-compat" if vtr else "--nolatches"
    pre = f"read_techlib {tools.techlib}; " if vtr else ""
    return f"{pre}read_verilog {flags} {path}; proc"


def run_design(tools: Tools, work: Path, kind: str, path: str) -> tuple[list[Diff], list[str]]:
    """Both runs on one design; the differences and a one-line note."""
    il = work / "in.il"
    read = odin_read(tools, kind, path)
    odin_text = _run([tools.odin, "-p", f"{read}; write_rtlil {il}; fsm -nomap"],
                     work / "odin.log", False)
    yos_text = _run([tools.yosys, "-p", f"read_rtlil {il}; fsm -nomap"], work / "yosys.log", True)
    diffs = compare_facts(yosys_log.parse_log(yos_text), odin_log.parse_log(odin_text))
    odin_out, yos_out = work / "odin_out.il", work / "yosys_out.il"
    _run([tools.odin, "-p", f"{read}; fsm; opt_clean; opt -fast; write_rtlil {odin_out}"],
         work / "odin_full.log", False)
    _run([tools.yosys, "-p", f"read_rtlil {il}; fsm; opt_clean; opt -fast; write_rtlil {yos_out}"],
         work / "yosys_full.log", True)
    odin_base, yos_base = work / "odin_base.il", work / "yosys_base.il"
    _run([tools.odin, "-p", f"{read}; opt_clean; opt -fast; write_rtlil {odin_base}"],
         work / "odin_base.log", False)
    _run([tools.yosys, "-p", f"read_rtlil {il}; opt_clean; opt -fast; write_rtlil {yos_base}"],
         work / "yosys_base.log", True)
    # what fsm changed, so that a difference opt has without any fsm is not counted here
    diffs += compare_histograms(
        hist_delta(histograms(yos_out.read_text()), histograms(yos_base.read_text())),
        hist_delta(histograms(odin_out.read_text()), histograms(odin_base.read_text())))
    n_fsm = len(_fsm_map(yosys_log.parse_log(yos_text)))
    return diffs, [f"{n_fsm} FSMs"]


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="fsm-diff", description=__doc__.split("\n")[0])
    ap.add_argument("--odin", default=str(REPO_ROOT / "build/release/odin3"))
    ap.add_argument("--yosys", default=fsm_probe.default_yosys())
    ap.add_argument("--techlib", default=str(REPO_ROOT / "lib/vtr.o3lib"))
    ap.add_argument("--corpus", type=Path, default=fsm_facts.DEFAULT_CORPUS)
    ap.add_argument("--expect", type=Path, default=DEFAULT_EXPECT)
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--micros", action="store_true", help="only the micros of the corpus")
    ap.add_argument("--design", nargs="*", default=[], metavar="NAME=PATH")
    ap.add_argument("--keep", type=Path, help="keep the work files under this directory")
    args = ap.parse_args(argv)
    try:
        rows = [(n, k, p) for n, k, p, _ in fsm_facts.read_corpus(args.corpus)]
        rows += [(d.split("=", 1)[0], "micro", d.split("=", 1)[1]) for d in args.design]
        expect = read_expect(args.expect)
        tools = Tools(args.odin, args.yosys, args.techlib)
        return _main_loop(tools, rows, expect, args)
    except (DiffError, fsm_probe.ProbeError, OSError) as exc:
        print(f"fsm-diff: {exc}", file=sys.stderr)
        return EXIT_ERROR
    except HardError as exc:
        print(f"fsm-diff: {exc}", file=sys.stderr)
        return EXIT_HARD


def _main_loop(tools: Tools, rows: list[tuple[str, str, str]],
               expect: Sequence[tuple[str, str, str]], args: argparse.Namespace) -> int:
    failed = False
    used: set[tuple[str, str]] = set()
    ran: list[str] = []
    for name, kind, rel in rows:
        if (args.only and name not in args.only) or (args.micros and kind != "micro"):
            continue
        path = rel if os.path.isabs(rel) else fsm_facts.design_path(kind, rel)
        with tempfile.TemporaryDirectory(prefix="fsm-diff-") as tmp:
            work = Path(tmp)
            diffs, note = run_design(tools, work, kind, path)
            if args.keep is not None:
                shutil.copytree(work, args.keep / name.replace("/", "_"), dirs_exist_ok=True)
        left, hit = apply_expect(name, diffs, expect)
        used |= hit
        ran.append(name)
        for diff in left:
            print(f"{name}: DIFF [{diff.stage}] {diff.text}")
        failed = failed or bool(left)
        print(f"{name}: {'ok' if not left else 'differs'} ({', '.join(note)})")
    for line in stale_lines(expect, used, ran):
        print(f"stale expect line: {line}")
        failed = True
    return EXIT_DIFFER if failed else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
