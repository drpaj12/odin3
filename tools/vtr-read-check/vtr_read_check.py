"""vtr-read-check: VPR reads every golden-equivalent BLIF (spec 3E §16.4, plan Task 11).

Usage::

    vtr-read-check [--golden-root DIR] [--arch ARCH.xml] [--odin3 PATH] [--vpr PATH]
                   [--checker PATH] [--work DIR] [--report FILE] [--limit N] [--timeout S]
    vtr-read-check --bad-corpus DIR [--arch ARCH.xml] [--vpr PATH] [--checker PATH] [--work DIR]

For every ``status=ok`` golden G under the golden root (every ``*.blif`` whose sibling ``.prov``
has the line ``status=ok``), smallest first and one at a time, in a fresh ``<work>/<n>/``::

    odin3 -p "read_techlib <repo>/lib/vtr.o3lib; read_blif G; write_blif --vtr W"   # W or a refusal
    vpr <arch> W --circuit_format blif --exit_before_pack on                         # when written
    vpr <arch> G --circuit_format blif --exit_before_pack on
    blif-vtr-check --strict --writer-log <the write's stderr> W     # when written
    blif-vtr-check G

``--exit_before_pack on`` makes VPR read, clean and verify the netlist, then stop: VPR's whole
reader at a fraction of a route's cost. The verdict of a file is spec §16.4's:

- a written W needs VPR exit 0 and the checker exit 0 (``--strict`` with the writer's log: a
  class-U ``unconn`` on a logic pin is allowed only where the writer warned of it);
- a refusal pairs with VPR's verdict on G per ``pairing_ok`` (the §16.4 table, plus the E1b
  amendment of plan point (e)); E3b, E8 (VPR accepts G) and E11 (depends) are passing but
  ``listed``;
- the checker's verdict on G equals VPR's.

A step the memory cap kills (VPR or the checker on the largest designs) is ``oom``, and the
file's verdict ``capped (listed)``: unverified, never ``pass``. A SIGKILL counts as the cap's only
when the capping cgroup's ``oom_kill`` counter moved during the step (read before and after);
otherwise (an outside ``kill -9``) the step is a crash and the file fails. The cap is printed in
GiB. A capped file must be in ``EXPECTED_CAPPED`` (with its evidence): an unlisted capped file
fails, and so does a listed file that no longer caps (a stale entry). ``--xl`` (9 GiB, the
controller's) is the way to read the listed files. (Agent default.)

``--bad-corpus DIR`` runs VPR and the checker on each ``*.blif`` under DIR (``tests/vtr/blif-bad``)
instead: VPR must reject exactly the files the checker rejects, and the message in a file's
``# vpr:`` first line must occur (as a substring) in VPR's output, which is searched on stdout and
stderr (an assertion failure aborts with its text on stderr). Files without that line (the
``strict/`` ones) must be accepted by both. The corpus is 18 tiny files, so this mode is not a
heavy run.

The report is ``<work>/report.tsv`` (header ``golden``, ``tool``, ``written|refused``,
``error class``, ``vpr_W``, ``vpr_G``, ``checker_W``, ``checker_G``, ``verdict``), one row per
file, flushed as it is made. A passing file's run directory is removed with ``safe_rmtree``;
a failing one is kept. Exit 0 when every verdict passes, 1 otherwise, 2 for usage or a failed
``verify_scope`` (the cap).

Heavy-run rule (plan point (a)): the main sweep starts with ``qor_heavy.reexec_under_lock``. Inside
a heavy run it checks the cap and continues; outside, with ``ODIN3_HEAVY_OK=1`` it re-runs itself
through ``tools/heavy-run`` with the caller's environment unchanged, else it exits 2. No 3E tool
sets ``ODIN3_HEAVY_OK``. The checker holds every token of a file in memory (about 3 GB on
``LU64PEEng``): it runs inside the heavy-run scope, never beside it.

Standard library only (plus ``tools/vtr-qor/qor_heavy.py``).
"""

from __future__ import annotations

import argparse
import os
import pwd
import re
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = Path(__file__).resolve()
sys.path.insert(0, str(REPO / "tools" / "vtr-qor"))
import qor_heavy  # noqa: E402
from qor_heavy import PREFIX, HeavyError  # noqa: E402

PROG = "vtr-read-check"
TECHLIB = REPO / "lib" / "vtr.o3lib"
HOME = Path(pwd.getpwuid(os.getuid()).pw_dir)
DEFAULT_GOLDEN = HOME / "odin3-ws" / "golden" / "k6_frac_N10_frac_chain_mem32K_40nm"
DEFAULT_WORK = HOME / "odin3-ws" / "work" / "vtr-read-check"
DEFAULT_ODIN3 = REPO / "build" / "release" / "odin3"
DEFAULT_CHECKER = REPO / "tools" / "blif-vtr-check" / "blif-vtr-check"
W_NAME = "W.blif"
WRITE_LOG = "write.log"
ARCH_NAME = "k6_frac_N10_frac_chain_mem32K_40nm.xml"
DEFAULT_TIMEOUT = 3600  # seconds per step (the largest design reads for a few minutes)
EXIT_USAGE = 2
HEADER = (
    "golden\ttool\twritten|refused\terror class\tvpr_W\tvpr_G\tchecker_W\tchecker_G\tverdict"
)
OK = "ok"
FAIL = "fail"
NA = "-"
PASS = "pass"
LISTED = "listed"
CAPPED_LISTED = "capped (listed)"
OOM = "oom"
SIGKILL_EXITS = (-9, 137)  # a SIGKILLed step: the cap's only when oom_kill moved (Step.oom_kills)
GIB = 1024**3
# Designs the 6 GiB cap is known to kill (the golden file's stem; every tool and tree). Evidence:
# the 3E-4 sweep (release build, tools/heavy-run, 2026-10-14) had VPR SIGKILLed with the cgroup's
# oom_kill counter moving, on both G and W, for all seven files of these four designs (the
# checker alone holds about 3 GB on LU64PEEng). An unlisted capped file is a failure.
EXPECTED_CAPPED = frozenset({"bgm", "LU32PEEng", "LU64PEEng", "LargeRam"})

# The refusal lines of `write_blif --vtr` (spec §11): the text that tells the class.
CLASS_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("E1", re.compile(r"falling-edge flip-flop")),
    ("E1b", re.compile(r"asynchronous set/reset flip-flop")),
    ("E2", re.compile(r"level-sensitive latch")),
    ("E3", re.compile(r"flip-flop without a clock")),
    ("E3b", re.compile(r"clock input .* is unconnected or x")),
    ("E4", re.compile(r"tri-state cell")),
    ("E5", re.compile(r"not a netlist cell")),
    ("E6", re.compile(r"is not a hard cell of a loaded tech library")),
    ("E7", re.compile(r" inputs exceed K=")),
    ("E8", re.compile(r"cover mixes on-set and off-set rows")),
    ("E9", re.compile(r"inout port")),
    ("E10", re.compile(r"hierarchy not flattened")),
    ("E11", re.compile(r"cannot be recovered from BLIF formals")),
    ("E12", re.compile(r" has \d+ drivers \(")),
    ("E13", re.compile(r"no top module")),
)
REFUSAL_LINE = re.compile(r"^odin3: error: write_blif: --vtr: (.*)$")
# `read_blif` itself refuses a multiply-driven net (PHASE3 #36): "<file>:<line>: net 'x' is driven
# by N cell outputs (one allowed)". It is class E12, but only for the Odin II goldens PHASE1 #15
# listed (the same list as tools/blif-roundtrip and tools/writer-check): any other read failure
# stays a failure.
READ_MULTI_DRIVER = re.compile(
    r"^odin3: error: \S.*:\d+: net '.*' is driven by \d+ cell outputs \(one allowed\)$"
)
MULTI_DRIVER_GOLDENS = frozenset({"elsif_both_defined.odin.blif", "multi_assignment.odin.blif"})
MORE_ERRORS = re.compile(r"\d+ more errors not shown")

# §16.4's pairing table: the refusal class -> what VPR says about G (substrings of its message).
# E1b amends the table (plan point (e)): an async flip-flop reaches a golden as a `.subckt`.
PAIRING: dict[str, tuple[str, ...]] = {
    "E1": ("Only rising edge latches supported",),
    "E1b": ("Subckt instantiates model",),
    "E2": ("Only rising edge latches supported",),
    "E3": ("Latch must have a clock",),
    "E5": ("Subckt instantiates model", "Blackbox BLIF model"),
    "E6": ("Subckt instantiates model", "Blackbox BLIF model"),
    "E7": ("BLIF .names input size",),
    "E10": ("Found multiple models with primitives",),
    # Spec: the R16 assertion (its text is the one tests/vtr/blif-bad/c7_twice.blif pins; a bare
    # "Assertion" would pair any VPR assertion). Measured on the two Odin II multi-driver goldens
    # (elsif_both_defined.odin.blif, multi_assignment.odin.blif; 2026-10-14 sweep): VPR's netlist
    # consistency check throws "Inconsistent block data sizes" instead; both are VPR refusing a
    # multi-driver net, so both pair.
    "E12": (
        "Assertion 'pin_net(pin_id) == net_id' failed",
        "Must be no existing net driver",
        "Inconsistent block data sizes",
    ),
    "E13": ("No non-blackbox models found",),
}
# VPR accepts G (an open pin, an undefined function): an intended refusal.
ACCEPTED_BY_VPR = frozenset({"E3b", "E8"})
# "depends; listed per file".
DEPENDS = frozenset({"E11"})
# E4 (no BLIF tri-state) and E9 (no BLIF inout) cannot occur in a golden: no pairing at all.

VPR_MESSAGE = re.compile(r"^Message: (.*)$", re.MULTILINE)
VPR_ASSERTION = re.compile(r"^.*Assertion '.*' failed.*$", re.MULTILINE)


def pairing_ok(refusal: str, vpr_g_message: str) -> bool:
    """Does the refusal class pair with VPR's verdict on G (§16.4's table, point (e))?

    ``vpr_g_message`` is VPR's error message on G, or ``""`` when VPR accepted G. E1b pairs with
    ``Subckt instantiates model`` (an async flip-flop reaches the golden as a ``.subckt``); E3b and
    E8 pair only with an accepted G; E11 pairs with anything (listed per file); E4 and E9 cannot
    occur in a golden and pair with nothing."""
    if refusal in DEPENDS:
        return True
    if refusal in ACCEPTED_BY_VPR:
        return vpr_g_message == ""
    needles = PAIRING.get(refusal)
    if needles is None or vpr_g_message == "":
        return False
    return any(needle in vpr_g_message for needle in needles)


def refusal_classes(stderr: str, golden: Path | None = None) -> list[str]:
    """The error classes of a refused write's log, in order of first appearance. With a golden
    named in MULTI_DRIVER_GOLDENS, a `read_blif` multi-driver refusal counts as E12."""
    found: list[str] = []
    read_gate = golden is not None and golden.name in MULTI_DRIVER_GOLDENS
    for line in stderr.splitlines():
        if read_gate and READ_MULTI_DRIVER.match(line) and "E12" not in found:
            found.append("E12")
            continue
        match = REFUSAL_LINE.match(line)
        if match is None or MORE_ERRORS.match(match[1]):
            continue
        cls = next((name for name, pat in CLASS_PATTERNS if pat.search(match[1])), "E?")
        if cls not in found:
            found.append(cls)
    return found


@dataclass
class Step:
    """One finished subprocess."""

    returncode: int
    output: str  # stdout then stderr
    stderr: str
    seconds: float
    timed_out: bool = False
    oom_kills: int = 0  # the capping cgroup's oom_kill counter moved by this much during the step


@dataclass
class VprRun:
    """VPR's verdict on one netlist: ``status`` is ok, fail (exit 1), crash (a signal or an
    assertion) or timeout; ``message`` is its error message ("" when ok)."""

    status: str
    message: str

    @property
    def accepted(self) -> bool:
        return self.status == OK


@dataclass
class Row:
    """One report row; ``reason`` (why a verdict fails) is printed, not written to the TSV."""

    golden: str
    tool: str
    written: str = NA
    error_class: str = NA
    vpr_w: str = NA
    vpr_g: str = NA
    checker_w: str = NA
    checker_g: str = NA
    verdict: str = FAIL
    reason: str = ""
    seconds: float = 0.0

    def cells(self) -> list[str]:
        return [self.golden, self.tool, self.written, self.error_class, self.vpr_w, self.vpr_g,
                self.checker_w, self.checker_g, self.verdict]


@dataclass
class ReadCtx:
    """What a run needs: the tools, the arch, the work directory and the golden root."""

    odin3: Path
    vpr: Path
    arch: Path
    checker: Path
    work: Path
    root: Path
    timeout: int = DEFAULT_TIMEOUT
    techlib: Path = TECHLIB
    counter: int = field(default=0)
    kills: Callable[[], int] | None = None  # the capping cgroup's oom_kill counter, if verified
    cap_gib: float | None = None

    def step(self, argv: Sequence[str], cwd: Path) -> Step:
        """``run_step`` with the oom_kill delta recorded."""
        return run_step(argv, cwd, self.timeout, self.kills)

    def fresh_dir(self) -> Path:
        """A new, empty ``<work>/<n>`` (``<n>.<k>`` when an earlier failed run left ``<n>``)."""
        self.counter += 1
        path = self.work / str(self.counter)
        extra = 0
        while path.exists():
            extra += 1
            path = self.work / f"{self.counter}.{extra}"
        path.mkdir(parents=True)
        return path


def run_step(
    argv: Sequence[str], cwd: Path, timeout: int, kills: Callable[[], int] | None = None
) -> Step:
    """Run argv in cwd with the caller's environment, capturing its output; a timeout is a step.
    ``kills`` reads the cap's oom_kill counter: its delta over the step is ``Step.oom_kills``."""
    before = kills() if kills is not None else 0
    step = _run_step(argv, cwd, timeout)
    if kills is not None:
        step.oom_kills = max(0, kills() - before)
    return step


def _run_step(argv: Sequence[str], cwd: Path, timeout: int) -> Step:
    start = time.monotonic()
    try:
        proc = subprocess.run(
            list(argv), cwd=cwd, capture_output=True, timeout=timeout, check=False
        )
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or b"").decode("utf-8", errors="replace")
        err = (exc.stderr or b"").decode("utf-8", errors="replace")
        return Step(-1, out + err, err, time.monotonic() - start, timed_out=True)
    except OSError as exc:
        text = f"cannot run {argv[0]}: {exc.strerror}"
        return Step(EXIT_USAGE, text, text, time.monotonic() - start)
    out = proc.stdout.decode("utf-8", errors="replace")
    err = proc.stderr.decode("utf-8", errors="replace")
    return Step(proc.returncode, out + err, err, time.monotonic() - start)


def vpr_verdict(step: Step) -> VprRun:
    """VPR's verdict from a finished run: exit 0 ok; exit 1 fail with its ``Message:``; a signal
    (an assertion aborts) or any other exit a crash, with the assertion's text when it has one;
    SIGKILL is ``oom`` only when the cap's oom_kill counter moved (the file is then unverified,
    not failed); any other SIGKILL is a crash."""
    if step.timed_out:
        return VprRun("timeout", "timed out")
    if step.returncode in SIGKILL_EXITS:
        if step.oom_kills > 0:
            return VprRun(OOM, "killed (memory cap)")
        return VprRun("crash", "SIGKILL without an oom_kill (not the memory cap)")
    if step.returncode == 0:
        return VprRun(OK, "")
    assertion = VPR_ASSERTION.search(step.output)
    if assertion is not None:
        return VprRun("crash", assertion[0].strip())
    message = VPR_MESSAGE.search(step.output)
    if message is not None:
        status = FAIL if step.returncode == 1 else "crash"
        return VprRun(status, message[1].strip())
    status = FAIL if step.returncode == 1 else "crash"
    return VprRun(status, f"VPR exited {step.returncode} without a message")


def run_vpr(ctx: ReadCtx, netlist: Path, where: Path) -> VprRun:
    """VPR's reader on netlist, in the fresh directory ``where`` (it writes its logs there)."""
    where.mkdir(parents=True, exist_ok=True)
    argv = [str(ctx.vpr), str(ctx.arch), str(netlist), "--circuit_format", "blif",
            "--exit_before_pack", "on"]
    return vpr_verdict(ctx.step(argv, where))


def checker_verdict(step: Step) -> str:
    """ok (exit 0), fail (exit 1: violations), oom (killed by the memory cap) or error (exit 2, a
    crash, a timeout or a SIGKILL the cap did not make)."""
    if step.timed_out:
        return "timeout"
    if step.returncode in SIGKILL_EXITS:
        return OOM if step.oom_kills > 0 else "error"
    return {0: OK, 1: FAIL}.get(step.returncode, "error")


def run_checker(ctx: ReadCtx, netlist: Path, extra: Sequence[str] = ()) -> str:
    argv = [str(ctx.checker), *extra, str(netlist)]
    return checker_verdict(ctx.step(argv, netlist.parent))


def tool_of(golden: Path) -> str:
    if golden.name.endswith(".parmys.blif"):
        return "parmys"
    return "odin" if golden.name.endswith(".odin.blif") else "other"


def write_netlist(ctx: ReadCtx, golden: Path, out: Path, where: Path) -> Step:
    """``odin3 -p "read_techlib …; read_blif G; write_blif --vtr W"`` in ``where``."""
    script = f'read_techlib "{ctx.techlib}"; read_blif "{golden}"; write_blif --vtr "{out}"'
    return ctx.step([str(ctx.odin3), "-p", script], where)


def judge_written(vpr_w: VprRun, checker_w: str) -> str:
    """The reason a written W fails, "" when it passes."""
    reasons = []
    if not vpr_w.accepted and vpr_w.status != OOM:
        reasons.append(f"VPR rejects W ({vpr_w.status}: {vpr_w.message})")
    if checker_w not in (OK, OOM):
        reasons.append(f"blif-vtr-check --strict W: {checker_w}")
    return "; ".join(reasons)


def judge_refused(classes: list[str], vpr_g: VprRun) -> tuple[str, str]:
    """(verdict, reason) of a refusal against VPR's verdict on G."""
    message = vpr_g.message if not vpr_g.accepted else ""
    if vpr_g.status == "timeout":
        return FAIL, "VPR timed out on G"
    if vpr_g.status == OOM:
        return PASS, ""  # unverified: the row is capped (run_one)
    if not any(pairing_ok(cls, message) for cls in classes):
        shown = message or "accepted"
        return FAIL, f"refusal {'+'.join(classes)} does not pair with VPR on G ({shown})"
    if any(cls in ACCEPTED_BY_VPR or cls in DEPENDS for cls in classes):
        return LISTED, ""
    return PASS, ""


def judge_agreement(vpr_g: VprRun, checker_g: str) -> str:
    """The reason the checker and VPR disagree on G, "" when they agree."""
    if checker_g in ("error", "timeout"):
        return f"blif-vtr-check G: {checker_g}"
    if vpr_g.status == "timeout":
        return "VPR timed out on G"
    if OOM in (vpr_g.status, checker_g):
        return ""
    if vpr_g.accepted != (checker_g == OK):
        return f"checker and VPR disagree on G (VPR {vpr_g.status}, checker {checker_g})"
    return ""


def check_written(row: Row, ctx: ReadCtx, where: Path) -> str:
    """VPR and the strict checker on W; fills the row, returns the failure reason or ""."""
    w_path = where / W_NAME
    vpr_w = run_vpr(ctx, w_path, where / "vpr_w")
    row.vpr_w = vpr_w.status
    log = ["--strict", "--writer-log", str(where / WRITE_LOG)]
    row.checker_w = run_checker(ctx, w_path, log)
    return judge_written(vpr_w, row.checker_w)


def run_one(g: Path, ctx: ReadCtx) -> Row:
    """The §16.4 steps for one golden; the run directory is removed when the verdict passes."""
    start = time.monotonic()
    row = Row(golden=os.path.relpath(g, ctx.root), tool=tool_of(g))
    where = ctx.fresh_dir()
    w_path = where / W_NAME
    write = write_netlist(ctx, g, w_path, where)
    (where / WRITE_LOG).write_text(write.stderr, encoding="utf-8", errors="replace")
    classes = refusal_classes(write.stderr, g)
    if write.returncode == 0 and w_path.is_file():
        row.written = "written"
    elif classes and not w_path.exists():
        row.written, row.error_class = "refused", "+".join(classes)
    else:
        row.written = "failed"
    vpr_g = run_vpr(ctx, g, where / "vpr_g")
    row.vpr_g = vpr_g.status
    row.checker_g = run_checker(ctx, g)
    verdict, reasons = PASS, []
    if row.written == "written":
        reasons.append(check_written(row, ctx, where))
    elif row.written == "refused":
        verdict, why = judge_refused(classes, vpr_g)
        reasons.append(why)
    else:
        last = write.stderr.strip().splitlines()[-1:] or [f"exit {write.returncode}"]
        reasons.append(f"odin3 failed: {last[0]}")
    reasons.append(judge_agreement(vpr_g, row.checker_g))
    capped = OOM in (row.vpr_g, row.vpr_w, row.checker_g, row.checker_w)
    stem = g.name.split(".")[0]
    if capped and stem not in EXPECTED_CAPPED:
        reasons.append(f"capped by the memory cap but not in EXPECTED_CAPPED ({stem})")
    elif not capped and stem in EXPECTED_CAPPED:
        reasons.append(f"listed in EXPECTED_CAPPED but read fine ({stem}): a stale entry")
    row.reason = "; ".join(r for r in reasons if r)
    row.verdict = FAIL if row.reason else CAPPED_LISTED if capped else verdict
    row.seconds = time.monotonic() - start
    if row.verdict != FAIL:
        remove_run(where, ctx.work)
    return row


def remove_run(where: Path, work: Path) -> None:
    """Remove a passing file's run directory with one absolute path (``safe_rmtree``); a refusal
    to delete keeps it and says so."""
    try:
        qor_heavy.safe_rmtree(where, work)
    except HeavyError as exc:
        print(f"{PROG}: kept {where}: {exc.message}", file=sys.stderr)


def collect(root: Path) -> list[Path]:
    """Every ``*.blif`` under root whose sibling ``.prov`` has the line ``status=ok``, smallest
    first (then by path)."""
    found = []
    for path in root.rglob("*.blif"):
        prov = path.with_suffix(".prov")
        try:
            lines = prov.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        if "status=ok" in lines:
            found.append(path)
    return sorted(found, key=lambda p: (p.stat().st_size, str(p)))


def bad_corpus(corpus: Path, ctx: ReadCtx) -> int:
    """VPR and the checker on each file of the corpus; 0 when they agree on every file."""
    failures = 0
    for path in sorted(corpus.rglob("*.blif")):
        expected = vpr_line(path)
        where = ctx.fresh_dir()
        step = ctx.step([str(ctx.vpr), str(ctx.arch), str(path), "--circuit_format", "blif",
                         "--exit_before_pack", "on"], where)
        checker = run_checker(ctx, path)
        rejected = step.returncode != 0
        problems = []
        if checker not in (OK, FAIL):
            problems.append(f"the checker {checker}")
        elif rejected != (checker == FAIL):
            problems.append(f"VPR {'rejects' if rejected else 'accepts'} it, the checker {checker}")
        if expected is None and rejected:
            problems.append("VPR rejects a file with no '# vpr:' line")
        if expected is not None and not rejected:
            problems.append(f"VPR accepts it; expected '{expected}'")
        if expected is not None and rejected and expected not in step.output:
            problems.append(f"VPR's message lacks '{expected}'")
        name = path.relative_to(corpus)
        if problems:
            failures += 1
            print(f"{PROG}: FAIL {name}: {'; '.join(problems)}")
        else:
            print(f"{PROG}: ok   {name}: VPR {'rejects' if rejected else 'accepts'}, "
                  f"checker {checker}")
    print(f"{PROG}: bad corpus: " + (f"{failures} failing" if failures else "all agree"))
    return 1 if failures else 0


def vpr_line(path: Path) -> str | None:
    """The text of the file's first-line ``# vpr: …`` comment, or None."""
    first = path.read_text(encoding="utf-8", errors="replace").splitlines()[:1]
    if first and first[0].startswith("# vpr: "):
        return first[0][len("# vpr: "):].strip()
    return None


def vtr_root() -> Path:
    """``$VTR_ROOT``, else the checkout next to the golden tree."""
    default = HOME / "odin3-ws" / "external" / "vtr-verilog-to-routing"
    return Path(os.environ.get("VTR_ROOT", default))


def absolute(path: Path) -> Path:
    """path made absolute without resolving symlinks (every step runs in its own directory)."""
    return Path(os.path.abspath(path))


def default_arch() -> Path:
    return vtr_root() / "vtr_flow" / "arch" / "timing" / ARCH_NAME


def default_vpr() -> Path:
    return vtr_root() / "vpr" / "vpr"


def _positive(text: str) -> int:
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return value


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog=PROG, description=__doc__.split("\n\n")[0])
    ap.add_argument("--golden-root", type=Path, default=DEFAULT_GOLDEN)
    ap.add_argument("--arch", type=Path, default=None, help="the k6 arch XML under $VTR_ROOT")
    ap.add_argument("--odin3", type=Path, default=DEFAULT_ODIN3)
    ap.add_argument("--vpr", type=Path, default=None, help="default $VTR_ROOT/vpr/vpr")
    ap.add_argument("--checker", type=Path, default=DEFAULT_CHECKER)
    ap.add_argument("--work", type=Path, default=DEFAULT_WORK)
    ap.add_argument("--report", type=Path, default=None, help="default <work>/report.tsv")
    ap.add_argument("--limit", type=_positive, default=None, help="only the N smallest goldens")
    ap.add_argument("--timeout", type=_positive, default=DEFAULT_TIMEOUT, metavar="SECONDS",
                    help="per step (default %(default)s)")
    ap.add_argument("--bad-corpus", type=Path, metavar="DIR", default=None,
                    help="run VPR and the checker on tests/vtr/blif-bad instead")
    return ap


def summarize(rows: list[Row], report: Path, ctx_cap: float | None = None) -> None:
    """The counts: written and refused per tool and class, verdicts, run times."""
    counts: dict[tuple[str, str], int] = {}
    for row in rows:
        key = (row.tool, row.written if row.written != "refused" else f"refused {row.error_class}")
        counts[key] = counts.get(key, 0) + 1
    print(f"{PROG}: {len(rows)} goldens")
    for (tool, what), n in sorted(counts.items()):
        print(f"  {tool}: {what}: {n}")
    for verdict in (PASS, LISTED, CAPPED_LISTED, FAIL):
        print(f"  verdict {verdict}: {sum(1 for r in rows if r.verdict == verdict)}")
    if ctx_cap is not None:
        print(f"  memory cap {ctx_cap:g} GiB: capped (listed) files were not read by VPR")
    print(f"  total {sum(r.seconds for r in rows):.0f} s; report {report}")


def sweep(args: argparse.Namespace, ctx: ReadCtx) -> int:
    goldens = collect(ctx.root)
    if args.limit is not None:
        goldens = goldens[: args.limit]
    if not goldens:
        print(f"{PROG}: no status=ok golden under {ctx.root}", file=sys.stderr)
        return EXIT_USAGE
    ctx.work.mkdir(parents=True, exist_ok=True)
    report = args.report if args.report is not None else ctx.work / "report.tsv"
    rows: list[Row] = []
    with report.open("w", encoding="utf-8") as out:
        out.write(HEADER + "\n")
        for golden in goldens:
            row = run_one(golden, ctx)
            rows.append(row)
            out.write("\t".join(row.cells()) + "\n")
            out.flush()
            if row.verdict == FAIL:
                print(f"{PROG}: FAIL {row.golden}: {row.reason}", flush=True)
    summarize(rows, report, ctx.cap_gib)
    return 1 if any(row.verdict == FAIL for row in rows) else 0


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    given = list(sys.argv[1:] if argv is None else argv)
    ctx = ReadCtx(
        odin3=absolute(args.odin3),
        vpr=absolute(args.vpr if args.vpr is not None else default_vpr()),
        arch=absolute(args.arch if args.arch is not None else default_arch()),
        checker=absolute(args.checker),
        work=absolute(args.work),
        root=absolute(args.golden_root),
        timeout=args.timeout,
    )
    try:
        qor_heavy.check_out_dir(ctx.work, HOME)
    except HeavyError as exc:
        print(f"{PREFIX}{exc.message}", file=sys.stderr)
        return exc.code
    if args.bad_corpus is not None:
        ctx.work.mkdir(parents=True, exist_ok=True)
        return bad_corpus(absolute(args.bad_corpus), ctx)
    try:
        scope = qor_heavy.reexec_under_lock(given, script=SCRIPT, lock=qor_heavy.HEAVY_LOCK)
    except HeavyError as exc:
        print(f"{PREFIX}{exc.message}", file=sys.stderr)
        return exc.code
    if isinstance(scope, qor_heavy.ScopeInfo):
        ctx.kills = lambda: qor_heavy.oom_kills(scope)
        ctx.cap_gib = scope.memory_max / GIB
    return sweep(args, ctx)


if __name__ == "__main__":
    sys.exit(main())
