"""proc-diff: Odin III's ``proc`` against Yosys 0.55's on the same RTLIL (2E spec §18.3, §18.4).

Usage::

    proc-diff --odin3 O --yosys Y [--top T] [--nolatches] [--no-trio] [--with-opt] [--bisect]
              [--mem-cap B] [--keep DIR] [--write] [--expect-fail F] [--allow-version V]
              [--techlib L] <file or dir>...

For each Verilog file F (a directory contributes its ``.v`` files; every file is sorted by size,
smallest first, by 2D's ``elab_sweep.collect`` when it is importable, else by ``os.stat``):

1. **Side A.** ``O [--top T] -p "read_verilog [--nolatches] F; opt_expr; opt_clean;
   opt -nodffe -nosdff; write_rtlil in.il; proc --noopt; write_rtlil a.il"``. ``--no-trio`` drops
   the three pre-``proc`` passes; ``--with-opt`` runs ``proc`` in place of ``proc --noopt``.
   ``a.il`` must hold no ``process`` (else ``proc-diff: a.il still holds processes``, exit 2).
2. **Side B.** ``Y -p "read_rtlil in.il; hierarchy -top T; proc -norom -noopt; opt_clean -purge;
   write_blif -blackbox -icells -param b.blif"`` (``proc -norom`` with ``--with-opt``; without
   ``--top``, ``hierarchy -auto-top``).
3. **A's BLIF.** ``Y -p "read_rtlil a.il; hierarchy -top T; opt_clean -purge; write_blif
   -blackbox -icells -param a.blif"``. ``-blackbox`` (fix round 1, amending the brief) writes each
   black-box or hard module's interface, so a design with a hard block can be compared.
4. **Normalise and compare.** Into a copy of each file (``a.norm.blif``, ``b.norm.blif``; made
   from the original every time, so bisect's repeats are safe), every ``.param P <bits>`` whose P
   is listed in ``int_params.txt`` (the registry's INT parameters, 2E plan point (o)) and whose
   value is at most 32 bits of 0/1 becomes its decimal value (Yosys writes an INT parameter as
   32 bits, a polarity it built from a constant as 1 bit), and a port-to-port buffer (``.names x
   y`` / ``1 1``) reads the root of its buffer chain (a buffer on a cycle stays as written), so
   the order of ``connect`` lines across processes (§20 #8) cannot matter. Then ``netlist-compare
   --implicit-models a.norm.blif b.norm.blif``, which also compares a ``$reduce_or``'s inputs as
   a set (§20 #8).

``--bisect`` (§18.4): when a file differs, for X in clean, rmdead, prune, arst, Odin III runs step
1's input through ``proc --stop-after X; write_rtlil a_X.il``, Yosys finishes it with the remaining
sub-passes (2E plan point (q); ``opt_expr -keepdc`` after them with ``--with-opt``), then steps
3-4 compare it with b.blif. It prints ``first difference after X`` or, when only the full run
differs, ``no phase differs``.

``--write`` (point (l)): when every file is identical, writes ``<slug>.il`` beside each file
(``<slug>.nolatches.il`` with ``--nolatches``) from ``O [--top T] -p "read_verilog [--nolatches] F;
proc --noopt; write_rtlil <slug>.il"``. Otherwise it is refused (exit 2) and nothing is written.

Documented divergences (§20): #8 and #9 are absorbed by the normaliser; Yosys 0.55's assertion on
an empty select (#17), on side B only, is recognised and reported as an expected difference, not
a failure (side A's BLIF is still made from ``a.il``). Other
known differences go in ``--expect-fail F``: lines ``<relative path> <one-line reason>`` (``#``
comments allowed), the path relative to the directory given (or the file as given). A reason that
cites ``§20 #N`` for N in 8, 9, 16, 17, 18 must see that divergence's outcome: a difference (#8,
#9, #18), Odin III's E3 (#16) or Yosys's assertion (#17). An unlisted failure, a listed pass or an
outcome that contradicts its cited divergence fails the run.

Every child runs through ``memcap.run(argv, cap)`` from ``tools/elab-check/memcap.py`` (2D-6),
imported lazily (point (t)), with ``capture_output=True`` when it takes that keyword (proc-diff
reads every child's output); while it is missing a run exits 2 with ``proc-diff:
tools/elab-check/memcap.py is missing (2D-6)``. A Yosys whose ``-V`` is not 0.55 is refused unless
``--allow-version V`` names it (the nightly drift job only).

A difference that survives ``SHUFFLE_TRIES`` reordered copies of side A is checked once more: when
netlist-compare tells either side from a reordered copy of itself (tied cells it cannot order, e.g.
twin ``$eq`` cells feeding a chain of muxes), the file is a ``tie``, not a difference (exit 3): the
verdict is unconfirmed, and the bridge (``elab-equiv``) is the arbiter.

Exit 0 identical (or only expected differences), 1 differ (the work directory is kept and named),
2 error, 3 tie (only ties, no difference).
"""

from __future__ import annotations

import argparse
import importlib.util
import inspect
import random
import re
import shutil
import sys
import tempfile
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from types import ModuleType
from typing import Any

import yosys_phase

TOOL = "proc-diff"
HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
# Point (t): imported lazily, so Task 10 lands before 2D-6; tests point these at fakes.
MEMCAP_PY = TOOLS / "elab-check" / "memcap.py"
ELAB_SWEEP_PY = TOOLS / "elab-check" / "elab_sweep.py"
INT_PARAMS = HERE / "int_params.txt"
NETLIST_COMPARE_PY = TOOLS / "netlist-compare" / "netlist_compare.py"

GIB = 1 << 30
DEFAULT_CAP = 4 * GIB  # 2E plan point (p): 4 GiB per child by default
INT_PARAM_BITS = 32  # Yosys writes an INT parameter as 32 bits
STDERR_TAIL = 20  # lines of a failing child's output shown
STDERR_ERRORS = 10  # and the first error lines of all of it
SHUFFLE_TRIES = 8  # netlist-compare reruns on a reordered copy of side A before "differs" stands
CELL_STARTS = (".subckt", ".names", ".latch", ".gate", ".mlatch")

EXIT_SAME, EXIT_DIFFER, EXIT_ERROR, EXIT_TIE = 0, 1, 2, 3

TRIO = ("opt_expr", "opt_clean", "opt -nodffe -nosdff")
BISECT_PHASES = ("clean", "rmdead", "prune", "arst")
LAST_PHASE = "dff"  # Yosys's proc ends with proc_dff; proc_memwr; proc_clean

YOSYS_ASSERT_17 = "ctrl_sig.size() == 1"  # proc_mux.cc:264, §20 #17
ODIN3_E3 = "does not settle the value of"  # §15 E3, §20 #16

# §20's divergences proc-diff knows: the outcome each one shows, and what it is.
OUT_DIFFER, OUT_ASSERT, OUT_E3 = "differ", "yosys-assert", "odin3-e3"
OUT_TIE = "tie"
TIE_TEXT = ("tie: netlist-compare cannot order this netlist's tied cells (a reordered copy of a "
            "side differs from it), so the difference is unconfirmed")
DIVERGENCES: dict[int, tuple[str, str]] = {
    8: (OUT_DIFFER, "processes in node-ID order (two latches sharing a mux)"),
    9: (OUT_DIFFER, "bits ordered by primary wire (a group spanning wires)"),
    16: (OUT_E3, "E3 after 100 rounds where Yosys 0.55 errors at 102"),
    17: (OUT_ASSERT, "Yosys 0.55 asserts on an empty parallel_case select; 2E lowers it"),
    18: (OUT_DIFFER, "Yosys 0.55 gives wrong logic (a case appended to a dead mux); 2E is right"),
}
_CITE = re.compile(r"§20 #(\d+)")
_PROCESS_LINE = re.compile(r"^\s*process\s", re.MULTILINE)
_PARAM_LINE = re.compile(r"^(\s*\.param\s+)(\S+)(\s+)([01]+)\s*$")
_SIZE = re.compile(r"^(\d+)([KMG]?)$")
_SIZE_UNITS = {"": 1, "K": 1 << 10, "M": 1 << 20, "G": GIB}


class ProcDiffError(Exception):
    """A run-level error: the message is printed and the run exits 2."""


# --------------------------------------------------------------------------- options


@dataclass(frozen=True)
class Options:
    odin3: str
    yosys: str
    top: str | None = None
    nolatches: bool = False
    trio: bool = True
    with_opt: bool = False
    cap: int = DEFAULT_CAP
    techlib: str | None = None  # read_techlib before read_verilog (the micros' hard blocks)


# --------------------------------------------------------------------------- the four steps


def _read_verilog(opts: Options, src: Path) -> str:
    tech = f"read_techlib {opts.techlib}; " if opts.techlib else ""
    return f"{tech}read_verilog{' --nolatches' if opts.nolatches else ''} {src}"


def _odin3_argv(opts: Options, commands: Iterable[str]) -> list[str]:
    top = ["--top", opts.top] if opts.top else []
    return [opts.odin3, *top, "-p", "; ".join(commands)]


def _hierarchy(opts: Options) -> str:
    return f"hierarchy -top {opts.top}" if opts.top else "hierarchy -auto-top"


def _yosys_argv(opts: Options, rtlil: Path, passes: Iterable[str], blif_out: Path) -> list[str]:
    script = [f"read_rtlil {rtlil}", _hierarchy(opts), *passes, "opt_clean -purge",
              f"write_blif -blackbox -icells -param {blif_out}"]
    return [opts.yosys, "-p", "; ".join(script)]


def _front(opts: Options, src: Path) -> list[str]:
    """read_verilog and, unless --no-trio, the pre-proc passes (mode P)."""
    return [_read_verilog(opts, src), *(TRIO if opts.trio else ())]


def step_a(opts: Options, src: Path, work: Path) -> list[str]:
    proc = "proc" if opts.with_opt else "proc --noopt"
    return _odin3_argv(opts, [*_front(opts, src), f"write_rtlil {work / 'in.il'}", proc,
                              f"write_rtlil {work / 'a.il'}"])


def step_b(opts: Options, work: Path) -> list[str]:
    proc = "proc -norom" if opts.with_opt else "proc -norom -noopt"
    return _yosys_argv(opts, work / "in.il", [proc], work / "b.blif")


def step_a_blif(opts: Options, work: Path) -> list[str]:
    return _yosys_argv(opts, work / "a.il", [], work / "a.blif")


def norm_path(blif_path: Path) -> Path:
    """Where step 4 writes the normalised copy of a BLIF file (the original is kept)."""
    return blif_path.with_name(blif_path.stem + ".norm.blif")


def step_compare(work: Path, a_blif: Path) -> list[str]:
    return [sys.executable, str(NETLIST_COMPARE_PY), "--implicit-models",
            str(norm_path(a_blif)), str(norm_path(work / "b.blif"))]


def bisect_passes(phase: str, with_opt: bool) -> list[str]:
    """Point (q): the Yosys sub-passes after Odin III's --stop-after phase."""
    done = yosys_phase.passes_upto(phase)
    passes = yosys_phase.passes_upto(LAST_PHASE)[len(done):]
    return [*passes, "opt_expr -keepdc"] if with_opt else passes


def step_bisect_a(opts: Options, src: Path, work: Path, phase: str) -> list[str]:
    return _odin3_argv(opts, [*_front(opts, src), f"proc --stop-after {phase}",
                              f"write_rtlil {work / f'a_{phase}.il'}"])


def step_bisect_blif(opts: Options, work: Path, phase: str) -> list[str]:
    return _yosys_argv(opts, work / f"a_{phase}.il", bisect_passes(phase, opts.with_opt),
                       work / f"a_{phase}.blif")


def step_golden(opts: Options, src: Path, out: Path) -> list[str]:
    """Point (l): the golden is read_verilog; proc --noopt; write_rtlil (stable under 2F)."""
    return _odin3_argv(opts, [_read_verilog(opts, src), "proc --noopt", f"write_rtlil {out}"])


# The four steps of §18.3, in order: (name, the tool it starts, its builder).
StepBuilder = Callable[[Options, Path, Path], list[str]]
STEPS: tuple[tuple[str, str, StepBuilder], ...] = (
    ("A", "odin3", step_a),
    ("B", "yosys", lambda opts, src, work: step_b(opts, work)),
    ("A-BLIF", "yosys", lambda opts, src, work: step_a_blif(opts, work)),
    ("compare", "netlist-compare", lambda opts, src, work: step_compare(work, work / "a.blif")),
)


# --------------------------------------------------------------------------- normalisation


def load_int_params(path: Path = INT_PARAMS) -> frozenset[str]:
    """The INT parameter names, one per line (point (o))."""
    try:
        return frozenset(line.strip() for line in path.read_text(encoding="utf-8").splitlines()
                         if line.strip())
    except OSError as exc:
        raise ProcDiffError(f"cannot read {path}: {exc}") from exc


def _param_line(line: str, ints: frozenset[str]) -> str:
    match = _PARAM_LINE.match(line)
    if match is None or match.group(2) not in ints or len(match.group(4)) > INT_PARAM_BITS:
        return line
    return f"{match.group(1)}{match.group(2)}{match.group(3)}{int(match.group(4), 2)}"


def _buffers(lines: Sequence[str]) -> dict[int, tuple[str, str]]:
    """Line index -> (input, output) of each one-input ``.names`` whose only row is ``1 1``."""
    found: dict[int, tuple[str, str]] = {}
    for i, line in enumerate(lines):
        tokens = line.split()
        if len(tokens) != 3 or tokens[0] != ".names" or i + 1 == len(lines):
            continue
        rest = next((r.strip() for r in lines[i + 2:] if r.strip()), "")  # blank lines skipped
        more_rows = rest[:1] in ("0", "1", "-")
        if lines[i + 1].split() == ["1", "1"] and not more_rows:
            found[i] = (tokens[1], tokens[2])
    return found


def _root(net: str, drives: dict[str, str]) -> str | None:
    """The root of net's buffer chain, or None when the chain runs into a cycle."""
    seen = {net}
    while net in drives:
        net = drives[net]
        if net in seen:
            return None
        seen.add(net)
    return net


def _buffer_roots(lines: list[str]) -> None:
    """Each buffer reads the root of its chain (per model), so connect order cannot matter."""
    buffers = _buffers(lines)
    starts = [i for i, line in enumerate(lines) if line.startswith(".model")]
    for start, end in zip(starts or [0], [*starts[1:], len(lines)], strict=True):
        in_model = {i: io for i, io in buffers.items() if start <= i < end}
        drives = {out: inp for inp, out in in_model.values()}
        for i, (inp, out) in in_model.items():
            root = _root(inp, drives)
            if root is not None:  # a buffer on a cycle stays as written
                lines[i] = f".names {root} {out}"


def normalise_text(text: str, ints: frozenset[str]) -> str:
    """Step 4's rewrite of one BLIF text."""
    lines = [_param_line(line, ints) for line in text.splitlines()]
    _buffer_roots(lines)
    return "".join(f"{line}\n" for line in lines)


def normalise_file(path: Path, ints: frozenset[str]) -> Path:
    """Writes the normalised copy of path (always from the original, so repeating it is safe)."""
    out = norm_path(path)
    out.write_text(normalise_text(path.read_text(encoding="utf-8"), ints), encoding="utf-8")
    return out


def shuffle_cells(text: str, seed: int) -> str:
    """The same netlist with the cells of each model in another order (deterministic per seed).

    A cell is its ``.subckt``/``.names``/``.latch`` line and the ``.param``/cover lines after it.
    netlist-compare's canonical form can depend on cell order when a model holds tied cells
    that refinement cannot tell apart (twin ``$eq`` cells feeding a chain of muxes); a reordered
    copy then canonicalises differently, and an *identical* verdict is sound whichever order gave
    it (the canonical text is always a renaming of the input), so a difference is only reported
    when no reordering of side A agrees with side B."""
    rng = random.Random(seed)
    lines = text.splitlines()
    out: list[str] = []
    i = 0
    while i < len(lines):
        out.append(lines[i])
        i += 1
        if not lines[i - 1].startswith(".model"):
            continue
        header: list[str] = []
        while i < len(lines) and not lines[i].startswith(CELL_STARTS + (".end",)):
            header.append(lines[i])
            i += 1
        blocks: list[list[str]] = []
        while i < len(lines) and not lines[i].startswith(".end"):
            if lines[i].startswith(CELL_STARTS) or not blocks:
                blocks.append([])
            blocks[-1].append(lines[i])
            i += 1
        rng.shuffle(blocks)
        out += header + [line for block in blocks for line in block]
    return "".join(f"{line}\n" for line in out)


def shuffled_path(norm: Path, number: int) -> Path:
    return norm.with_name(f"{norm.stem}.shuf{number}.blif")


# --------------------------------------------------------------------------- children


def load_module(name: str, path: Path) -> ModuleType | None:
    """Imports the Python file at path as module name; None when it is missing or unloadable."""
    if not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        return None
    if str(path.parent) not in sys.path:  # its sibling imports (elab_sweep imports memcap)
        sys.path.insert(0, str(path.parent))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module  # @dataclass looks its module up in sys.modules
    spec.loader.exec_module(module)
    return module


@dataclass
class Child:
    """A finished child: its exit status and its output (stdout + stderr)."""

    code: int
    stdout: str
    stderr: str


def _capture_kwargs(run: Callable[..., Any]) -> dict[str, bool]:
    """``capture_output=True`` when memcap.run takes it: proc-diff reads each child's output
    (§20 #16's E3, #17's assert, the diff), so a memcap that captures only on request must."""
    try:
        params = inspect.signature(run).parameters.values()
    except (TypeError, ValueError):
        return {}
    if any(p.name == "capture_output" or p.kind is p.VAR_KEYWORD for p in params):
        return {"capture_output": True}
    return {}


def _text(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return str(value)


@dataclass
class Runner:
    """Starts every child through memcap.run (point (t)), loaded on first use."""

    cap: int
    memcap: ModuleType | None = None

    def run(self, argv: list[str]) -> Child:
        if self.memcap is None:
            self.memcap = load_module("memcap", MEMCAP_PY)
            if self.memcap is None:
                raise ProcDiffError("tools/elab-check/memcap.py is missing (2D-6)")
        try:
            done = self.memcap.run(argv, self.cap, **_capture_kwargs(self.memcap.run))
        except OSError as exc:
            return Child(EXIT_ERROR, "", f"cannot run {argv[0]}: {exc}")
        except (ValueError, RuntimeError) as exc:  # e.g. a cap above memcap's limit
            raise ProcDiffError(f"memcap refused {argv[0]}: {exc}") from exc
        return Child(int(done.returncode), _text(getattr(done, "stdout", "")),
                     _text(getattr(done, "stderr", "")))


# --------------------------------------------------------------------------- one file


@dataclass
class Outcome:
    """One file's result: code 0 identical, 1 differ, 2 error; kind names the outcome."""

    code: int
    kind: str
    detail: str = ""
    known: int | None = None  # a divergence recognised without a listing
    lines: list[str] = field(default_factory=list)  # printed under the file's line


def _tail(child: Child) -> list[str]:
    """The first ``STDERR_ERRORS`` error lines, then the last ``STDERR_TAIL`` lines: a failing
    child's errors come before its warnings and its summary, and an expect-fail needle (elab-sweep)
    is matched against what is printed."""
    lines = (child.stdout + child.stderr).splitlines()
    errors = [line for line in lines if "error" in line.lower()][:STDERR_ERRORS]
    tail = lines[-STDERR_TAIL:]
    return errors + [line for line in tail if line not in errors]


def _failed_step(step: str, tool: str, child: Child) -> Outcome:
    output = child.stdout + child.stderr
    if step == "B" and YOSYS_ASSERT_17 in output:
        return Outcome(EXIT_DIFFER, OUT_ASSERT, DIVERGENCES[17][1], known=17)
    kind = OUT_E3 if step == "A" and ODIN3_E3 in output else "error"
    return Outcome(EXIT_ERROR, kind, f"{tool} exited {child.code} (step {step})",
                   lines=_tail(child))


def _compare(runner: Runner, argv: list[str], ints: frozenset[str]) -> Child:
    """Writes the normalised copies netlist-compare reads (argv's last two paths, each from its
    original, never from an earlier copy), then runs netlist-compare on them."""
    for norm in (Path(argv[-2]), Path(argv[-1])):
        original = norm.with_name(norm.name.removesuffix(".norm.blif") + ".blif")
        try:
            normalise_file(original, ints)
        except (OSError, UnicodeDecodeError) as exc:
            return Child(EXIT_ERROR, "", f"cannot read {original}: {exc}")
    child = runner.run(argv)
    if child.code != EXIT_DIFFER:
        return child
    side_a = Path(argv[-2])
    for number in range(1, SHUFFLE_TRIES + 1):  # see shuffle_cells: identical is always sound
        copy = shuffled_path(side_a, number)
        copy.write_text(shuffle_cells(side_a.read_text(encoding="utf-8"), number),
                        encoding="utf-8")
        again = runner.run([*argv[:-2], str(copy), argv[-1]])
        if again.code != EXIT_DIFFER:
            return again
    if _order_dependent(runner, argv):
        return Child(EXIT_TIE, child.stdout, child.stderr)
    return child


def _order_dependent(runner: Runner, argv: list[str]) -> bool:
    """True when netlist-compare tells either side from a reordered copy of itself: it cannot
    order this netlist's tied cells, so a difference between the two sides proves nothing."""
    for side in (Path(argv[-2]), Path(argv[-1])):
        for number in range(SHUFFLE_TRIES + 1, SHUFFLE_TRIES + 3):
            copy = shuffled_path(side, number)
            copy.write_text(shuffle_cells(side.read_text(encoding="utf-8"), number),
                            encoding="utf-8")
            if runner.run([*argv[:-2], str(side), str(copy)]).code == EXIT_DIFFER:
                return True
    return False


def _compare_outcome(child: Child, work: Path) -> Outcome:
    if child.code == EXIT_SAME:  # overwritten, so a rerun under --keep never shows an older diff
        (work / "diff.txt").write_text("identical\n", encoding="utf-8")
        return Outcome(EXIT_SAME, "identical")
    if child.code == EXIT_DIFFER:
        (work / "diff.txt").write_text(child.stdout, encoding="utf-8")
        return Outcome(EXIT_DIFFER, OUT_DIFFER, lines=child.stdout.splitlines())
    if child.code == EXIT_TIE:
        (work / "diff.txt").write_text(child.stdout, encoding="utf-8")
        return Outcome(EXIT_TIE, OUT_TIE, TIE_TEXT, lines=[TIE_TEXT, *child.stdout.splitlines()])
    return _failed_step("compare", "netlist-compare", child)


def _run_steps(runner: Runner, opts: Options, src: Path, work: Path,
               ints: frozenset[str]) -> Outcome:
    known: Outcome | None = None  # §20 #17: B asserted; A's BLIF is still made from a.il
    for step, tool, build in STEPS:
        argv = build(opts, src, work)
        if step == "compare":
            return known or _compare_outcome(_compare(runner, argv, ints), work)
        child = runner.run(argv)
        if child.code != 0:
            failed = _failed_step(step, tool, child)
            if failed.known is None:
                return failed
            known = failed
        elif step == "A" and _PROCESS_LINE.search(_read(work / "a.il")):
            return Outcome(EXIT_ERROR, "error", "a.il still holds processes")
    raise AssertionError("unreachable: the compare step returns")


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError:
        return ""


def bisect(runner: Runner, opts: Options, src: Path, work: Path, ints: frozenset[str]) -> str:
    """§18.4: the first phase after which Odin III's partial result differs from b.blif."""
    for phase in BISECT_PHASES:
        for argv in (step_bisect_a(opts, src, work, phase),
                     step_bisect_blif(opts, work, phase)):
            child = runner.run(argv)
            if child.code != 0:
                return f"bisect stopped: {Path(argv[0]).name} exited {child.code} after {phase}"
        child = _compare(runner, step_compare(work, work / f"a_{phase}.blif"), ints)
        if child.code == EXIT_DIFFER:
            return f"first difference after {phase}"
        if child.code != EXIT_SAME:
            return f"bisect stopped: netlist-compare exited {child.code} after {phase}"
    return "no phase differs"


# --------------------------------------------------------------------------- collection


@dataclass(frozen=True)
class Item:
    """A Verilog file and its name in lines and expect lists (relative to its directory)."""

    path: Path
    rel: str


def _relative(path: Path, roots: Sequence[Path]) -> str:
    for root in roots:
        try:
            return path.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            continue
    return path.as_posix()


def _walk(paths: Sequence[str]) -> list[Item]:
    items: list[Item] = []
    for arg in paths:
        path = Path(arg)
        if path.is_dir():
            items += [Item(f, f.relative_to(path).as_posix()) for f in sorted(path.rglob("*.v"))
                      if f.is_file()]
        elif path.is_file():
            items.append(Item(path, path.as_posix()))
        else:
            raise ProcDiffError(f"{arg}: no such file or directory")
    return sorted(items, key=lambda item: (item.path.stat().st_size, item.rel))


def collect(paths: Sequence[str]) -> list[Item]:
    """Every file, smallest first: 2D's elab_sweep.collect when importable, else os.stat."""
    sweep = load_module("elab_sweep", ELAB_SWEEP_PY)
    if sweep is None or not hasattr(sweep, "collect"):
        return _walk(paths)
    roots = [Path(p) for p in paths if Path(p).is_dir()]
    found = [Path(getattr(f, "path", f)) for f in sweep.collect(list(paths))]  # 2D-7a: Item
    return [Item(f, _relative(f, roots)) for f in found]


# --------------------------------------------------------------------------- expect lists


def load_expect(path: str | None) -> dict[str, str]:
    """``<relative path> <one-line reason>`` per line; ``#`` comments and blank lines skipped."""
    if path is None:
        return {}
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as exc:
        raise ProcDiffError(f"cannot read {path}: {exc}") from exc
    listed: dict[str, str] = {}
    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        rel, _, reason = line.partition(" ")
        if not reason.strip():
            raise ProcDiffError(f"{path}:{lineno}: '{rel}' has no reason")
        listed[rel] = reason.strip()
    return listed


def cited(reason: str) -> int | None:
    """The §20 divergence a reason cites, when proc-diff knows it."""
    for match in _CITE.finditer(reason):
        number = int(match.group(1))
        if number in DIVERGENCES:
            return number
    return None


def verdict(outcome: Outcome, reason: str | None) -> tuple[int, str]:
    """(exit contribution, the file's line) of an outcome under its listing (None: unlisted)."""
    if reason is None:
        if outcome.known is not None:
            return EXIT_SAME, f"expected difference (§20 #{outcome.known}: {outcome.detail})"
        return outcome.code, _describe(outcome)
    if outcome.code == EXIT_SAME:
        return EXIT_DIFFER, f"identical, but listed as expected to fail ({reason})"
    number = cited(reason)
    if number is not None and DIVERGENCES[number][0] != outcome.kind:
        want = DIVERGENCES[number][0]
        return EXIT_DIFFER, f"listed as §20 #{number} ({want}), but {_describe(outcome)}"
    return EXIT_SAME, f"expected failure ({reason}): {_describe(outcome)}"


def _worse(a: int, b: int) -> int:
    """The graver exit code: error over difference over tie over identical."""
    rank = {EXIT_SAME: 0, EXIT_TIE: 1, EXIT_DIFFER: 2, EXIT_ERROR: 3}
    return a if rank.get(a, 3) >= rank.get(b, 3) else b


def _describe(outcome: Outcome) -> str:
    text = {EXIT_SAME: "identical", EXIT_DIFFER: "differs", EXIT_TIE: "tie"}.get(outcome.code,
                                                                                   "error")
    return f"{text}: {outcome.detail}" if outcome.detail else text


# --------------------------------------------------------------------------- the run


def _work_dir(keep: str | None, item: Item, used: set[str]) -> Path:
    """A temporary directory, or ``DIR/<sanitised rel>`` (``-2``, ``-3``… when names collide)."""
    if keep is None:
        return Path(tempfile.mkdtemp(prefix=f"proc-diff-{item.path.stem}-"))
    base = re.sub(r"[^\w.-]", "_", item.rel.removesuffix(".v"))
    slug, n = base, 1
    while slug in used:
        n += 1
        slug = f"{base}-{n}"
    used.add(slug)
    work = Path(keep) / slug
    work.mkdir(parents=True, exist_ok=True)
    return work


@dataclass
class Run:
    opts: Options
    runner: Runner
    ints: frozenset[str]
    keep: str | None
    do_bisect: bool
    used: set[str] = field(default_factory=set)

    def one(self, item: Item) -> Outcome:
        work = _work_dir(self.keep, item, self.used)
        outcome = _run_steps(self.runner, self.opts, item.path.resolve(), work, self.ints)
        if outcome.kind in (OUT_DIFFER, OUT_TIE):
            if self.do_bisect and outcome.kind == OUT_DIFFER:
                outcome.detail = bisect(self.runner, self.opts, item.path.resolve(), work,
                                        self.ints)
            outcome.lines.insert(0, f"files kept in {work}")
        elif self.keep is None:
            shutil.rmtree(work, ignore_errors=True)
        return outcome


def check_version(runner: Runner, yosys: str, allow: str | None) -> None:
    child = runner.run([yosys, "-V"])
    version = child.stdout.strip()
    if child.code != 0:
        why = child.stderr.strip() or f"exit {child.code}"
        raise ProcDiffError(f"cannot run {yosys} -V: {why}")
    if not yosys_phase.version_ok(version, allow):
        raise ProcDiffError(f"{yosys} is '{version}', not Yosys 0.55 "
                            "(--allow-version accepts another)")


def write_goldens(run: Run, items: Sequence[Item]) -> None:
    """Point (l): ``<slug>.il`` (``<slug>.nolatches.il``) beside each file."""
    suffix = ".nolatches.il" if run.opts.nolatches else ".il"
    for item in items:
        with tempfile.TemporaryDirectory(prefix="proc-diff-golden-") as tmp:
            out = Path(tmp) / "golden.il"
            child = run.runner.run(step_golden(run.opts, item.path.resolve(), out))
            if child.code != 0 or not out.is_file():
                raise ProcDiffError(f"{item.rel}: writing the golden failed "
                                    f"(odin3 exited {child.code})")
            dest = item.path.with_name(item.path.stem + suffix)
            shutil.copyfile(out, dest)
            print(f"{TOOL}: wrote {dest}")


def _report(item: Item, line: str, outcome: Outcome) -> None:
    print(f"{item.rel}: {line}")
    for extra in outcome.lines:
        print(f"  {extra}")


def warn_names(items: Sequence[Item], expect: dict[str, str]) -> None:
    """Warns about a name two files share (one expect entry covers both) and stale entries."""
    names = [item.rel for item in items]
    for rel in sorted({rel for rel in names if names.count(rel) > 1}):
        print(f"{TOOL}: warning: several files are named '{rel}'", file=sys.stderr)
    for rel in sorted(set(expect) - set(names)):
        print(f"{TOOL}: warning: expect-fail entry '{rel}' matches no file", file=sys.stderr)


def run_all(run: Run, items: Sequence[Item], expect: dict[str, str], write: bool) -> int:
    warn_names(items, expect)
    worst = EXIT_SAME
    every_identical = True
    for item in items:
        outcome = run.one(item)
        code, line = verdict(outcome, expect.get(item.rel))
        _report(item, line, outcome)
        if code != EXIT_SAME and outcome.code == EXIT_ERROR:
            print(f"{TOOL}: {outcome.detail}", file=sys.stderr)
        worst = _worse(worst, code)
        every_identical = every_identical and outcome.code == EXIT_SAME
    if len(items) > 1:
        print(f"{TOOL}: {len(items)} files, exit {worst}")
    if write:
        if not every_identical:
            print(f"{TOOL}: --write refused: the run is not identical", file=sys.stderr)
            return EXIT_ERROR
        write_goldens(run, items)
    return worst


def parse_size(text: str) -> int:
    match = _SIZE.match(text.strip().upper())
    if match is None:
        raise argparse.ArgumentTypeError(f"bad size {text!r} (want e.g. 4G, 512M)")
    return int(match.group(1)) * _SIZE_UNITS[match.group(2)]


def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--top", help="the top module (default: hierarchy -auto-top)")
    ap.add_argument("--nolatches", action="store_true", help="read_verilog --nolatches")
    ap.add_argument("--no-trio", action="store_true", help="no pre-proc opt passes on side A")
    ap.add_argument("--with-opt", action="store_true", help="proc / proc -norom (opt_expr)")
    ap.add_argument("--bisect", action="store_true", help="name the first phase that differs")
    ap.add_argument("--mem-cap", type=parse_size, default=DEFAULT_CAP, help="per-child cap")
    ap.add_argument("--keep", metavar="DIR", help="keep every work directory under DIR")
    ap.add_argument("--write", action="store_true", help="write goldens when identical")
    ap.add_argument("--expect-fail", metavar="F", help="expected failures with reasons")
    ap.add_argument("--allow-version", metavar="V", help="accept a Yosys whose -V names V")
    ap.add_argument("--techlib", metavar="L", help="read_techlib L before each file (hard blocks)")
    ap.add_argument("inputs", nargs="+", metavar="FILE_OR_DIR")
    return ap


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    opts = Options(odin3=args.odin3, yosys=args.yosys, top=args.top, nolatches=args.nolatches,
                   trio=not args.no_trio, with_opt=args.with_opt, cap=args.mem_cap,
                   techlib=args.techlib)
    runner = Runner(cap=args.mem_cap)
    try:
        expect = load_expect(args.expect_fail)
        items = collect(args.inputs)
        ints = load_int_params()
        check_version(runner, args.yosys, args.allow_version)
        run = Run(opts, runner, ints, args.keep, args.bisect)
        return run_all(run, items, expect, args.write)
    except ProcDiffError as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_ERROR
    except Exception as exc:  # e.g. a 2D-6/2D-7 module of another shape: an error, not "differ"
        print(f"{TOOL}: internal error: {type(exc).__name__}: {exc}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
