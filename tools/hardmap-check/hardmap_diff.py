"""hardmap-diff: Odin III's pre-ABC netlist against Parmys's golden (3C spec 9.3; plan Task 4).

Usage::

    hardmap-diff [--odin3 P] [--yosys P] [--golden-root D] [--arch A] [--expect F]... [--keep DIR]
                 [--techlib L] [--script S] [--netlist-compare N] [--equiv-check E]
                 [--equiv-timeout S] [--timeout S] [--root R] DESIGN.v...

Per design:

1. the golden is ``<golden-root>/vtr/<d>/<d>.parmys.blif`` for a file under ``benchmarks/verilog``
   and ``regression/verilog/<group>/<leaf>/<leaf>.parmys.blif`` for a micro (exit-check's rule);
   ``--arch`` must name the arch of the golden's ``.prov`` (``arch=``), when both say one, and
   Odin III then reads it with ``read_arch`` instead of ``read_techlib`` (4.2);
2. Odin III runs the script of ``phase3_pre_abc.script`` (``read_techlib; read_verilog --nolatches
   --vtr-compat; proc; fsm; opt; wreduce; opt_clean; opt -full; flatten; opt -full; hardmap;
   opt -full -ffbits; lower; opt -fast; compact; write_blif --vtr``), ``--top`` the first
   ``.model`` of the golden. A ``run wreduce first`` warning on its output (``hardmap`` left a
   ``$mul``, ``$add`` or ``$sub`` soft only because the script did not narrow it) is the class
   ``warned``: a failure no list absorbs (3C-2a review M-2);
3. the hard blocks of both BLIFs are counted by model (``adder``, ``multiply``,
   ``single_port_ram``, ``dual_port_ram``) with the ``.latch`` count; the difference (Odin III
   minus Parmys) must equal what the expect file lists for the design, else ``counts-differ``;
4. ``netlist-compare --implicit-models``: exit 0 is ``identical``; else ``equiv-check
   --match-unnamed``: 0 ``equivalent``, 1 with ``NOT equivalent`` ``differs``, 4 ``unproven``.
   Exit 2 with "cannot be matched one-to-one" is ``unpaired``: some unnamed hard block has no
   partner because its input functions differ between the files, so the netlists differ upstream
   of it (a stop of the pairing, not a tool error). For a design listed ``D2b`` an ``unpaired`` or
   ``differs`` result is retried once against a copy of the golden with the declared divergence
   undone (below), so that the rest of the netlist is still compared and proved.

The expect files (``--expect``, repeatable) hold lines ``<path> <id> <reason>`` (``#`` comments),
``path`` relative to ``--root`` (else the part after ``micro/verilog/`` or ``benchmarks/verilog/``,
else the file name). ``id`` is ``D<n>[a-z]`` (the design differs; the divergence of spec 10),
``unproven`` (equiv-check cannot decide; the counts are the evidence), ``no-golden`` (Parmys has no
golden) or ``counts:<+N|-N> <unit>`` (the unit's count differs from the golden by exactly N; the
function is not compared). Every line needs a reason. The class of a listed design is
``listed:<id>[,<id>]``. A listed entry that does not occur (the design matches, or the delta is
not seen) is ``stale`` and fails: the list shrinks. Entries for designs not run are ignored.

D2b normalisation (``normalize_d2b``): Parmys leaves the ``b`` pad pins of a ``$sub`` chain on
``unconn`` (a + 0 + c), where ``a - b`` needs ``~0`` (spec 10, D2b; ``subtractor.cc`` lines
517-527, "connect those pins to unconn"). The copy ties ``b`` of every ``adder`` whose own cout or
sumout net has ``MIN~`` (a ``$sub`` chain stage) that is on ``unconn`` to ``vcc``; nothing else
changes (soundness: ``normalize_d2b``). It is made only for a design whose expect entries hold
``D2b``, and only after an ``unpaired`` or ``differs`` result. D2b absorbs nothing else: the
line is ``listed:D2b`` only when the retry is ``equivalent``; a retry that differs fails, one that
is unproven needs its own ``unproven`` line.

Exit codes: 0 every design identical, equivalent or listed; 1 a failing class (``counts-differ``,
``differs``, ``unpaired``, ``unproven``, ``warned``, ``stale``, ``no-golden``); 2 usage, a missing
golden or tool, or Odin III exited with an error; 3 a child crashed (signal) or timed out; 77 no
golden root (CI). Heavy designs run under ``tools/heavy-run``, which caps the memory. ``--yosys``
is accepted so one command line fits every check tool; this tool does not run Yosys.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from collections import Counter
from collections.abc import Callable, Iterator, Sequence
from dataclasses import dataclass, field
from pathlib import Path

import elab_sweep
import exit_check

TOOL = "hardmap-diff"
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SCRIPT = HERE / "phase3_pre_abc.script"
ADDER_LIB = REPO / "tests" / "hardmap" / "adder_lib.v"
NETLIST_COMPARE = REPO / "tools" / "netlist-compare" / "netlist-compare"
EQUIV_CHECK = REPO / "tools" / "equiv-check" / "equiv-check"
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_HARD, EXIT_SKIP = 0, 1, 2, 3, 77
UNITS = ("adder", "multiply", "single_port_ram", "dual_port_ram", "latch")
WARN_NEEDLE = "run wreduce first"
SOURCE_MARKS = (("micro", "verilog"), ("benchmarks", "verilog"))
DEFAULT_TIMEOUT = 7200.0
DEFAULT_EQUIV_TIMEOUT = 600
EQUIV_UNPROVEN, EQUIV_HARD = 4, 5
DIVERGENCE = re.compile(r"D\d+[a-z]?")
PLAIN_IDS = ("unproven", "no-golden", "lower-gap")
GAP_NEEDLES = ("no lowering rule for cell type", "has no bit-level form")
MODEL = re.compile(r"^\.model\s+(\S+)", re.MULTILINE)
SUBCKT = re.compile(r"^\.subckt\s+(\S+)")
RTLIL_CELL = re.compile(r"^\s*cell\s+\\(\S+)\s")
HARD_UNITS = tuple(unit for unit in UNITS if unit != "latch")
NOT_EQUIVALENT = "NOT equivalent"
UNPAIRED_NEEDLE = "cannot be matched one-to-one"
D2B = "D2b"
SUB_CHAIN_MARK = "MIN~"  # Parmys names the nets of a `$sub` chain `$sub~N^MIN~M-K[i]`
ADDER_B_ON_UNCONN = re.compile(r"(?<!\S)(b(?:\[0\])?)=unconn(?!\S)")
CHAIN_OUTPUT_PINS = ("cout", "sumout")
PASSING = ("identical", "equivalent")


class ToolError(Exception):
    """A usage or input problem: exit 2."""


class LowerGap(Exception):
    """Odin III stopped in ``lower``, which has no rule for a cell yet (3B-2): not hardmap's."""

    def __init__(self, text: str, log: str) -> None:
        super().__init__(text)
        self.log = log


class HardFailure(Exception):
    """A child died of a signal or timed out: exit 3."""


@dataclass(frozen=True)
class Entry:
    """One expect line: ``ident`` is ``D1``, ``unproven``, ``no-golden`` or ``counts``."""

    ident: str
    reason: str
    unit: str = ""
    delta: int = 0


@dataclass
class Config:
    odin3: str
    golden_root: Path
    arch: str | None
    techlib: Path
    adder_lib: Path | None
    netlist_compare: str
    equiv_check: str
    timeout: float
    equiv_timeout: int
    root: Path | None
    keep: Path | None
    expect: dict[str, list[Entry]] = field(default_factory=dict)


@dataclass(frozen=True)
class Result:
    key: str
    cls: str
    passed: bool
    detail: str = ""


# ---- script and keys ----------------------------------------------------------------------


def script_text(template: Path, lib: Path, design: Path, out: Path,
                adder_lib: Path | None = None, rtlil: bool = False,
                arch: str | None = None) -> str:
    """The ``-p`` string: the script file's commands joined by ``; `` with the names filled.

    With ``arch`` (a VPR XML path) the ``read_techlib`` line is ``read_arch`` of that file, so the
    hard blocks and ``partial_map``'s rules come from the arch record (4.2)."""
    lines = [ln.strip() for ln in template.read_text(encoding="utf-8").splitlines()]
    commands = [ln for ln in lines if ln and not ln.startswith("#")]
    if rtlil:  # stop before `lower` and write the RTLIL view (the lower-gap fallback)
        commands = [*commands[:commands.index("lower")], "write_rtlil @OUT@"]
    if arch is not None:
        commands = [f"read_arch {arch}" if ln.startswith("read_techlib") else ln for ln in commands]
    if adder_lib is None:
        commands = [ln for ln in commands if "@ADDERLIB@" not in ln]
    text = "; ".join(commands)
    text = text.replace("@LIB@", str(lib)).replace("@DESIGN@", str(design))
    return text.replace("@OUT@", str(out)).replace("@ADDERLIB@", str(adder_lib))


def design_key(path: Path, root: Path | None) -> str:
    """The name of ``path`` in the expect files."""
    resolved = path.resolve()
    if root is not None:
        try:
            return resolved.relative_to(root.resolve()).as_posix()
        except ValueError:
            return path.name
    parts = resolved.parts
    for mark in SOURCE_MARKS:
        for i in range(len(parts) - len(mark) - 1, -1, -1):
            if parts[i:i + len(mark)] == mark:
                return Path(*parts[i + len(mark):]).as_posix()
    return path.name


# ---- counts -------------------------------------------------------------------------------


def blif_lines(path: Path) -> Iterator[str]:
    with path.open(encoding="utf-8", errors="replace") as handle:
        yield from handle


def count_blocks(path: Path) -> Counter[str]:
    """The ``.subckt`` count of each hard model, and ``latch`` for the ``.latch`` lines."""
    counts: Counter[str] = Counter()
    for line in blif_lines(path):
        if line.startswith(".latch"):
            counts["latch"] += 1
            continue
        match = SUBCKT.match(line)
        if match and match.group(1) in UNITS:
            counts[match.group(1)] += 1
    return counts


def count_rtlil(path: Path, top: str) -> Counter[str]:
    """The hard cells of module ``top`` in an RTLIL text (other modules are not the netlist)."""
    counts: Counter[str] = Counter()
    inside = False
    for line in blif_lines(path):
        if line.startswith("module "):
            inside = line.split()[1] == "\\" + top
            continue
        match = RTLIL_CELL.match(line) if inside else None
        if match and match.group(1) in HARD_UNITS:
            counts[match.group(1)] += 1
    return counts


def count_delta(odin: Counter[str], gold: Counter[str]) -> dict[str, int]:
    """Odin III minus Parmys, per unit, nonzero entries only."""
    delta = {unit: odin[unit] - gold[unit] for unit in UNITS}
    return {unit: n for unit, n in delta.items() if n}


def first_model(path: Path) -> str:
    for line in blif_lines(path):
        match = MODEL.match(line)
        if match:
            return match.group(1)
    raise ToolError(f"{path}: no .model line")


# ---- expect files -------------------------------------------------------------------------


def parse_entry(words: list[str], where: str) -> tuple[str, Entry]:
    """One expect line (already split): ``<path> <id> [<unit>] <reason...>``."""
    if len(words) < 3:
        raise ToolError(f"{where}: want '<path> <id> <reason>'")
    path, ident = words[0], words[1]
    if ident.startswith("counts:"):
        if len(words) < 4 or words[2] not in UNITS:
            raise ToolError(f"{where}: 'counts:<+N|-N> <unit> <reason>' with a unit of {UNITS}")
        try:
            delta = int(ident[len("counts:"):])
        except ValueError:
            raise ToolError(f"{where}: bad count delta '{ident}'") from None
        if delta == 0:
            raise ToolError(f"{where}: a count delta of 0 lists nothing")
        return path, Entry("counts", " ".join(words[3:]), words[2], delta)
    if ident not in PLAIN_IDS and not DIVERGENCE.fullmatch(ident):
        raise ToolError(f"{where}: unknown id '{ident}' (D<n>, unproven, no-golden, counts:...)")
    return path, Entry(ident, " ".join(words[2:]))


def read_expect(files: Sequence[Path]) -> dict[str, list[Entry]]:
    table: dict[str, list[Entry]] = {}
    for file in files:
        for number, raw in enumerate(file.read_text(encoding="utf-8").splitlines(), 1):
            line = raw.strip()
            if line and not line.startswith("#"):
                path, entry = parse_entry(line.split(), f"{file}:{number}")
                table.setdefault(path, []).append(entry)
    return table


# ---- running ------------------------------------------------------------------------------


def run_child(argv: Sequence[str], timeout: float) -> subprocess.CompletedProcess[str]:
    """Runs a child; a signal death or a timeout is ``HardFailure``, no such tool ``ToolError``."""
    try:
        proc = subprocess.run(list(argv), capture_output=True, text=True, errors="replace",
                              check=False, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise HardFailure(f"{Path(argv[0]).name} timed out after {timeout:.0f} s") from None
    except OSError as exc:
        raise ToolError(f"cannot run {argv[0]}: {exc}") from None
    if proc.returncode < 0:
        raise HardFailure(f"{Path(argv[0]).name} died of signal {-proc.returncode}")
    return proc


def tail(text: str, lines: int = 3) -> str:
    return " | ".join(text.strip().splitlines()[-lines:])


def run_odin(cfg: Config, design: Path, top: str, work: Path) -> tuple[Path, str]:
    """Odin III's script; returns (BLIF, its output). Exit != 0 is ``ToolError``."""
    out = work / "odin.blif"
    script = script_text(SCRIPT, cfg.techlib, design, out, cfg.adder_lib, arch=cfg.arch)
    proc = run_child([cfg.odin3, "--top", top, "-p", script], cfg.timeout)
    log = proc.stderr + proc.stdout
    (work / "odin.log").write_text(log, encoding="utf-8")
    gap_lines = [ln for ln in proc.stderr.splitlines() if any(n in ln for n in GAP_NEEDLES)]
    if proc.returncode != 0 and gap_lines:
        gap = gap_lines[0]
        raise LowerGap(gap.replace("odin3: error: ", "").strip(), log)
    if proc.returncode != 0:
        raise ToolError(f"odin3 exited {proc.returncode}: {tail(proc.stderr)}")
    return out, log


def is_sub_chain_adder(tokens: list[str]) -> bool:
    """A ``.subckt adder`` (exact model name) of a ``$sub`` chain: ``MIN~`` in the net of its own
    ``cout`` or ``sumout`` pin. A ``$add`` adder that only reads a ``$sub`` net (``a``, ``cin``)
    does not qualify."""
    if len(tokens) < 2 or tokens[0] != ".subckt" or tokens[1] != "adder":
        return False
    for token in tokens[2:]:
        pin, _, net = token.partition("=")
        pin = pin.split("[", 1)[0]
        if pin in CHAIN_OUTPUT_PINS and SUB_CHAIN_MARK in net:
            return True
    return False


def normalize_d2b(golden: Path, dest: Path) -> int:
    """Writes to ``dest`` a copy of the Parmys ``golden`` with D2b undone and returns the number
    of pins changed: on every ``.subckt adder`` (the exact model name) whose own ``cout`` or
    ``sumout`` net carries ``MIN~`` (a stage of a ``$sub`` chain), a ``b`` pin on ``unconn``
    (``b=unconn`` or ``b[0]=unconn``) becomes ``vcc``. Nothing else changes. A statement with a
    continuation line (``\\`` at the end) is rewritten as one line only when it changes.

    Soundness. In a Parmys `$sub` chain a real ``b`` bit is the output of a NOT node
    (``a + ~b + 1``); ``unconn`` is written only by ``connect_nodes(pad_node, ...)`` for a pin that
    has no operand bit, the pad of the narrower operand (subtractor.cc 517-527). So a ``b`` on
    ``unconn`` in such a stage is a pad, and ``~pad`` would be ``vcc``. The rewrite therefore turns
    the golden into the RTL-correct form; an equivalence proved against it shows Odin III equals the
    RTL-correct Parmys netlist, not Parmys's own. A golden whose ``b`` pins are not scalar or not
    named ``b`` is not rewritten (no pins changed: the caller keeps the ``unpaired`` result)."""
    changed = 0
    with golden.open(encoding="utf-8", errors="replace") as src, \
            dest.open("w", encoding="utf-8") as out:
        pending: list[str] = []
        for line in src:
            pending.append(line)
            if line.rstrip("\n").endswith("\\"):
                continue
            statement = " ".join(part.rstrip("\n").rstrip("\\").strip() for part in pending)
            if is_sub_chain_adder(statement.split()) and ADDER_B_ON_UNCONN.search(statement):
                out.write(ADDER_B_ON_UNCONN.sub(r"\1=vcc", statement) + "\n")
                changed += 1
            else:
                out.writelines(pending)
            pending = []
        out.writelines(pending)
    return changed


def run_equiv(cfg: Config, golden: Path, out: Path, work: Path, log: str) -> tuple[str, str]:
    """One ``equiv-check --match-unnamed``: (class, its output). A class is ``equivalent``,
    ``differs``, ``unproven`` or ``unpaired``; anything else is a tool failure."""
    eq = run_child([cfg.equiv_check, "--match-unnamed", "--timeout", str(cfg.equiv_timeout),
                    str(golden), str(out)], cfg.timeout)
    text = eq.stdout + eq.stderr
    (work / log).write_text(text, encoding="utf-8")
    if eq.returncode == 0:
        return "equivalent", text
    if eq.returncode == 1 and NOT_EQUIVALENT in text:
        return "differs", text
    if eq.returncode == EQUIV_UNPROVEN:
        return "unproven", text
    if eq.returncode == 2 and UNPAIRED_NEEDLE in text:
        return "unpaired", text
    if eq.returncode == EQUIV_HARD:
        raise HardFailure("equiv-check: ABC died or ran out of memory")
    raise ToolError(f"equiv-check exited {eq.returncode}: {tail(eq.stderr + eq.stdout)}")


def unpaired_summary(text: str) -> str:
    """``3 only in A, 3 only in B: multiply 3`` from the lines equiv-check lists."""
    sides: Counter[str] = Counter()
    kinds: Counter[str] = Counter()
    for line in text.splitlines():
        match = re.match(r"\s+only in ([AB]): (\S+) unnamed", line)
        if match:
            sides[match.group(1)] += 1
            kinds[match.group(2)] += 1
    kind_text = ", ".join(f"{name} {n}" for name, n in sorted(kinds.items()))
    return f"{sides['A']} only in A, {sides['B']} only in B: {kind_text}"


@dataclass(frozen=True)
class Compared:
    """The class of the comparison, whether the golden had D2b undone for it, and a note."""

    cls: str
    normalized: bool = False
    note: str = ""


def compare_netlists(cfg: Config, golden: Path, out: Path, work: Path,
                     try_d2b: bool = False) -> Compared:
    """The structural / functional comparison. With ``try_d2b`` (the design is listed ``D2b``) an
    ``unpaired`` or ``differs`` result is retried once on a golden with D2b undone."""
    nc = run_child([cfg.netlist_compare, "--implicit-models", str(golden), str(out)], cfg.timeout)
    if nc.returncode == 0:
        return Compared("identical")
    if nc.returncode != 1:
        raise ToolError(f"netlist-compare exited {nc.returncode}: {tail(nc.stderr)}")
    cls, text = run_equiv(cfg, golden, out, work, "equiv.log")
    if cls not in ("unpaired", "differs") or not try_d2b:
        return Compared(cls, False, unpaired_summary(text) if cls == "unpaired" else "")
    note = unpaired_summary(text) if cls == "unpaired" else "equiv-check found a difference"
    fixed = work / "golden.d2b.blif"
    changed = normalize_d2b(golden, fixed)
    if changed == 0:
        return Compared(cls, False, f"{note}; the golden has no `$sub` chain pad to undo")
    cls, _ = run_equiv(cfg, fixed, out, work, "equiv.d2b.log")
    return Compared(cls, True, f"{note}; D2b undone on {changed} adder pins of the golden")


# ---- the verdict --------------------------------------------------------------------------


def listed_ids(entries: list[Entry]) -> str:
    ids: list[str] = []
    for entry in entries:
        ident = entry.ident if entry.ident != "counts" else f"counts:{entry.delta:+d} {entry.unit}"
        if ident not in ids:
            ids.append(ident)
    return ",".join(ids)


def fmt_delta(delta: dict[str, int]) -> str:
    return ", ".join(f"{unit} {n:+d}" for unit, n in sorted(delta.items())) or "none"


def stale_others(key: str, entries: list[Entry], used: tuple[str, ...]) -> Result | None:
    """``stale`` when an entry other than the ones the verdict used did not occur."""
    rest = [e for e in entries if e.ident not in used]
    if rest:
        return Result(key, "stale", False,
                      f"listed {listed_ids(rest)} beside {', '.join(used)}, which did not occur")
    return None


def judge_compare(key: str, entries: list[Entry], compared: Compared) -> Result:
    """The listing logic after the comparison ``compared``. ``D2b`` absorbs only the class
    ``unpaired``, and only when the retry on the golden with D2b undone proves equivalence (or is
    ``unproven`` with its own ``unproven`` line); a plain ``differs`` is never D2b's."""
    cls = compared.cls
    divergences = [e for e in entries if DIVERGENCE.fullmatch(e.ident)]
    unproven = [e for e in entries if e.ident == "unproven"]
    d2b = [e for e in divergences if e.ident == D2B]
    others = [e for e in divergences if e.ident != D2B]
    if cls == "equivalent" and compared.normalized and d2b:
        stale = stale_others(key, entries, (D2B,))
        if stale is not None:
            return stale
        return Result(key, f"listed:{D2B}", True, f"{d2b[0].reason}; {compared.note}; "
                      "equivalent to the golden with D2b undone")
    if cls in PASSING:
        if entries:
            return Result(key, "stale", False, f"listed {listed_ids(entries)} but it is {cls}")
        return Result(key, cls, True)
    if cls == "unpaired":
        return Result(key, "unpaired", False,
                      f"unnamed hard blocks cannot be paired ({compared.note}); the inputs "
                      "of some differ between the files" +
                      ("" if d2b else "; list D2b if it is the subtractor pad (spec 10)"))
    if compared.normalized:  # a listed D2b design: what is left after undoing it
        if cls == "unproven" and unproven:
            stale = stale_others(key, entries, (D2B, "unproven"))
            if stale is not None:
                return stale
            return Result(key, "listed:unproven", True, f"{unproven[0].reason}; {compared.note}")
        return Result(key, cls, False, f"after undoing D2b ({compared.note})")
    if cls == "differs" and others:
        return Result(key, f"listed:{listed_ids(others)}", True, others[0].reason)
    if cls == "unproven" and unproven:
        return Result(key, "listed:unproven", True, unproven[0].reason)
    return Result(key, cls, False, "not in the expect files")


def judge(key: str, entries: list[Entry], delta: dict[str, int],
          compare: Callable[[], Compared]) -> Result:
    """Counts first, then (when no count is listed) the netlists."""
    counts = [e for e in entries if e.ident == "counts"]
    want = {e.unit: e.delta for e in counts}
    if delta != want:
        return Result(key, "counts-differ", False,
                      f"Odin III minus Parmys: {fmt_delta(delta)}; listed: {fmt_delta(want)}")
    if counts:
        others = [e for e in entries if e.ident != "counts"]
        if others:
            return Result(key, "stale", False,
                          f"listed {listed_ids(others)} beside counts: the netlists are not "
                          "compared, so it can never occur")
        return Result(key, f"listed:{listed_ids(entries)}", True, counts[0].reason)
    return judge_compare(key, entries, compare())


def judge_gap(key: str, entries: list[Entry], text: str, delta: dict[str, int]) -> Result:
    """Odin III stopped in ``lower``: the hard-block counts of the RTLIL before ``lower`` still
    have to equal the listed ones (``counts-differ`` otherwise); then a failure unless the design
    is listed ``lower-gap``. ``delta`` covers the hard units only (the RTLIL has no ``.latch``)."""
    want = {e.unit: e.delta for e in entries if e.ident == "counts" and e.unit != "latch"}
    if delta != want:
        return Result(key, "counts-differ", False,
                      f"in lower-gap, RTLIL before lower: {fmt_delta(delta)}; "
                      f"listed: {fmt_delta(want)}")
    listed = [e for e in entries if e.ident == "lower-gap"]
    if listed:
        return Result(key, "listed:lower-gap", True, f"counts equal Parmys's; {listed[0].reason}")
    return Result(key, "lower-gap", False, f"counts equal Parmys's; {text}")


def gap_delta(cfg: Config, design: Path, golden: Path, work: Path) -> dict[str, int]:
    """Count difference (Odin III minus Parmys) from the RTLIL written before ``lower``."""
    top = first_model(golden)
    out = work / "odin.il"
    script = script_text(SCRIPT, cfg.techlib, design, out, cfg.adder_lib, rtlil=True, arch=cfg.arch)
    proc = run_child([cfg.odin3, "--top", top, "-p", script], cfg.timeout)
    if proc.returncode != 0:
        raise ToolError(f"odin3 (before lower) exited {proc.returncode}: {tail(proc.stderr)}")
    gold = count_blocks(golden)
    return count_delta(count_rtlil(out, top), Counter({u: gold[u] for u in HARD_UNITS}))


def check_arch(cfg: Config, golden: Path) -> None:
    prov = golden.with_suffix(".prov")
    if cfg.arch is None or not prov.is_file():
        return
    fields = exit_check.read_prov(prov)
    recorded = fields.get("arch")
    if recorded and Path(recorded).stem != Path(cfg.arch).stem:
        raise ToolError(f"{golden.name}: golden arch '{recorded}' is not --arch '{cfg.arch}'")


def wreduce_warning(key: str, log: str) -> Result | None:
    """The ``warned`` result when Odin III's output holds the ``run wreduce first`` warning."""
    for line in log.splitlines():
        if WARN_NEEDLE in line:
            return Result(key, "warned", False, line.strip())
    return None


def find_golden(cfg: Config, design: Path) -> Path:
    try:
        return Path(exit_check.golden_path(cfg.golden_root, design))
    except ValueError as exc:
        raise ToolError(f"{design}: {exc}") from None


def check_design(cfg: Config, design: Path, work: Path) -> Result:
    key = design_key(design, cfg.root)
    entries = cfg.expect.get(key, [])
    golden = find_golden(cfg, design)
    if not golden.is_file():
        if any(e.ident == "no-golden" for e in entries):
            return Result(key, "listed:no-golden", True, entries[0].reason)
        raise ToolError(f"{key}: no golden at {golden}")
    check_arch(cfg, golden)
    try:
        out, log = run_odin(cfg, design, first_model(golden), work)
    except LowerGap as gap:
        warned = wreduce_warning(key, gap.log)
        if warned is not None:
            return warned
        return judge_gap(key, entries, str(gap), gap_delta(cfg, design, golden, work))
    warned = wreduce_warning(key, log)
    if warned is not None:
        return warned
    delta = count_delta(count_blocks(out), count_blocks(golden))
    try_d2b = any(e.ident == D2B for e in entries)
    return judge(key, entries, delta,
                 lambda: compare_netlists(cfg, golden, out, work, try_d2b))


def run_design(cfg: Config, design: Path) -> Result:
    """``check_design`` with a work directory; ``--keep`` keeps it, else it is temporary."""
    key = design_key(design, cfg.root)
    if cfg.keep is not None:
        work = cfg.keep / re.sub(r"[^A-Za-z0-9_.-]+", "_", key)
        work.mkdir(parents=True, exist_ok=True)
        return check_design(cfg, design, work)
    with tempfile.TemporaryDirectory(prefix="hardmap-diff.") as tmp:
        return check_design(cfg, design, Path(tmp))


# ---- main ---------------------------------------------------------------------------------


def parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", default=str(REPO / "build" / "release" / "odin3"))
    ap.add_argument("--yosys", help="accepted for one command line across tools; not used")
    ap.add_argument("--golden-root", type=Path, default=elab_sweep.load_micro_top().DEFAULT_ROOT)
    ap.add_argument("--arch", help="the arch XML; checked against the golden's .prov")
    ap.add_argument("--expect", type=Path, action="append", default=[])
    ap.add_argument("--keep", type=Path, help="keep each design's work files under this directory")
    ap.add_argument("--techlib", type=Path, default=REPO / "lib" / "vtr.o3lib")
    ap.add_argument("--adder-lib", default=str(ADDER_LIB),
                    help="the n-bit soft adder of vtr_primitives.v, read with --lib ('' for none)")
    ap.add_argument("--netlist-compare", default=str(NETLIST_COMPARE))
    ap.add_argument("--equiv-check", default=str(EQUIV_CHECK))
    ap.add_argument("--equiv-timeout", type=int, default=DEFAULT_EQUIV_TIMEOUT)
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    ap.add_argument("--root", type=Path, help="the directory the expect paths are relative to")
    ap.add_argument("designs", nargs="+", type=Path, metavar="DESIGN.v")
    return ap.parse_args(argv)


def make_config(args: argparse.Namespace) -> Config:
    return Config(
        odin3=args.odin3, golden_root=args.golden_root, arch=args.arch, techlib=args.techlib,
        adder_lib=Path(args.adder_lib) if args.adder_lib else None,
        netlist_compare=args.netlist_compare, equiv_check=args.equiv_check, timeout=args.timeout,
        equiv_timeout=args.equiv_timeout, root=args.root, keep=args.keep,
        expect=read_expect(args.expect))


def report(result: Result) -> None:
    detail = f" ({result.detail})" if result.detail else ""
    print(f"{TOOL}: {result.key}: {result.cls.upper() if not result.passed else result.cls}"
          f"{detail}")


def run_all(cfg: Config, designs: Sequence[Path]) -> int:
    results: list[Result] = []
    code = EXIT_OK
    for design in designs:
        try:
            result = run_design(cfg, design)
        except HardFailure as exc:
            result, code = Result(design_key(design, cfg.root), "hard", False, str(exc)), EXIT_HARD
        except ToolError as exc:
            result = Result(design_key(design, cfg.root), "error", False, str(exc))
            code = max(code, EXIT_ERROR)
        report(result)
        results.append(result)
        if not result.passed and code == EXIT_OK:
            code = EXIT_FAIL
    classes = Counter(r.cls.split(":")[0] for r in results)
    print(f"{TOOL}: {len(results)} designs: " + ", ".join(f"{n} {c}" for c, n in
                                                         sorted(classes.items())))
    return code


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        cfg = make_config(args)
    except (ToolError, OSError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_ERROR
    if not cfg.golden_root.is_dir():
        print(f"{TOOL}: skipped: no golden root {cfg.golden_root}", file=sys.stderr)
        return EXIT_SKIP
    return run_all(cfg, args.designs)


if __name__ == "__main__":
    sys.exit(main())
