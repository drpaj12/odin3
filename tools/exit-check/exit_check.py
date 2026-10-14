"""exit-check: the Phase 2 exit-test harness (2H; PHASE2 #4 and decision 13's exit-bar ruling).

Usage::

    exit-check --odin3 O --yosys Y --arch A [--sp-splice S] [--netlist-compare N]
               [--equiv-check E] [--golden-root D] [--techlib L] [--primitives P]
               [--vtr-config-template X | --config C | --no-config]
               [--expect-fail F]... [--expect-unproven F]... [--expect-no-golden F]...
               [--expect-differs F]...
               [--eqx-record F]... [--min-checked N] [--vtr-root V] [--jobs J] [--mem-cap C]
               [--cap-for SUFFIX=C]...
               [--equiv-timeout S] [--equiv-box-budget S] [--work DIR] [--root R] [--limit N]
               [--partial]
               <dir or file>...

Every ``.v`` under the directories (and the files) given is one design, run **smallest first** (all
together, ties by relative path; ``--limit N`` keeps the N smallest that have a Parmys golden). For
each design:

1. its Parmys golden is found: ``D/regression/verilog/<group>/<leaf>/<leaf>.parmys.blif`` for a
   micro under ``micro/verilog`` (``micro-top``'s rule), ``D/vtr/<name>/<name>.parmys.blif`` for a
   VTR benchmark under ``benchmarks/verilog``; ``D`` defaults to the k6_frac tree of
   ``micro-top``. A design whose golden does not exist and whose oracle run left no ``.prov`` at
   all (the oracle never ran on it: the 41 Koios designs have no k6_frac golden) is **skipped (no
   golden)**, counted apart (a ``no-golden`` bucket that is never in a pass count) and no failure;
   one with a ``.prov`` is checked against it (below): status ``failed`` is a **reject** (Parmys
   itself rejects the design, or an oracle harness gap listed as ``no-golden``), anything else
   without a golden BLIF is an ``error``;
2. ``sp-splice`` (Odin III's front end through ``opt`` and ``proc``, then Yosys 0.55 with Parmys
   for the rest of VTR's script) runs with the same ``--arch`` and ``--top`` (the golden's first
   ``.model``) and the same Parmys config as the goldens' run (VTR's
   ``basic_odin_config_split.xml``, filled per design by ``write_parmys_config``: a fixed hard
   ``multiply``, memories split to the arch's address width, min multiplier size 3, adder
   threshold 1), and the macros the golden's ``.prov`` records as ``defines=`` (``run-oracle.sh
   --define``); it caps its own children at ``--mem-cap``;
3. ``netlist-compare --implicit-models`` golden vs output (Parmys leaves a ``$pow`` and the like
   as a ``.subckt`` with no model): exit 0 is ``identical``;
4. else ``equiv-check --x-report --match-unnamed`` (Parmys writes no ``.cname`` for a hard block
   and numbers its output nets with counters, ``$mul~14[0]``, so equiv-check pairs the blocks by
   simulation signature, not by name) golden vs output, classified by its exit code: 0
   ``equivalent``; 3 ``eq-x`` (equivalent outside the x-cones; the removed outputs are listed);
   1 ``differs``, but only when its output carries the line ``equiv-check: NOT equivalent`` (an
   exit 1 without it, a Python crash or a failing ``prlimit``, is an ``error``, ``hard`` when it
   is a traceback); 4 ``unproven`` (ABC timed out, or the check failed only where black-box pairs
   were guessed: a tool limit); 5 ``hard`` (ABC died of a signal or equiv-check ran out of
   memory); anything else ``error``. Each of these children runs under ``memcap.run``, and so
   does sp-splice itself, with the process-group-killing timeout: a timeout leaves no orphaned
   odin3 or Yosys child.

Every golden is first checked against its ``.prov``: ``vtr_commit`` against the VTR checkout
(``--vtr-root``, else ``$VTR_ROOT``; its tree must be clean and the golden's ``vtr_dirty_files``
0), ``arch_sha256`` against ``--arch``, ``source_sha256`` against the design (a comment-only
difference, ``// elab-check:`` lines and trailing blanks, is accepted when the upstream file named
by ``source=`` still has the recorded hash) and, for a golden BLIF, ``blif_sha256`` against the
file. A mismatch is an ``error`` that says so: the comparison would be against a golden that is
not this design's, this arch's or this VTR's.

Result classes (decision 13; the Phase 2 exit claim is "micros + VTR identical/equivalent to
Parmys"):

=============== ======= ===============================================================
class           passes  meaning
=============== ======= ===============================================================
identical       yes     netlist-compare: the same netlist
equivalent      yes     equiv-check proved it
eq-x            record  equivalent outside x-cones; passes only when ``--eqx-record`` lists
                        the design with exactly the outputs observed (count, hash, buses)
listed-reject   yes     Parmys itself rejects the design (``--expect-fail`` + needle)
unproven        no      ABC/SAT timeout or an inconclusive guess; exit 0 only when every such
                        design is in ``--expect-unproven`` (printed in its own bucket)
no-golden       no      no golden, or an oracle failure that is a harness gap (missing include
                        or ``-D``) or a size limit (the Parmys run killed at its memory cap,
                        ``--expect-no-golden`` + needle); own bucket, never counted
differs         no      equiv-check's NOT-equivalent verdict; exit 0 only for a design in
                        ``--expect-differs`` (KNOWN DIFFERS: the Parmys netlist is itself wrong,
                        reviewed; own bucket, never a pass)
error           no      a tool or usage error
hard            no      a crash, a signal, a memory-cap kill or our own timeout; never listable
=============== ======= ===============================================================

``--expect-fail`` (listed-reject), ``--expect-no-golden`` and ``--expect-unproven`` take
elab-sweep's list: ``<relative path> [needle] <reason>``; the path is relative to ``--root``
(else the directory argument, else the file name); the needle must appear in the design's log.
``--eqx-record`` takes the eq-x record (``design <path> <count> <sha256> <reason>`` and
``bus <path> <name> <count>`` lines, see ``tests/exit``). An entry whose design now passes
(XPASS), a needle that does not match, an eq-x whose outputs differ from the record, and
(without ``--partial``) an entry that names no design fail the run. ``error`` and ``hard`` are
never listable; ``differs`` only by ``--expect-differs``, whose every entry must carry the
evidence that the Parmys golden is the wrong side (an independent reference, not Odin III's say-so).

The run also fails when fewer than ``--min-checked N`` designs (default 1) were proven
(identical + equivalent + recorded eq-x): a wrong or empty golden tree cannot report green. A
``--golden-root`` that is not a directory is a usage error.

Assumptions the exit statement carries (equiv-check's, unchanged here): a latch init of 2 or 3
(don't care, unknown) is read as 0, so an Odin III init 0 against a Parmys init 2 is accepted; RAM
contents and initial state are not modelled (a paired RAM is a cut point whose input pins are
compared) and the two ports of a ``dual_port_ram`` are interchangeable (Parmys numbers them in a
hash-table order, so a same-address write race may go either way); ``--odin3-pre``/
``--scc-precheck`` are off, so a design with a combinational loop that Odin III accepts and
Parmys rejects has a ``failed`` golden and is a listed-reject, not an error;
"identical" is identical under Odin III's ``--vtr-compat``.

Output: one line per design as it finishes, then a summary table, every cause that is not a pass,
and a totals line. ``--work DIR`` (else a fresh temporary directory, printed and left in place)
holds ``NNNN-<design>/exit-check.log`` per design with every child's output, beside the splice's
own files. Nothing is deleted. ``--jobs J`` runs J designs at once, refused when J times the cap
exceeds 60% of MemTotal or the heavy slice; a design with a ``--cap-for`` cap larger than
``--mem-cap`` runs alone.

Exit codes: 0 every design identical, equivalent, a recorded eq-x, a listed reject, a listed
unproven or no-golden, and at least ``--min-checked`` proven; 1 anything else; 3 a ``hard``
failure (a crash, signal, memory-cap kill or timeout) happened; 2 usage or input error; 77
skipped (an ASan odin3 and no way to cap it).
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from collections.abc import Sequence
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "elab-check"))
import elab_sweep  # noqa: E402
import memcap  # noqa: E402
from elab_equiv import format_cap  # noqa: E402

TOOL = "exit-check"
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_HARD, EXIT_SKIP = 0, 1, 2, 3, 77
IDENTICAL, EQUIVALENT, EQ_X, DIFFERS, ERROR, SKIP, NO_GOLDEN = (
    "identical", "equivalent", "eq-x", "differs", "error", "skip", "no-golden")
UNPROVEN, REJECT, CAPPED = "unproven", "reject", "capped"
PASSING = (IDENTICAL, EQUIVALENT)
# Verdict classes (see verdict()): the passing ones, the two buckets that are neither pass nor
# failure, and every cause that fails the run.
CLS_EQX_OK, CLS_REJECT_OK = "eqx-recorded", "listed-reject"
CLS_UNPROVEN_OK, CLS_NOGOLDEN, CLS_NOGOLDEN_OK = "unproven-listed", "nogolden", "nogolden-listed"
CLS_DEFECT_OK = "differs-listed"
FAIL_CLASSES = (EQ_X, "eqx-mismatch", DIFFERS, ERROR, "hard", UNPROVEN, REJECT, CAPPED, "XPASS",
                "needle")
TIMEOUT_S = elab_sweep.TIMEOUT_S
EQUIV_EXIT_XCONE = 3  # equiv-check --x-report: equivalent outside the x-cones
EQUIV_EXIT_UNPROVEN = 4  # ABC timed out, or only guessed black-box pairs failed: no verdict
EQUIV_EXIT_HARD = 5  # ABC died of a signal, or equiv-check ran out of memory
SPLICE_EXIT_SKIP = 77
LOG_LINES = 60  # lines of one child's output kept in a design's log
WHY_WIDTH = 160
MICRO_MARK = ("micro", "verilog")
VTR_MARK = ("benchmarks", "verilog")
_SAFE = re.compile(r"[^A-Za-z0-9_.-]+")
# sp-splice reports a child that died of a signal or the cap as "failed (exit -9)" / "(exit 137)".
_SIGNALLED = re.compile(r"\(exit (-\d+|137)\)")
_PATH = re.compile(r"(?:/[^/\s:]+)+/([^/\s:]+)")
_NOT_EQUIV = re.compile(r"^equiv-check: (NOT equivalent.*|x-report: .*)$", re.MULTILINE)
_VERDICT_LINE = re.compile(r"^equiv-check: NOT equivalent", re.MULTILINE)
_BUS_INDEX = re.compile(r"\[\d+\]$")
ELAB_DIRECTIVE = "// elab-check:"


class UsageError(elab_sweep.UsageError):
    """A bad argument or input file (exit code 2)."""


@dataclass(frozen=True)
class Result:
    kind: str  # identical, equivalent, eq-x, differs, error, skip, reject, capped or unproven
    secs: float = 0.0
    why: str = ""
    output: str = ""  # the design's log: the needle of an expect-fail entry must appear in it
    hard: bool = False  # a signal, a cap kill, a timeout or a child that cannot run
    cones: tuple[str, ...] = ()  # eq-x: the outputs removed


@dataclass(frozen=True)
class Config:
    odin3: str
    yosys: str
    arch: str
    sp_splice: str
    netlist_compare: str
    equiv_check: str
    golden_root: Path
    work: Path
    cap: int
    caps: dict[str, int]
    vtr_root: Path
    vtr_commit: str
    arch_sha256: str
    min_checked: int = 1
    techlib: str | None = None
    primitives: str | None = None
    config: str | None = None
    config_template: Path | None = None  # VTR's basic_odin_config_split.xml, filled per design
    min_mult: int = 3
    min_adder: int = 1
    equiv_timeout: int = 600
    equiv_box_budget: int | None = None  # equiv-check --box-probe-budget (None: its default)

    def cap_of(self, item: elab_sweep.Item) -> int:
        for suffix, cap in self.caps.items():
            if item.rel == suffix or item.rel.endswith("/" + suffix):
                return cap
        return self.cap


# ---- Parmys's config -------------------------------------------------------------------------

TEMPLATE_UNDER_VTR = "vtr_flow/misc/basic_odin_config_split.xml"


def memory_addr_width(arch: Path) -> int:
    """The largest RAM address width in the architecture (VTR's determine_memory_addr_width):
    the ``addr`` input ports of the ``*port_ram`` primitives."""
    width = 0
    for elem in ET.parse(arch).getroot().iter("pb_type"):
        if "port_ram" in (elem.get("blif_model") or ""):
            for port in elem.findall("input"):
                if "addr" in (port.get("name") or ""):
                    width = max(width, int(port.get("num_pins") or 0))
    return width


def write_parmys_config(cfg: Config, source: Path, work: Path) -> Path:
    """The ``-c`` file of VTR's Parmys stage (``basic_odin_config_split.xml`` with its
    placeholders filled as ``vtr/parmys/parmys.py`` fills them): the hard multiplier is fixed at
    the architecture's ``multiply`` (``fixed="1"``), the memories split to the architecture's
    address width, ``-min_hard_mult_size`` 3 and ``-min_hard_adder_size`` 1 (run_vtr_flow's
    defaults, which the goldens used)."""
    assert cfg.config_template is not None
    text = cfg.config_template.read_text(encoding="utf-8")
    fills = {"YYY": Path(cfg.arch).name, "TTT": "Verilog", "ZZZ": "parmys.out.blif",
             "PPP": str(memory_addr_width(Path(cfg.arch))), "MMM": str(cfg.min_mult),
             "AAA": str(cfg.min_adder), "XXX": str(source)}
    for key, value in fills.items():
        text = text.replace(key, value)
    work.mkdir(parents=True, exist_ok=True)
    out = work / "parmys_config.xml"
    out.write_text(text, encoding="utf-8")
    return out


# ---- goldens ----------------------------------------------------------------------------------


def _after(parts: tuple[str, ...], mark: tuple[str, ...]) -> tuple[str, ...] | None:
    """The path components after the last occurrence of ``mark`` in ``parts``, else None."""
    for i in range(len(parts) - len(mark), -1, -1):
        if parts[i:i + len(mark)] == mark:
            return parts[i + len(mark):]
    return None


def golden_path(root: Path, source: Path) -> Path:
    """The Parmys golden BLIF of ``source``: a VTR benchmark's is ``vtr/<name>/<leaf>.parmys.blif``
    (``<name>`` keeps a ``koios/`` directory), a micro's is ``micro-top``'s."""
    parts = source.resolve().parts
    vtr = _after(parts, VTR_MARK)
    if vtr:
        name = Path(*vtr).with_suffix("")
        return root / "vtr" / name / f"{name.name}.parmys.blif"
    return Path(elab_sweep.load_micro_top().golden_path(root, str(source)))


def read_prov(path: Path) -> dict[str, str]:
    """The ``key=value`` lines of a golden's ``.prov``."""
    return dict(line.partition("=")[::2] for line in
                path.read_text(encoding="utf-8", errors="replace").splitlines() if "=" in line)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_key(path: Path) -> str:
    """A design's text without ``// elab-check:`` directive lines and trailing blanks."""
    lines = [line.rstrip() for line in path.read_text(encoding="utf-8", errors="replace")
             .splitlines() if not line.lstrip().startswith(ELAB_DIRECTIVE)]
    return "\n".join(lines).strip()


def source_matches(cfg: Config, path: Path, prov: dict[str, str]) -> bool:
    """The design is the source the golden was made from: the same bytes, or the same text apart
    from comment directives and trailing blanks when the upstream file ``source=`` names still has
    the recorded hash."""
    recorded = prov.get("source_sha256", "")
    if sha256_file(path) == recorded:
        return True
    upstream = cfg.vtr_root / prov.get("source", "")
    return (bool(recorded) and upstream.is_file() and sha256_file(upstream) == recorded
            and source_key(upstream) == source_key(path))


def prov_problem(cfg: Config, item: elab_sweep.Item, golden: Path, prov: dict[str, str]) -> str:
    """Why the golden's provenance is not this run's (empty when it matches): vtr_commit, a clean
    VTR tree, arch, source and, for a golden BLIF, its own hash."""
    def short(value: str | None) -> str:
        return (value or "missing")[:12]

    bad = []
    if prov.get("vtr_commit") != cfg.vtr_commit:
        bad.append(f"vtr_commit golden={short(prov.get('vtr_commit'))} "
                   f"current={short(cfg.vtr_commit)}")
    if prov.get("vtr_dirty_files") != "0":
        bad.append(f"the golden was made from a VTR tree with vtr_dirty_files="
                   f"{prov.get('vtr_dirty_files', 'missing')}")
    if prov.get("arch_sha256") != cfg.arch_sha256:
        bad.append(f"arch_sha256 golden={short(prov.get('arch_sha256'))} "
                   f"current={short(cfg.arch_sha256)}")
    if not source_matches(cfg, item.path, prov):
        bad.append(f"source_sha256 golden={short(prov.get('source_sha256'))} "
                   f"current={short(sha256_file(item.path))}")
    if prov.get("status") == "ok" and golden.is_file() \
            and prov.get("blif_sha256") != sha256_file(golden):
        bad.append(f"blif_sha256 golden={short(prov.get('blif_sha256'))} "
                   f"current={short(sha256_file(golden))}")
    if not bad:
        return ""
    return ("golden provenance mismatch (regenerate it with tools/run-oracle.sh): "
            + "; ".join(bad))


def golden_status(golden: Path, prov: dict[str, str]) -> Result | None:
    """The Result of a golden that cannot be compared with, None when it can: ``failed`` is a
    Parmys rejection (a REJECT), ``capped`` an oracle run killed at its memory cap (CAPPED: no
    verdict on the design; a size limit that ``--expect-no-golden`` may list), any other status
    without a BLIF an error."""
    status = prov.get("status", "")
    if status == "ok" and golden.is_file():
        return None
    if status == "failed":
        return Result(REJECT, why=f"no Parmys golden: Parmys rejected the design "
                      f"(status={status})")
    if status == "capped":
        return Result(CAPPED, why="no Parmys golden: the Parmys run was killed at its memory cap "
                      "(no verdict on the design) (status=capped)")
    if status == "ok":
        return Result(ERROR, why=f"no Parmys golden: {golden} does not exist (status=ok)")
    return Result(ERROR, why=f"no Parmys golden: Parmys run {status or 'unknown'} (no verdict) "
                  f"(status={status})")


# ---- running the children ---------------------------------------------------------------------


def clip(text: str, lines: int = LOG_LINES) -> str:
    rows = text.rstrip().splitlines()
    if len(rows) <= lines:
        return "\n".join(rows)
    return "\n".join([*rows[:lines], f"... ({len(rows) - lines} more lines)"])


def one_line(text: str) -> str:
    """One line of at most WHY_WIDTH characters, every absolute path cut to its file name."""
    line = _PATH.sub(r"\1", " ".join(text.split()))
    return line if len(line) <= WHY_WIDTH else line[:WHY_WIDTH - 3] + "..."


def run_capped(argv: Sequence[str], cap: int, log: list[str]) -> subprocess.CompletedProcess[str] \
        | Result:
    """A child under ``memcap.run``; its command line goes to ``log``. A child that cannot run or
    times out is a hard error Result."""
    log.append("$ " + " ".join(argv))
    try:
        return memcap.run(list(argv), cap, capture_output=True, timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return Result(ERROR, why=f"timed out after {TIMEOUT_S} s", hard=True)
    except OSError as exc:
        return Result(ERROR, why=f"cannot run {argv[0]}: {exc}", hard=True)


def splice_why(done: subprocess.CompletedProcess[str], cap: int) -> tuple[str, bool]:
    """(why, hard) of a failed sp-splice: its first message line; a signal or the cap is hard."""
    text = (done.stdout or "") + (done.stderr or "")
    message = elab_sweep.first_error(text.replace("sp-splice: error: ", ""))
    hard = done.returncode < 0 or _SIGNALLED.search(text) is not None
    if hard and re.search(r"\(exit (-9|137)\)", text):
        return f"out of memory (cap {format_cap(cap)}): {message}", True
    return f"sp-splice: {message}", hard


def define_pre(prov: dict[str, str], work: Path) -> list[str]:
    """``--odin3-pre`` for the macros the oracle was given with ``run-oracle.sh --define`` (the
    ``defines=`` line of the golden's ``.prov``): a file list of ``+define+`` lines, read as the
    project before the design. Odin III finds a header beside the design by itself, so the
    ``include_dirs=`` line needs nothing here."""
    defines = prov.get("defines", "").split()
    if not defines:
        return []
    work.mkdir(parents=True, exist_ok=True)
    flist = work / "defines.f"
    flist.write_text("".join(f"+define+{d}\n" for d in defines), encoding="utf-8")
    return ["--odin3-pre", f"read_project --format f {flist}"]


def splice_argv(cfg: Config, item: elab_sweep.Item, top: str, work: Path,
                config: str | None, pre: Sequence[str] = ()) -> list[str]:
    argv = [cfg.sp_splice, "--odin3", cfg.odin3, "--yosys", cfg.yosys, "--arch", cfg.arch,
            "--top", top, "--mem-cap", str(cfg.cap_of(item)), "--keep", str(work), *pre]
    for flag, value in (("--techlib", cfg.techlib), ("--primitives", cfg.primitives),
                        ("--config", config)):
        if value:
            argv += [flag, value]
    return [*argv, str(item.path)]


def run_splice(cfg: Config, item: elab_sweep.Item, top: str, work: Path,
               log: list[str], pre: Sequence[str] = ()) -> Path | Result:
    """sp-splice on the design: the output BLIF, or the Result of its failure. sp-splice caps its
    own children at the design's cap; it runs under ``memcap.run`` as well, with the timeout, so
    that a timeout kills its whole process group and leaves no odin3 or Yosys child behind."""
    config = cfg.config
    if cfg.config_template is not None:
        config = str(write_parmys_config(cfg, item.path, work))
    argv = splice_argv(cfg, item, top, work, config, pre)
    done = run_capped(argv, cfg.cap_of(item), log)
    if isinstance(done, Result):
        return done
    log.append(clip((done.stdout or "") + (done.stderr or "")))
    if done.returncode == SPLICE_EXIT_SKIP:
        return Result(SKIP, why="cannot be capped")
    if done.returncode != 0:
        why, hard = splice_why(done, cfg.cap_of(item))
        return Result(ERROR, why=why, hard=hard)
    lines = (done.stdout or "").strip().splitlines()
    out = Path(lines[-1]) if lines else work / "out.blif"
    if not out.is_file():
        return Result(ERROR, why=f"sp-splice wrote no {out}")
    return out


def compare_failure(done: subprocess.CompletedProcess[str], cap: int, what: str) -> Result:
    """The error Result of a comparison child that exited with a code it does not define."""
    text = "\n".join(line.removeprefix(f"{what}: ")
                     for line in ((done.stdout or "") + (done.stderr or "")).splitlines()
                     if not line.startswith(f"{what}: notice:"))
    hard = done.returncode < 0 or done.returncode == 137
    return Result(ERROR, why=(elab_sweep.child_why(done, cap) if hard
                              else f"{what}: " + one_line(elab_sweep.first_error(text))),
                  hard=hard)


def differs_why(text: str) -> str:
    """The reason line(s) of an equiv-check exit 1."""
    marks = [m.group(1) for m in _NOT_EQUIV.finditer(text)]
    return one_line("; ".join(marks)) if marks else one_line(elab_sweep.first_error(text))


def cone_names(text: str) -> tuple[str, ...]:
    return tuple(line.partition(":")[2].strip() for line in text.splitlines()
                 if line.startswith("x-cone:"))


def run_equiv(cfg: Config, golden: Path, out: Path, cap: int, log: list[str]) -> Result:
    """``equiv-check --x-report golden out`` classified."""
    budget = [] if cfg.equiv_box_budget is None else ["--box-probe-budget",
                                                      str(cfg.equiv_box_budget)]
    got = run_capped([cfg.equiv_check, "--x-report", "--match-unnamed", "--timeout",
                      str(cfg.equiv_timeout), *budget, str(golden), str(out)], cap, log)
    if isinstance(got, Result):
        return got
    text = (got.stdout or "") + (got.stderr or "")
    log.append(clip(text))
    code = got.returncode
    if code == 0:
        return Result(EQUIVALENT)
    if code == EQUIV_EXIT_XCONE:
        cones = cone_names(text)
        with contextlib.suppress(OSError):  # every excluded output, for the eq-x review
            (out.parent / "x-cones.txt").write_text("\n".join(cones) + "\n", encoding="utf-8")
        return Result(EQ_X, why=f"{len(cones)} output(s) in x-cones: " + one_line(
            ", ".join(cones)), cones=cones)
    if code == 1 and _VERDICT_LINE.search(text):
        return Result(DIFFERS, why=differs_why(text))
    if code == EQUIV_EXIT_UNPROVEN:  # an ABC timeout or a failed guess: a tool limit, no verdict
        note = re.search(r"^equiv-check: (unproven: .*|inconclusive: .*)$", text, re.MULTILINE)
        return Result(UNPROVEN, why=one_line(note.group(1) if note else "no verdict"))
    if code == EQUIV_EXIT_HARD:
        return Result(ERROR, why="equiv-check: " + one_line(elab_sweep.first_error(text)),
                      hard=True)
    if code == 1:  # exit 1 without the verdict line: a crash or a failing prlimit, not "differs"
        return Result(ERROR, why="equiv-check exited 1 without a NOT-equivalent verdict: "
                      + one_line(elab_sweep.first_error(text)),
                      hard="Traceback (most recent call last)" in text)
    return compare_failure(got, cap, "equiv-check")


def classify(cfg: Config, golden: Path, out: Path, cap: int, log: list[str]) -> Result:
    """netlist-compare, then equiv-check --x-report. A netlist-compare error is logged and the
    equivalence check decides."""
    got = run_capped([cfg.netlist_compare, "--implicit-models", str(golden), str(out)], cap, log)
    if isinstance(got, Result):
        return got
    log.append(clip((got.stdout or "") + (got.stderr or "")))
    if got.returncode == 0:
        return Result(IDENTICAL)
    if got.returncode < 0 or got.returncode == 137:
        return compare_failure(got, cap, "netlist-compare")
    if got.returncode != 1:
        log.append(f"netlist-compare exit {got.returncode}: not a verdict; equiv-check decides")
    return run_equiv(cfg, golden, out, cap, log)


def design_dir(cfg: Config, index: int, item: elab_sweep.Item) -> Path:
    return cfg.work / f"{index:04d}-{_SAFE.sub('_', item.rel)}"


def pipeline(cfg: Config, item: elab_sweep.Item, work: Path, log: list[str]) -> Result:
    golden = golden_path(cfg.golden_root, item.path)
    prov_file = golden.with_suffix(".prov")
    if not prov_file.is_file():
        if golden.is_file():
            return Result(ERROR, why=f"{golden.name} has no provenance record ({prov_file.name}): "
                          "a golden of unknown origin")
        return Result(NO_GOLDEN, why=f"{golden.name} does not exist")  # the oracle never ran
    prov = read_prov(prov_file)
    problem = prov_problem(cfg, item, golden, prov)
    if problem:
        return Result(ERROR, why=problem)
    unusable = golden_status(golden, prov)
    if unusable is not None:
        oracle_log = golden.with_suffix(".log")
        if oracle_log.is_file():  # the oracle's own error lines: a listing's needle may be there
            log.append("oracle log: " + clip(oracle_log.read_text(encoding="utf-8",
                                                                  errors="replace")))
        return unusable
    try:
        top = str(elab_sweep.load_micro_top().first_model(golden))
    except (OSError, ValueError) as exc:
        return Result(ERROR, why=f"golden has no top: {exc}")
    log.append(f"golden {golden}\ntop {top}")
    work.mkdir(parents=True, exist_ok=True)
    out = run_splice(cfg, item, top, work, log, define_pre(prov, work))
    if isinstance(out, Result):
        return out
    return classify(cfg, golden, out, cfg.cap_of(item), log)


def run_design(cfg: Config, item: elab_sweep.Item, index: int) -> Result:
    """One design through the pipeline; its log is written to ``NNNN-<design>/exit-check.log``."""
    start = time.monotonic()
    work = design_dir(cfg, index, item)
    log = [f"design {item.rel} ({item.size} B)"]
    result = pipeline(cfg, item, work, log)
    log.append(f"result {result.kind}: {result.why}".rstrip(": "))
    text = "\n".join(log)
    try:
        work.mkdir(parents=True, exist_ok=True)
        (work / "exit-check.log").write_text(text + "\n", encoding="utf-8")
    except OSError as exc:
        text += f"\n(cannot write the log: {exc})"
    return dataclasses.replace(result, secs=time.monotonic() - start, output=text)


# ---- the lists and the eq-x record -------------------------------------------------------------


@dataclass(frozen=True)
class EqxRecord:
    """A reviewed eq-x design: the outputs equiv-check excluded (count, hash of the sorted names,
    count per bus) and why they are x."""

    count: int
    sha256: str
    reason: str
    buses: dict[str, int]


def bus_counts(names: Sequence[str]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for name in names:
        bus = _BUS_INDEX.sub("", name)
        counts[bus] = counts.get(bus, 0) + 1
    return dict(sorted(counts.items()))


def cones_digest(names: Sequence[str]) -> str:
    return hashlib.sha256("\n".join(sorted(names)).encode("utf-8")).hexdigest()


def read_eqx(path: Path) -> dict[str, EqxRecord]:
    """The eq-x record: ``design <path> <count> <sha256> <reason>`` and ``bus <path> <name>
    <count>`` lines (blank lines and ``#`` comments skipped); the bus counts must add up."""
    designs: dict[str, tuple[int, str, str]] = {}
    buses: dict[str, dict[str, int]] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        parts = raw.split(None, 4)
        if not parts or parts[0].startswith("#"):
            continue
        where = f"{path}:{number}"
        try:
            if parts[0] == "design" and len(parts) == 5:
                if parts[1] in designs:
                    raise UsageError(f"{where}: '{parts[1]}' is recorded twice")
                designs[parts[1]] = (int(parts[2]), parts[3], parts[4].strip())
            elif parts[0] == "bus" and len(parts) == 4:
                buses.setdefault(parts[1], {})[parts[2]] = int(parts[3])
            else:
                raise UsageError(f"{where}: expected 'design <path> <count> <sha256> <reason>' "
                                 "or 'bus <path> <name> <count>'")
        except ValueError as exc:
            raise UsageError(f"{where}: {exc}") from exc
    for rel in buses:
        if rel not in designs:
            raise UsageError(f"{path}: bus lines for '{rel}' without a design line")
    for rel, (count, _, _) in designs.items():
        if sum(buses.get(rel, {}).values()) != count:
            raise UsageError(f"{path}: '{rel}': the bus counts add up to "
                             f"{sum(buses.get(rel, {}).values())}, not {count}")
    return {rel: EqxRecord(count, sha, reason, dict(sorted(buses[rel].items())))
            for rel, (count, sha, reason) in designs.items()}


@dataclass(frozen=True)
class Lists:
    """The reviewed exception lists: Parmys rejections, oracle harness gaps, unproven designs and
    the eq-x record. A design is in at most one."""

    reject: dict[str, elab_sweep.Expect] = dataclasses.field(default_factory=dict)
    nogolden: dict[str, elab_sweep.Expect] = dataclasses.field(default_factory=dict)
    unproven: dict[str, elab_sweep.Expect] = dataclasses.field(default_factory=dict)
    eqx: dict[str, EqxRecord] = dataclasses.field(default_factory=dict)
    defect: dict[str, elab_sweep.Expect] = dataclasses.field(default_factory=dict)

    def paths(self) -> set[str]:
        return {*self.reject, *self.nogolden, *self.unproven, *self.eqx, *self.defect}

    def listed(self, rel: str) -> bool:
        return rel in self.paths()

    def check_disjoint(self) -> None:
        seen: set[str] = set()
        for table in (self.reject, self.nogolden, self.unproven, self.eqx, self.defect):
            for rel in table:
                if rel in seen:
                    raise UsageError(f"'{rel}' is in more than one exception list")
                seen.add(rel)


def read_lists(args: argparse.Namespace) -> Lists:
    lists = Lists(
        reject=elab_sweep.merge_expect(args.expect_fail),
        nogolden=elab_sweep.merge_expect(args.expect_no_golden),
        unproven=elab_sweep.merge_expect(args.expect_unproven),
        defect=elab_sweep.merge_expect(args.expect_differs),
        eqx={rel: rec for path in args.eqx_record for rel, rec in read_eqx(path).items()})
    lists.check_disjoint()
    return lists


# ---- verdicts and the summary -----------------------------------------------------------------


def _needle_problem(entry: elab_sweep.Expect, result: Result) -> bool:
    return bool(entry.needle) and entry.needle not in result.output


def eqx_verdict(head: str, record: EqxRecord | None, result: Result) -> tuple[str, str]:
    if record is None:
        return EQ_X, f"eq-x       {head}: {result.why} (no reviewed record: does not count)"
    count, digest, buses = len(result.cones), cones_digest(result.cones), bus_counts(result.cones)
    if (count, digest, buses) != (record.count, record.sha256, record.buses):
        return "eqx-mismatch", (f"eq-x       {head}: the excluded outputs ({count}) are not the "
                                f"reviewed record's ({record.count}); re-review: {result.why}")
    return CLS_EQX_OK, f"eq-x (rec) {head}: {count} outputs as reviewed: {record.reason}"


def verdict(item: elab_sweep.Item, result: Result, lists: Lists) -> tuple[str, str]:
    """(class, line): the class is one of the CLS_* names, a result kind or a failure cause."""
    head = f"{item.rel} ({item.size} B, {result.secs:.3f} s)"
    kind, rel = result.kind, item.rel
    if kind == SKIP:
        return "skip", f"skip       {head}: {result.why}"
    if kind == NO_GOLDEN:
        return CLS_NOGOLDEN, f"no-golden  {head}: {result.why}"
    if result.hard:  # a crash, cap kill or timeout is never the listed result
        note = " (listed, but a signal, a memory cap or a timeout is never an expected result)" \
            if lists.listed(rel) else ""
        return "hard", f"hard       {head}: {result.why}{note}"
    if kind in PASSING:
        if lists.listed(rel):
            return "XPASS", (f"XPASS      {head}: listed as an exception but is {kind}")
        return kind, f"{kind:<10} {head}"
    if kind == EQ_X:
        return eqx_verdict(head, lists.eqx.get(rel), result)
    table, ok_class = {REJECT: (lists.reject, CLS_REJECT_OK),
                       UNPROVEN: (lists.unproven, CLS_UNPROVEN_OK),
                       CAPPED: (lists.nogolden, CLS_NOGOLDEN_OK),
                       DIFFERS: (lists.defect, CLS_DEFECT_OK)}.get(kind, ({}, ""))
    entry = table.get(rel)
    if kind == REJECT and entry is None and rel in lists.nogolden:
        entry, ok_class = lists.nogolden[rel], CLS_NOGOLDEN_OK
    if entry is None:
        suffix = " (listed, but not for this result)" if lists.listed(rel) else ""
        return kind, f"{kind:<10} {head}: {result.why}{suffix}"
    if _needle_problem(entry, result):
        return "needle", (f"{kind:<10} {head}: fails differently from the listing: expected "
                          f"[{entry.needle}], got {result.why}")
    label = {CLS_REJECT_OK: "listed", CLS_UNPROVEN_OK: "unproven", CLS_NOGOLDEN_OK: "no-golden",
             CLS_DEFECT_OK: "known-diff"}[ok_class]
    return ok_class, f"{label:<10} {head}: {entry.reason}"


class Sweep:
    def __init__(self, cfg: Config, lists: Lists, jobs: int) -> None:
        self.cfg = cfg
        self.lists = lists
        self.jobs = jobs
        self.counts: dict[str, int] = {}
        self.causes: list[str] = []

    def report(self, item: elab_sweep.Item, result: Result) -> None:
        cls, line = verdict(item, result, self.lists)
        print(line, flush=True)
        self.counts[cls] = self.counts.get(cls, 0) + 1
        if cls in FAIL_CLASSES:
            self.causes.append(f"{cls}: {item.rel}: {result.why}")

    def run(self, items: Sequence[elab_sweep.Item]) -> None:
        pending: list[tuple[elab_sweep.Item, Future[Result]]] = []
        with ThreadPoolExecutor(max_workers=self.jobs) as pool:
            for index, item in enumerate(items):
                if self.cfg.cap_of(item) > self.cfg.cap:  # a raised cap runs alone
                    self.drain(pending, 0)
                    self.report(item, run_design(self.cfg, item, index))
                    continue
                pending.append((item, pool.submit(run_design, self.cfg, item, index)))
                self.drain(pending, self.jobs)
            self.drain(pending, 0)

    def drain(self, pending: list[tuple[elab_sweep.Item, Future[Result]]], keep: int) -> None:
        while len(pending) > keep:
            item, future = pending.pop(0)
            self.report(item, future.result())

    def count(self, *classes: str) -> int:
        return sum(self.counts.get(cls, 0) for cls in classes)

    def proven(self) -> int:
        """Designs whose Odin III netlist was proven the Parmys netlist (or equal outside a
        reviewed x record)."""
        return self.count(IDENTICAL, EQUIVALENT, CLS_EQX_OK)

    def failures(self) -> int:
        return self.count(*FAIL_CLASSES)


SUMMARY_ROWS = (
    (IDENTICAL, "identical"), (EQUIVALENT, "equivalent"),
    (CLS_EQX_OK, "eq-x, reviewed record"), (CLS_REJECT_OK, "listed-reject (Parmys rejects)"),
    (CLS_UNPROVEN_OK, "UNPROVEN (listed; not proven)"),
    (CLS_DEFECT_OK, "KNOWN DIFFERS (Parmys defect, listed)"),
    (CLS_NOGOLDEN, "no-golden (never ran)"), (CLS_NOGOLDEN_OK, "no-golden (harness gap, listed)"),
    ("skip", "skipped (cannot cap)"),
    (EQ_X, "FAIL eq-x, no record"), ("eqx-mismatch", "FAIL eq-x, record differs"),
    (DIFFERS, "FAIL differs"), (ERROR, "FAIL error"), ("hard", "FAIL hard (crash/kill/timeout)"),
    (UNPROVEN, "FAIL unproven, not listed"), (REJECT, "FAIL reject, not listed"),
    (CAPPED, "FAIL oracle capped, not listed"),
    ("XPASS", "FAIL listed but passing"), ("needle", "FAIL listed, wrong needle"))


def summary_lines(state: Sweep, total: int, stale: Sequence[str], secs: float,
                  min_checked: int) -> list[str]:
    counts = state.counts
    lines = ["", f"{TOOL}: summary ({total} designs)", f"  {'class':<34}{'designs':>8}",
             f"  {'-' * 42}"]
    lines += [f"  {label:<34}{counts[cls]:>8}" for cls, label in SUMMARY_ROWS if counts.get(cls)]
    if stale:
        lines.append(f"  {'FAIL stale list entries':<34}{len(stale):>8}")
    lines += [f"  {line}" for line in state.causes[:200]] if state.causes else []
    nogolden = state.count(CLS_NOGOLDEN, CLS_NOGOLDEN_OK)
    known = state.count(CLS_DEFECT_OK)
    lines.append(f"{TOOL}: {total} designs: PROVEN {state.proven()} ({counts.get(IDENTICAL, 0)} "
                 f"identical, {counts.get(EQUIVALENT, 0)} equivalent, {counts.get(CLS_EQX_OK, 0)} "
                 f"eq-x with a reviewed record), {counts.get(CLS_REJECT_OK, 0)} listed-reject; "
                 f"NOT PROVEN {state.count(CLS_UNPROVEN_OK)} unproven (listed)"
                 + (f", {known} known differs (listed)" if known else "") + "; NOT CHECKED "
                 f"{nogolden} no-golden; {state.failures()} failing, {len(stale)} stale; "
                 f"{secs:.1f} s")
    lines.append(f"{TOOL}: assumptions: latch init 2/3 read as 0; RAM contents and init not "
                 "modelled; dual_port_ram ports 1 and 2 interchangeable; --vtr-compat")
    if state.proven() < min_checked:
        lines.append(f"{TOOL}: FAIL: only {state.proven()} design(s) proven, --min-checked is "
                     f"{min_checked} (a wrong or empty golden tree reports nothing)")
    return lines


def exit_code(state: Sweep, stale: Sequence[str], min_checked: int) -> int:
    ran = sum(n for cls, n in state.counts.items() if cls not in ("skip", CLS_NOGOLDEN))
    if state.count("skip") and not ran:  # nothing could run: an uncappable ASan odin3
        return EXIT_SKIP
    if state.count("hard"):
        return EXIT_HARD
    if stale or state.failures() or state.proven() < min_checked:
        return EXIT_FAIL
    return EXIT_OK


def sweep(cfg: Config, items: list[elab_sweep.Item], lists: Lists, jobs: int,
          partial: bool) -> int:
    state = Sweep(cfg, lists, jobs)
    start = time.monotonic()
    print(f"{TOOL}: work directory {cfg.work}", flush=True)
    state.run(items)
    seen = {item.rel for item in items}
    stale = [] if partial else sorted(rel for rel in lists.paths() if rel not in seen)
    for rel in stale:
        print(f"STALE {rel}: listed as an exception but not in the corpus")
    lines = summary_lines(state, len(items), stale, time.monotonic() - start, cfg.min_checked)
    print("\n".join(lines))
    (cfg.work / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return exit_code(state, stale, cfg.min_checked)


# ---- arguments --------------------------------------------------------------------------------


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    tools = HERE.parent
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary (Release for a sweep)")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--arch", required=True, help="the architecture file the goldens used")
    ap.add_argument("--sp-splice", default=str(tools / "opt-check" / "sp-splice"))
    ap.add_argument("--netlist-compare", default=str(tools / "netlist-compare" / "netlist-compare"))
    ap.add_argument("--equiv-check", default=str(tools / "equiv-check" / "equiv-check"))
    ap.add_argument("--golden-root", type=Path, default=None,
                    help="the Parmys golden tree of the arch (default: micro-top's)")
    ap.add_argument("--techlib", help="sp-splice --techlib (default: its own lib/vtr.o3lib)")
    ap.add_argument("--primitives", help="sp-splice --primitives (default under $VTR_ROOT)")
    ap.add_argument("--config", help="sp-splice --config: one Parmys config file for every design "
                    "(instead of VTR's, filled per design)")
    ap.add_argument("--vtr-config-template", type=Path, metavar="FILE",
                    help="VTR's basic_odin_config_split.xml, filled per design as VTR's Parmys "
                    "stage fills it (default: under $VTR_ROOT)")
    ap.add_argument("--no-config", action="store_true",
                    help="pass no Parmys config (the goldens were made with VTR's)")
    ap.add_argument("--min-hard-mult-size", type=int, default=3,
                    help="run_vtr_flow's -min_hard_mult_size (default 3, as the goldens)")
    ap.add_argument("--min-hard-adder-size", type=int, default=1,
                    help="run_vtr_flow's -min_hard_adder_size (default 1, as the goldens)")
    ap.add_argument("--expect-fail", type=Path, action="append", default=[],
                    help="listed rejections (Parmys itself rejects the design): <relative path> "
                    "[needle] <reason> lines (repeatable)")
    ap.add_argument("--expect-no-golden", type=Path, action="append", default=[],
                    help="designs whose oracle run failed for a harness reason (missing include "
                    "or -D): same syntax; they are counted as no-golden, never as passing")
    ap.add_argument("--expect-differs", type=Path, action="append", default=[],
                    help="designs equiv-check proves NOT equivalent to a Parmys netlist that is "
                    "itself wrong (a reviewed Parmys defect): same syntax; a separate KNOWN "
                    "DIFFERS bucket, never a pass")
    ap.add_argument("--expect-unproven", type=Path, action="append", default=[],
                    help="designs equiv-check cannot decide (timeout, guessed pairs): same "
                    "syntax; a separate unproven bucket, never a pass")
    ap.add_argument("--eqx-record", type=Path, action="append", default=[],
                    help="the reviewed eq-x record (design/bus lines, see tests/exit)")
    ap.add_argument("--min-checked", type=int, default=1, metavar="N",
                    help="fail unless at least N designs were proven (default 1)")
    ap.add_argument("--vtr-root", type=Path, help="the VTR checkout the goldens came from "
                    "(default $VTR_ROOT): their vtr_commit is checked against it")
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--mem-cap", default="4G", help="per child (default 4G)")
    ap.add_argument("--cap-for", action="append", default=[], metavar="SUFFIX=CAP",
                    help="a larger cap for one design, which then runs alone")
    ap.add_argument("--equiv-box-budget", type=int, metavar="S",
                    help="equiv-check's --box-probe-budget: seconds for the ABC probes that pair "
                    "multipliers the simulation cannot tell apart (default: equiv-check's 120)")
    ap.add_argument("--equiv-timeout", type=int, default=600, metavar="S",
                    help="equiv-check's ABC timeout in seconds (default 600)")
    ap.add_argument("--work", type=Path, help="the work directory (default: a fresh temp dir)")
    ap.add_argument("--root", type=Path, help="make listed paths relative to this directory")
    ap.add_argument("--limit", type=int, metavar="N",
                    help="run only the N smallest designs that have a Parmys golden (a smoke run)")
    ap.add_argument("--partial", action="store_true", help="a subset: skip the stale-entry check")
    ap.add_argument("inputs", nargs="+", help="directories or files")
    return ap.parse_args(argv)


def need_exe(path: str) -> str:
    found = shutil.which(path)
    if found is None:
        raise UsageError(f"{path} is not an executable file")
    return os.path.abspath(found)


def work_dir(arg: Path | None) -> Path:
    if arg is None:
        return Path(tempfile.mkdtemp(prefix="exit-check-"))
    arg.mkdir(parents=True, exist_ok=True)
    return arg.resolve()


def config_template(args: argparse.Namespace) -> Path | None:
    """VTR's Parmys config template, unless ``--config`` or ``--no-config`` replaces it."""
    if args.no_config or args.config:
        return None
    root = os.environ.get("VTR_ROOT")
    template = args.vtr_config_template or (Path(root) / TEMPLATE_UNDER_VTR if root else None)
    if template is None or not template.is_file():
        raise UsageError("VTR's basic_odin_config_split.xml not found: set VTR_ROOT, pass "
                         "--vtr-config-template or --config, or --no-config")
    return template.resolve()


def vtr_root_of(args: argparse.Namespace) -> Path:
    root = args.vtr_root or os.environ.get("VTR_ROOT")
    if not root:
        raise UsageError("the VTR checkout is unknown: pass --vtr-root or set VTR_ROOT (the "
                         "goldens' vtr_commit is checked against it)")
    return Path(root).resolve()


def vtr_state(root: Path) -> tuple[str, int]:
    """(HEAD commit, number of modified tracked files) of the VTR checkout ``root``."""
    def git(*words: str) -> str:
        done = subprocess.run(["git", "-C", str(root), *words], capture_output=True, text=True,
                              check=False)
        if done.returncode != 0:
            raise UsageError(f"{root}: not a git checkout ({done.stderr.strip()})")
        return done.stdout

    return (git("rev-parse", "HEAD").strip(),
            len(git("status", "--porcelain", "--untracked-files=no").splitlines()))


def build_config(args: argparse.Namespace) -> Config:
    # Usage errors first (the executables), then anything that depends on the environment.
    exes = {name: need_exe(getattr(args, name))
            for name in ("odin3", "yosys", "sp_splice", "netlist_compare", "equiv_check")}
    caps: dict[str, int] = {}
    for spec in args.cap_for:
        suffix, _, size = spec.partition("=")
        if not suffix or not size:
            raise UsageError(f"--cap-for {spec!r}: expected SUFFIX=CAP")
        caps[suffix] = elab_sweep.parse_size(size)
    golden = Path(args.golden_root or elab_sweep.load_micro_top().DEFAULT_ROOT)
    if not golden.is_dir():
        raise UsageError(f"{golden}: the golden root is not a directory")
    template = config_template(args)
    arch = Path(args.arch)
    if not arch.is_file():
        raise UsageError(f"{args.arch}: no such architecture file")
    if args.min_checked < 0:
        raise UsageError(f"--min-checked {args.min_checked} is negative")
    vtr_root = vtr_root_of(args)
    commit, dirty = vtr_state(vtr_root)
    if dirty:
        raise UsageError(f"{vtr_root}: {dirty} modified file(s); the goldens come from a clean "
                         "VTR tree")
    return Config(
        odin3=exes["odin3"], yosys=exes["yosys"], arch=str(arch.resolve()),
        sp_splice=exes["sp_splice"], netlist_compare=exes["netlist_compare"],
        equiv_check=exes["equiv_check"], golden_root=Path(golden).resolve(),
        work=work_dir(args.work), cap=elab_sweep.parse_size(args.mem_cap), caps=caps,
        techlib=args.techlib, primitives=args.primitives, config=args.config,
        config_template=template, min_mult=args.min_hard_mult_size,
        min_adder=args.min_hard_adder_size, equiv_timeout=args.equiv_timeout,
        equiv_box_budget=args.equiv_box_budget,
        vtr_root=vtr_root, vtr_commit=commit, arch_sha256=sha256_file(arch),
        min_checked=args.min_checked)


def limit_items(cfg: Config, items: list[elab_sweep.Item], limit: int | None) \
        -> list[elab_sweep.Item]:
    """``--limit N``: the N smallest designs whose golden exists."""
    if limit is None:
        return items
    if limit < 1:
        raise UsageError(f"--limit {limit} is not positive")
    have = [item for item in items if golden_path(cfg.golden_root, item.path).is_file()]
    return have[:limit]


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        cfg = build_config(args)
        elab_sweep.check_jobs(args.jobs, cfg.cap, list(cfg.caps.values()))
        memcap.check_cap(max([cfg.cap, *cfg.caps.values()]))
        memcap.wrap([cfg.odin3], cfg.cap)  # an uncappable ASan odin3 skips before any design
        lists = read_lists(args)
        root = args.root.resolve() if args.root is not None else None
        items = limit_items(cfg, elab_sweep.collect(args.inputs, root), args.limit)
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP
    except (elab_sweep.UsageError, ValueError, OSError, RuntimeError) as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_USAGE
    if not items:
        print(f"{TOOL}: no input files", file=sys.stderr)
        return EXIT_USAGE
    return sweep(cfg, items, lists, args.jobs, args.limit is not None or args.partial)


if __name__ == "__main__":
    sys.exit(main())
