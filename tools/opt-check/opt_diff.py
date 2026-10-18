"""opt-diff: Odin III's opt against Yosys 0.55's on the same input (2F spec 20.3; plan Task 17).

Usage::

    opt-diff --odin3 O --yosys Y [--top T] [--script S]... [--expect F] [--root R] [--techlib L]
             [--netlist-compare N] [--mem-cap C] [--work DIR] [--no-refine]
             [--refine-timeout S] [--verbose] FILE...

For each ``S`` (default ``phase2``; ``phase2``, ``phase3`` (Phase 2's lines then ``wreduce;
opt_clean; opt -full``), ``wreduce``, ``opt -full``, ``opt -fast``, ``opt -fast -noff``,
``opt -nodffe -nosdff``), per design (the FILEs together):

1. Odin III: ``read_verilog --nolatches FILE...; hierarchy; proc; opt_expr -keepdc; write_rtlil
   in.il; S --opt-stats; write_rtlil odin.il; stats``. For ``phase2``, which holds ``proc``,
   ``in.il`` is written before ``proc``: ``read_verilog ...; hierarchy; write_rtlil in.il;
   opt_expr; ...``;
2. Yosys 0.55: ``read_rtlil in.il; S; write_rtlil yosys.il; stat`` with ``YOSYS_COVER_DIR``. Its
   ``phase2`` is Parmys's lines without ``compact``: ``opt_expr; opt_clean; opt -nodffe -nosdff;
   proc -norom; opt`` (Parmys's ``procs -norom``);
3. **histogram**: the per-module cell-type counts of ``stats`` and ``stat`` are equal; for the
   scripts that hold ``wreduce`` also **widths**: the widths and signed flags of every ``$add``,
   ``$sub`` and ``$mul`` are equal as a multiset;
4. **refine**: ``yosys.il`` is refined by ``odin.il`` (``opt-equiv``'s check, Yosys's result as
   the gold), so Odin III resolved no x differently where Yosys kept it defined;
5. **identity**: both through Yosys ``hierarchy -auto-top; flatten; memory; techmap; opt -fast;
   dffunmap; opt -fast -noff; opt_clean -purge; write_blif -true + vcc -false + gnd -undef + unconn
   -blackbox`` (``memory`` is added to the spec's lowering: BLIF has no memory cell), then
   ``netlist-compare --implicit-models`` (exit 0; an exit 2, e.g. an inout with two drivers, is a
   difference of this step);
6. the rule counters of both, printed for a design with a difference (``--verbose``: always); no
   gate.

A difference fails unless ``--expect F`` lists it, except an **unproven refine** (the SAT proof hit
its size limit and the BLIF cec gave no verdict: text ``unproven: ...``), which no listing absorbs:
it is exit 4 below. Lines ``<path> <script> <step> <reason>``
(``#`` comments), ``path`` relative to ``--root`` (else the file name), ``script`` one of
``phase2 phase3 wreduce full fast fast-noff nodffe-nosdff`` or ``*``, ``step`` ``histogram``,
``widths``, ``refine`` or ``identity``. A listed difference that does not occur fails too (a
stale entry). The work directory this tool made is removed after a clean run; ``--work DIR`` and
a failing run keep it.

Exit codes (one convention across the opt tools): 0 no unexpected difference; 1 a difference, or a
stale entry; 2 usage, or Odin III or Yosys exited with an error; 3 hard: a child (Odin III, Yosys or
netlist-compare) crashed, was killed by a signal or its memory cap, or timed out (never an expected
difference: no expect line absorbs it); 4 unproven: no verdict exists on Odin III for some step and
nothing else failed. That is an unproven refine, or Yosys (its step-2 run or a lowering for the BLIF
steps) killed by the memory cap (a limit of the oracle, not a crash; an Odin III or netlist-compare
cap kill, and any timeout, stay exit 3). A sweep may list exit 4 as ``untested for now (size
limit)`` and must insist on it: **any other unlisted difference, in any script, makes the exit 1**,
so one unproven refine cannot hide another difference. Every script's result is printed before the
tool exits, whatever the code; a Yosys cap kill in one script does not stop the next scripts.
(Yosys killed by the host's OOM killer or an outside SIGKILL looks the same as the cap kill: exit
4.) 77 skipped (an ASan child and no systemd-run). The work directory this tool made (under
``$TMPDIR``) is removed after a clean run, after an error and after exit 4; a failure (1 or 3)
keeps it.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import memcap
import opt_equiv
import opt_rules

TOOL = "opt-diff"
HERE = Path(__file__).resolve().parent
NETLIST_COMPARE = HERE.parent / "netlist-compare" / "netlist-compare"
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_CRASH, EXIT_UNPROVEN, EXIT_SKIP = 0, 1, 2, 3, 4, 77
STEPS = ("histogram", "widths", "refine", "identity")
UNPROVEN_PREFIX = "unproven:"  # the refine difference text of a proof that reached its size limit
UNTESTED_REASON = "untested for now (size limit)"  # PHASE2 #29: what a reason may say only then
PHASE3 = HERE / "phase3.script"
WIDTH_SCRIPTS = ("wreduce", "phase3")  # the scripts that hold wreduce: widths are compared
ODIN_FRONT = "proc", "opt_expr -keepdc"
LOWERING = ("hierarchy -auto-top", "flatten", "memory", "techmap", "opt -fast", "dffunmap",
            "opt -fast -noff", "opt_clean -purge",
            "write_blif -true + vcc -false + gnd -undef + unconn -blackbox")
DEFAULT_CAP = 2 << 30
TIMEOUT_SECONDS = opt_equiv.TIMEOUT_SECONDS


@dataclass(frozen=True)
class Script:
    """One ``S``: its id (for the expect list), the Odin III and Yosys command lists, and whether
    it holds ``proc`` itself (then ``in.il`` is written before ``proc``)."""

    slug: str
    odin: tuple[str, ...]
    yosys: tuple[str, ...]
    trio: bool
    front: tuple[str, ...] = ODIN_FRONT  # what runs before in.il is written, when not trio


def _phase2(slug: str = "phase2", path: Path = opt_equiv.PHASE2) -> Script:
    commands = tuple(opt_equiv.phase2_commands(path))
    # Parmys runs `procs -norom` (Yosys's `proc` would also make ROMs: $meminit and $memrd_v2),
    # and `compact` is not a Yosys line.
    yosys = tuple("proc -norom" if c == "proc" else c for c in commands if c != "compact")
    return Script(slug, commands, yosys, True)


def _single(slug: str, command: str) -> Script:
    return Script(slug, (command,), (command,), False)


def script_named(name: str) -> Script:
    """The ``S`` a ``--script`` value names: ``phase2`` or one of the four ``opt`` spellings."""
    words = " ".join(name.split())
    if words == "phase2":
        return _phase2()
    if words == "phase3":  # Phase 2's lines, then wreduce; opt_clean; opt -full (3G WR-9)
        return _phase2("phase3", PHASE3)
    if words == "wreduce":  # wreduce alone, on what `proc; opt` made (Parmys's order)
        return Script("wreduce", ("wreduce",), ("wreduce",), False, (*ODIN_FRONT, "opt"))
    table = {"opt -full": "full", "opt -fast": "fast", "opt -fast -noff": "fast-noff",
             "opt -nodffe -nosdff": "nodffe-nosdff"}
    if words not in table:
        raise opt_equiv.ToolError(
            f"unknown script '{name}': phase2, phase3, wreduce or one of {sorted(table)}")
    return _single(table[words], words)


def odin_script(script: Script, files: Sequence[str], top: str | None, in_il: Path,
                odin_il: Path, techlib: str | None = None) -> str:
    """Step 1's Odin III ``-p`` script (``read_verilog --nolatches``, ruling I8)."""
    front = opt_equiv.read_commands(files, top, True, techlib)
    middle = [] if script.trio else list(script.front)
    body = opt_equiv.with_stats(script.odin)
    return "; ".join([*front, *middle, f"write_rtlil {in_il}", *body, f"write_rtlil {odin_il}",
                      "stats"])


def yosys_script(script: Script, in_il: Path, yosys_il: Path) -> str:
    """Step 2's Yosys 0.55 ``-p`` script."""
    return "; ".join([f"read_rtlil {in_il}", *script.yosys, f"write_rtlil {yosys_il}", "stat"])


def lowering_script(il: Path, blif: Path) -> str:
    """Step 5's common lowering of one side to BLIF."""
    steps = list(LOWERING[:-1]) + [f"{LOWERING[-1]} {blif}"]
    return "; ".join([f"read_rtlil {il}", *steps])


@dataclass(frozen=True)
class Expect:
    reason: str


def parse_expect(text: str) -> dict[tuple[str, str, str], Expect]:
    """``<path> <script> <step> <reason>`` lines; ``ToolError`` on a malformed one."""
    found: dict[tuple[str, str, str], Expect] = {}
    slugs = {"*", *(script_named(n).slug for n in (
        "phase2", "phase3", "wreduce", "opt -full", "opt -fast", "opt -fast -noff",
        "opt -nodffe -nosdff"))}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split(None, 3)
        if len(fields) != 4 or fields[1] not in slugs or fields[2] not in STEPS:
            raise opt_equiv.ToolError(
                f"expect line {number}: want '<path> <script> <step> <reason>' with script in "
                f"{sorted(slugs)} and step in {STEPS}, got '{raw.strip()}'")
        found[(fields[0], fields[1], fields[2])] = Expect(fields[3])
    return found


@dataclass
class Result:
    """The findings of one design under one script: step to difference text."""

    script: Script
    differences: dict[str, str] = field(default_factory=dict)
    odin_counters: dict[str, int] = field(default_factory=dict)
    yosys_counters: dict[str, int] = field(default_factory=dict)


class Diff:
    """One design through §20.3's steps."""

    def __init__(self, args: argparse.Namespace, work: Path) -> None:
        self.args = args
        self.work = work
        self.cap: int = args.mem_cap
        self.checker = opt_equiv.Checker(args.yosys, work, self.cap, top=args.top)
        self.techlib = str(Path(args.techlib).resolve()) if args.techlib else None

    def child(self, what: str, argv: Sequence[str], env: dict[str, str] | None = None
              ) -> subprocess.CompletedProcess[str]:
        """The child under memcap; a timeout, signal or cap kill is a ``CrashError`` naming
        ``what`` (exit 3)."""
        return opt_equiv.capped(what, argv, self.cap, cwd=self.work, env=env)

    def must(self, what: str, argv: Sequence[str], env: dict[str, str] | None = None) -> str:
        done = self.child(what, argv, env)
        log = (done.stdout or "") + (done.stderr or "")
        if done.returncode != 0:
            raise opt_equiv.ToolError(f"{what} exit {done.returncode}: {opt_equiv.why(log)}")
        return log

    def run(self, script: Script, files: Sequence[str]) -> Result:
        run_dir = self.work / script.slug
        run_dir.mkdir(exist_ok=True)
        in_il, odin_il, yosys_il = (run_dir / n for n in ("in.il", "odin.il", "yosys.il"))
        result = Result(script)
        top = self.args.top
        odin_log = self.must("odin3", [opt_equiv.exe(self.args.odin3),
                                       *(["--top", top] if top else []), "-p",
                                       odin_script(script, files, top, in_il, odin_il,
                                                   self.techlib)])
        cover = run_dir / "cover"
        cover.mkdir(exist_ok=True)
        yosys_log = self.must("yosys", [opt_equiv.exe(self.args.yosys), "-p",
                                        yosys_script(script, in_il, yosys_il)],
                              {**os.environ, "YOSYS_COVER_DIR": str(cover)})
        result.odin_counters = opt_equiv.parse_counters(odin_log)
        result.yosys_counters = yosys_cover(cover)
        self.compare(result, run_dir, (odin_log, yosys_log))
        return result

    def compare(self, result: Result, run_dir: Path, logs: tuple[str, str]) -> None:
        odin_hist = opt_rules.odin_histogram(logs[0])
        yosys_hist = opt_rules.yosys_histogram(logs[1])
        if not odin_hist or not yosys_hist:  # two empty histograms compare equal: no verdict
            side = "odin3" if not odin_hist else "yosys"
            raise opt_equiv.ToolError(f"no module found in the {side} statistics output")
        diff = opt_rules.histogram_diff(odin_hist, yosys_hist)
        if diff:
            result.differences["histogram"] = diff
        if result.script.slug in WIDTH_SCRIPTS:  # the widths and flags of every $add $sub $mul
            widths = opt_rules.signature_diff(
                (run_dir / "odin.il").read_text(encoding="utf-8"),
                (run_dir / "yosys.il").read_text(encoding="utf-8"), opt_rules.WORD_ARITH_TYPES)
            if widths:
                result.differences["widths"] = widths
        if not self.args.no_refine:  # --no-refine: the SAT step times out on the big designs
            ok, reason = self.refine(run_dir, result.script.slug)
            if not ok:
                result.differences["refine"] = reason
        ident = self.identity(run_dir)
        if ident:
            result.differences["identity"] = ident

    def refine(self, run_dir: Path, slug: str) -> tuple[bool, str]:
        """Step 4. A SAT proof that times out falls back to the BLIF cec of the two lowered
        netlists (``--no-cec``: it stays a hard timeout). The cec's "no verdict" is a difference,
        worded ``unproven``, never a pass."""
        limit = ""
        try:
            return self.checker.refines(run_dir / "yosys.il", run_dir / "odin.il", f"{slug}.refine")
        except opt_equiv.CrashError as exc:
            if not exc.size_limit or self.args.no_cec:
                raise
            limit = str(exc)
        blifs = self.lower(run_dir)
        verdict, text = opt_equiv.cec_blifs(blifs[0], blifs[1], self.cap, self.work)
        if verdict == opt_equiv.CEC_PROVED:
            return True, ""
        if verdict == opt_equiv.CEC_DIFFERS:
            return False, text
        return False, (f"unproven: the SAT proof hit its size limit ({limit}); {text}")

    def lower(self, run_dir: Path) -> list[Path]:
        """Both sides to BLIF (step 5's lowering); made once per run directory."""
        blifs = []
        for side in ("odin", "yosys"):
            blif = run_dir / f"{side}.blif"
            if not blif.is_file():
                self.must(f"yosys ({side} lowering)",
                          [opt_equiv.exe(self.args.yosys), "-p",
                           lowering_script(run_dir / f"{side}.il", blif)])
            blifs.append(blif)
        return blifs

    def identity(self, run_dir: Path) -> str:
        """Step 5: empty when the two lowered netlists are identical."""
        blifs = self.lower(run_dir)
        done = self.child("netlist-compare",
                          [self.args.netlist_compare, "--quiet", "--implicit-models", str(blifs[0]),
                           str(blifs[1])])
        if done.returncode == 0:
            return ""
        if done.returncode == 1:
            return "netlist-compare: not identical"
        raise opt_equiv.ToolError(  # not a verdict about the netlists: never an identity difference
            f"netlist-compare cannot compare them (exit {done.returncode}): "
            f"{opt_equiv.why((done.stdout or '') + (done.stderr or ''))}")


def yosys_cover(cover: Path) -> dict[str, int]:
    """The ``opt.*`` cover counts of a ``YOSYS_COVER_DIR``."""
    text = "".join(p.read_text(encoding="utf-8", errors="replace") for p in sorted(cover.iterdir()))
    counts = opt_rules.cover_counts(text)
    return {k: v for k, v in counts.items() if k.startswith("opt.") and v > 0}


def expected(key: str, slug: str, step: str, table: dict[tuple[str, str, str], Expect]
             ) -> Expect | None:
    return table.get((key, slug, step)) or table.get((key, "*", step))


def verdict(name: str, key: str, results: Sequence[Result],
            table: dict[tuple[str, str, str], Expect], verbose: bool, killed: bool = False
            ) -> tuple[list[str], bool, bool]:
    """The output lines of one design, whether it failed (a difference nobody listed, or a listed
    one that did not occur) and whether a refine step is unproven. An unproven refine is never
    absorbed by a listing and is not a failure: it is the exit 4 of a run where nothing else fails.
    A listed reason that says ``untested for now (size limit)`` is true only when some step has no
    verdict (``unproven``, or ``killed``: a script lost to the Yosys cap); otherwise the listing
    would count a design with nothing untested as a pass, which fails."""
    lines: list[str] = []
    failed = False
    unproven = False
    untested_listed: list[str] = []
    for res in results:
        tag = f"{TOOL}: {name} [{res.script.slug}]"
        for step in STEPS:
            reason = expected(key, res.script.slug, step, table)
            found = res.differences.get(step)
            if step == "refine" and found is not None and found.startswith(UNPROVEN_PREFIX):
                note = (f" (listed as '{reason.reason}', but a listing never makes an unproven "
                        "step a pass)" if reason is not None else "")
                lines.append(f"{tag}: refine: DIFFERS: {found}{note}")
                unproven = True
            elif found is None and reason is not None:
                lines.append(f"{tag}: {step}: stale entry in the expect list ({reason.reason})")
                failed = True
            elif found is not None and reason is None:
                lines.append(f"{tag}: {step}: DIFFERS: {found}")
                failed = True
            elif found is not None and reason is not None:
                lines.append(f"{tag}: {step}: differs as listed ({reason.reason})")
                if reason.reason.startswith(UNTESTED_REASON):
                    untested_listed.append(f"[{res.script.slug}] {step}")
        if not res.differences and not any(
                expected(key, res.script.slug, s, table) for s in STEPS):
            lines.append(f"{tag}: ok")
        if res.differences or verbose:
            lines.append(f"{tag}: counters odin3: {opt_equiv.counter_text(res.odin_counters)}; "
                         f"yosys: {opt_equiv.counter_text(res.yosys_counters)}")
    if untested_listed and not (unproven or killed):
        which = ", ".join(untested_listed)
        lines.append(f"{TOOL}: {name}: DIFFERS: {which} say '{UNTESTED_REASON}' but no step is "
                     "unproven: nothing is untested, so the listing would be a pass; say what is "
                     "true (a listing is never a pass)")
        failed = True
    return lines, failed, unproven


def rel_key(files: Sequence[str], root: Path | None) -> str:
    """The expect-list key of a design: the first file, relative to ``root`` when under it."""
    path = Path(files[0]).resolve()
    if root is not None and root.resolve() in path.parents:
        return str(path.relative_to(root.resolve()))
    return path.name


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--top", help="the top module")
    ap.add_argument("--script", action="append", default=[], metavar="S",
                    help="phase2 (default) or an opt spelling; repeatable")
    ap.add_argument("--expect", type=Path, help="tests/opt/diff_expect.txt")
    ap.add_argument("--techlib", help="read this tech library (.o3lib) first: the hard blocks")
    ap.add_argument("--root", type=Path, help="expect-list paths are relative to this directory")
    ap.add_argument("--netlist-compare", default=str(NETLIST_COMPARE), metavar="N")
    ap.add_argument("--mem-cap", type=opt_equiv.parse_size, default=DEFAULT_CAP, metavar="SIZE",
                    help="per child (default 2G)")
    ap.add_argument("--work", type=Path, help="the work directory (kept)")
    ap.add_argument("--no-refine", action="store_true",
                    help="leave the refinement step out (it times out on the large designs)")
    ap.add_argument("--refine-timeout", type=float, default=opt_equiv.REFINE_TIMEOUT_SECONDS,
                    metavar="S", help="seconds for one refinement proof (default 60)")
    ap.add_argument("--no-cec", action="store_true",
                    help="no BLIF cec when the SAT proof times out (the timeout stays hard)")
    ap.add_argument("--verbose", action="store_true", help="print the counters of every design")
    ap.add_argument("files", nargs="+", help="the Verilog files of one design")
    return ap.parse_args(argv)


def execute(args: argparse.Namespace, work: Path) -> int:
    files = [str(Path(f).resolve()) for f in args.files]
    scripts = [script_named(n) for n in (args.script or ["phase2"])]
    table = parse_expect(args.expect.read_text(encoding="utf-8")) if args.expect else {}
    diff = Diff(args, work)
    name, key = Path(files[0]).name, rel_key(files, args.root)
    results: list[Result] = []
    killed: list[tuple[Script, opt_equiv.CrashError]] = []
    for script in scripts:
        try:
            results.append(diff.run(script, files))
        except opt_equiv.CrashError as exc:
            if not yosys_cap_kill(exc):
                show(verdict(name, key, results, table, args.verbose)[0])  # keep what was found
                raise
            killed.append((script, exc))  # no verdict on this script; the next ones still run
        except opt_equiv.ToolError:
            show(verdict(name, key, results, table, args.verbose)[0])
            raise
    lines, failed, unproven = verdict(name, key, results, table, args.verbose, bool(killed))
    show([*lines, *(f"{TOOL}: {name} [{s.slug}]: unproven (no verdict): {e}" for s, e in killed)])
    for _, kill in killed:
        print(f"{TOOL}: unproven: {kill}", file=sys.stderr)
    if failed:
        return EXIT_FAIL
    return EXIT_UNPROVEN if unproven or killed else EXIT_OK


def show(lines: Sequence[str]) -> None:
    for line in lines:
        print(line, flush=True)


def yosys_cap_kill(exc: opt_equiv.CrashError) -> bool:
    """Yosys's own step-2 run or a BLIF lowering was killed by the memory cap: the oracle is too
    big for the cap, which says nothing about Odin III (exit 4). The refinement child
    (``yosys-refine``), Odin III, netlist-compare and every timeout stay hard."""
    return exc.cap_killed and (exc.what == "yosys" or exc.what.startswith("yosys ("))


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    opt_equiv.REFINE_TIMEOUT_SECONDS = args.refine_timeout
    made = args.work is None
    work = Path(tempfile.mkdtemp(prefix="opt-diff-")) if made else args.work
    work.mkdir(parents=True, exist_ok=True)
    work = work.resolve()
    code = EXIT_ERROR
    try:
        code = execute(args, work)
    except opt_equiv.CrashError as exc:
        print(f"{TOOL}: HARD: {exc}", file=sys.stderr)
        code = EXIT_CRASH
    except (OSError, opt_equiv.ToolError, ValueError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        code = EXIT_SKIP
    if made and code in (EXIT_OK, EXIT_ERROR, EXIT_UNPROVEN, EXIT_SKIP):
        shutil.rmtree(work, ignore_errors=True)  # the directory this run made, and only it
    elif made:
        print(f"{TOOL}: work directory kept: {work}", file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
