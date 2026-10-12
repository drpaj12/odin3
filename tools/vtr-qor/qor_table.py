"""qor_table: the QoR table's verdict half — runs, ratios, geomeans and the gate (3H spec QH-6.1,
6.2, §8.1-8.3, §10 E-QH13/E-QH14; plan Task 8). Task 9 adds the writers.

- ``load_runs`` reads one key per leg (``--keys`` or the leg's ``latest``) from
  ``<out>/<vtr12>/<arch>/<leg>/<key12>/<d>/``: each job's ``status`` word and ``metrics.json``.
  A job whose ``status`` is ``ok`` but whose ``metrics.json`` is not an ``ok`` record of the same
  leg and design is ``incomplete``: a result is never scored from a partial attempt.
- ``metric`` is §6.1's *present*: a finite number other than VTR's ``-1`` default.
- ``ratio`` and ``geomean`` are §6.1's; the gate compares ratios and geomeans exactly (as
  fractions: ``geomean <= b`` iff ``prod r <= b**n``), never rounded, so a geomean of exactly
  1.20 passes and 1.2001 fails; ``fmt_ratio`` prints 4 decimals within 0.005 of a bound.
- ``gate`` applies §6.2's rules 1-7 in order and collects every violation, with §10's texts.
  The verdict is INCOMPLETE (exit 3) only when a reference job (P; O′/P′ when those legs are in
  the table) is not ``ok``, a P fcheck is ``error``/``capped``, the table has no O (or no P)
  leg, or a run is at a VTR commit other than the pin (point (p)); otherwise FAIL (exit 1)
  when any rule is violated, else PASS (exit 0).

TSV inputs (``explanations.tsv``, ``divergences.tsv``, ``equiv-exceptions.tsv``) are three
tab-separated fields per line; lines starting ``#`` and empty lines are skipped.
"""

from __future__ import annotations

import json
import math
import re
from collections.abc import Collection, Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path

import qor_legs
import qor_sched
from qor_heavy import HeavyError

PAIRS = (("O", "P"), ("Oprime", "P"), ("Pprime", "P"), ("O", "Oprime"), ("R", "P"),
         ("Oprov", "O"))
SECTIONS = ("netlist", "packed", "routed", "cost", "verdicts")
VTR_MISSING = -1                    # VTR's parser default for a missing key (§6.1)
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
P_SIDE_FCHECK = frozenset({"error", "capped"})
EQUIV_OK = frozenset({"equivalent", "ram"})
EQUIV_DIFFERS = "differs"
RERUN_SAME = "same"


@dataclass(frozen=True)
class Run:
    """One job of the table: its leg, design and arch basename, the leg key it was read from,
    its status word, whether it is a gate design, and its ``metrics.json`` object (``{}`` when
    there is none)."""

    leg: str
    design: str
    arch: str
    key12: str
    status: str
    gating: bool
    metrics: Mapping[str, object]


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


def _read_metrics(job_dir: Path, leg: str, design: str) -> dict[str, object]:
    """``metrics.json`` when it is a record of this leg and design, else ``{}``."""
    path = job_dir / "metrics.json"
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
               gating=gating if isinstance(gating, bool) else design in gate, metrics=metrics)


def _legs_to_load(arch_dir: Path, keys: Mapping[str, str],
                  legs: Sequence[str] | None) -> tuple[str, ...]:
    if legs is not None:
        return tuple(dict.fromkeys(qor_legs.normalise_leg(leg) for leg in legs))
    return tuple(leg for leg in qor_legs.LEGS if leg in keys or _real_dir(arch_dir / leg))


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
    legs to load; by default, every leg in ``keys`` or with a directory there. E-QH13 (exit 1):
    ``table: no complete key for leg <L> on <arch>``. The runs follow ``sort_runs``."""
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
    if isinstance(value, bool) or not isinstance(value, int | float):
        return None
    number = float(value)
    if not math.isfinite(number) or number == VTR_MISSING:
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
    """``exp(mean(ln r))`` over the values; None when there are none."""
    if not values:
        return None
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
        rows.append((fields[0], fields[1], fields[2]))
    return rows


# ---------------------------------------------------------------- the gate


@dataclass(frozen=True)
class Verdict:
    """``state`` is ``PASS``, ``FAIL`` or ``INCOMPLETE``; ``violations`` in rule order."""

    state: str
    violations: tuple[str, ...]


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
    incomplete: bool = False

    def fail(self, text: str) -> None:
        self.lines.append(text)

    def hold(self, text: str) -> None:
        self.lines.append(text)
        self.incomplete = True

    def verdict(self) -> Verdict:
        state = INCOMPLETE if self.incomplete else FAIL if self.lines else PASS
        return Verdict(state=state, violations=tuple(self.lines))


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


def _bound_exact(value: float) -> Fraction:
    """A bound as written (``1.20`` is exactly 6/5), not its binary approximation."""
    return Fraction(repr(value))


def _rule3_bands(ctx: _Ctx, compared: Sequence[str], out: _Collect) -> None:
    bands = ctx.inp.bands
    gmax, dmax = _bound_exact(bands.geomean_max), _bound_exact(bands.per_design_max)
    pair = f"{ctx.x}/{ctx.y}"
    for name in bands.metrics:
        exact: list[tuple[str, Fraction]] = []
        for design in compared:
            x = metric(ctx.index[(ctx.x, design)], name)
            y = metric(ctx.index[(ctx.y, design)], name)
            if x is not None and y is not None and x > 0 and y > 0:
                exact.append((design, Fraction(x) / Fraction(y)))
        product = math.prod((r for _, r in exact), start=Fraction(1))
        value = geomean([float(r) for _, r in exact])
        if value is not None and product > gmax ** len(exact):
            out.fail(f"gate: FAIL: {name} geomean {pair} {fmt_ratio(value, ctx.bounds)} > "
                     f"{bands.geomean_max:.{BOUND_DECIMALS}f}")
        for design, r in exact:
            if r > dmax:
                out.fail(f"gate: FAIL: {name} {design} {pair} {fmt_ratio(float(r), ctx.bounds)}"
                         f" > {bands.per_design_max:.{BOUND_DECIMALS}f}")


def _count(value: float | None) -> str:
    if value is None:
        return MISSING
    return str(int(value)) if value.is_integer() else f"{value:g}"


def _rule4_equal(ctx: _Ctx, compared: Sequence[str], out: _Collect) -> None:
    explained = {(row[0], row[1]) for row in ctx.inp.explanations}
    for name in ctx.inp.bands.equal_or_explained:
        for design in compared:
            a = metric(ctx.index[(ctx.x, design)], name)
            b = metric(ctx.index[(ctx.y, design)], name)
            if (a is not None and a == b) or (design, name) in explained:
                continue
            out.fail(f"gate: FAIL: {name} {design} {ctx.x} {_count(a)} != {ctx.y} {_count(b)} "
                     "(no row in explanations.tsv)")


def _divergence_applies(ctx: _Ctx, design: str, reference: str) -> bool:
    for row_design, cls, _ in ctx.inp.divergences:
        if row_design != design:
            continue
        if cls in STANDS_ALONE or (cls == BASELINE_FAILS and reference == FCHECK_FAIL):
            return True
    return False


def _rule5_fcheck(ctx: _Ctx, usable: Sequence[str], out: _Collect) -> None:
    for design in usable:
        mine = text_field(ctx.index[(ctx.x, design)], "fcheck")
        ref_run = ctx.index.get((ctx.y, design))
        reference = text_field(ref_run, "fcheck") if ref_run is not None else MISSING
        if reference in P_SIDE_FCHECK:
            out.hold(f"gate: INCOMPLETE: {design} {ctx.y} fcheck is {reference} (reference); "
                     "rerun it")
        elif mine == FCHECK_FAIL and not _divergence_applies(ctx, design, reference):
            out.fail(f"gate: FAIL: {design} {ctx.x} fcheck fail (no row in divergences.tsv)")
        elif mine not in (FCHECK_PASS, FCHECK_FAIL):
            out.fail(f"gate: FAIL: {design} {ctx.x} fcheck {mine}")


def _rule6_equiv(ctx: _Ctx, usable: Sequence[str], out: _Collect) -> None:
    excepted = {row[0] for row in ctx.inp.equiv_exceptions}
    for design in usable:
        verdict = text_field(ctx.index[(ctx.x, design)], "equiv")
        if verdict == EQUIV_DIFFERS:
            out.fail(f"gate: FAIL: {design} equiv differs")
        elif verdict not in EQUIV_OK and design not in excepted:
            out.fail(f"gate: FAIL: {design} equiv {verdict} (not in equiv-exceptions.tsv)")


def _rule7_rerun(ctx: _Ctx, usable: Sequence[str], out: _Collect) -> None:
    for design in usable:
        verdict = text_field(ctx.index[(ctx.x, design)], "rerun")
        if verdict != RERUN_SAME:
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
