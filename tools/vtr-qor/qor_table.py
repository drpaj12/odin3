"""qor_table: the QoR table — runs, ratios, geomeans, the gate and the writers (3H spec QH-6,
§8.1-8.3, §10 E-QH13/E-QH14; plan Tasks 8 and 9).

- ``load_runs`` reads one key per leg (``--keys`` or the leg's ``latest``) from
  ``<out>/<vtr12>/<arch>/<leg>/<key12>/<d>/``: each job's ``status`` word and ``metrics.json``.
  A job whose ``status`` is ``ok`` but whose ``metrics.json`` is not an ``ok`` record of the same
  leg and design is ``incomplete``: a result is never scored from a partial attempt.
- ``metric`` is §6.1's *present*: a finite number other than VTR's ``-1`` default.
- ``ratio`` and ``geomean`` are §6.1's (a report-only ratio of 0 makes the geomean 0.0). The
  gate compares ratios and geomeans exactly, as fractions of the decimals VTR printed and
  ``bands.json`` states (``geomean <= b`` iff ``prod r <= b**n``), never rounded: CPD 3.6/2.4
  meets 1.50, a geomean of exactly 1.20 passes and 1.2001 fails; ``fmt_ratio`` prints 4
  decimals within 0.005 of a bound.
- ``gate`` applies §6.2's rules 1-7 in order and collects every violation, with §10's texts.
  The verdict is INCOMPLETE (exit 3) only when a reference job (P; O′/P′ when those legs are in
  the table) is not ``ok``, a P fcheck is ``error``/``capped``, an O fcheck/equiv/rerun did not
  run (``skipped`` or no verdict; ruling D1), the table has no O (or no P) leg, or a run is at
  a VTR commit other than the pin (point (p)); otherwise FAIL (exit 1) when any rule is
  violated, else PASS (exit 0).
- The writers (§6.3, §6.4): ``write_md`` (``qor.md``), ``write_csv`` (``runs.csv``,
  ``ratios.csv``) and ``write_json`` (``runs.json``). Every byte is a function of the runs (their
  ``metrics.json`` and ``job.json``), the committed inputs and VTR's golden files; the header's
  date and host are the newest job's ``job.json`` end time and host record, never the clock
  (T7). Rows follow §2's order with the extras after the 19; legs follow ``LEGS``, written
  ``O'``/``P'`` (point (s)). Numbers come from ``ok`` runs only.

TSV inputs (``explanations.tsv``, ``divergences.tsv``, ``equiv-exceptions.tsv``) are three
tab-separated fields per line; lines starting ``#`` and empty lines are skipped.
"""

from __future__ import annotations

import csv
import io
import json
import math
import os
import re
from collections.abc import Callable, Collection, Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from datetime import datetime
from fractions import Fraction
from pathlib import Path
from types import MappingProxyType

import qor_legs
import qor_parse
import qor_sched
from qor_heavy import XL_SLICE, HeavyError

PAIRS = (("O", "P"), ("Oprime", "P"), ("Pprime", "P"), ("O", "Oprime"), ("R", "P"),
         ("Oprov", "O"))
SECTIONS = ("netlist", "packed", "routed", "cost", "verdicts")
PASS, FAIL, INCOMPLETE = "PASS", "FAIL", "INCOMPLETE"
EXIT_TABLE = 1                      # E-QH13
NEAR_BOUND = 0.005                  # §6: 4 decimals within this of a gate bound
COARSE_DECIMALS, FINE_DECIMALS = 3, 4
BOUND_DECIMALS = 2
BANDS_SCHEMA = 1
PAIR_LEN = 2
TSV_FIELDS = 3
COMMENT = "#"
LATEST = "latest"
KEY12 = re.compile(r"[0-9a-f]{12}")
VTR12 = 12

OK = qor_sched.OK
MISSING = "missing"                 # no job (or no status word, or no verdict) in the table
INCOMPLETE_JOB = "incomplete"       # status ok, but no matching ok metrics.json
SUCCESS = "success"
# The legs whose jobs are references for the attribution rows (§6.2), when in the table.
ATTRIBUTION_REFERENCES = ("Oprime", "Pprime")
# §8.1: a divergence class that lets an O fcheck ``fail`` pass on its own.
STANDS_ALONE = frozenset({"mem-init-dropped", "async-reset-made-sync"})
BASELINE_FAILS = "baseline-fails"
FCHECK_PASS, FCHECK_FAIL = "pass", "fail"
P_SIDE_FCHECK = frozenset({"error", "capped", "timeout"})
# PHASE3 #16: on a design outside EQUIV_REQUIRED a capped or timed-out fcheck is informational.
FCHECK_INFO = frozenset({"capped", "timeout"})
EQUIV_OK = frozenset({"equivalent", "ram"})
EQUIV_DIFFERS = "differs"
EQUIV_INCOMPLETE = frozenset({"capped"})  # ABC killed by the memory cap: unmeasured, not failed
RERUN_SAME = "same"
# Ruling D1: a check that did not run is unmeasured (INCOMPLETE), not failed.
NOT_RUN = frozenset({"skipped", MISSING})


@dataclass(frozen=True)
class Run:
    """One job of the table: its leg, design and arch basename, the leg key it was read from,
    its status word, whether it is a gate design, its ``metrics.json`` object (``{}`` when
    there is none) and its ``job.json`` object (``{}`` when there is none; the writers read the
    end time, the host, the slice and ``cost_warnings`` from it)."""

    leg: str
    design: str
    arch: str
    key12: str
    status: str
    gating: bool
    metrics: Mapping[str, object]
    job: Mapping[str, object] = field(default_factory=dict)


# ---------------------------------------------------------------- loading


def _no_key(leg: str, arch: str) -> HeavyError:
    return HeavyError(EXIT_TABLE, f"table: no complete key for leg {leg} on {arch}")


def _read_word(path: Path) -> str | None:
    try:
        if path.is_symlink() or not path.is_file():
            return None
        return path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None


def _job_dirs(key_dir: Path) -> list[Path]:
    """The job directories of a leg key, sorted; symlinks are never followed (skipped)."""
    try:
        entries = sorted(key_dir.iterdir())
    except OSError:
        return []
    return [e for e in entries if _real_dir(e)]


def _key_dir(leg_dir: Path, leg: str, arch: str, given: str | None) -> Path:
    """The leg key's directory: ``given`` (``--keys``) or the one ``latest`` names. E-QH13 when
    there is none, it is not a key12, or it holds no job."""
    key12 = given if given is not None else _read_word(leg_dir / LATEST)
    if key12 is None or not KEY12.fullmatch(key12) or leg_dir.is_symlink():
        raise _no_key(leg, arch)
    key_dir = leg_dir / key12
    if key_dir.is_symlink() or not _job_dirs(key_dir):
        raise _no_key(leg, arch)
    return key_dir


def _read_metrics(job_dir: Path, leg: str, design: str,
                  name: str = "metrics.json") -> dict[str, object]:
    """``metrics.json`` (or ``job.json``) when it is a record of this leg and design, else
    ``{}``."""
    path = job_dir / name
    if path.is_symlink():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if not isinstance(data, dict) or data.get("leg") != leg or data.get("design") != design:
        return {}
    return data


def _load_job(job_dir: Path, leg: str, arch: str, gate: Collection[str]) -> Run:
    design = job_dir.name
    metrics = _read_metrics(job_dir, leg, design)
    status = _read_word(job_dir / "status") or MISSING
    if status == OK and metrics.get("status") != OK:
        status = INCOMPLETE_JOB
    gating = metrics.get("gating")
    return Run(leg=leg, design=design, arch=arch, key12=job_dir.parent.name, status=status,
               gating=gating if isinstance(gating, bool) else design in gate, metrics=metrics,
               job=_read_metrics(job_dir, leg, design, "job.json"))


def _legs_to_load(arch_dir: Path, keys: Mapping[str, str],
                  legs: Sequence[str] | None) -> tuple[str, ...]:
    if legs is not None:
        return tuple(dict.fromkeys(qor_legs.normalise_leg(leg) for leg in legs))
    return tuple(leg for leg in qor_legs.LEGS
                 if leg in keys or _read_word(arch_dir / leg / LATEST) is not None)


def _real_dir(path: Path) -> bool:
    return not path.is_symlink() and path.is_dir()


def sort_runs(runs: Iterable[Run], order: Sequence[str]) -> list[Run]:
    """§6's row order: §2's design order (unknown designs last, by name), legs as ``LEGS``."""
    rank = {design: i for i, design in enumerate(order)}
    legs = {leg: i for i, leg in enumerate(qor_legs.LEGS)}
    return sorted(runs, key=lambda r: (rank.get(r.design, len(rank)), r.design,
                                       legs.get(r.leg, len(legs)), r.leg))


def load_runs(out: Path, vtr12: str, arch: str, keys: Mapping[str, str], *,
              legs: Sequence[str] | None = None) -> list[Run]:
    """Every job of one key per leg under ``<out>/<vtr12>/<arch>``. ``keys`` maps leg →
    ``key12`` (``O'``/``P'`` accepted); a leg not given reads its ``latest``. ``legs`` names the
    legs to load; by default, every leg in ``keys`` or with a ``latest`` file (a leg whose
    ``latest`` is retracted mid-rerun is left out rather than refusing the table). E-QH13
    (exit 1): ``table: no complete key for leg <L> on <arch>`` for a leg to load. The runs follow
    ``sort_runs``."""
    given = {qor_legs.normalise_leg(leg): key12 for leg, key12 in keys.items()}
    arch_dir = out / vtr12 / arch
    sets = qor_sched.load_sets()
    runs: list[Run] = []
    for leg in _legs_to_load(arch_dir, given, legs):
        key_dir = _key_dir(arch_dir / leg, leg, arch, given.get(leg))
        runs.extend(_load_job(job_dir, leg, arch, sets.gate) for job_dir in _job_dirs(key_dir))
    return sort_runs(runs, sets.order)


# ---------------------------------------------------------------- metrics, ratios, geomeans


def _present(value: object) -> float | None:
    """§6.1's *present* for a ``metrics.json`` value: ``qor_parse.present``'s rule (finite, not
    VTR's ``-1``) applied to a JSON number rather than to parsed text; bools are not numbers."""
    if isinstance(value, bool) or not isinstance(value, int | float):
        return None
    number = float(value)
    if not math.isfinite(number) or number == float(qor_parse.ABSENT):
        return None
    return number


def _lookup(run: Run, name: str) -> object:
    for section in SECTIONS:
        block = run.metrics.get(section)
        if isinstance(block, Mapping) and name in block:
            return block[name]
    return None


def metric(run: Run, name: str) -> float | None:
    """The metric ``name`` from the run's ``netlist``, ``packed``, ``routed``, ``cost`` or
    ``verdicts`` section when it is present (§6.1: a finite number, not ``-1``), else None."""
    return _present(_lookup(run, name))


def text_field(run: Run, name: str) -> str:
    """A string field (``vpr_status``, a verdict) from the same sections; ``missing`` else."""
    value = _lookup(run, name)
    return value if isinstance(value, str) and value else MISSING


def ratio(x: float | None, y: float | None, gated: bool) -> float | None:
    """§6.1: gated, defined only when both are present and > 0; report-only, when both are
    present, x >= 0 and y > 0, and 1.0 when both are 0. Otherwise None (undefined)."""
    if x is None or y is None:
        return None
    if gated:
        return x / y if x > 0 and y > 0 else None
    if x == 0 and y == 0:
        return 1.0
    return x / y if x >= 0 and y > 0 else None


def geomean(values: Sequence[float]) -> float | None:
    """``exp(mean(ln r))`` over the values; None when there are none. A report-only ratio of 0
    (§6.1: X = 0, Y > 0) makes it 0.0, the limit of that formula; ratios are never negative."""
    if not values:
        return None
    if any(v == 0 for v in values):
        return 0.0
    return math.exp(math.fsum(math.log(v) for v in values) / len(values))


def geomean_n(values: Sequence[float | None]) -> tuple[float | None, int]:
    """The geomean over the defined values and their count ``n`` (printed ``(n/|S|)``)."""
    defined = [v for v in values if v is not None]
    return geomean(defined), len(defined)


def index_runs(runs: Iterable[Run]) -> dict[tuple[str, str], Run]:
    """``(leg, design)`` → run (the last one wins)."""
    return {(r.leg, r.design): r for r in runs}


def pair_ratios(runs: Sequence[Run], pair: tuple[str, str], name: str,
                designs: Sequence[str], *, gated: bool) -> list[float | None]:
    """The ratio of each design (in ``designs`` order) for the pair X/Y and metric ``name``,
    from ``ok`` runs only; None where either run is missing, not ok, or the ratio undefined."""
    index = index_runs(r for r in runs if r.status == OK)
    out: list[float | None] = []
    for design in designs:
        x, y = index.get((pair[0], design)), index.get((pair[1], design))
        out.append(None if x is None or y is None
                   else ratio(metric(x, name), metric(y, name), gated))
    return out


def fmt_ratio(value: float, bounds: Sequence[float]) -> str:
    """3 decimals, or 4 when the value lies within 0.005 of any bound (§6)."""
    near = any(abs(value - bound) < NEAR_BOUND for bound in bounds)
    return f"{value:.{FINE_DECIMALS if near else COARSE_DECIMALS}f}"


# ---------------------------------------------------------------- inputs


@dataclass(frozen=True)
class Bands:
    """``tests/vtr/qor/bands.json`` (§6.2)."""

    pair: tuple[str, str]
    set: str
    metrics: tuple[str, ...]
    geomean_max: float
    per_design_max: float
    equal_or_explained: tuple[str, ...]


def _strings(data: Mapping[str, object], name: str, path: Path) -> tuple[str, ...]:
    value = data.get(name)
    if not isinstance(value, list) or not value \
            or not all(isinstance(v, str) and v for v in value):
        raise ValueError(f"{path}: '{name}' is not a list of names")
    return tuple(value)


def _bound(data: Mapping[str, object], name: str, path: Path) -> float:
    value = data.get(name)
    if isinstance(value, bool) or not isinstance(value, int | float) \
            or not math.isfinite(value) or value <= 0:
        raise ValueError(f"{path}: '{name}' is not a positive number")
    return float(value)


def load_bands(path: Path) -> Bands:
    """Read and check ``bands.json``; ValueError naming the file on any defect."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ValueError(f"{path}: cannot read: {exc}") from exc
    if not isinstance(data, dict) or data.get("schema") != BANDS_SCHEMA \
            or isinstance(data.get("schema"), bool):
        raise ValueError(f"{path}: not a schema {BANDS_SCHEMA} bands file")
    pair = _strings(data, "pair", path)
    if len(pair) != PAIR_LEN:
        raise ValueError(f"{path}: 'pair' is not two legs")
    set_name = data.get("set")
    if not isinstance(set_name, str) or not set_name:
        raise ValueError(f"{path}: 'set' is not a set name")
    return Bands(pair=(pair[0], pair[1]), set=set_name, metrics=_strings(data, "metrics", path),
                 geomean_max=_bound(data, "geomean_max", path),
                 per_design_max=_bound(data, "per_design_max", path),
                 equal_or_explained=_strings(data, "equal_or_explained", path))


def load_tsv(path: Path) -> list[tuple[str, str, str]]:
    """The rows of a ``<design>\\t<key>\\t<text>`` file: lines starting ``#`` and empty lines
    skipped; any other line must be exactly three non-empty tab-separated fields, else
    ``ValueError("<path>:<line>: expected 3 tab-separated fields")``."""
    rows: list[tuple[str, str, str]] = []
    text = path.read_text(encoding="utf-8")
    for number, line in enumerate(text.splitlines(), start=1):
        if not line or line.startswith(COMMENT):
            continue
        fields = line.split("\t")
        if len(fields) != TSV_FIELDS or not all(f.strip() for f in fields):
            raise ValueError(f"{path.absolute()}:{number}: expected 3 tab-separated fields")
        rows.append((fields[0].strip(), fields[1].strip(), fields[2].strip()))
    return rows


# ---------------------------------------------------------------- the gate


@dataclass(frozen=True)
class Verdict:
    """``state`` is ``PASS``, ``FAIL`` or ``INCOMPLETE``; ``violations`` in rule order."""

    state: str
    violations: tuple[str, ...]
    notes: tuple[str, ...] = ()


@dataclass(frozen=True)
class GateInputs:
    runs: Sequence[Run]
    bands: Bands
    gate_set: Sequence[str]
    explanations: Sequence[tuple[str, str, str]]
    divergences: Sequence[tuple[str, str, str]]
    equiv_exceptions: Sequence[tuple[str, str, str]]


@dataclass
class _Collect:
    lines: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    incomplete: bool = False

    def note(self, text: str) -> None:
        """Informational: neither a violation nor a hold (PHASE3 #16)."""
        self.notes.append(text)

    def fail(self, text: str) -> None:
        self.lines.append(text)

    def hold(self, text: str) -> None:
        self.lines.append(text)
        self.incomplete = True

    def verdict(self) -> Verdict:
        state = INCOMPLETE if self.incomplete else FAIL if self.lines else PASS
        return Verdict(state=state, violations=tuple(self.lines), notes=tuple(self.notes))


@dataclass(frozen=True)
class _Ctx:
    inp: GateInputs
    index: Mapping[tuple[str, str], Run]
    x: str
    y: str
    bounds: tuple[float, float]


def _check_drift(runs: Sequence[Run], out: _Collect) -> None:
    """Point (p): a run at a VTR commit other than the pin makes the table report-only."""
    commits = set()
    for run in runs:
        commit = run.metrics.get("vtr_commit")
        if isinstance(commit, str) and commit and commit != qor_legs.VTR_PIN:
            commits.add(commit)
    for commit in sorted(commits):
        out.hold(f"gate: INCOMPLETE: VTR at {commit[:VTR12]} is not the pin (report-only run)")


def _check_references(ctx: _Ctx, out: _Collect) -> None:
    legs_in = {r.leg for r in ctx.inp.runs}
    for leg in (ctx.y, *ATTRIBUTION_REFERENCES):
        if leg != ctx.y and (leg not in legs_in or leg == ctx.x):
            continue
        for design in ctx.inp.gate_set:
            run = ctx.index.get((leg, design))
            status = run.status if run is not None else MISSING
            if status != OK:
                out.hold(f"gate: INCOMPLETE: {design} {leg} is {status} (reference); rerun it")


def _rule1_status(ctx: _Ctx, out: _Collect) -> list[str]:
    """Rule 1: every gate design's O job ok with ``vpr_status=success``. Returns those designs."""
    usable = []
    for design in ctx.inp.gate_set:
        run = ctx.index.get((ctx.x, design))
        vpr = text_field(run, "vpr_status") if run is not None else MISSING
        if run is not None and run.status == OK and vpr == SUCCESS:
            usable.append(design)
        elif run is not None and (run.status == OK or vpr not in (MISSING, SUCCESS)):
            out.fail(f"gate: FAIL: {design} {ctx.x} vpr_status={vpr}")
        else:
            out.fail(f"table: {design} has no ok {ctx.x} job")
    return usable


def _rule2_present(ctx: _Ctx, compared: Sequence[str], out: _Collect) -> None:
    for name in ctx.inp.bands.metrics:
        for design in compared:
            for leg in (ctx.x, ctx.y):
                value = metric(ctx.index[(leg, design)], name)
                if value is None or value <= 0:
                    out.fail(f"gate: FAIL: {name} missing for {design} ({leg})")


def _decimal(value: float) -> Fraction:
    """A value or bound as written (``1.20`` is exactly 6/5, CPD ``3.6`` exactly 18/5), not its
    binary approximation: a JSON float's ``repr`` is its shortest decimal, which is the number
    VTR printed (or ``bands.json`` states), so a ratio exactly on a bound meets it."""
    return Fraction(repr(value))


def _rule3_bands(ctx: _Ctx, compared: Sequence[str], out: _Collect) -> None:
    bands = ctx.inp.bands
    gmax, dmax = _decimal(bands.geomean_max), _decimal(bands.per_design_max)
    pair = f"{ctx.x}/{ctx.y}"
    for name in bands.metrics:
        exact: list[tuple[str, Fraction]] = []
        for design in compared:
            x = metric(ctx.index[(ctx.x, design)], name)
            y = metric(ctx.index[(ctx.y, design)], name)
            if x is not None and y is not None and x > 0 and y > 0:
                exact.append((design, _decimal(x) / _decimal(y)))
        product = math.prod((r for _, r in exact), start=Fraction(1))
        value = geomean([float(r) for _, r in exact])
        if value is not None and product > gmax ** len(exact):
            out.fail(f"gate: FAIL: {name} geomean {pair} {fmt_ratio(value, ctx.bounds)} > "
                     f"{bands.geomean_max:.{BOUND_DECIMALS}f}")
        for design, r in exact:
            if r > dmax:
                out.fail(f"gate: FAIL: {name} {design} {pair} {fmt_ratio(float(r), ctx.bounds)}"
                         f" > {bands.per_design_max:.{BOUND_DECIMALS}f}")


def _count(value: float) -> str:
    return str(int(value)) if value.is_integer() else f"{value:g}"


def _rule4_equal(ctx: _Ctx, compared: Sequence[str], out: _Collect) -> None:
    """Rule 4: O = P or a row in explanations.tsv. A missing value is rule 2's text (review M5):
    two missing counts are not "O != P", and no row explains a value VTR did not print."""
    explained = {(row[0], row[1]) for row in ctx.inp.explanations}
    for name in ctx.inp.bands.equal_or_explained:
        for design in compared:
            a = metric(ctx.index[(ctx.x, design)], name)
            b = metric(ctx.index[(ctx.y, design)], name)
            if a is None or b is None:
                for leg, value in ((ctx.x, a), (ctx.y, b)):
                    if value is None:
                        out.fail(f"gate: FAIL: {name} missing for {design} ({leg})")
            elif a != b and (design, name) not in explained:
                out.fail(f"gate: FAIL: {name} {design} {ctx.x} {_count(a)} != {ctx.y} "
                         f"{_count(b)} (no row in explanations.tsv)")


def _not_run(ctx: _Ctx, design: str, check: str, verdict: str, out: _Collect) -> None:
    """Ruling D1 (Task 8 review): a check not run (``skipped``, or no verdict) is unmeasured,
    so INCOMPLETE, never FAIL."""
    out.hold(f"gate: INCOMPLETE: {design} {ctx.x} {check} {verdict} (not run); run it")


def _divergence_applies(ctx: _Ctx, design: str, reference: str) -> bool:
    for row_design, cls, _ in ctx.inp.divergences:
        if row_design != design:
            continue
        if cls in STANDS_ALONE or (cls == BASELINE_FAILS and reference == FCHECK_FAIL):
            return True
    return False


def _fcheck_fail(ctx: _Ctx, design: str, out: _Collect) -> None:
    out.fail(f"gate: FAIL: {design} {ctx.x} fcheck fail (no row in divergences.tsv)")


def _rule5_fcheck(ctx: _Ctx, usable: Sequence[str], out: _Collect) -> None:
    declared = {row[0] for row in ctx.inp.divergences}
    for design in usable:
        mine = text_field(ctx.index[(ctx.x, design)], "fcheck")
        ref_run = ctx.index.get((ctx.y, design))
        reference = text_field(ref_run, "fcheck") if ref_run is not None else MISSING
        if design not in qor_sched.EQUIV_REQUIRED and mine != FCHECK_FAIL and (
                mine in FCHECK_INFO or reference in FCHECK_INFO):
            out.note(f"gate: note: {design} fcheck {mine} (reference {reference}); informational "
                     "on a design outside PHASE3 #16's small set")
        elif reference in P_SIDE_FCHECK:
            out.hold(f"gate: INCOMPLETE: {design} {ctx.y} fcheck is {reference} (reference); "
                     "rerun it")
            if mine == FCHECK_FAIL and design not in declared:  # review M1: Odin III's own
                _fcheck_fail(ctx, design, out)
        elif mine in NOT_RUN:
            _not_run(ctx, design, "fcheck", mine, out)
        elif mine == FCHECK_FAIL and not _divergence_applies(ctx, design, reference):
            _fcheck_fail(ctx, design, out)
        elif mine not in (FCHECK_PASS, FCHECK_FAIL):
            out.fail(f"gate: FAIL: {design} {ctx.x} fcheck {mine}")


def _rule6_equiv(ctx: _Ctx, usable: Sequence[str], out: _Collect) -> None:
    """Rule 6, narrowed by PHASE3 #16: only a design in ``EQUIV_REQUIRED`` is held to
    equivalence. Any other design's verdict is a note, except ``differs``, which still fails."""
    excepted = {row[0] for row in ctx.inp.equiv_exceptions}
    for design in usable:
        verdict = text_field(ctx.index[(ctx.x, design)], "equiv")
        if verdict == EQUIV_DIFFERS:
            out.fail(f"gate: FAIL: {design} equiv differs")
        elif design not in qor_sched.EQUIV_REQUIRED:
            if verdict not in NOT_RUN and verdict not in EQUIV_OK:  # the table cell says the rest
                out.note(f"gate: note: {design} equiv {verdict}; {qor_sched.EQUIV_NOT_REQUIRED}")
        elif verdict in EQUIV_OK or design in excepted:
            continue
        elif verdict in EQUIV_INCOMPLETE:
            out.hold(f"gate: INCOMPLETE: {design} {ctx.x} equiv {verdict}; rerun it")
        elif verdict in NOT_RUN:
            _not_run(ctx, design, "equiv", verdict, out)
        else:
            out.fail(f"gate: FAIL: {design} equiv {verdict} (not in equiv-exceptions.tsv)")


def _rule7_rerun(ctx: _Ctx, usable: Sequence[str], out: _Collect) -> None:
    for design in usable:
        verdict = text_field(ctx.index[(ctx.x, design)], "rerun")
        if verdict in NOT_RUN:
            _not_run(ctx, design, "rerun", verdict, out)
        elif verdict != RERUN_SAME:
            out.fail(f"gate: FAIL: {design} rerun {verdict}")


def gate(inp: GateInputs) -> Verdict:
    """§6.2's verdict over the gate set, rules 1-7 in order, every violation collected."""
    x, y = inp.bands.pair
    ctx = _Ctx(inp=inp, index=index_runs(inp.runs), x=x, y=y,
               bounds=(inp.bands.geomean_max, inp.bands.per_design_max))
    out = _Collect()
    _check_drift(inp.runs, out)
    legs_in = {r.leg for r in inp.runs}
    for leg in (x, y):
        if leg not in legs_in:
            out.hold(f"gate: INCOMPLETE: no {leg} leg in the table")
    if not legs_in >= {x, y}:
        return out.verdict()
    _check_references(ctx, out)
    usable = _rule1_status(ctx, out)
    compared = [d for d in usable
                if (ref := ctx.index.get((y, d))) is not None and ref.status == OK]
    _rule2_present(ctx, compared, out)
    _rule3_bands(ctx, compared, out)
    _rule4_equal(ctx, compared, out)
    _rule5_fcheck(ctx, usable, out)
    _rule6_equiv(ctx, usable, out)
    _rule7_rerun(ctx, usable, out)
    return out.verdict()


# ---------------------------------------------------------------- the writers (§6.3, §6.4)

REPO_INPUTS = qor_legs.REPO_ROOT / "tests" / "vtr" / "qor"
BANDS_JSON = REPO_INPUTS / "bands.json"
TABLE_FILES = ("qor.md", "runs.csv", "ratios.csv", "runs.json")
TABLE_SCHEMA = 1
SHORT = 12
DIRTY = "-dirty"
UNKNOWN = "unknown"
STAMP_UNKNOWN = "00000000-0000"
ISO_UTC = "%Y-%m-%dT%H:%M:%SZ"
STAMP = "%Y%m%d-%H%M"
GEOMEAN_GATE, GEOMEAN_EXTRA = "GEOMEAN_GATE", "GEOMEAN_EXTRA"
DISPLAY_LEGS = {"Oprime": "O'", "Pprime": "P'"}
# The legs whose ABC exchange is named (point (e)): their depth comes from the ``# abc:`` header.
EXCHANGE_LEGS = ("Pprime", "O", "Oprov")
TICK, CROSS, OPEN = "✓", "✗", "?"
NOT_GATED = "not gated"
DECISION3 = "Gate (decision 3)"
# Agent default (Task 9 fix round 1): EArch tables gate over the smoke set; never decision 3's.
EARCH_ARCH = "EArch"
EARCH_SMOKE_GATE = "Gate (EArch smoke; agent default, not decision 3)"


def gate_title(arch: str) -> str:
    """The gate section's and ``vtr-qor gate``'s title: decision 3's only on k6."""
    return EARCH_SMOKE_GATE if arch == EARCH_ARCH else DECISION3


def narrowed_reason(n: int) -> str:
    """Ruling N1: a k6 table without all 19 gate designs is not gated (and INCOMPLETE)."""
    return f"decision 3 needs the 19 (this table has {n})"


def narrowed_verdict(n: int) -> Verdict:
    return Verdict(state=INCOMPLETE, violations=(f"gate: INCOMPLETE: {narrowed_reason(n)}",))
RUNS_HEADER = (
    "arch", "design", "leg", "key12", "status", "gating", "vpr_status", "luts", "ffs", "adders",
    "mults", "ram_slices", "depth", "abc_depth", "num_clb", "num_memories", "num_mult",
    "device_width", "device_height", "min_chan_width", "critical_path_delay_ns", "fmax_mhz",
    "crit_path_routed_wirelength", "frontend_wall_s", "frontend_peak_rss_mib", "vpr_time_s",
    "vpr_total_runtime_s", "max_vpr_mem_mib", "fcheck", "equiv", "rerun", "blif_sha256",
    "vtr_commit", "odin3_commit")
RATIO_METRICS = (
    "luts", "ffs", "adders", "mults", "ram_slices", "depth", "num_clb", "num_memories",
    "num_mult", "min_chan_width", "critical_path_delay_ns", "crit_path_routed_wirelength",
    "frontend_wall_s", "frontend_peak_rss_mib", "vpr_time_s", "max_vpr_mem_mib")
RATIOS_HEADER = ("arch", "pair", "design", "n", *RATIO_METRICS)
RUN_TEXT_FIELDS = frozenset({"vpr_status", "fcheck", "equiv", "rerun", "blif_sha256"})
# §6: CPD 3 decimals, Fmax and times 1, memory 0; every other number is a count, printed as is.
DECIMALS = MappingProxyType({
    "critical_path_delay_ns": 3, "fmax_mhz": 1, "frontend_wall_s": 1, "vpr_time_s": 1,
    "vpr_total_runtime_s": 1, "frontend_peak_rss_mib": 0, "max_vpr_mem_mib": 0})
NETLIST_COLUMNS = ("luts", "ffs", "adders", "mults", "ram_slices", "depth")
COST_COLUMNS = ("frontend_wall_s", "frontend_peak_rss_mib", "vpr_time_s", "max_vpr_mem_mib")
FRONTEND_COST = frozenset({"frontend_wall_s", "frontend_peak_rss_mib"})
GATE_SHORT = MappingProxyType({"num_clb": "CLB", "min_chan_width": "W",
                               "critical_path_delay_ns": "CPD",
                               "crit_path_routed_wirelength": "WL"})
ATTRIBUTION = ((("Oprime", "P"), "front end"), (("Pprime", "P"), "ABC"), (("O", "Oprime"), "ABC"),
               (("O", "P"), "total"))
# The cross-check (§12 item 5): our metric, VTR's golden column.
XCHECK = (("num_clb", "num_clb"), ("min_chan_width", "min_chan_width"),
          ("critical_path_delay_ns", "critical_path_delay"),
          ("crit_path_routed_wirelength", "crit_path_routed_wirelength"))
XCHECK_TASK = (Path("vtr_flow") / "tasks" / "regression_tests" / "vtr_reg_nightly_test3"
               / "vtr_reg_qor_chain" / "config")
PASS_REQUIREMENTS_DIR = Path("vtr_flow") / "parse" / "pass_requirements"
PASS_REQUIREMENTS_KEY = "pass_requirements_file="
INCLUDE = "%include"
RANGE = re.compile(r"Range\(\s*([^,()]+)\s*,\s*([^,()]+)\s*\)")
MAX_INCLUDE_DEPTH = 8
COST_CAVEAT = (
    "`*`: a stage log had no `time -v` figures (job.json `cost_warnings`), so the front-end "
    "cost is incomplete. `xl`: the job ran in the 9 GiB `odin3-heavyxl.slice` (plan point (d)). "
    "Comparability (spec §18 item 2): P's front end is two VTR stages, Parmys and ABC, timed by "
    "VTR's own `time -v`, and VTR's ABC stage also pays a BLIF write and read per clock domain; "
    "an Odin III leg's front end is one `odin3` process timed by the harness (plus VTR's ABC "
    "stage for O' and R). Both are wall time and peak RSS, but not measured the same way.")


def leg_name(leg: str) -> str:
    """Point (s): ``O'`` and ``P'`` in CSV and Markdown; the other legs as they are."""
    return DISPLAY_LEGS.get(leg, leg)


def pair_name(pair: tuple[str, str]) -> str:
    return f"{leg_name(pair[0])}/{leg_name(pair[1])}"


def fmt_value(name: str, value: float | None) -> str:
    """§6's precision for one value: CPD 3 decimals, Fmax and times 1, memory 0, counts as
    they are; ``""`` when it is not available."""
    if value is None:
        return ""
    places = DECIMALS.get(name)
    return _count(value) if places is None else f"{value:.{places}f}"


def _fmt_r(value: float | None, bounds: Sequence[float]) -> str:
    return "" if value is None else fmt_ratio(value, bounds)


# ---------------------------------------------------------------- the header


@dataclass(frozen=True)
class TableHeader:
    """The table's provenance (§6.3): ``host`` and ``date_utc`` are the newest job's
    ``job.json`` host record and end time, never the clock."""

    arch: str
    label: str
    vtr12: str
    odin3_commit12: str | None
    build12: str | None
    abc_sha12: str | None
    legs: Mapping[str, str]
    host: str
    date_utc: str


def _short(value: object) -> str | None:
    """The first 12 characters of a commit or SHA-256, keeping a ``-dirty`` mark."""
    if not isinstance(value, str) or not value:
        return None
    return value[:SHORT] + (DIRTY if value.endswith(DIRTY) else "")


def _first(runs: Sequence[Run], name: str) -> str | None:
    for run in runs:
        found = _short(run.metrics.get(name))
        if found is not None:
            return found
    return None


def _host(job: Mapping[str, object]) -> str:
    host = job.get("host")
    if not isinstance(host, Mapping):
        return UNKNOWN
    cpu, mem = host.get("cpu"), host.get("mem_gib")
    parts = [cpu if isinstance(cpu, str) and cpu else UNKNOWN]
    if isinstance(mem, int | float) and not isinstance(mem, bool):
        parts.append(f"{mem:g} GiB")
    return ", ".join(parts)


def _newest(runs: Sequence[Run]) -> tuple[str, str]:
    """The newest job end time (``job.json`` ``ended``, else ``metrics.json`` ``started``)
    and that job's host; ties broken by design and leg, so the result is a function of the
    files alone."""
    best: tuple[str, str, str] | None = None
    host = UNKNOWN
    for run in runs:
        when = run.job.get("ended") or run.metrics.get("started")
        if not isinstance(when, str) or not when:
            continue
        key = (when, run.design, run.leg)
        if best is None or key > best:
            best, host = key, _host(run.job)
    return (UNKNOWN, UNKNOWN) if best is None else (best[0], host)


def table_header(runs: Sequence[Run], *, arch: str, label: str, vtr12: str) -> TableHeader:
    """The header of a table over ``runs``: the leg keys, the odin3 commit, build and ABC
    script of the first run that has one (legs in ``LEGS`` order), and the newest job's date
    and host."""
    ordered = sorted(runs, key=lambda r: (_leg_rank(r.leg), r.design))
    legs = {run.leg: run.key12 for run in reversed(ordered)}
    date, host = _newest(runs)
    return TableHeader(arch=arch, label=label, vtr12=vtr12,
                       odin3_commit12=_first(ordered, "odin3_commit"),
                       build12=_first(ordered, "odin3_build"),
                       abc_sha12=_first(ordered, "abc_script_sha256"),
                       legs={leg: legs[leg] for leg in sorted(legs, key=_leg_rank)},
                       host=host, date_utc=date)


def table_stamp(header: TableHeader) -> str:
    """``YYYYMMDD-HHMM`` of the header date (the table directory's prefix)."""
    try:
        return datetime.strptime(header.date_utc, ISO_UTC).strftime(STAMP)
    except ValueError:
        return STAMP_UNKNOWN


def _leg_rank(leg: str) -> int:
    return qor_legs.LEGS.index(leg) if leg in qor_legs.LEGS else len(qor_legs.LEGS)


# ---------------------------------------------------------------- the layout


@dataclass(frozen=True)
class _Layout:
    """What every writer needs: the designs in row order (the 19, then the extras, then any
    other), the legs in the table, the runs by (leg, design), the gate and extra sets, the
    bands and their bounds."""

    runs: Sequence[Run]
    designs: tuple[str, ...]
    legs: tuple[str, ...]
    index: Mapping[tuple[str, str], Run]
    gate: tuple[str, ...]
    extra: tuple[str, ...]
    bands: Bands
    bounds: tuple[float, float]
    arch: str

    def ok(self, leg: str, design: str) -> Run | None:
        run = self.index.get((leg, design))
        return run if run is not None and run.status == OK else None

    def value(self, leg: str, design: str, name: str) -> float | None:
        run = self.ok(leg, design)
        return None if run is None else metric(run, name)

    def pairs(self) -> list[tuple[str, str]]:
        return [pair for pair in PAIRS if pair[0] in self.legs and pair[1] in self.legs]

    def ratio(self, pair: tuple[str, str], design: str, name: str) -> float | None:
        gated = name in self.bands.metrics
        return ratio(self.value(pair[0], design, name), self.value(pair[1], design, name), gated)

    def geomean(self, pair: tuple[str, str], name: str,
                designs: Sequence[str]) -> tuple[float | None, int]:
        return geomean_n([self.ratio(pair, design, name) for design in designs])


def _layout(runs: Sequence[Run], bands: Bands | None,
            gate_set: Sequence[str] | None = None) -> _Layout:
    sets = qor_sched.load_sets()
    bands = bands if bands is not None else load_bands(BANDS_JSON)
    rank = {design: i for i, design in enumerate((*sets.gate, *sets.extra))}
    designs = sorted({r.design for r in runs}, key=lambda d: (rank.get(d, len(rank)), d))
    ordered = sorted(runs, key=lambda r: (rank.get(r.design, len(rank)), r.design,
                                          _leg_rank(r.leg), r.leg))
    legs = sorted({r.leg for r in runs}, key=lambda leg: (_leg_rank(leg), leg))
    return _Layout(runs=ordered, designs=tuple(designs), legs=tuple(legs),
                   index=index_runs(ordered),
                   gate=sets.gate if gate_set is None else tuple(gate_set),
                   extra=tuple(d for d in sets.extra if d in designs), bands=bands,
                   bounds=(bands.geomean_max, bands.per_design_max),
                   arch=ordered[0].arch if ordered else "")


def _write_text(path: Path, text: str) -> None:
    """Write by atomic rename (no partial table file is ever left behind)."""
    if path.is_symlink():
        raise OSError(f"refusing to write {path}: it is a symlink")
    tmp = path.with_name(f".{path.name}.tmp")
    tmp.write_text(text, encoding="utf-8", newline="")
    os.replace(tmp, path)


# ---------------------------------------------------------------- CSV (§6.4)


def _csv(rows: Iterable[Sequence[str]]) -> str:
    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerows(rows)
    return buffer.getvalue()


def _run_cells(layout: _Layout, run: Run) -> list[str]:
    ok = run.status == OK
    cells = []
    for name in RUNS_HEADER:
        if name in ("arch", "design", "key12", "status"):
            cells.append(str(getattr(run, name)))
        elif name == "leg":
            cells.append(leg_name(run.leg))
        elif name == "gating":
            cells.append("true" if run.gating else "false")
        elif name in ("vtr_commit", "odin3_commit"):
            value = run.metrics.get(name)
            cells.append(value if isinstance(value, str) else "")
        elif name in RUN_TEXT_FIELDS:
            text = text_field(run, name) if ok else MISSING
            cells.append("" if text == MISSING else text)
        else:
            cells.append(fmt_value(name, metric(run, name) if ok else None))
    return cells


def _ratio_rows(layout: _Layout, pair: tuple[str, str], arch: str) -> list[list[str]]:
    name = pair_name(pair)
    rows = []
    for design in layout.designs:
        if (pair[0], design) in layout.index or (pair[1], design) in layout.index:
            rows.append([arch, name, design, "1",
                         *(_fmt_r(layout.ratio(pair, design, m), layout.bounds)
                           for m in RATIO_METRICS)])
    for label, designs in ((GEOMEAN_GATE, layout.gate), (GEOMEAN_EXTRA, layout.extra)):
        both = sum(1 for d in designs if layout.ok(pair[0], d) and layout.ok(pair[1], d))
        rows.append([arch, name, label, str(both),
                     *(_fmt_r(layout.geomean(pair, m, designs)[0], layout.bounds)
                       for m in RATIO_METRICS)])
    return rows


def write_csv(runs: Sequence[Run], runs_path: Path, ratios_path: Path, *,
              bands: Bands | None = None, gate_set: Sequence[str] | None = None) -> None:
    """``runs.csv`` (one row per run) and ``ratios.csv`` (one row per pair and design, then
    ``GEOMEAN_GATE`` and ``GEOMEAN_EXTRA`` per pair), §6.4's headers, RFC 4180 with ``\\n``.
    A pair is written when both its legs are in the table. A geomean row's ``n`` is the number
    of its set's designs with an ``ok`` run on both legs; each metric's own count of defined
    ratios is in ``runs.json``. ``bands`` defaults to the committed ``bands.json`` (its
    metrics are the gated ones, §6.1; its bounds set the 4-decimal rule). ``gate_set`` is the
    ``GEOMEAN_GATE`` set (default: the 19)."""
    layout = _layout(runs, bands, gate_set)
    arch = layout.runs[0].arch if layout.runs else ""
    _write_text(runs_path, _csv([RUNS_HEADER, *(_run_cells(layout, r) for r in layout.runs)]))
    rows: list[Sequence[str]] = [RATIOS_HEADER]
    for pair in layout.pairs():
        rows.extend(_ratio_rows(layout, pair, arch))
    _write_text(ratios_path, _csv(rows))


# ---------------------------------------------------------------- runs.json


def _header_json(header: TableHeader) -> dict[str, object]:
    return {"arch": header.arch, "label": header.label, "vtr12": header.vtr12,
            "odin3_commit12": header.odin3_commit12, "build12": header.build12,
            "abc_sha12": header.abc_sha12, "legs": dict(header.legs), "host": header.host,
            "date_utc": header.date_utc}


def _run_json(run: Run) -> dict[str, object]:
    """The run's ``metrics.json`` object, with the table's ``key12`` and ``status`` (the
    ``status`` word, which may say the job did not end ok)."""
    base: dict[str, object] = dict(run.metrics) if run.metrics else {
        "schema": 1, "leg": run.leg, "design": run.design, "arch": run.arch}
    base.update(key12=run.key12, status=run.status)
    return base


def _geomeans_json(layout: _Layout) -> list[dict[str, object]]:
    out: list[dict[str, object]] = []
    for pair in layout.pairs():
        for label, designs in ((GEOMEAN_GATE, layout.gate), (GEOMEAN_EXTRA, layout.extra)):
            for name in RATIO_METRICS:
                value, n = layout.geomean(pair, name, designs)
                out.append({"pair": pair_name(pair), "set": label, "metric": name,
                            "geomean": value, "n": n, "of": len(designs)})
    return out


def verdict_json(verdict: Verdict | None) -> dict[str, object] | None:
    if verdict is None:
        return None
    gate_object: dict[str, object] = {"pass": verdict.state == PASS,
                                      "violations": list(verdict.violations)}
    if verdict.notes:
        gate_object["notes"] = list(verdict.notes)
    return gate_object


def verdict_state(gate_object: Mapping[str, object]) -> str:
    """The state of a ``runs.json`` gate object: PASS when ``pass``, INCOMPLETE when a
    violation is a ``gate: INCOMPLETE:`` line (E-QH14b), else FAIL."""
    if gate_object.get("pass") is True:
        return PASS
    lines = gate_object.get("violations")
    if isinstance(lines, list) and any(isinstance(line, str)
                                       and line.startswith("gate: INCOMPLETE:") for line in lines):
        return INCOMPLETE
    return FAIL


def write_json(runs: Sequence[Run], verdict: Verdict | None, header: TableHeader, path: Path,
               *, bands: Bands | None = None, gate_set: Sequence[str] | None = None) -> None:
    """``runs.json``: ``{"schema", "header", "runs": [metrics.json objects], "geomeans":
    [per pair, set and metric: geomean and n of |S|], "gate": {"pass", "violations"}}``
    (``"gate": null`` without a verdict: the table is not gated), sorted keys, indent 1."""
    layout = _layout(runs, bands, gate_set)
    data = {"schema": TABLE_SCHEMA, "header": _header_json(header),
            "runs": [_run_json(run) for run in layout.runs], "geomeans": _geomeans_json(layout),
            "gate": verdict_json(verdict)}
    _write_text(path, json.dumps(data, sort_keys=True, indent=1) + "\n")


# ---------------------------------------------------------------- VTR's golden (§12 item 5)


@dataclass(frozen=True)
class GoldenXcheck:
    """VTR's ``golden_results.txt`` for ``vtr_reg_qor_chain``: ``values[(design, column)]``
    (numeric columns only), and the ``Range(min, max)`` bands of its pass requirements by
    metric. Report-only: a value outside its band is listed ``outside`` and fails nothing
    (PHASE3 exit item 5)."""

    values: Mapping[tuple[str, str], float]
    bands: Mapping[str, tuple[float, float]]


def _config_value(config: Path, key: str) -> str:
    for line in config.read_text(encoding="utf-8").splitlines():
        text = line.split("#", 1)[0].strip()
        if text.startswith(key):
            return text[len(key):].strip()
    raise ValueError(f"{config}: no {key.rstrip('=')}")


def _requirement_lines(path: Path) -> list[str]:
    """VTR's ``load_config_lines``: ``#`` comments and blank lines dropped, ``%include "f"``
    expanded in place relative to the including file (iteratively, at most 8 deep)."""
    out: list[str] = []
    stack: list[tuple[Path, list[str]]] = [(path, path.read_text(encoding="utf-8").splitlines())]
    while stack:
        current, lines = stack[-1]
        if not lines:
            stack.pop()
            continue
        line = lines.pop(0).split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith(INCLUDE):
            if len(stack) > MAX_INCLUDE_DEPTH:
                raise ValueError(f"{current}: %include nested too deep")
            child = current.parent / line[len(INCLUDE):].strip().strip('"')
            stack.append((child, child.read_text(encoding="utf-8").splitlines()))
        else:
            out.append(line)
    return out


def _bands(path: Path) -> dict[str, tuple[float, float]]:
    bands = {}
    for line in _requirement_lines(path):
        name, _, expr = line.partition(";")
        found = RANGE.fullmatch(expr.strip())
        if found is not None:
            bands[name.strip()] = (float(found.group(1)), float(found.group(2)))
    return bands


def _golden_values(path: Path, arch: str | None) -> dict[tuple[str, str], float]:
    """The numeric cells of each row (of ``<arch>.xml`` only, when ``arch`` is given)."""
    lines = [line.rstrip("\t") for line in path.read_text(encoding="utf-8").splitlines()]
    if not lines:
        raise ValueError(f"{path}: empty")
    columns = lines[0].split("\t")
    if "circuit" not in columns:
        raise ValueError(f"{path}: no circuit column")
    values = {}
    for line in lines[1:]:
        cells = dict(zip(columns, line.split("\t"), strict=False))
        if arch is not None and Path(cells.get("arch", "")).stem != arch:
            continue
        design = Path(cells.get("circuit", "")).stem
        for column, cell in cells.items():
            try:
                number = float(cell)
            except ValueError:
                continue
            if design and math.isfinite(number):
                values[(design, column)] = number
    return values


def load_golden_xcheck(vtr_root: Path, *, arch: str | None = None) -> GoldenXcheck:
    """Read ``vtr_reg_qor_chain``'s ``golden_results.txt`` (only the rows of ``<arch>.xml``
    when ``arch`` is given: the task is k6-only, so an EArch table gets no values) and the pass
    requirements its ``config.txt`` names (under ``vtr_flow/parse/pass_requirements``). OSError
    or ValueError when they cannot be read."""
    config_dir = vtr_root / XCHECK_TASK
    name = _config_value(config_dir / "config.txt", PASS_REQUIREMENTS_KEY)
    return GoldenXcheck(values=_golden_values(config_dir / "golden_results.txt", arch),
                        bands=_bands(vtr_root / PASS_REQUIREMENTS_DIR / name))


def golden_ratio(golden: GoldenXcheck, value: float | None, design: str,
                 column: str) -> tuple[float | None, str]:
    """P's value over VTR's golden and VTR's own band verdict (``RangePassRequirement``): equal
    values are ``ok``; otherwise ``ok`` when min <= ratio <= max, else ``outside``. ``""``
    when either value is missing or the metric has no band."""
    want = _present(golden.values.get((design, column)))
    if value is None or want is None:
        return None, ""
    band = golden.bands.get(column)
    if want == 0:
        return None, ("" if band is None else "ok" if value == 0 else "outside")
    found = value / want
    if band is None:
        return found, ""
    return found, "ok" if value == want or band[0] <= found <= band[1] else "outside"


# ---------------------------------------------------------------- qor.md (§6.3)


def _md_table(header: Sequence[str], rows: Iterable[Sequence[str]]) -> list[str]:
    def line(cells: Sequence[str]) -> str:
        return "| " + " | ".join(cells) + " |"
    return [line(header), "|" + "---|" * len(header), *(line(row) for row in rows), ""]


def _with_n(text: str, n: int, size: int) -> str:
    return f"{text} ({n}/{size})" if text and n < size else text


def _geomean_cell(layout: _Layout, pair: tuple[str, str], name: str, designs: Sequence[str],
                  *, bold: bool = False) -> str:
    value, n = layout.geomean(pair, name, designs)
    text = _fmt_r(value, layout.bounds)
    return _with_n(f"**{text}**" if text and bold else text, n, len(designs))


def _max_cell(layout: _Layout, pair: tuple[str, str], name: str,
              designs: Sequence[str], *, lowest: bool = False) -> str:
    """The largest ratio and its design (the smallest with ``lowest``); the first wins ties."""
    best: tuple[float, str] | None = None
    for design in designs:
        value = layout.ratio(pair, design, name)
        if value is not None and (best is None or (value < best[0] if lowest
                                                   else value > best[0])):
            best = (value, design)
    return "" if best is None else f"{fmt_ratio(best[0], layout.bounds)} ({best[1]})"


def _ok_mark(design: str, violations: Sequence[str]) -> str:
    mine = [line for line in violations if design in line.split()]
    if not mine:
        return TICK
    return OPEN if all(line.startswith("gate: INCOMPLETE:") for line in mine) else CROSS


def _compare_rows(layout: _Layout, pair: tuple[str, str], designs: Sequence[str],
                  violations: Sequence[str] | None) -> list[list[str]]:
    x, y = pair
    rows = []
    for design in designs:
        row = [design]
        for name in layout.bands.metrics:
            row += [fmt_value(name, layout.value(y, design, name)),
                    fmt_value(name, layout.value(x, design, name)),
                    _fmt_r(layout.ratio(pair, design, name), layout.bounds)]
        rows.append(row + ([] if violations is None else [_ok_mark(design, violations)]))
    blank = [] if violations is None else [""]
    size = len(designs)
    rows.append([f"**geomean ({size})**", *(c for name in layout.bands.metrics for c in (
        "", "", _geomean_cell(layout, pair, name, designs, bold=True))), *blank])
    rows.append([f"max ({size})", *(c for name in layout.bands.metrics for c in (
        "", "", _max_cell(layout, pair, name, designs))), *blank])
    return rows


def _compare_header(layout: _Layout, pair: tuple[str, str], ok: bool) -> list[str]:
    x, y = (leg_name(leg) for leg in pair)
    head = ["design"]
    for name in layout.bands.metrics:
        short = GATE_SHORT.get(name, name)
        head += [f"{short} {y}", f"{short} {x}", f"{x}/{y}"]
    return head + (["ok"] if ok else [])


def _md_gate(layout: _Layout, verdict: Verdict | None, narrowed: int | None) -> list[str]:
    pair = layout.bands.pair
    if pair[0] not in layout.legs:
        return []
    title = gate_title(layout.arch)
    if narrowed is not None:
        return [f"## {title}: {NOT_GATED} — {narrowed_reason(narrowed)}", ""]
    if verdict is None or not layout.gate:
        return [f"## {title}: {NOT_GATED}", "", "No gate design in this table.", ""]
    state, violations = verdict.state, verdict.violations
    lines = [f"## {title}: {state}", ""]
    lines += _md_table(_compare_header(layout, pair, True),
                       _compare_rows(layout, pair, layout.gate, violations))
    lines += [f"- {line}" for line in violations] or ["No violations."]
    lines += [f"- {line}" for line in verdict.notes]
    lines.append("")
    if layout.extra:
        lines += [f"## Non-gating rows ({', '.join(layout.extra)})", ""]
        lines += _md_table(_compare_header(layout, pair, False),
                           _compare_rows(layout, pair, layout.extra, None))
    return lines


def _per_leg_rows(layout: _Layout, cells: Callable[[Run], list[str]]) -> list[list[str]]:
    rows = []
    for design in layout.designs:
        for leg in layout.legs:
            run = layout.index.get((leg, design))
            if run is not None:
                rows.append([design, leg_name(leg), *cells(run)])
    return rows


def _exchange(run: Run) -> str:
    if run.leg not in EXCHANGE_LEGS:
        return ""
    netlist = run.metrics.get("netlist")
    value = netlist.get("abc_exchange") if isinstance(netlist, Mapping) else None
    return value if isinstance(value, str) and value else "none"


def _ratio_row(layout: _Layout, design: str, pair: tuple[str, str],
               names: Sequence[str]) -> list[str] | None:
    if layout.ok(pair[0], design) is None or layout.ok(pair[1], design) is None:
        return None
    return [design, pair_name(pair),
            *(_fmt_r(layout.ratio(pair, design, name), layout.bounds) for name in names)]


def _with_ratios(layout: _Layout, rows: list[list[str]], names: Sequence[str],
                 width: int) -> list[list[str]]:
    """The per-leg rows with an O/P row after each design's legs (when both ran ok)."""
    out: list[list[str]] = []
    for i, row in enumerate(rows):
        out.append(row)
        last = i + 1 == len(rows) or rows[i + 1][0] != row[0]
        ratio_row = _ratio_row(layout, row[0], ("O", "P"), names) if last else None
        if ratio_row is not None:
            out.append(ratio_row + [""] * (width - len(ratio_row)))
    return out


def _md_netlist(layout: _Layout) -> list[str]:
    def cells(run: Run) -> list[str]:
        ok = run.status == OK
        return [*(fmt_value(n, metric(run, n) if ok else None)
                  for n in (*NETLIST_COLUMNS, "abc_depth")), _exchange(run)]
    head = ["design", "leg", *NETLIST_COLUMNS, "abc_depth", "exchange"]
    rows = _with_ratios(layout, _per_leg_rows(layout, cells), NETLIST_COLUMNS, len(head))
    return ["## Netlist", "", *_md_table(head, rows)]


def _hard_blocks(layout: _Layout, run: Run) -> str:
    if run.leg == "P" or run.status != OK:
        return ""
    names = ("num_memories", "num_mult")
    mine = [metric(run, n) for n in names]
    base = [layout.value("P", run.design, n) for n in names]
    if None in mine or None in base:
        return ""
    return "= P" if mine == base else "≠ P"


def _md_packed(layout: _Layout) -> list[str]:
    def cells(run: Run) -> list[str]:
        ok = run.status == OK
        w, h = (metric(run, n) if ok else None for n in ("device_width", "device_height"))
        device = f"{_count(w)}×{_count(h)}" if w is not None and h is not None else ""
        return [*(fmt_value(n, metric(run, n) if ok else None)
                  for n in ("num_clb", "num_memories", "num_mult")), device,
                _hard_blocks(layout, run)]
    head = ["design", "leg", "CLB", "memories", "mults", "W×H", "hard blocks"]
    return ["## Packed", "", *_md_table(head, _per_leg_rows(layout, cells))]


def _md_routed(layout: _Layout) -> list[str]:
    def cells(run: Run) -> list[str]:
        ok = run.status == OK
        vpr = text_field(run, "vpr_status") if ok else MISSING
        return [run.status, "" if vpr == MISSING else vpr,
                *(fmt_value(n, metric(run, n) if ok else None)
                  for n in ("min_chan_width", "critical_path_delay_ns", "fmax_mhz",
                            "crit_path_routed_wirelength"))]
    head = ["design", "leg", "status", "vpr_status", "min W", "CPD (ns)", "Fmax (MHz)", "WL"]
    return ["## Routed", "", *_md_table(head, _per_leg_rows(layout, cells))]


def _md_cost(layout: _Layout) -> list[str]:
    def cells(run: Run) -> list[str]:
        ok = run.status == OK
        warned = bool(run.job.get("cost_warnings"))
        out = []
        for name in COST_COLUMNS:
            text = fmt_value(name, metric(run, name) if ok else None)
            out.append(f"{text}*" if text and warned and name in FRONTEND_COST else text)
        parmys = run.job.get("parmys_stage_wall_s")  # P' (point (o)): both front-end parts
        if ok and run.leg == "Pprime" and isinstance(parmys, float) and out[0]:
            out[0] = f"{out[0]} + {fmt_value('frontend_wall_s', parmys)} Parmys"
        return [*out, "xl" if run.job.get("slice") == XL_SLICE else ""]
    head = ["design", "leg", "front-end wall (s)", "front-end peak (MiB)", "VPR time (s)",
            "VPR mem (MiB)", "slice"]
    rows = _with_ratios(layout, _per_leg_rows(layout, cells), COST_COLUMNS, len(head))
    return ["## Cost", "", *_md_table(head, rows), COST_CAVEAT, ""]


def _exchange_line(layout: _Layout) -> str:
    parts = []
    for leg in EXCHANGE_LEGS:
        counts: dict[str, int] = {}
        for run in layout.runs:
            if run.leg == leg and run.status == OK:
                counts[_exchange(run)] = counts.get(_exchange(run), 0) + 1
        if counts:
            parts.append(f"{leg_name(leg)} " + ", ".join(
                f"{word} {counts[word]}" for word in sorted(counts)))
    return "ABC exchange (3D I8; runs per exchange): " + "; ".join(parts) if parts else ""


def _md_attribution(layout: _Layout) -> list[str]:
    present = [(pair, what) for pair, what in ATTRIBUTION if pair in layout.pairs()]
    if not present:
        return []
    head = ["metric", *(f"{pair_name(pair)} ({what})" for pair, what in present)]
    rows = [[name, *(_geomean_cell(layout, pair, name, layout.gate) for pair, _ in present)]
            for name in layout.bands.metrics]
    lines = ["## Attribution", "", f"Geomeans over the {len(layout.gate)} gate designs.", ""]
    lines += _md_table(head, rows)
    exchange = _exchange_line(layout)
    return lines + ([exchange, ""] if exchange else [])


def _md_noise(layout: _Layout) -> list[str]:
    pair = ("R", "P")
    if pair not in layout.pairs():
        return []
    rows = [[name, _geomean_cell(layout, pair, name, layout.gate),
             _max_cell(layout, pair, name, layout.gate, lowest=True),
             _max_cell(layout, pair, name, layout.gate)] for name in layout.bands.metrics]
    return ["## Noise floor", "", "VPR's own spread runs both ways: the smallest and the largest "
            "R/P bound it.", "", *_md_table(
                ["metric", f"R/P geomean ({len(layout.gate)})", "min", "max"], rows)]


def _verdict_cell(run: Run | None, name: str) -> str:
    if run is None:
        return ""
    if run.status != OK:
        return run.status
    return text_field(run, name)


def _md_checks(layout: _Layout, exceptions: Sequence[tuple[str, str, str]]) -> list[str]:
    legs = [leg for leg in ("P", "O") if leg in layout.legs]
    if not legs:
        return []
    excepted = {row[0]: row[1] for row in exceptions}
    head = ["design", *(f"fcheck {leg}" for leg in legs)]
    if "O" in legs:
        head += ["equiv", "rerun"]
    rows = []
    for design in layout.designs:
        row = [design, *(_verdict_cell(layout.index.get((leg, design)), "fcheck")
                         for leg in legs)]
        if "O" in legs:
            run = layout.index.get(("O", design))
            equiv = _verdict_cell(run, "equiv")
            if design not in qor_sched.EQUIV_REQUIRED and equiv in ("", "skipped", MISSING):
                equiv = qor_sched.EQUIV_NOT_REQUIRED
            elif equiv and design in excepted:
                equiv += f" (exception: {excepted[design]})"
            row += [equiv, _verdict_cell(run, "rerun")]
        rows.append(row)
    return ["## Functional check and determinism", "", *_md_table(head, rows)]


def _md_divergences(layout: _Layout, divergences: Sequence[tuple[str, str, str]]) -> list[str]:
    if "O" not in layout.legs:
        return []
    rows = [list(row) for row in divergences if row[0] in layout.designs]
    body = _md_table(["design", "class", "text"], rows) if rows else ["None.", ""]
    return ["## Declared divergences", "", *body]


def _md_name_style(layout: _Layout) -> list[str]:
    pair = ("Oprov", "O")
    if pair not in layout.pairs():
        return []
    names = ("vpr_time_s", "max_vpr_mem_mib")
    rows = []
    for design in layout.designs:
        row = [design]
        for name in names:
            row += [fmt_value(name, layout.value("O", design, name)),
                    fmt_value(name, layout.value("Oprov", design, name)),
                    _fmt_r(layout.ratio(pair, design, name), layout.bounds)]
        rows.append(row)
    head = ["design", "VPR time O", "VPR time Oprov", "Oprov/O", "VPR mem O", "VPR mem Oprov",
            "Oprov/O"]
    return ["## Name style", "", *_md_table(head, rows)]


def _xcheck_rows(layout: _Layout, golden: GoldenXcheck,
                 designs: Sequence[str]) -> list[list[str]]:
    rows = []
    ratios: dict[str, list[float | None]] = {name: [] for name, _ in XCHECK}
    for design in designs:
        row, outside = [design], []
        for name, column in XCHECK:
            mine = layout.value("P", design, name)
            found, band = golden_ratio(golden, mine, design, column)
            ratios[name].append(found)
            want = _present(golden.values.get((design, column)))
            row += [fmt_value(name, mine), fmt_value(name, want), _fmt_r(found, ())]
            if band == "outside":
                outside.append(name)
        rows.append([*row, ", ".join(outside)])
    size = len(designs)
    means = []
    for name, _ in XCHECK:
        value, n = geomean_n(ratios[name])
        means += ["", "", _with_n(_fmt_r(value, ()), n, size)]
    rows.append([f"**geomean ({size})**", *means, ""])
    return rows


def _md_xcheck(layout: _Layout, golden: GoldenXcheck | None) -> list[str]:
    if "P" not in layout.legs or golden is None:
        return []
    head = ["design"]
    for name, _ in XCHECK:
        short = GATE_SHORT.get(name, name)
        head += [f"{short} P", f"{short} golden", "P/golden"]
    head.append("outside")
    bands = ", ".join(f"{column} {golden.bands[column][0]:.2f}–{golden.bands[column][1]:.2f}"
                      for _, column in XCHECK if column in golden.bands)
    if not any(design == d for design, _ in golden.values for d in layout.designs):
        return ["## Cross-check against VTR's golden", "",
                f"VTR's `vtr_reg_qor_chain` golden has no row for these designs on {layout.arch}"
                "; no cross-check.", ""]
    lines = ["## Cross-check against VTR's golden", "",
             "P against VTR's `golden_results.txt` for `vtr_reg_qor_chain` (another machine and "
             "VPR revision), with the bands of its pass requirements: " + (bands or "none") + ". "
             "A metric outside its band is listed `outside` and fails nothing (PHASE3 exit item "
             "5).", ""]
    lines += _md_table(head, _xcheck_rows(layout, golden, layout.gate))
    if layout.extra:
        lines += [f"Non-gating rows ({', '.join(layout.extra)}):", ""]
        lines += _md_table(head, _xcheck_rows(layout, golden, layout.extra))
    return lines


def _md_title(header: TableHeader) -> list[str]:
    parts = [f"VTR {header.vtr12}"]
    if header.odin3_commit12 is not None:
        parts.append(f"odin3 {header.odin3_commit12} (build {header.build12 or UNKNOWN})")
    if header.abc_sha12 is not None:
        parts.append(f"ABC script {header.abc_sha12}")
    legs = ", ".join(f"{leg_name(leg)} {key}" for leg, key in header.legs.items())
    parts.append(f"legs {legs}" if legs else "no legs")
    return [f"# Odin III QoR — {header.arch} — {header.label}", "",
            " · ".join(parts), f"· host {header.host} · {header.date_utc}", ""]


def write_md(runs: Sequence[Run], verdict: Verdict | None, header: TableHeader, path: Path, *,
             golden: GoldenXcheck | None, bands: Bands | None = None,
             divergences: Sequence[tuple[str, str, str]] = (),
             equiv_exceptions: Sequence[tuple[str, str, str]] = (),
             gate_set: Sequence[str] | None = None, narrowed: int | None = None) -> None:
    """``qor.md``: §6.3's sections in order. The gate section (with the non-gating rows) only
    when O is in the table; Netlist, Packed, Routed and Cost always; Attribution, Noise floor,
    Declared divergences and Name style when their legs are in it; the functional check when P
    or O is; the cross-check against VTR's golden whenever P is (and ``golden`` is given).
    ``gate_set`` is the set the gate rows cover (default: the 19); an empty one, or no
    ``verdict``, writes the gate section as not gated; so does ``narrowed`` (a k6 table holding
    only that many of the 19, ruling N1), with its reason."""
    layout = _layout(runs, bands, gate_set)
    lines = _md_title(header)
    lines += _md_gate(layout, verdict, narrowed)
    lines += _md_netlist(layout) + _md_packed(layout) + _md_routed(layout) + _md_cost(layout)
    lines += _md_attribution(layout) + _md_noise(layout)
    lines += _md_checks(layout, equiv_exceptions) + _md_divergences(layout, divergences)
    lines += _md_name_style(layout) + _md_xcheck(layout, golden)
    while lines and lines[-1] == "":
        lines.pop()
    _write_text(path, "\n".join(lines) + "\n")
