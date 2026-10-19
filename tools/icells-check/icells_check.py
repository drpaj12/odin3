"""icells-check: Verilog read-back at every level (5.2 spec sections 7 and 8).

Usage::

    icells-check --level word|gate|lut|lpm[,LEVEL...] [--odin3 O] [--yosys Y] [--scripts DIR]
                 [--lib L] [--expect F] [--root R] [--work DIR] [--all] DESIGN.v...

Per design and level, Odin III runs the level's script (``tests/icells/<level>.script``, the LPM
level is the 5.1 ``tests/lpm/lpm.script``) that ends by writing the gold (``G.il`` by
``write_rtlil`` at the word and LPM levels, ``G.blif`` by ``write_blif`` at the gate and LUT
levels: the netlist as it was written) and ``W.v`` (``write_verilog``). Then, with ``--check`` (the
IR invariants), ``[read_techlib L;] read_verilog -icells W.v; proc; opt_clean`` writes
``W2.v`` and then the read-back (``R.il``, or ``R.blif`` after ``lower``, so the netlist can be
written as BLIF at the gate and LUT levels), and the same read of ``W2.v`` writes ``W3.v``. The
legs, in order; the first failure of a design is printed:

1. ``read``: the read-back runs and exits 0.
2. ``equivalent``: the gold and the read-back. Word and LPM levels: RTLIL through
   ``opt_equiv.Checker.judge`` (2F's refinement, Yosys 0.55: ``equiv_make``, ``equiv_simple
   -undef``, ``equiv_induct -undef``, a clock-aware lowering, hard blocks cut by name and width, a
   base case from the initial state; a gold x is a don't care; Yosys has no SAT model of ``$pow``:
   that pair is *unproven*, never ok). Gate and LUT levels: BLIF through ``tools/equiv-check`` (ABC
   cec/dsec), because the RTLIL writer has no form for a bit storage cell (``$_DFF_P_``) and the
   BLIF writer none for word cells and memories: each level uses the one tool its writer can feed.
3. ``fixpoint``: reading what the read-back wrote changes no cell: the census of cells by type
   (``stats``) of generation 2 (``W2.v`` read) equals that of generation 3 (``W3.v`` read). The
   text of ``W3.v`` equals ``W2.v`` (comments dropped) when the netlist has no storage written as
   an ``always`` block; where it has, each read names the ``reg`` of the flip-flop anew and the
   text grows by one alias per flip-flop (spec X-9), so the text is reported, not required.
4. ``instances``: every ``$``-type instance of ``W.v`` is an instance of the same type, name and
   parameters in ``W2.v`` with the same connected pins (an open pin, an x pin and a pin on a
   generated ``$n<ID>`` net count as equal; generated names are compared as one token).

``--expect F`` lists the designs that cannot pass a level, one per line (``#`` comments)::

    <level> <design file name> <outcome> <reason...>

with outcome ``unproven``, ``error:script`` (the level's own script, not the read-back, could not
process the design), ``error:check`` (the equivalence checker could not judge the pair) or
``fail:<leg>``. A listed pair that passes, or ends in another outcome,
fails the run (stale or wrong), and an unlisted failure fails it: no exception is silent. The last
line is ``icells-check: compared N of M (U unproven, X excepted)``.

Exit codes: 0 ok; 1 a leg failed or an expect line is stale; 2 usage, Odin III or tool error; 3
hard (a child crashed or timed out); 4 unproven (nobody listed it); 77 skipped.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import memcap
import opt_equiv

TOOL = "icells-check"
ROOT = Path(__file__).resolve().parents[2]
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_CRASH, EXIT_UNPROVEN, EXIT_SKIP = 0, 1, 2, 3, 4, 77
LEVELS = ("word", "gate", "lut", "lpm")
TIMEOUT = 900.0
DEFAULT_CAP = 4 << 30
NO_SAT_MODEL = "No SAT model available for cell"
RTLIL_LEVELS = ("word", "lpm")
EQUIV_CHECK = ROOT / "tools" / "equiv-check" / "equiv-check"
OUTCOMES = (
    "unproven",
    "error:script",
    "fail:read",
    "fail:equivalent",
    "fail:fixpoint",
    "fail:instances",
)


class ToolError(Exception):
    """A bad argument or an unusable tool (exit 2)."""


@dataclass
class Verdict:
    """One design at one level: ``ok``, ``unproven`` or ``fail:<leg>``, and why."""

    outcome: str
    reason: str = ""


# --- pure helpers (unit-tested) -------------------------------------------------------------


def strip_comments(text: str) -> str:
    """Drops the trailing ``// ...`` comment of every line (the provenance locations)."""
    return re.sub(r"[ \t]*//.*$", "", text, flags=re.M)


INSTANCE_RE = re.compile(
    r"^[ \t]*(?:\(\*.*?\*\)[ \t]*)?\\(\$[^ \t]+)[ \t]+(?:#\((.*?)\)[ \t]+)?(\\?\S+)[ \t]*\(",
    re.M,
)
PIN_RE = re.compile(r"^\s*\.(\w+)\((.*?)\),?\s*(?://.*)?$")
GENERATED = re.compile(r"\\?\$[nwcp]\d+(?:\$\d+)? ?")
NET_NAME = re.compile(r"\\\$[nwp]\d+(?:\$\d+)?")
X_PIN = re.compile(r"^(?:1'bx|\{1'bx(?:, ?1'bx)*\})$")


def normalise_pin(expr: str, uses: dict[str, int]) -> str:
    """A pin expression with every generated name as one token; an x pin, and a pin on generated
    nets that nothing else in the file uses (the fresh net of an open output), are empty."""
    text = expr.strip()
    if not text or X_PIN.match(text):
        return ""
    names = NET_NAME.findall(text)
    if (
        names
        and all(uses.get(n, 0) <= 1 for n in names)
        and not NET_NAME.sub("", text).strip(" {},")
    ):
        return ""
    return GENERATED.sub("$N ", text).strip()


def instances(text: str) -> list[tuple[str, str, str, dict[str, str]]]:
    """(type, parameter text, instance name, connected pins) of every ``$`` instance, file order."""
    uses: dict[str, int] = {}
    body = re.sub(r"^[ \t]*(?:wire|reg)\b[^;]*;", "", text, flags=re.M)  # declarations are no use
    for name in NET_NAME.findall(body):
        uses[name] = uses.get(name, 0) + 1
    found: list[tuple[str, str, str, dict[str, str]]] = []
    for match in INSTANCE_RE.finditer(text):
        kind, params, name = match.group(1), (match.group(2) or "").strip(), match.group(3)
        end = text.index(");", match.end())
        pins: dict[str, str] = {}
        for line in text[match.end() : end].splitlines():
            pin = PIN_RE.match(line)
            if pin and normalise_pin(pin.group(2), uses):
                pins[pin.group(1)] = normalise_pin(pin.group(2), uses)
        found.append((kind, params, name, pins))
    return found


def compare_instances(written: str, read: str) -> str:
    """'' when every ``$`` instance of ``written`` came back as itself, else the first change."""
    one, two = instances(written), instances(read)
    if len(one) != len(two):
        return f"{len(one)} instances written, {len(two)} read back"
    for a, b in zip(one, two, strict=True):
        if a != b:
            return f"instance {a[2]} of {a[0]} changed: {a} -> {b}"
    return ""


def parse_expect(text: str) -> dict[tuple[str, str], tuple[str, str]]:
    """``(level, design) -> (outcome, reason)`` from the expect file text."""
    table: dict[tuple[str, str], tuple[str, str]] = {}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        words = line.split(None, 3)
        if len(words) < 4 or words[0] not in LEVELS or words[2] not in OUTCOMES:
            raise ToolError(f"expect line {number}: want '<level> <design> <outcome> <reason>'")
        table[(words[0], words[1])] = (words[2], words[3])
    return table


def judge_expected(verdict: Verdict, listed: tuple[str, str] | None) -> tuple[bool, str]:
    """(passes the run, the report word) of a verdict against its expect entry."""
    if listed is None:
        return verdict.outcome == "ok", verdict.outcome
    if verdict.outcome == listed[0]:
        return True, f"excepted ({listed[0]}: {listed[1]})"
    if verdict.outcome == "ok":
        return False, f"STALE expect entry ({listed[0]}: {listed[1]}); it passes now"
    return False, f"{verdict.outcome} but the expect file says {listed[0]}"


CENSUS_RE = re.compile(r"stats: module (\S+): cell (\S+) (\d+)")


def census(log: str) -> dict[tuple[str, str], int]:
    """(module, cell type) -> count from the ``stats`` lines of an Odin III log."""
    return {(m.group(1), m.group(2)): int(m.group(3)) for m in CENSUS_RE.finditer(log)}


TOP_RE = re.compile(r"read_verilog: elaborated top '([^']+)'")


def top_flag(log: str) -> list[str]:
    """``--top NAME`` for the read-backs: the written file has no top marker, and a module that
    opt left uninstantiated would make the top ambiguous."""
    found = TOP_RE.search(log)
    return ["--top", found.group(1)] if found else []


PRIVATE_NAME = re.compile(r"(?<![\\\w$])\$([cnwp]\d+(?:\$\d+)?)(?![\w$])")


CONNECT_RE = re.compile(r"^[ \t]+connect \\\S+ (.*)$")
GEN_TOKEN = re.compile(r"\\?\$r?[cnwp]\d+(?:\$\d+)?")


def normalise_readback(il: str) -> str:
    """The read-back RTLIL with its own private names moved aside (``$n5`` becomes ``$rn5``)."""
    return PRIVATE_NAME.sub(r"$r\1", il)


def promote_generated(il: str) -> str:
    """The gold RTLIL with the writer's generated names made public (``$c21`` becomes ``\\$c21``).
    The writer gives an unnamed cell or net a generated name and the reader keeps it as the
    object's own, public name; a hard block is cut under the name of its cell, and Yosys pairs the
    cut pins of the two sides only by public names, so the gold must show the same name."""
    return PRIVATE_NAME.sub(r"\\$\1", il)


def drop_open_pins(il: str) -> str:
    """The RTLIL with the open pins of black-box cells removed, on either side of the comparison.
    An open pin is a connection to the constant x or to generated nets nothing else in the module
    reads or drives: the gold has a fresh net there (an open output, an input the IR leaves
    unconnected) and the read-back the same or the constant x (spec X-3), so both mean no
    connection."""
    out: list[str] = []
    for module in re.split(r"(?m)^(?=module )", il):
        uses: dict[str, int] = {}
        for line in module.splitlines():
            found = CONNECT_RE.match(line)
            for token in GEN_TOKEN.findall(found.group(1) if found else ""):
                uses[token] = uses.get(token, 0) + 1
        in_box = False
        for line in module.splitlines(keepends=True):
            if line.startswith("  cell "):
                in_box = not line.split()[1].startswith("$")
            elif line.startswith("  end"):
                in_box = False
            found = CONNECT_RE.match(line) if in_box else None
            if found and is_open(found.group(1), uses):
                continue
            out.append(line)
    return "".join(out)


def is_open(value: str, uses: dict[str, int]) -> bool:
    """A connection value that is only x bits, or only generated nets used once in the module."""
    if re.fullmatch(r"\d+'x+", value.strip()):
        return True
    tokens = GEN_TOKEN.findall(value)
    rest = GEN_TOKEN.sub("", value).strip(" {}")
    return bool(tokens) and not rest and all(uses.get(t, 0) == 1 for t in tokens)


def level_script(template: str, subs: dict[str, str]) -> str:
    """The script text of a level: comments dropped, lines joined with ``; ``."""
    lines = [ln.strip() for ln in template.splitlines()]
    text = "; ".join(ln for ln in lines if ln and not ln.startswith("#"))
    for key, value in subs.items():
        text = text.replace(key, value)
    return re.sub(r"\s+", " ", text).replace(" ;", ";").strip()


def lpm_template(text: str) -> str:
    """The 5.1 LPM script with ``write_rtlil @IL@`` put before its ``write_verilog @OUT@``."""
    if "write_verilog @OUT@" not in text:
        raise ToolError("tests/lpm/lpm.script has no 'write_verilog @OUT@'")
    return text.replace("write_verilog @OUT@", "write_rtlil @IL@; write_verilog @OUT@")


# --- running --------------------------------------------------------------------------------


def odin3_run(
    odin3: str, script: str, work: Path, flags: Sequence[str] = ()
) -> subprocess.CompletedProcess[str]:
    argv = [odin3, *flags, "-p", script]
    try:
        return subprocess.run(
            argv, capture_output=True, text=True, timeout=TIMEOUT, cwd=work, check=False
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise opt_equiv.CrashError(
            "odin3", str(exc), isinstance(exc, subprocess.TimeoutExpired)
        ) from exc


def last_lines(done: subprocess.CompletedProcess[str], count: int = 4) -> str:
    log = (done.stderr or "") + (done.stdout or "")
    errors = [ln for ln in log.splitlines() if "error" in ln]
    return " | ".join((errors or log.splitlines())[-count:])


@dataclass
class Config:
    odin3: str
    yosys: str
    scripts: Path
    lib: Path
    lpm_script: Path
    cap: int
    abc: str = ""


def template_for(cfg: Config, level: str) -> str:
    if level == "lpm":
        return lpm_template(cfg.lpm_script.read_text(encoding="utf-8"))
    path = cfg.scripts / f"{level}.script"
    if not path.is_file():
        raise ToolError(f"no script for level {level}: {path}")
    return path.read_text(encoding="utf-8")


def read_back_script(cfg: Config, level: str, source: Path, outs: tuple[Path | None, Path]) -> str:
    """``read_verilog -icells`` of source, then proc, opt_clean (lower at the BLIF levels) and the
    writes: outs is (the gold-format file or None, the Verilog)."""
    lib = f"read_techlib {cfg.lib}; " if level == "lpm" else ""
    gold, verilog = outs
    lower = "" if level in RTLIL_LEVELS else "lower; "
    fmt = "write_rtlil" if level in RTLIL_LEVELS else "write_blif --vtr"
    write_gold = f"{lower}{fmt} {gold}" if gold else ""
    return (
        f"{lib}read_verilog -icells {source}; proc; opt_clean; stats; "
        f"write_verilog {verilog}{'; ' + write_gold if write_gold else ''}"
    )


def run_legs(cfg: Config, level: str, design: Path, work: Path) -> Verdict:
    ext = "il" if level in RTLIL_LEVELS else "blif"
    paths = {name: work / name for name in (f"G.{ext}", "W.v", f"R.{ext}", "W2.v", "W3.v")}
    subs = {
        "@DESIGN@": str(design),
        "@OUT@": str(paths["W.v"]),
        "@IL@": str(paths["G.il"] if ext == "il" else paths["G.blif"]),
        "@LIB@": str(cfg.lib),
        "@FAMILY@": "",
    }
    done = odin3_run(cfg.odin3, level_script(template_for(cfg, level), subs), work)
    if done.returncode != 0:
        raise ToolError(f"{design.name}: the {level} script failed: {last_lines(done)}")
    flags = ["--check", *top_flag((done.stderr or "") + (done.stdout or ""))]
    steps = (
        (paths["W.v"], (paths[f"R.{ext}"], paths["W2.v"])),
        (paths["W2.v"], (None, paths["W3.v"])),
    )
    logs: list[str] = []
    for source, outs in steps:
        done = odin3_run(cfg.odin3, read_back_script(cfg, level, source, outs), work, flags)
        if done.returncode != 0:
            return Verdict("fail:read", last_lines(done))
        logs.append((done.stderr or "") + (done.stdout or ""))
    equal = judge_equivalent(cfg, level, paths, design.stem + "." + level, work)
    return equal if equal.outcome != "ok" else judge_text(paths, census(logs[0]), census(logs[1]))


def run_one(cfg: Config, level: str, design: Path, work: Path) -> Verdict:
    """run_legs, with a script that cannot process the design as the outcome ``error:script``
    (a design a level cannot take is listed in the expect file, never skipped silently)."""
    try:
        return run_legs(cfg, level, design, work)
    except ToolError as exc:
        if "script failed" in str(exc):
            return Verdict("error:script", str(exc).split("script failed: ", 1)[-1][-240:])
        raise


def judge_blif(cfg: Config, paths: dict[str, Path]) -> Verdict:
    argv = [str(EQUIV_CHECK), str(paths["G.blif"]), str(paths["R.blif"])]
    env = {**os.environ, **({"ODIN3_ABC": cfg.abc} if cfg.abc else {})}
    try:
        done = subprocess.run(
            argv, capture_output=True, text=True, timeout=TIMEOUT, check=False, env=env
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise opt_equiv.CrashError("equiv-check", str(exc), True) from exc
    text = " | ".join(((done.stdout or "") + (done.stderr or "")).strip().splitlines()[-3:])
    if done.returncode == 0:
        return Verdict("ok")
    if done.returncode == 1:
        return Verdict("fail:equivalent", text)
    if done.returncode == EXIT_UNPROVEN:
        return Verdict("unproven", text)
    raise ToolError(f"equiv-check exit {done.returncode}: {text}")


def judge_equivalent(
    cfg: Config, level: str, paths: dict[str, Path], tag: str, work: Path
) -> Verdict:
    if level not in RTLIL_LEVELS:
        return judge_blif(cfg, paths)
    checker = opt_equiv.Checker(cfg.yosys, work, cfg.cap, True, True, None)
    read = paths["R.il"]
    read.write_text(normalise_readback(read.read_text(encoding="utf-8")), encoding="utf-8")
    gold = paths["G.il"]
    gold.write_text(promote_generated(gold.read_text(encoding="utf-8")), encoding="utf-8")
    for path in (gold, read):
        path.write_text(drop_open_pins(path.read_text(encoding="utf-8")), encoding="utf-8")
    try:
        ok, text, inconclusive = checker.judge(paths["G.il"], read, tag)
    except opt_equiv.CrashError as exc:
        if exc.size_limit:
            return Verdict("unproven", str(exc))
        raise
    except opt_equiv.ToolError as exc:  # the checker could not judge the pair (no verdict)
        return Verdict("error:check", str(exc)[-240:])
    if not ok and NO_SAT_MODEL in text:
        return Verdict("unproven", "Yosys has no SAT model of the cell (spec 20.2): " + text[-120:])
    if not ok:
        return Verdict("unproven" if inconclusive else "fail:equivalent", text[-240:])
    return Verdict("ok")


def judge_text(
    paths: dict[str, Path], gen2: dict[tuple[str, str], int], gen3: dict[tuple[str, str], int]
) -> Verdict:
    """The fixpoint and the instance legs on the written Verilog."""
    if gen2 != gen3:
        return Verdict(
            "fail:fixpoint", f"reading the read-back changed the cells: {gen2} -> {gen3}"
        )
    text2, text3 = (paths[n].read_text(encoding="utf-8") for n in ("W2.v", "W3.v"))
    problem = compare_instances(paths["W.v"].read_text(encoding="utf-8"), text2)
    if problem:
        return Verdict("fail:instances", problem)
    same = strip_comments(text2) == strip_comments(text3)
    return Verdict("ok", "text fixpoint" if same else "census fixpoint")


# --- the command line -------------------------------------------------------------------------


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n")[0])
    ap.add_argument("--level", required=True, help="word, gate, lut, lpm (comma separated)")
    ap.add_argument("--odin3", default=str(ROOT / "build" / "debug" / "odin3"))
    ap.add_argument("--yosys", default=os.environ.get("ODIN3_YOSYS055", "yosys"))
    ap.add_argument("--scripts", type=Path, default=ROOT / "tests" / "icells")
    ap.add_argument("--lib", type=Path, default=ROOT / "lib" / "lpm.o3lib")
    ap.add_argument("--lpm-script", type=Path, default=ROOT / "tests" / "lpm" / "lpm.script")
    ap.add_argument("--expect", type=Path)
    ap.add_argument("--work", type=Path, help="keep every file here")
    ap.add_argument("--abc", default=os.environ.get("ODIN3_ABC", ""), help="ABC for equiv-check")
    ap.add_argument("--mem-cap", default="4G")
    ap.add_argument("files", nargs="+", type=Path)
    return ap.parse_args(argv)


def execute(args: argparse.Namespace, work: Path) -> int:
    levels = args.level.split(",")
    bad = [lv for lv in levels if lv not in LEVELS]
    if bad:
        raise ToolError(f"unknown level {bad[0]} (want one of {', '.join(LEVELS)})")
    cap = opt_equiv.parse_size(args.mem_cap)
    cfg = Config(args.odin3, args.yosys, args.scripts, args.lib, args.lpm_script, cap, args.abc)
    table = parse_expect(args.expect.read_text(encoding="utf-8")) if args.expect else {}
    worst, unproven, compared, excepted, unproven_n = EXIT_OK, False, 0, 0, 0
    for level in levels:
        for number, design in enumerate(args.files):
            run_dir = work / f"{level}-{number:04d}"
            run_dir.mkdir(exist_ok=True)
            verdict = run_one(cfg, level, design.resolve(), run_dir)
            listed = table.get((level, design.name))
            passed, word = judge_expected(verdict, listed)
            detail = f" ({verdict.reason})" if verdict.reason else ""
            print(f"{TOOL}: {level} {design.name}: {word}{detail}", flush=True)
            compared += verdict.outcome in ("ok", "fail:equivalent")
            unproven_n += verdict.outcome == "unproven"
            excepted += listed is not None and passed
            if not passed and verdict.outcome == "unproven" and listed is None:
                unproven = True
            elif not passed:
                worst = EXIT_FAIL
    total = len(levels) * len(args.files)
    print(
        f"{TOOL}: compared {compared} of {total} ({unproven_n} unproven, {excepted} excepted)",
        flush=True,
    )
    return worst if worst else (EXIT_UNPROVEN if unproven else EXIT_OK)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    keep = args.work is not None
    work = Path(args.work).resolve() if keep else Path(tempfile.mkdtemp(prefix="icells-check-"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        code = execute(args, work)
    except opt_equiv.CrashError as exc:
        print(f"{TOOL}: HARD: {exc}", file=sys.stderr)
        print(f"{TOOL}: work directory kept: {work}", file=sys.stderr)
        return EXIT_CRASH
    except (OSError, ToolError, opt_equiv.ToolError, ValueError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        print(f"{TOOL}: work directory kept: {work}", file=sys.stderr)
        return EXIT_ERROR
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP
    if not keep and code in (EXIT_OK, EXIT_UNPROVEN):
        shutil.rmtree(work, ignore_errors=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
