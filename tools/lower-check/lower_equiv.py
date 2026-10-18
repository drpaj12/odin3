"""lower-equiv: ``lower`` refines its input, proved in Yosys 0.55 (3B spec 14.4; plan Task 9).

Usage::

    lower-equiv --odin3 O --yosys Y [--top T] [--pre CMD]... [--lower-args A] [--techlib L]
                [--parmys-techlibs DIR] [--parmys-exact] [--expect F] [--root R] [--mem-cap C]
                [--work DIR] [--each] FILE...

Per design (the FILEs together; with ``--each``, every FILE is its own design):

1. Odin III: ``read_verilog --nolatches FILE...; hierarchy; <pre>; lower --lower-snapshot DIR
   <A>`` (``pre`` defaults to ``proc; opt``), which writes ``lower-before.il`` (``write_rtlil`` of
   the design as ``lower`` found it) and ``lower-after.blif`` (``write_blif`` of the bit cells);
2. gold: ``read_rtlil lower-before.il`` with the declared synchronous-conversion maps of spec 6.3
   (``maps/``, the asynchronous flip-flops of decision 19) applied, so the divergence of decision
   19 is the *specification*, not a failure; gate: ``read_blif -wideports lower-after.blif``;
3. Yosys 0.55 proves the gate refines the gold with 2F's refinement script (``opt_equiv.Checker``:
   ``equiv_make gold gate eq; equiv_simple -undef -seq 4; equiv_induct -undef; equiv_status
   -assert``) and its clock-aware lowering family (``async2sync``, ``techmap``, ``memory``,
   ``clk2fflogic``): an x in the gold is a don't-care, so ``$shiftx``, ``$pmux`` with several
   selects and out-of-range ``$mem`` reads are judged correctly, and a flipped edge, a wrong
   clock net or a wrong function is a counterexample.

A refinement that the SAT flow cannot decide inside 60 s is ``unproven`` (exit 4), never ``ok``.
A difference fails unless ``--expect F`` lists it: lines ``<path> refine <reason>`` (``#``
comments), ``path`` relative to ``--root`` (else the file name); a listed difference that does not
occur fails too (a stale entry). A design whose snapshot holds no word cell compared nothing:
``NOTHING TO COMPARE`` (exit 1). The last line of every run is ``lower-equiv: compared N of M
designs (U unproven, E empty)``: N counts the designs with a verdict, proved or not a refinement
(the corpus sweeps sum it and fail below a floor, 3B-4 review H3).

Exit codes (one convention across the opt tools): 0 every design refines; 1 a design does not,
compared nothing, or a stale entry; 2 usage, or Odin III or Yosys exited with an error; 3 hard: a
child crashed, was killed by a signal or its memory cap, or timed out (never absorbed by an expect
list); 4 unproven (a SAT size limit; no design failed); 77 skipped (an ASan child and no
systemd-run). The work directory is made under ``$ODIN3_LOWER_WORK`` (else ``$TMPDIR``) and is
never removed.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import lower_common as lc
import lower_diff as ld
import memcap
import opt_equiv

TOOL = "lower-equiv"
EXIT_UNPROVEN = 4
BEFORE_NAME = "lower-before.il"
AFTER_NAME = "lower-after.blif"
TOP_LINE = "attribute \\top 1\n"
# The word cell types that the ``maps/`` flip-flop conversions rewrite (spec 6.3): only a design
# that holds one needs the techmap step.
MAPPED_TYPES = frozenset({"$adff", "$adffe", "$aldff", "$aldffe", "$dffsr", "$dffsre"})


@dataclass
class Verdict:
    """One design: ``ok``, ``differs`` (``detail`` is Yosys's last lines), ``unproven`` or
    ``empty`` (nothing to compare)."""

    kind: str
    detail: str = ""
    cells: int = 0


def snapshot_script(files: Sequence[str], top: str | None, techlib: str | None,
                    pre: Sequence[str], lower_args: str, snap: Path) -> str:
    """Step 1's Odin III ``-p`` script."""
    front = lc.read_front(files, top, techlib, True)
    lower = f"lower --lower-snapshot {lc.quote(str(snap))} {lower_args}".rstrip()
    return "; ".join([*front, *pre, lower])


def snapshot(odin3: str, script: str, top: str | None, snap: Path, cap: int) -> tuple[Path, Path]:
    """Runs Odin III in ``snap``; (before.il, after.blif). ``ToolError`` when it fails or wrote no
    file."""
    done = lc.capped("odin3", [lc.exe(odin3), *lc.odin_args(top), "-p", script], cap, cwd=snap)
    log = (done.stdout or "") + (done.stderr or "")
    if done.returncode != 0:
        raise lc.ToolError(f"odin3 exit {done.returncode}: {lc.why(log)}")
    before, after = snap / BEFORE_NAME, snap / AFTER_NAME
    for path in (before, after):
        if not path.is_file() or not path.read_text(encoding="utf-8").strip():
            raise lc.ToolError(f"odin3 wrote no {path.name}: {lc.why(log)}")
    return before, after


def synchronous_maps(parmys: Path | None, exact: bool) -> list[Path]:
    """The declared-reference maps of spec 6.3 (``--parmys-exact``: Parmys's enable-over-reset)."""
    names = [ld.PARMYS_EXACT.get(name, name) if exact else name for name in ld.FF_MAPS]
    return [ld.resolve_map(name, parmys) for name in names]


def run_yosys(yosys: str, script: str, work: Path, cap: int) -> None:
    done = lc.capped("yosys", [lc.exe(yosys), "-p", script], cap, cwd=work)
    if done.returncode != 0:
        raise lc.ToolError(f"yosys exit {done.returncode}: "
                           f"{lc.why((done.stdout or '') + (done.stderr or ''))}")


def make_gold(yosys: str, before: Path, parmys: tuple[Path | None, bool], work: Path, cap: int
              ) -> Path:
    """The gold: ``before`` with the maps applied when it holds an asynchronous cell (else a copy
    of it). Written to ``gold.il``. ``parmys`` = (Parmys's techlibs directory, exact)."""
    text = before.read_text(encoding="utf-8")
    gold = work / "gold.il"
    if not lc.cell_types(text) & MAPPED_TYPES:
        gold.write_text(text, encoding="utf-8")
        return gold
    maps = synchronous_maps(*parmys)
    steps = [f"read_rtlil {before}", *(f"techmap -map {m}" for m in maps), f"write_rtlil {gold}"]
    run_yosys(yosys, "; ".join(steps), work, cap)
    return gold


PORT_DECL = re.compile(
    r"^  wire (?P<mid>.*?)\s*(?P<dir>input|output) (?P<num>\d+) (?P<name>\\\S+)$")
WIDTH_WORD = re.compile(r"\bwidth (\d+)")
OFFSET_WORD = re.compile(r"\boffset (-?\d+)")
END_LINE = "\nend\n"


def module_span(text: str, top: str) -> tuple[int, int]:
    """(start, end) offsets of the body of module ``top`` in an RTLIL text (after its header
    line, up to its ``end``); (-1, -1) when it is absent."""
    head = f"\nmodule {top}\n"
    start = text.find(head)
    if start < 0:
        return -1, -1
    start += len(head)
    return start, text.index(END_LINE, start) + 1


def port_shapes(body: str) -> dict[str, tuple[str, str, int, int, str]]:
    """name -> (direction, port number, width, offset, the wire's flags) of a module's ports."""
    shapes: dict[str, tuple[str, str, int, int, str]] = {}
    for line in body.splitlines():
        found = PORT_DECL.match(line)
        if found is None:
            continue
        mid = found.group("mid")
        width = WIDTH_WORD.search(mid)
        offset = OFFSET_WORD.search(mid)
        shapes[found.group("name")] = (found.group("dir"), found.group("num"),
                                       int(width.group(1)) if width else 1,
                                       int(offset.group(1)) if offset else 0, mid)
    return shapes


def rename_signal(line: str, old: str, new: str) -> str:
    """``line`` with the signal token ``old`` replaced by ``new``. Only the signal operands of a
    ``connect`` are touched (never a cell's name, type or port names)."""
    stripped = line.lstrip()
    if not stripped.startswith("connect "):
        return line
    indent = line[:len(line) - len(stripped)]
    words = stripped.split(" ")
    first = 2 if len(indent) >= len("    ") else 1  # a cell's `connect \PORT sig`: sig from 2
    pattern = re.compile(rf"(?<!\S){re.escape(old)}(?!\S)")
    tail = pattern.sub(lambda _: new, " ".join(words[first:]))
    return indent + " ".join(words[:first]) + " " + tail


def bridge_port(body: str, name: str, gold: tuple[str, str, int, int, str]) -> str:
    """``body`` (a gate module) with its wide port ``name`` re-exposed with the gold's shape (low
    index and width), or unchanged when it already has that shape or lacks one of the bits (a
    missing bit is a real difference: the port match then reports it)."""
    direction, number, width, offset, flags = gold
    mine = port_shapes(body).get(name)
    if mine is None or mine[2:4] == (width, offset) or mine[0] != direction:
        return body
    low = offset - mine[3]  # the gate's raw index of the gold's lowest bit
    if low < 0 or low + width > mine[2]:
        return body
    wide = f"{name}$wide"
    rewritten = []
    for line in body.splitlines():
        if PORT_DECL.match(line) and line.endswith(f" {name}"):
            line = f"  wire width {mine[2]} offset {mine[3]} {wide}" if mine[3] else \
                f"  wire width {mine[2]} {wide}"
        else:
            line = rename_signal(line, name, wide)
        rewritten.append(line)
    span = f"[{low + width - 1}:{low}]" if width > 1 else f"[{low}]"
    here, there = (name, f"{wide} {span}") if direction == "output" else (f"{wide} {span}", name)
    rewritten += [f"  wire {flags} {direction} {number} {name}", f"  connect {here} {there}"]
    return "\n".join(rewritten) + "\n"


def normalise_ports(gate: str, gold: str, top: str) -> str:
    """The gate's module ``top`` with every port given the gold's width and low index.

    ``read_blif -wideports`` makes a width-8 port ``leds`` of the bits ``leds[1]``..``leds[7]``
    of a gold ``output [7:1] leds``; the equivalence flow matches ports by name, width and offset,
    so a correct lowering would be reported as a difference. Each wide port is kept as
    ``leds$wide`` and bridged to a new port of the gold's shape (bit for bit)."""
    g_start, g_end = module_span(gold, top)
    start, end = module_span(gate, top)
    if g_start < 0 or start < 0:
        return gate
    body = gate[start:end]
    for name, shape in port_shapes(gold[g_start:g_end]).items():
        body = bridge_port(body, name, shape)
    return gate[:start] + body + gate[end:]


def make_gate(yosys: str, after: Path, gold: tuple[Path, str], work: Path, cap: int) -> Path:
    """The gate: Yosys's reading of the BLIF as RTLIL (``gate.il``), the module ``top`` marked and
    its ports in the gold's shape. ``gold`` = (gold.il, the top module's RTLIL name)."""
    gate = work / "gate.il"
    script = f"read_blif -wideports {after}; opt_clean -purge; write_rtlil {gate}"
    run_yosys(yosys, script, work, cap)
    text = gate.read_text(encoding="utf-8")
    top = gold[1]
    marker = f"\nmodule {top}\n"
    if marker not in text:
        raise lc.ToolError(f"the lowered BLIF has no module {top[1:]}")
    text = normalise_ports(text, gold[0].read_text(encoding="utf-8"), top)
    gate.write_text(text.replace(marker, f"\n{TOP_LINE}module {top}\n", 1), encoding="utf-8")
    return gate


def cec_gold_and_blif(yosys: str, gold: Path, after: Path, work: Path, cap: int) -> tuple[str, str]:
    """The BLIF cec (``equiv-check``) of the gold, lowered to bits by Yosys, against the lowered
    BLIF as Odin III wrote it (with the bit gates' models appended): (verdict, text)."""
    gold_blif = work / "lower.gold.blif"
    script = opt_equiv.cec_lowering_script(gold, gold_blif)
    done = lc.capped("yosys (cec lowering)", [lc.exe(yosys), "-p", script], cap, cwd=work)
    if done.returncode != 0:
        return opt_equiv.CEC_UNPROVEN, "BLIF cec: the gold's lowering failed: " + lc.why(
            (done.stdout or "") + (done.stderr or ""))
    gate_blif = work / "lower.gate.blif"
    gate_blif.write_text(lc.with_gate_models(after.read_text(encoding="utf-8")), encoding="utf-8")
    return cec_blifs(gold_blif, gate_blif, cap, work)


def cec_blifs(gold: Path, gate: Path, cap: int, work: Path) -> tuple[str, str]:
    """``equiv-check`` with undriven nets (the x of a memory read past its words) tied to 0. A
    "proved" is a proof: the gate equals one resolution of the gold's x, and every resolution is a
    legal behaviour of the gold. A "differs" with x in either side is not a counterexample
    (another resolution might match), so it is reported as a difference of the cec only."""
    try:
        done = lc.capped("equiv-check", [lc.exe(str(opt_equiv.EQUIV_CHECK)), "--allow-undriven",
                                         str(gold), str(gate)], cap, cwd=work,
                         timeout=opt_equiv.CEC_TIMEOUT_SECONDS)
    except lc.CrashError as exc:
        return opt_equiv.CEC_UNPROVEN, f"BLIF cec: {exc}"
    text = lc.why((done.stdout or "") + (done.stderr or ""))
    if done.returncode == 0:
        return opt_equiv.CEC_PROVED, ""
    if done.returncode == 1:  # (with x tied to 0 a false alarm is possible; the SAT flow failed)
        return opt_equiv.CEC_DIFFERS, f"BLIF cec: not equivalent: {text}"
    return opt_equiv.CEC_UNPROVEN, f"BLIF cec: no verdict (exit {done.returncode}): {text}"


def refine(yosys: str, gold: Path, gate: Path, after: Path, work: Path, cap: int,
           top: str | None) -> tuple[str, str]:
    """("ok" | "differs" | "unproven", detail) for gate against gold. A crash or a tool error
    propagates (never a verdict)."""
    checker = opt_equiv.Checker(yosys, work, cap, True, True, top)
    try:
        ok, last = checker.refines(gold, gate, "lower")
    except opt_equiv.RefineAbort as exc:
        if PORT_MISMATCH.search(str(exc)):  # a port the gate lacks is a gate that does not refine
            return "differs", str(exc)
        raise
    except lc.CrashError as exc:
        if exc.size_limit:  # a SAT timeout or a checker cap kill: no verdict (opt-equiv's rule)
            return "unproven", str(exc)
        raise
    if ok:
        return "ok", ""
    # The SAT flow failed. Its induction cannot pair the registers of a memory (the gold's words
    # and the gate's flip-flops share no names), so a refinement it cannot prove is judged again by
    # the BLIF cec of opt-equiv (both sides lowered to bit level, equiv-check). A cec "proved" is a
    # proof; "differs" confirms; "unproven" (the checker's limits) is "unproven"
    # failure, which is the conservative verdict.
    verdict, text = cec_gold_and_blif(yosys, gold, after, work, cap)
    if verdict == opt_equiv.CEC_PROVED:
        return "ok", "proved by the BLIF cec (the SAT flow could not pair the registers)"
    if verdict == opt_equiv.CEC_DIFFERS:
        return "differs", f"{last} | {text}"
    if "$mem" not in lc.cell_types(gold.read_text(encoding="utf-8")):
        return "differs", last  # without a memory the SAT flow's failed proof is the verdict
    return "unproven", f"the SAT flow failed ({last.strip()[-120:]}) and {text}"


def check_pair(yosys: str, before: Path, after: Path, work: Path, cap: int,
               options: tuple[str | None, Path | None, bool]) -> Verdict:
    """Steps 2 and 3 on a snapshot pair. ``options`` = (top, parmys techlibs, parmys-exact)."""
    top, parmys, exact = options
    cells = ld.word_cells(before.read_text(encoding="utf-8"))
    if cells == 0:
        return Verdict("empty")
    gold = make_gold(yosys, before, (parmys, exact), work, cap)
    gold_top = opt_equiv.top_module(gold.read_text(encoding="utf-8"), top)
    gate = make_gate(yosys, after, (gold, gold_top), work, cap)
    kind, detail = refine(yosys, gold, gate, after, work, cap, top)
    return Verdict(kind, detail, cells)


def check_design(args: argparse.Namespace, files: Sequence[str], run_dir: Path) -> Verdict:
    techlib = str(Path(args.techlib).resolve()) if args.techlib else None
    pre = args.pre if args.pre else list(ld.DEFAULT_PRE)
    snap = Path(tempfile.mkdtemp(prefix="snap-", dir=run_dir))  # fresh: never a stale snapshot
    script = snapshot_script(files, args.top, techlib, pre, args.lower_args, snap)
    before, after = snapshot(args.odin3, script, args.top, snap, args.mem_cap)
    parmys = Path(args.parmys_techlibs) if args.parmys_techlibs else ld.vtr_techlibs()
    return check_pair(args.yosys, before, after, run_dir, args.mem_cap,
                      (args.top, parmys, args.parmys_exact))


PORT_MISMATCH = re.compile(r"Can't match (?:gold|gate) port")
EXPECT_LINE = re.compile(r"^(\S+)\s+refine\s+(\S.*)$")


def parse_expect(text: str) -> dict[str, str]:
    """``<path> refine <reason>`` lines; ``ToolError`` on a malformed one."""
    found: dict[str, str] = {}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        match = EXPECT_LINE.match(line)
        if match is None:
            raise lc.ToolError(f"expect line {number}: want '<path> refine <reason>', got "
                               f"'{raw.strip()}'")
        found[match.group(1)] = match.group(2)
    return found


def report_line(name: str, key: str, verdict: Verdict, table: dict[str, str],
                allow_empty: bool = False) -> tuple[str, int]:
    """(output line, exit code contribution: 0, 1 or 4) of one design."""
    tag = f"{TOOL}: {name}"
    listed = table.get(key)
    if verdict.kind == "ok":
        if listed is not None:
            return f"{tag}: stale entry in the expect list ({listed})", lc.EXIT_FAIL
        return f"{tag}: ok ({verdict.cells} cells)", lc.EXIT_OK
    if verdict.kind == "differs" and listed is not None:
        return f"{tag}: not a refinement, as listed ({listed})", lc.EXIT_OK
    if verdict.kind == "differs":
        return f"{tag}: NOT A REFINEMENT: {verdict.detail}", lc.EXIT_FAIL
    if verdict.kind == "unproven":
        return f"{tag}: UNPROVEN: {verdict.detail}", EXIT_UNPROVEN
    if allow_empty:
        return f"{tag}: nothing to compare (no word cell; allowed)", lc.EXIT_OK
    return f"{tag}: NOTHING TO COMPARE: the snapshot holds no word cell (folded away?)", \
        lc.EXIT_FAIL


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--top", help="the top module")
    ap.add_argument("--lower-args", default="", metavar="A", help="arguments of the lower pass")
    ap.add_argument("--pre", action="append", metavar="CMD",
                    help="a command between hierarchy and lower (repeatable; default proc, opt)")
    ap.add_argument("--expect", type=Path, help="designs that are expected not to refine")
    ap.add_argument("--root", type=Path, help="expect-list paths are relative to this directory")
    ap.add_argument("--techlib", help="read this tech library (.o3lib) first")
    ap.add_argument("--parmys-techlibs", metavar="DIR",
                    help="Parmys's techlibs (default $VTR_ROOT/parmys/parmys-plugin/techlibs)")
    ap.add_argument("--parmys-exact", action="store_true",
                    help="Parmys's unmodified enable-over-reset maps (spec 16 row 4)")
    ap.add_argument("--mem-cap", type=lc.parse_size, default=lc.DEFAULT_CAP, metavar="SIZE",
                    help="per child (default 3G)")
    ap.add_argument("--work", type=Path, help="the work directory")
    ap.add_argument("--allow-empty", action="store_true",
                    help="a design with no word cell after the pre commands is not a failure")
    ap.add_argument("--each", action="store_true", help="every FILE is its own design")
    ap.add_argument("files", nargs="+", help="the Verilog (or BLIF) files")
    return ap.parse_args(argv)


def execute(args: argparse.Namespace, work: Path) -> int:
    files = [str(Path(f).resolve()) for f in args.files]
    designs = [[f] for f in files] if args.each else [files]
    table = parse_expect(args.expect.read_text(encoding="utf-8")) if args.expect else {}
    ld.check_yosys(args.yosys, work, args.mem_cap)
    worst = lc.EXIT_OK
    unproven = False
    kinds: list[str] = []
    for number, design in enumerate(designs):
        run_dir = work / f"d{number:04d}"
        run_dir.mkdir(exist_ok=True)
        verdict = check_design(args, design, run_dir)
        line, code = report_line(Path(design[0]).name, ld.rel_key(design, args.root), verdict,
                                 table, args.allow_empty)
        print(line, flush=True)
        kinds.append(verdict.kind)
        unproven = unproven or code == EXIT_UNPROVEN
        worst = lc.EXIT_FAIL if code == lc.EXIT_FAIL else worst
    compared = sum(kind in ("ok", "differs") for kind in kinds)
    print(f"{TOOL}: compared {compared} of {len(kinds)} designs ({kinds.count('unproven')} "
          f"unproven, {kinds.count('empty')} empty)", flush=True)
    return worst if worst else (EXIT_UNPROVEN if unproven else lc.EXIT_OK)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    work = lc.make_work(args.work, "lower-equiv-")
    try:
        return execute(args, work)
    except lc.CrashError as exc:
        print(f"{TOOL}: HARD: {exc}", file=sys.stderr)
        print(f"{TOOL}: work directory kept: {work}", file=sys.stderr)
        return lc.EXIT_CRASH
    except (OSError, lc.ToolError, ValueError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return lc.EXIT_ERROR
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return lc.EXIT_SKIP


if __name__ == "__main__":
    sys.exit(main())
