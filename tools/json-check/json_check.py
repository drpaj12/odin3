"""json-check: write_json, read_json, write_json at every level (5.3 spec sections 6 and 9).

Usage::

    json-check --level word|gate|lut[,LEVEL...] [--odin3 O] [--yosys Y] [--scripts DIR]
               [--expect F] [--work DIR] [--abc A] [--mem-cap C] DESIGN.v...
    json-check --level word|gate|lut[,LEVEL...] --yosys-dir DIR [--odin3 O] [--yosys Y] ...

Per design and level, Odin III runs the level script (``tests/json/<level>.script``) that ends by
writing the gold (``G.il`` by ``write_rtlil`` at the word level, ``G.blif`` by ``write_blif`` at the
gate and LUT levels: the netlist as it was written, generation 1) and file 1 (``F1.json``). Then,
with ``--check`` (the IR invariants), ``read_json F1`` writes file 2 (``F2.json``) and the read-back
(``R.il`` or ``R.blif``), and ``read_json F2`` writes file 3 (``F3.json``). The legs, in order; the
first failure of a design is printed:

1. ``read``: both read-backs run and exit 0 (``check`` clean in Debug on generation 2).
2. ``fixpoint`` (R-1): file 2 equals file 3 byte for byte; the first differing line is printed.
3. ``equivalent`` (R-3): the gold and the read-back. Word level: RTLIL through
   ``opt_equiv.Checker.judge`` (Yosys 0.55); gate and LUT levels: BLIF through
   ``tools/equiv-check``, for the reasons of 5.2 (the RTLIL writer has no bit storage cells,
   ``write_blif`` no word cells).
4. ``compare`` (R-3, gate and LUT levels): ``netlist-compare`` of the gold and the read-back is
   reported as ``identical`` or ``equivalent-only`` (a cover that is not an isop cover, a ``$sop``
   read back as ``$lut``); only a pair that is not equivalent fails.

The R-2 report: file 1 and file 2 may differ in generated names and net numbers, and in the
``$sop`` to ``$lut`` conversion. Each row says whether they are the same, the same once the
generated names and bit numbers are normalised, or else the first differing normalised line.

``--yosys-dir DIR`` reads the committed Yosys fixtures ``DIR/<level>/<micro>.json`` instead (spec
T-3, rows labelled ``y-<level>``): ``read_json`` with ``--check``, R-1 on files 2 and 3, and, when
Yosys 0.55 is known (``--yosys``), equivalence of the read-back with Yosys's own reading of the
same file (``read_json; write_rtlil`` or ``write_blif``). Without Yosys the equivalence is not run
and the row says so.

``--expect F`` lists the pairs that cannot pass, one per line (``#`` comments)::

    <level|y-level> <design file name> <outcome> <reason...>

with outcome ``unproven``, ``error:script``, ``error:check`` or ``fail:<leg>``. A listed pair that
passes or ends in another outcome fails the run, and an unlisted failure fails it: no exception is
silent. The last line is ``json-check: compared N of M (U unproven, X excepted); ...``. A run that
compared nothing fails. ``--smallest-first`` runs the designs by size (fixtures always are).

Exit codes: 0 ok; 1 a leg failed, an expect line is stale or nothing was compared; 2 usage, Odin III
or tool error; 3 hard (a child crashed or timed out); 4 unproven (nobody listed it); 77 skipped.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from subprocess import CompletedProcess
from typing import Any

import icells_check as ic

import memcap
import opt_equiv

TOOL = "json-check"
ROOT = Path(__file__).resolve().parents[2]
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_CRASH, EXIT_UNPROVEN, EXIT_SKIP = 0, 1, 2, 3, 4, 77
LEVELS = ("word", "gate", "lut")
FIXTURE_LEVELS = tuple(f"y-{level}" for level in LEVELS)
RTLIL_LEVELS = ("word",)
NETLIST_COMPARE = ROOT / "tools" / "netlist-compare" / "netlist-compare"
OUTCOMES = (
    "unproven",
    "error:script",
    "error:check",
    "fail:read",
    "fail:fixpoint",
    "fail:equivalent",
    "fail:compare",
)


class ToolError(Exception):
    """A bad argument or an unusable tool (exit 2)."""


@dataclass
class Row:
    """One design at one level: the outcome, why, and what the report shows."""

    outcome: str
    reason: str = ""
    kind: str = ""  # identical | equivalent-only | "" (not compared)
    counts: tuple[int, int, int] = (0, 0, 0)  # cells, wires, nets of file 2
    diff: str = ""  # R-2: same | normalised | the first differing normalised line
    equivalence: bool = True  # the equivalence leg ran


# --- pure helpers (unit-tested) -------------------------------------------------------------

GENERATED_NAME = re.compile(r'"\$[a-z]*\d+(?:\$\d+)?"')
SRC_ATTRIBUTE = re.compile(r'^\s*attribute \\src ".*"\n', re.M)
MODULE_ATTRIBUTE = re.compile(r"^attribute \\(?!top |blackbox )\S+ .*\n", re.M)
TOP_ATTRIBUTE = re.compile(r"^attribute \\top 32'0+1$", re.M)


def plain_rtlil(text: str) -> str:
    """RTLIL the checker can find the top of: no ``src`` attribute and no module attribute but
    ``top`` and ``blackbox`` (they sit between ``attribute \\top 1`` and ``module``, where the
    checker looks for the top), and a 32-bit ``top`` (read from a Yosys module attribute) as
    ``attribute \\top 1``."""
    text = MODULE_ATTRIBUTE.sub("", SRC_ATTRIBUTE.sub("", text))
    return TOP_ATTRIBUTE.sub("attribute \\\\top 1", text)


def rank_bits(module: dict[str, Any]) -> None:
    """Replaces the integer bit numbers of one module by the order of their first use: ports, then
    the connections of the cells, then netnames. The two generations number nets differently
    (R-2 (c)) but list ports and cells in the same order, so equal wiring gets equal numbers."""
    lists: list[list[Any]] = []
    ports, cells = module.get("ports", {}), module.get("cells", {})
    if isinstance(ports, dict):
        lists += [e["bits"] for e in ports.values() if isinstance(e.get("bits"), list)]
    if isinstance(cells, dict):
        for cell in cells.values():
            conns = cell.get("connections", {}) if isinstance(cell, dict) else {}
            lists += [v for v in conns.values() if isinstance(v, list)]
    names = module.get("netnames", {})
    if isinstance(names, dict):
        lists += [e["bits"] for e in names.values() if isinstance(e.get("bits"), list)]
    rank: dict[int, int] = {}
    for lst in lists:
        for bit in lst:
            if isinstance(bit, int):
                rank.setdefault(bit, len(rank))
    for lst in lists:
        lst[:] = [rank[b] if isinstance(b, int) else b for b in lst]


def normalise_json(text: str) -> str:
    """R-2 (a) to (c): a generated name (``"$c12"``, ``"$n34"``) is ``"$N"``, and the bit numbers
    of each module by the order of first use (the constant strings stay)."""
    data = json.loads(text)
    for module in data.get("modules", {}).values():
        rank_bits(module)
    return GENERATED_NAME.sub('"$N"', json.dumps(data, indent=1))


def first_difference(one: str, two: str) -> str:
    """'' when the texts are equal, else the first differing line as ``line N: <a> | <b>``."""
    if one == two:
        return ""
    lines1, lines2 = one.splitlines(), two.splitlines()
    for number in range(max(len(lines1), len(lines2))):
        a = lines1[number] if number < len(lines1) else "<end of file>"
        b = lines2[number] if number < len(lines2) else "<end of file>"
        if a != b:
            return f"line {number + 1}: {a.strip()[:80]} | {b.strip()[:80]}"
    return "texts differ"


def json_counts(text: str) -> tuple[int, int, int]:
    """(cells, netnames, distinct bit numbers) over every module of a JSON netlist."""
    data = json.loads(text)
    cells = wires = 0
    bits: set[int] = set()
    for module in data.get("modules", {}).values():
        cells += len(module.get("cells", {}))
        wires += len(module.get("netnames", {}))
        for entry in module.get("netnames", {}).values():
            bits.update(b for b in entry.get("bits", []) if isinstance(b, int))
    return cells, wires, len(bits)


def diff_note(file1: str, file2: str) -> str:
    """R-2 for the report: ``same``, ``normalised`` (equal once R-2 (a) to (c) are applied), or the
    first differing normalised line."""
    if file1 == file2:
        return "same"
    return first_difference(normalise_json(file1), normalise_json(file2)) or "normalised"


def parse_expect(text: str) -> dict[tuple[str, str], tuple[str, str]]:
    """``(level, design) -> (outcome, reason)`` from the expect file text."""
    table: dict[tuple[str, str], tuple[str, str]] = {}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        words = line.split(None, 3)
        if (
            len(words) < 4
            or words[0] not in (*LEVELS, *FIXTURE_LEVELS)
            or words[2] not in OUTCOMES
        ):
            raise ToolError(f"expect line {number}: want '<level> <design> <outcome> <reason>'")
        table[(words[0], words[1])] = (words[2], words[3])
    return table


EXPECT_KEY = re.compile(r"(\$\w+)|`([^`]+)`")


def expect_keys(reason: str) -> list[str]:
    """The tokens an expect reason names that the observed error must contain: each ``$``-word
    (a cell type, a parameter) and each backquoted phrase."""
    return [m.group(1) or m.group(2) for m in EXPECT_KEY.finditer(reason)]


def judge_listed(verdict: ic.Verdict, listed: tuple[str, str] | None) -> tuple[bool, str]:
    """``ic.judge_expected``, but a listed failure matches only when its keys (``expect_keys``)
    all appear in the observed text: ``fail:read`` must not hide another read failure of the
    same file. An ``unproven`` entry matches on the outcome (its text is the tool's own)."""
    passed, word = ic.judge_expected(verdict, listed)
    if not passed or listed is None or verdict.outcome == "unproven":
        return passed, word
    keys = expect_keys(listed[1])
    if not keys:
        return False, f"{verdict.outcome} but the expect entry names no `key` or $token to match"
    for key in keys:
        found = re.search(re.escape(key) + r"(?![\w$])", verdict.reason)
        if found is None:
            return False, (
                f"UNEXPECTED {verdict.outcome} ({verdict.reason}): the expect entry "
                f"({listed[1]}) names {key}, not in the observed text"
            )
    return True, word


# --- running --------------------------------------------------------------------------------


@dataclass
class Config:
    odin3: str
    yosys: str
    scripts: Path
    cap: int
    abc: str = ""
    techlibs: dict[str, list[str]] | None = None

    def icfg(self) -> ic.Config:
        """The 5.2 configuration its equivalence legs are run with."""
        return ic.Config(
            self.odin3, self.yosys, self.scripts, Path("."), Path("."), self.cap, self.abc
        )


def libs_text(cfg: Config, design: Path) -> str:
    """The tech library commands of a design (a fixture ``X.json`` is the design ``X.v``)."""
    table = cfg.techlibs or {}
    return "; ".join(table.get(design.name, table.get(design.stem + ".v", [])))


def first_error(done: CompletedProcess[str]) -> str:
    """The first error line of a failed odin3 run (the located message), else its last line."""
    lines = ((done.stderr or "") + (done.stdout or "")).splitlines()
    marked = [ln for ln in lines if ln.lstrip().startswith(("odin3: error", "error"))]
    errors = marked or [ln for ln in lines if "error" in ln]
    text = (errors or lines or ["no output"])[0]
    return text.split("odin3: error: ", 1)[-1][-300:]


def read_back_script(libs: str, source: Path, outs: tuple[Path, str]) -> str:
    """``read_json`` of source, then ``write_json`` to outs[0] and the extra writer command."""
    head = f"{libs}; " if libs else ""
    tail = f"; {outs[1]}" if outs[1] else ""
    return f"{head}read_json {source}; write_json {outs[0]}{tail}"


def read_back_writer(level: str, path: Path) -> str:
    """The command that writes the read-back netlist: RTLIL at the word level; otherwise BLIF
    after ``lower``, which makes the gate-level flip-flops with a reset or an enable (Yosys
    ``$_SDFF_*``, ``$_DFFE_*``, ...) the plain ``$_DFF_P_`` plus logic that VPR takes (a no-op
    on a netlist that is lowered already)."""
    if level in RTLIL_LEVELS:
        return f"write_rtlil {path}"
    return f"lower; write_blif --vtr {path}"


def read_both(cfg: Config, level: str, flags: Sequence[str], paths: dict[str, Path]) -> Row | None:
    """Generations 2 and 3 (two odin3 runs); a Row when a read-back fails, else None. The first
    run also writes the read-back netlist (RTLIL at the word level, BLIF otherwise)."""
    libs = libs_text(cfg, paths["design"])
    steps = (
        (paths["F1"], (paths["F2"], read_back_writer(level, paths["R"]))),
        (paths["F2"], (paths["F3"], "")),
    )
    for source, outs in steps:
        text = read_back_script(libs, source, outs)
        done = ic.odin3_run(cfg.odin3, text, paths["F2"].parent, flags)
        if done.returncode != 0:
            return Row("fail:read", first_error(done))
    return None


def judge_legs(cfg: Config, level: str, paths: dict[str, Path], tag: str, run_dir: Path) -> Row:
    """R-1, then R-3 (equivalence, then the netlist-compare verdict)."""
    file1, file2, file3 = (paths[n].read_text(encoding="utf-8") for n in ("F1", "F2", "F3"))
    counts = json_counts(file2)
    note = diff_note(file1, file2)
    problem = first_difference(file2, file3)
    if problem:
        reason = f"file 2 differs from file 3, {problem}"
        return Row("fail:fixpoint", reason, "", counts, note)
    verdict = judge_equivalent(cfg, level, paths, tag, run_dir)
    if verdict.outcome != "ok":
        return Row(verdict.outcome, verdict.reason, "", counts, note)
    if level in RTLIL_LEVELS:
        return Row("ok", "", "", counts, note)
    return compare_row(paths, counts, note)


def judge_equivalent(
    cfg: Config, level: str, paths: dict[str, Path], tag: str, run_dir: Path
) -> ic.Verdict:
    """The 5.2 equivalence legs over the files G.il/R.il (word) or G.blif/R.blif (gate, LUT)."""
    ext = "il" if level in RTLIL_LEVELS else "blif"
    if ext == "il":
        for key in ("G", "R"):
            text = paths[key].read_text(encoding="utf-8")
            paths[key].write_text(plain_rtlil(text), encoding="utf-8")
    named = {f"G.{ext}": paths["G"], f"R.{ext}": paths["R"]}
    return ic.judge_equivalent(cfg.icfg(), "word" if ext == "il" else level, named, tag, run_dir)


def compare_row(paths: dict[str, Path], counts: tuple[int, int, int], note: str) -> Row:
    """R-3: netlist-compare of the gold and the read-back, identical or equivalent-only."""
    argv = [str(NETLIST_COMPARE), str(paths["G"]), str(paths["R"])]
    try:
        done = subprocess.run(
            argv, capture_output=True, text=True, timeout=ic.TIMEOUT, check=False
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise opt_equiv.CrashError("netlist-compare", str(exc), True) from exc
    text = " | ".join(((done.stdout or "") + (done.stderr or "")).strip().splitlines()[:2])
    if done.returncode == 0:
        return Row("ok", "", "identical", counts, note)
    if done.returncode == 1:
        return Row("ok", f"cover form: {text[:120]}", "equivalent-only", counts, note)
    return Row("fail:compare", f"netlist-compare exit {done.returncode}: {text}", "", counts, note)


def run_design(cfg: Config, level: str, design: Path, work: Path) -> Row:
    """One design at one level (spec T-2); ``error:script`` when the level script cannot take it."""
    ext = "il" if level in RTLIL_LEVELS else "blif"
    paths = {
        "design": design,
        "G": work / f"G.{ext}",
        "R": work / f"R.{ext}",
        "F1": work / "F1.json",
        "F2": work / "F2.json",
        "F3": work / "F3.json",
    }
    template = (cfg.scripts / f"{level}.script").read_text(encoding="utf-8")
    subs = {
        "@DESIGN@": str(design),
        "@IL@": str(paths["G"]),
        "@JSON@": str(paths["F1"]),
        "@TECHLIB@": libs_text(cfg, design),
    }
    done = ic.odin3_run(cfg.odin3, ic.level_script(template, subs), work)
    if done.returncode != 0:
        return Row("error:script", ic.last_lines(done)[-240:])
    flags = ["--check", *ic.top_flag((done.stderr or "") + (done.stdout or ""))]
    failed = read_both(cfg, level, flags, paths)
    if failed is not None:
        return failed
    return judge_legs(cfg, level, paths, design.stem + "." + level, work)


# --- the Yosys fixtures ---------------------------------------------------------------------


GATE_FF_FAMILY = re.compile(r'"type":\s*"\$_(?:DFFE?|SDFF(?:CE|E)?)_[PN]{2}[01]?[PN]?_"')
ASYNC_LEGALIZE = "dffs with async set or reset are not supported"
LEGALIZE = (
    "dfflegalize -cell $_DFF_P_ 01 -cell $_DFF_N_ 01 -cell $_DLATCH_P_ 01 -cell $_DLATCH_N_ 01"
)


def gold_commands(level: str, fixture: Path, gold: Path) -> str:
    """Yosys's own reading of the fixture, written as RTLIL or BLIF. A BLIF fixture with a
    gate-level flip-flop that has a reset or an enable is first legalised to the plain
    ``$_DFF_P_``/``$_DFF_N_`` (Yosys's own conversion, the gold of what ``lower`` does): Yosys's
    BLIF would hold the cell as a ``.subckt`` that equiv-check cannot read."""
    if level in RTLIL_LEVELS:
        return f"read_json {fixture}; write_rtlil {gold}"
    legalise = GATE_FF_FAMILY.search(fixture.read_text(encoding="utf-8")) is not None
    return f"read_json {fixture}; {LEGALIZE + '; ' if legalise else ''}write_blif {gold}"


def yosys_gold(cfg: Config, level: str, fixture: Path, gold: Path, work: Path) -> Row | None:
    """Yosys 0.55's own reading of the fixture, written as RTLIL or BLIF; a Row when it fails
    (``unproven`` when Yosys has no plain-flip-flop form of an asynchronous reset to compare)."""
    argv = [cfg.yosys, "-q", "-p", gold_commands(level, fixture, gold)]
    try:
        done = subprocess.run(
            argv, capture_output=True, text=True, timeout=ic.TIMEOUT, cwd=work, check=False
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise opt_equiv.CrashError("yosys", str(exc), True) from exc
    if done.returncode != 0 and ASYNC_LEGALIZE in done.stderr:
        return Row(
            "unproven",
            "Yosys has no plain flip-flop form of an asynchronous reset to compare with: "
            "`lower` makes it synchronous (PHASE3 decision 19), a different netlist",
            "",
            (0, 0, 0),
            "",
            False,
        )
    if done.returncode != 0:
        return Row("error:check", "Yosys cannot read the fixture: " + done.stderr[-160:])
    return None


def run_fixture(cfg: Config, level: str, fixture: Path, work: Path) -> Row:
    """One committed Yosys fixture (spec T-3): read, R-1 on files 2 and 3, and, with Yosys,
    equivalence of the read-back with Yosys's own reading."""
    ext = "il" if level in RTLIL_LEVELS else "blif"
    paths = {
        "design": fixture,
        "G": work / f"G.{ext}",
        "R": work / f"R.{ext}",
        "F1": fixture,
        "F2": work / "F2.json",
        "F3": work / "F3.json",
    }
    failed = read_both(cfg, level, ["--check"], paths)
    if failed is not None:
        return failed
    counts = json_counts(paths["F2"].read_text(encoding="utf-8"))
    problem = first_difference(
        paths["F2"].read_text(encoding="utf-8"), paths["F3"].read_text(encoding="utf-8")
    )
    if problem:
        return Row("fail:fixpoint", f"file 2 differs from file 3, {problem}", "", counts)
    if not cfg.yosys:
        return Row("ok", "equivalence not run: no Yosys 0.55", "", counts, "", False)
    failed = yosys_gold(cfg, level, fixture, paths["G"], work)
    if failed is not None:
        return failed
    verdict = judge_equivalent(cfg, level, paths, fixture.stem + "." + level, work)
    return Row(verdict.outcome, verdict.reason, "", counts, "", True)


# --- the command line -----------------------------------------------------------------------


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n")[0])
    ap.add_argument("--level", required=True, help="word, gate, lut (comma separated)")
    ap.add_argument("--odin3", default=str(ROOT / "build" / "debug" / "odin3"))
    ap.add_argument("--yosys", default=os.environ.get("ODIN3_YOSYS055", ""))
    ap.add_argument("--scripts", type=Path, default=ROOT / "tests" / "json")
    ap.add_argument("--expect", type=Path)
    ap.add_argument("--work", type=Path, help="keep every file here")
    ap.add_argument("--abc", default=os.environ.get("ODIN3_ABC", ""), help="ABC for equiv-check")
    ap.add_argument("--mem-cap", default="4G")
    ap.add_argument(
        "--techlib-list",
        type=Path,
        default=ROOT / "tests" / "json" / "techlib.txt",
        help="lines '<design file name> <tech library>': designs that read one first",
    )
    ap.add_argument("--yosys-dir", type=Path, help="read the fixtures DIR/<level>/*.json")
    ap.add_argument(
        "--smallest-first", action="store_true", help="run the designs by size, smallest first"
    )
    ap.add_argument("files", nargs="*", type=Path)
    return ap.parse_args(argv)


def items_for(args: argparse.Namespace, level: str) -> list[Path]:
    """The designs (or fixtures) one level runs, in command line (or sorted) order."""
    if args.yosys_dir is None:
        files: list[Path] = list(args.files)
        if args.smallest_first:
            files.sort(key=lambda f: (f.stat().st_size, f.name))
        return files
    found = sorted(
        (args.yosys_dir / level).glob("*.json"), key=lambda f: (f.stat().st_size, f.name)
    )
    if not found:
        raise ToolError(f"no fixtures in {args.yosys_dir / level}")
    return found


@dataclass
class Tally:
    compared: int = 0
    excepted: int = 0
    unproven: int = 0
    identical: int = 0
    equivalent_only: int = 0
    differing: int = 0  # file 1 and file 2 differ before normalisation
    unexplained: int = 0  # ... and after it
    total: int = 0
    worst: int = EXIT_OK
    open_unproven: bool = False


def report(level: str, item: Path, row: Row, listed: tuple[str, str] | None) -> tuple[bool, str]:
    """Prints one row; returns (passes the run, the report word)."""
    cells, wires, nets = row.counts
    passed, word = judge_listed(ic.Verdict(row.outcome, row.reason), listed)
    detail = f" ({row.reason})" if row.reason and row.outcome != "ok" else ""
    kind = f" {row.kind}" if row.kind else ""
    note = row.reason if row.outcome == "ok" and row.reason else ""
    shape = f" cells {cells} wires {wires} nets {nets}" if cells or wires else ""
    diff = f" file1-vs-file2: {row.diff}" if row.diff else ""
    print(
        f"{TOOL}: {level} {item.name}: {word}{kind}{detail}{shape}{diff}"
        + (f" [{note}]" if note else ""),
        flush=True,
    )
    return passed, word


def account(tally: Tally, row: Row, listed: tuple[str, str] | None, passed: bool) -> None:
    tally.total += 1
    tally.compared += row.outcome in ("ok", "fail:equivalent") and row.equivalence
    tally.unproven += row.outcome == "unproven"
    tally.excepted += listed is not None and passed
    tally.identical += row.kind == "identical"
    tally.equivalent_only += row.kind == "equivalent-only"
    tally.differing += row.diff not in ("", "same")
    tally.unexplained += row.diff not in ("", "same", "normalised")
    if not passed and row.outcome == "unproven" and listed is None:
        tally.open_unproven = True
    elif not passed:
        tally.worst = EXIT_FAIL


def execute(args: argparse.Namespace, work: Path) -> int:
    levels = args.level.split(",")
    bad = [lv for lv in levels if lv not in LEVELS]
    if bad:
        raise ToolError(f"unknown level {bad[0]} (want one of {', '.join(LEVELS)})")
    if args.yosys_dir is None and not args.files:
        raise ToolError("no designs given (and no --yosys-dir)")
    if args.yosys and not Path(args.yosys).is_file():
        raise ToolError(f"--yosys (or ODIN3_YOSYS055) names no file: {args.yosys}")
    yosys = str(Path(args.yosys).resolve()) if args.yosys else ""
    odin3, abc = str(Path(args.odin3).resolve()), args.abc and str(Path(args.abc).resolve())
    cfg = Config(odin3, yosys, args.scripts, opt_equiv.parse_size(args.mem_cap), abc)
    cfg.techlibs = ic.parse_techlibs(args.techlib_list)
    if args.yosys_dir is not None and not yosys:
        print(f"{TOOL}: no Yosys 0.55: the fixtures are read and fixpointed, not compared")
    table = parse_expect(args.expect.read_text(encoding="utf-8")) if args.expect else {}
    tally = Tally()
    fixtures = args.yosys_dir is not None
    for level in levels:
        for number, item in enumerate(items_for(args, level)):
            run_dir = work / f"{level}-{number:04d}"
            run_dir.mkdir(exist_ok=True)
            label = f"y-{level}" if fixtures else level
            run = run_fixture if fixtures else run_design
            try:
                row = run(cfg, level, item.resolve(), run_dir)
            except ic.ToolError as exc:
                raise ToolError(str(exc)) from exc
            listed = table.get((label, item.name))
            passed, _ = report(label, item, row, listed)
            account(tally, row, listed, passed)
    print(
        f"{TOOL}: compared {tally.compared} of {tally.total} ({tally.unproven} unproven, "
        f"{tally.excepted} excepted); identical {tally.identical}, equivalent-only "
        f"{tally.equivalent_only}; file 1 and file 2 differ before normalisation in "
        f"{tally.differing}, after it in {tally.unexplained}",
        flush=True,
    )
    if tally.compared == 0 and not (fixtures and not cfg.yosys):
        print(f"{TOOL}: nothing was compared", file=sys.stderr)
        return EXIT_FAIL
    if tally.unexplained:
        print(
            f"{TOOL}: file 1 and file 2 differ after normalisation in {tally.unexplained}: "
            "R-2 allows generated names, net numbers and $sop to $lut only",
            file=sys.stderr,
        )
        return EXIT_FAIL
    return tally.worst or (EXIT_UNPROVEN if tally.open_unproven else EXIT_OK)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    keep = args.work is not None
    work = Path(args.work).resolve() if keep else Path(tempfile.mkdtemp(prefix="json-check-"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        return execute(args, work)
    except opt_equiv.CrashError as exc:
        print(f"{TOOL}: HARD: {exc}", file=sys.stderr)
        return EXIT_CRASH
    except (OSError, ToolError, opt_equiv.ToolError, ic.ToolError, ValueError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_ERROR
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP
    finally:
        if not keep:  # a failing design is described on stdout; --work DIR keeps the files
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
