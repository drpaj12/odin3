"""opt-rules: the per-rule goldens of the opt sub-passes (2F spec 20.1; plan Task 16, points f, g).

Usage::

    opt-rules --odin3 O [--yosys Y] [--rules rules.txt] [--root R] [--write] [--mem-cap C]
              [--root R] <rule dir or file>...

``rules.txt`` has one row per rule, ``<sub>/<rule> <counter> <yosys id> [pending|dev=N]``, where
``<sub>`` is ``expr merge muxtree reduce share dff clean`` (the sub-pass ``opt_<sub>``) and
``<rule>.v`` (or ``<rule>.blif`` for the bit-gate, ``$sop`` and ``$_DFF_*`` rules) sits beside
``<rule>.il``, its golden. ``#`` starts a comment. The source's ``// options: ...`` line gives the
sub-pass options (Yosys spellings, which Odin III accepts too).

Per row, smallest source first, Odin III runs plan point (g)::

    read_verilog --nolatches R.v; proc --noopt; write_rtlil in.il;
    opt_<sub> <options> --opt-stats; stats; write_rtlil out.il

(``read_blif R.blif`` for a BLIF source; a ``// prepass: <commands>`` line adds Odin III
commands before ``write_rtlil in.il``, e.g. ``opt_expr`` to make a control signal constant), and checks that the counter is at least 1 and that
``out.il`` equals ``<rule>.il``. With ``--yosys`` Yosys 0.55 runs
``read_rtlil in.il; debug opt_<sub> <options>; stat`` under ``YOSYS_COVER_DIR``; its cover file must
count the row's cover id (``opt.opt_expr.const_and``; Yosys has cover points in ``opt_expr`` only),
or its log must match ``log:<regex>`` (the other passes), and its per-module cell-type histogram
must equal Odin III's ``stats`` (without ``$port_*`` and constants). A row ``dev=N`` is a
documented divergence (spec 17 row N): rows 2 and 24 are checked against an adjusted Yosys side
(``$pos`` ignored; the pass, ``opt_clean``, the pass), any other row's Yosys difference is reported,
not failed. A ``pending``
row is listed and not run. The refinement check of Task 17 is not run here.

``--write`` (which needs ``--yosys``) writes the ``.il`` goldens only when every row passes every
check; on any failure nothing is written. Every child runs through ``memcap.run`` (one at a time).

Exit codes: 0 every row passed; 1 a row failed; 2 a usage error or an unusable tool; 77 skipped (an
ASan odin3 and no ``systemd-run``).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import memcap

TOOL = "opt-rules"
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_SKIP = 0, 1, 2, 77
SUBS = ("expr", "merge", "muxtree", "reduce", "share", "dff", "clean")
SOURCE_SUFFIXES = (".v", ".blif")
NO_YOSYS_ID = "-"
LOG_PREFIX = "log:"
COVER_PREFIX = "opt."
PENDING = "pending"
DEV_PREFIX = "dev="
DEFAULT_CAP = 2 << 30
TIMEOUT_SECONDS = 120.0
TAIL_LINES = 6

# Pseudo-counters: the INFO line of a sub-pass call carries the count in group 1.
INFO_COUNTERS = {
    "merge.removed": r"opt_merge: removed (\d+) cells",
    "muxtree.ports": r"opt_muxtree: removed (\d+) multiplexer ports",
    "reduce.changes": r"opt_reduce: performed (\d+) changes",
    "share.merged": r"opt_share: merged (\d+) cells",
    "dff.changed": r"opt_dff: (\d+) flip-flops changed",
    "dff.constbits": r"opt_dff: \d+ flip-flops changed, (\d+) constant bits",
    "clean.cells": r"opt_clean: removed (\d+) unused cells",
    "clean.nets": r"opt_clean: removed \d+ unused cells, (\d+) unused nets",
    "clean.wires": r"opt_clean: removed \d+ unused cells, \d+ unused nets and (\d+) unused wires",
}
STAT_LINE = re.compile(r"opt: rule (\S+) (\d+)\s*$", re.M)
ODIN_CELL = re.compile(r"stats: module (\S+): cell (\S+) (\d+)\s*$", re.M)
ODIN_MODULE = re.compile(r"stats: module (\S+): ports", re.M)
YOSYS_SECTION = re.compile(r"^=== (.+?) ===\s*$", re.M)
YOSYS_CELL = re.compile(r"^\s+(\S+)\s+(\d+)\s*$", re.M)
YOSYS_COUNT_LINE = re.compile(r"Number of cells:")
# `$sop` only appears as a constant generator (a `.names` with no inputs): it has no RTLIL form.
IGNORED_TYPES = re.compile(r"^\$(port_|_CONST|sop$)")
OPTIONS_LINE = re.compile(r"^(?://|#)\s*options:[ \t]*(.*?)[ \t]*$", re.M)
PREPASS_LINE = re.compile(r"^(?://|#)\s*prepass:[ \t]*(.*?)[ \t]*$", re.M)

# Documented divergences (spec 17) whose Yosys side is adjusted so the check still holds:
# row 2: Yosys keeps `$pos` where an identity is wiring here; row 24: Yosys reads a constant
# through a connected net only after the next opt_clean, so its converged result is the one that
# Odin III reaches in one call (the script runs the pass, opt_clean, then the pass again).
DEV_IGNORED_YOSYS_TYPES = {"2": ("$pos",)}
DEV_CONVERGED = ("24",)

Histogram = dict[str, dict[str, int]]


class UsageError(Exception):
    """A bad command line or rules file (exit 2)."""


@dataclass(frozen=True)
class Rule:
    """One row of rules.txt."""

    name: str  # "<sub>/<rule>"
    counter: str
    yosys_id: str
    status: str = ""  # "", "pending" or "dev=N"

    @property
    def sub(self) -> str:
        return self.name.split("/", 1)[0]

    @property
    def pending(self) -> bool:
        return self.status == PENDING

    @property
    def deviation(self) -> str:
        return self.status[len(DEV_PREFIX):] if self.status.startswith(DEV_PREFIX) else ""


@dataclass
class Outcome:
    """What one row's checks found: ``failures`` fail the row, ``notes`` are informational."""

    rule: Rule
    failures: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    out_il: str = ""
    skipped: bool = False

    @property
    def ok(self) -> bool:
        return not self.failures


def parse_rules(text: str) -> list[Rule]:
    """The rows of a rules.txt; ``UsageError`` on a malformed one."""
    rules: list[Rule] = []
    seen: set[str] = set()
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].split()
        if not line:
            continue
        if len(line) not in (3, 4):
            raise UsageError(f"rules line {number}: want '<sub>/<rule> <counter> <yosys id> "
                             f"[pending|dev=N]', got '{raw.strip()}'")
        name, counter, yosys_id = line[:3]
        status = line[3] if len(line) == 4 else ""
        sub = name.split("/", 1)[0]
        if "/" not in name or sub not in SUBS:
            raise UsageError(f"rules line {number}: '{name}' is not <sub>/<rule>, sub in {SUBS}")
        if status and status != PENDING and not re.fullmatch(r"dev=\d+", status):
            raise UsageError(f"rules line {number}: status '{status}' is not pending or dev=N")
        if name in seen:
            raise UsageError(f"rules line {number}: '{name}' is listed twice")
        seen.add(name)
        rules.append(Rule(name, counter, yosys_id, status))
    return rules


def source_options(source: str) -> list[str]:
    """The ``// options:`` words of a rule source."""
    found = OPTIONS_LINE.search(source)
    return found.group(1).split() if found else []


def source_prepass(source: str) -> list[str]:
    """The ``// prepass:`` commands of a rule source (``;`` separated), Odin III spelling."""
    found = PREPASS_LINE.search(source)
    return [c.strip() for c in found.group(1).split(";") if c.strip()] if found else []


def odin_script(rule: Rule, source: Path, options: Sequence[str], in_il: Path, out_il: Path,
                prepass: Sequence[str] = ()) -> str:
    """Plan point (g)'s Odin III side, ``--nolatches`` on its read. The prepass commands run
    between the read and ``write_rtlil in.il``, so Yosys reads their result."""
    if source.suffix == ".blif":
        read = [f"read_blif {source}"]
    else:
        read = [f"read_verilog --nolatches {source}", "proc --noopt"]
    sub_pass = " ".join([f"opt_{rule.sub}", *options, "--opt-stats"])
    return "; ".join([*read, *prepass, f"write_rtlil {in_il}", sub_pass, "stats",
                      f"write_rtlil {out_il}"])


def yosys_script(rule: Rule, options: Sequence[str], in_il: Path) -> str:
    """Plan point (g)'s Yosys side."""
    sub_pass = f"debug {' '.join([f'opt_{rule.sub}', *options])}"
    again = [f"opt_clean; {sub_pass}"] if rule.deviation in DEV_CONVERGED else []
    return "; ".join([f"read_rtlil {in_il}", sub_pass, *again, "stat"])


def counter_value(rule: Rule, log: str) -> int:
    """The rule's counter in Odin III's log: a ``--opt-stats`` line or an INFO pseudo-counter."""
    if rule.counter in INFO_COUNTERS:
        found = re.search(INFO_COUNTERS[rule.counter], log)
        return int(found.group(1)) if found else 0
    for name, count in STAT_LINE.findall(log):
        if name == rule.counter:
            return int(count)
    return 0


def golden_path(source: Path) -> Path:
    """The golden beside a source: ``<rule>.v`` or ``<rule>.blif`` gives ``<rule>.il``."""
    return source.with_name(source.name[:-len(source.suffix)] + ".il")


def with_suffix(path: Path, suffix: str) -> Path:
    """``path`` plus ``suffix``; rule names hold dots (``fine.group``), so not ``with_suffix``."""
    return path.with_name(path.name + suffix)


def clean_module(name: str) -> str:
    return name.lstrip("\\")


def odin_histogram(log: str) -> Histogram:
    """Per-module cell-type counts from ``stats`` lines (ports and constants left out)."""
    hist: Histogram = {clean_module(m): {} for m in ODIN_MODULE.findall(log)}
    for module, cell_type, count in ODIN_CELL.findall(log):
        if not IGNORED_TYPES.match(cell_type):
            hist.setdefault(clean_module(module), {})[cell_type] = int(count)
    return hist


def yosys_histogram(log: str, ignored: Sequence[str] = ()) -> Histogram:
    """Per-module cell-type counts from Yosys's ``stat`` sections (design hierarchy left out)."""
    hist: Histogram = {}
    marks = list(YOSYS_SECTION.finditer(log))
    for index, mark in enumerate(marks):
        name = mark.group(1)
        if name == "design hierarchy":
            continue
        end = marks[index + 1].start() if index + 1 < len(marks) else len(log)
        body = log[mark.end():end]
        cells = YOSYS_COUNT_LINE.search(body)
        counts: dict[str, int] = {}
        if cells:
            for cell_type, count in YOSYS_CELL.findall(body[cells.end():]):
                if cell_type.startswith(("$", "\\")) and cell_type not in ignored:
                    counts[cell_type] = int(count)
        hist[clean_module(name)] = counts
    return hist


def cover_counts(text: str) -> dict[str, int]:
    """A Yosys cover file: id to count (each line ends ``<count> <id>``)."""
    counts: dict[str, int] = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[-2].isdigit():
            counts[fields[-1]] = counts.get(fields[-1], 0) + int(fields[-2])
    return counts


def histogram_diff(odin: Histogram, yosys: Histogram) -> str:
    """Empty when the histograms agree, else one line naming the first difference."""
    for module in sorted(set(odin) | set(yosys)):
        mine, theirs = odin.get(module, {}), yosys.get(module, {})
        if mine != theirs:
            return (f"module '{module}': odin3 {sorted(mine.items())} "
                    f"vs yosys {sorted(theirs.items())}")
    return ""


def yosys_checks(rule: Rule, log: str, cover_text: str, odin: Histogram) -> list[str]:
    """The Yosys-side findings (the cover id or log pattern, the histogram), empty when all hold."""
    found: list[str] = []
    if rule.yosys_id.startswith(LOG_PREFIX):
        if not re.search(rule.yosys_id[len(LOG_PREFIX):], log):
            found.append(f"yosys log has no match for {rule.yosys_id}")
    elif rule.yosys_id != NO_YOSYS_ID:
        counts = cover_counts(cover_text)
        if counts.get(rule.yosys_id, 0) < 1:
            fired = sorted(i for i, n in counts.items() if n > 0 and i.startswith(COVER_PREFIX))
            found.append(f"yosys cover file has no count for {rule.yosys_id} (fired: {fired})")
    ignored = DEV_IGNORED_YOSYS_TYPES.get(rule.deviation, ())
    diff = histogram_diff(odin, yosys_histogram(log, ignored))
    if diff:
        found.append(f"cell histogram differs: {diff}")
    return found


def tail(text: str) -> str:
    return " | ".join(text.strip().splitlines()[-TAIL_LINES:])


def exe(name: str) -> str:
    return str(Path(name).resolve()) if "/" in name else name


class Runner:
    """Runs the rows of one invocation."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.cap: int = args.mem_cap
        self.work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="opt-rules-"))
        self.work.mkdir(parents=True, exist_ok=True)

    def child(self, argv: Sequence[str], env: dict[str, str] | None = None
              ) -> subprocess.CompletedProcess[str]:
        return memcap.run(list(argv), self.cap, capture_output=True, cwd=self.work, env=env,
                          timeout=TIMEOUT_SECONDS)

    def run_odin(self, rule: Rule, source: Path, options: list[str], run_dir: Path,
                 outcome: Outcome) -> str | None:
        """Odin III's side; the log, or None after a failure was recorded."""
        prepass = source_prepass(source.read_text(encoding="utf-8"))
        script = odin_script(rule, source, options, run_dir / "in.il", run_dir / "out.il", prepass)
        done = self.child([exe(self.args.odin3), "-p", script])
        log = (done.stdout or "") + (done.stderr or "")
        if done.returncode != 0:
            outcome.failures.append(f"odin3 exit {done.returncode}: {tail(log)}")
            return None
        count = counter_value(rule, log)
        if count < 1:
            outcome.failures.append(f"counter {rule.counter} is {count}, want at least 1")
        outcome.out_il = (run_dir / "out.il").read_text(encoding="utf-8")
        return log

    def run_yosys(self, rule: Rule, options: list[str], run_dir: Path, odin_log: str,
                  outcome: Outcome) -> None:
        cover = run_dir / "cover"
        cover.mkdir(exist_ok=True)
        env = {**_environment(), "YOSYS_COVER_DIR": str(cover)}
        script = yosys_script(rule, options, run_dir / "in.il")
        done = self.child([exe(self.args.yosys), "-p", script], env=env)
        log = (done.stdout or "") + (done.stderr or "")
        if done.returncode != 0:
            outcome.failures.append(f"yosys exit {done.returncode}: {tail(log)}")
            return
        cover_text = "".join(path.read_text(encoding="utf-8", errors="replace")
                             for path in sorted(cover.iterdir()))
        findings = yosys_checks(rule, log, cover_text, odin_histogram(odin_log))
        if rule.deviation and rule.deviation not in (*DEV_CONVERGED, *DEV_IGNORED_YOSYS_TYPES):
            outcome.notes.extend(f"deviation {rule.deviation}: {item}" for item in findings)
        else:
            outcome.failures.extend(findings)

    def one(self, rule: Rule, source: Path) -> Outcome:
        outcome = Outcome(rule)
        if rule.pending:
            outcome.skipped = True
            return outcome
        source = source.resolve()
        options = source_options(source.read_text(encoding="utf-8"))
        run_dir = (self.work / rule.name.replace("/", "_")).resolve()
        run_dir.mkdir(parents=True, exist_ok=True)
        try:
            log = self.run_odin(rule, source, options, run_dir, outcome)
            if log is not None and self.args.yosys:
                self.run_yosys(rule, options, run_dir, log, outcome)
        except subprocess.TimeoutExpired:
            outcome.failures.append(f"timed out after {TIMEOUT_SECONDS:.0f} s")
        golden = golden_path(source)
        if outcome.out_il and not self.args.write:
            if not golden.is_file():
                outcome.failures.append(f"no golden {golden.name}")
            elif golden.read_text(encoding="utf-8") != outcome.out_il:
                outcome.failures.append(f"out.il differs from {golden.name} ({run_dir / 'out.il'})")
        return outcome


def _environment() -> dict[str, str]:
    import os

    return dict(os.environ)


def collect(rules: Sequence[Rule], inputs: Sequence[str], rules_root: Path
            ) -> list[tuple[Rule, Path]]:
    """The rows selected by the inputs (a rule dir, a sub dir, or a source file), with their
    sources, smallest source first (pending rows, which have none, last in rules.txt order)."""
    wanted: list[Path] = [Path(item).resolve() for item in inputs]
    picked: list[tuple[Rule, Path]] = []
    for rule in rules:
        base = rules_root / rule.name
        source = next((with_suffix(base, s) for s in SOURCE_SUFFIXES
                       if with_suffix(base, s).is_file()), with_suffix(base, ".v"))
        if any(source.resolve() == w or w in source.resolve().parents for w in wanted):
            picked.append((rule, source))
    picked.sort(key=lambda item: (item[0].pending, size_of(item[1]), item[0].name))
    return picked


def size_of(path: Path) -> int:
    return path.stat().st_size if path.is_file() else 0


def orphan_sources(rules: Sequence[Rule], rules_root: Path) -> list[str]:
    """Sources under the rules root that no row lists."""
    listed = {rule.name for rule in rules}
    found: list[str] = []
    for path in sorted(rules_root.glob("*/*")):
        if path.suffix in SOURCE_SUFFIXES and f"{path.parent.name}/{path.stem}" not in listed:
            found.append(f"{path.parent.name}/{path.stem}")
    return found


def report(outcome: Outcome) -> str:
    name = outcome.rule.name
    if outcome.skipped:
        return f"{TOOL}: {name}: pending"
    if outcome.ok:
        extra = f" ({'; '.join(outcome.notes)})" if outcome.notes else ""
        return f"{TOOL}: {name}: ok{extra}"
    return f"{TOOL}: {name}: FAIL: {'; '.join(outcome.failures)}"


def run(args: argparse.Namespace) -> int:
    rules_path = Path(args.rules)
    rules_root = Path(args.root) if args.root else rules_path.parent
    rules = parse_rules(rules_path.read_text(encoding="utf-8"))
    if args.write and not args.yosys:
        raise UsageError("--write needs --yosys: a golden is never seeded without the oracle")
    selected = collect(rules, args.inputs, rules_root)
    if not selected:
        raise UsageError("no rule matches the inputs")
    runner = Runner(args)
    outcomes: list[Outcome] = []
    for rule, source in selected:
        if not rule.pending and not source.is_file():
            outcome = Outcome(rule, ["no source file"])
        else:
            outcome = runner.one(rule, source)
        outcomes.append(outcome)
        print(report(outcome), flush=True)
    for name in orphan_sources(rules, rules_root):
        print(f"{TOOL}: {name}: not listed in {rules_path.name}")
    failed = [o for o in outcomes if not o.ok]
    pending = [o for o in outcomes if o.skipped]
    print(f"{TOOL}: {len(outcomes)} rules: {len(outcomes) - len(failed) - len(pending)} ok, "
          f"{len(pending)} pending, {len(failed)} failed")
    if args.write:
        if failed:
            print(f"{TOOL}: --write refused: {len(failed)} rules failed; nothing written")
            return EXIT_FAIL
        for (_rule, source), outcome in zip(selected, outcomes, strict=True):
            if not outcome.skipped:
                golden_path(source).write_text(outcome.out_il, encoding="utf-8")
        print(f"{TOOL}: wrote {len(outcomes) - len(pending)} goldens")
    return EXIT_FAIL if failed else EXIT_OK


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    doc = __doc__ or ""
    ap = argparse.ArgumentParser(prog=TOOL, description=doc.split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", help="Yosys 0.55: cover ids, logs and histograms")
    ap.add_argument("--rules", default=str(Path(__file__).resolve().parents[2] / "tests" / "opt"
                                           / "rules" / "rules.txt"), help="the rules table")
    ap.add_argument("--root", metavar="DIR",
                    help="where <sub>/<rule>.v live (default: beside the rules table)")
    ap.add_argument("--write", action="store_true",
                    help="write the goldens, only if every row passes (needs --yosys)")
    ap.add_argument("--mem-cap", type=_size, default=DEFAULT_CAP, metavar="SIZE",
                    help="per child (default 2G)")
    ap.add_argument("--work", metavar="DIR", help="the work directory (kept)")
    ap.add_argument("inputs", nargs="+", help="rule directories, or rule source files")
    return ap.parse_args(argv)


def _size(text: str) -> int:
    units = {"k": 1 << 10, "m": 1 << 20, "g": 1 << 30}
    scale = units.get(text[-1:].lower(), 1)
    digits = text[:-1] if text[-1:].lower() in units else text
    if not digits.isdigit():
        raise argparse.ArgumentTypeError(f"bad size '{text}'")
    return int(digits) * scale


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    try:
        return run(args)
    except (OSError, UsageError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_ERROR
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP


if __name__ == "__main__":
    sys.exit(main())
