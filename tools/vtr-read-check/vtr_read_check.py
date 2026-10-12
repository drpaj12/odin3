"""vtr-read-check: VPR reads every golden-equivalent BLIF (3E spec §16.4, plan Task 11).

Usage::

    vtr-read-check [--golden-root DIR] [--arch ARCH.xml] [--odin3 PATH] [--checker PATH]
                   [--work DIR] [--report FILE] [--limit N] [--bad-corpus DIR]

For each k6 golden G (every ``*.blif`` whose sibling ``.prov`` says ``status=ok``), smallest
first, one at a time, in a fresh ``<work>/<n>/``:

1. ``odin3 -p "read_techlib <repo>/lib/vtr.o3lib; read_blif G; write_blif --vtr W"``;
2. ``$VTR_ROOT/vpr/vpr <arch> W --circuit_format blif --exit_before_pack on`` (when written);
3. the same on G;
4. ``blif-vtr-check --strict --writer-log <step 1's stderr> W`` and ``blif-vtr-check G``.

Verdicts (§16.4): a written W needs VPR exit 0 and the checker exit 0; a refusal's classes pair
with VPR's message on G (``pairing_ok``: the table plus plan point (e)); and the checker's
verdict on G equals VPR's. ``--bad-corpus DIR`` instead runs VPR and the checker (without
``--strict``) on each ``DIR/*.blif`` and requires that VPR rejects exactly the files the checker
rejects, with each ``# vpr:`` line of the file a substring of VPR's stdout or stderr (VPR's
assertion failures abort on stderr; ``vpr_throw`` errors print to stdout).

It is a heavy run (plan point (a)): ``main`` first calls ``qor_heavy.reexec_under_lock``, which
inside ``tools/heavy-run`` verifies the cap and otherwise re-runs this script through it when the
caller set ``ODIN3_HEAVY_OK=1``. A passing file's run directory is removed (``safe_rmtree``, one
absolute path under ``--work``); a failing one is kept. Report: TSV ``--report`` (default
``<work>/report.tsv``, under the same roots as ``--work``). A golden named in ``EXCLUDED`` is not
run: its row is ``excluded``, with the reason printed, and it does not fail the run. Earlier
runs' ``<work>/<n>/`` directories are removed first. Exit 0 when every verdict passes, 1
otherwise, 2 for usage or a failed cap check.
"""

from __future__ import annotations

import argparse
import os
import pwd
import re
import resource
import signal
import subprocess
import sys
import time
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "vtr-qor"))
import qor_heavy  # noqa: E402
from qor_heavy import HeavyError  # noqa: E402

PROG = "vtr-read-check"
PREFIX = f"{PROG}: "
SCRIPT = Path(__file__).resolve()
EXIT_OK, EXIT_FAIL, EXIT_USAGE = 0, 1, 2
K6 = "k6_frac_N10_frac_chain_mem32K_40nm"
ARCH_REL = Path("vtr_flow/arch/timing") / f"{K6}.xml"
VPR_REL = Path("vpr/vpr")
TECHLIB = REPO_ROOT / "lib" / "vtr.o3lib"
DEFAULT_ODIN3 = REPO_ROOT / "build" / "release" / "odin3"
DEFAULT_CHECKER = REPO_ROOT / "tools" / "blif-vtr-check" / "blif-vtr-check"
# One step's wall-clock cap (odin3, one VPR read or one checker run of one file).
STEP_TIMEOUT_S = 3600
HEADER = "\t".join(("golden", "tool", "written|refused", "error class", "vpr_W", "vpr_G",
                    "checker_W", "checker_G", "verdict"))
NOT_RUN = "-"
VERDICT_PASS, VERDICT_FAIL = "pass", "fail"
VERDICT_INTENDED, VERDICT_LISTED = "intended", "listed"
VERDICT_EXCLUDED = "excluded"
PASSING = frozenset({VERDICT_PASS, VERDICT_INTENDED, VERDICT_LISTED, VERDICT_EXCLUDED})
# Goldens not run, by file name, with the reason (3E spec §16.4; PHASE3 #29). An explicit list,
# not a size threshold: each exclusion is a recorded decision, and a new large golden is run
# (and fails loudly) until someone decides otherwise. blif-roundtrip --vtr keeps the same list.
EXCLUDED: dict[str, str] = {
    "LargeRam.odin.blif": "915 MB: the checker needs about 23 GB (3 GB at 117 MB, scaled), "
                          "more than any heavy slice; needs a streaming checker or VPR on a "
                          "larger host",
}
# VPR's assertion failures abort (SIGABRT): a verdict. Any other signal (an OOM kill) is none.
VPR_ABORT = -signal.SIGABRT
REFUSAL_PREFIX = "odin3: error: write_blif: --vtr: "
VPR_LINE = re.compile(r"^#\s*vpr:\s*(.*?)\s*$")
# The writer's class-U clock warning (plan point (j)): such a W is listed for the full VPR flow
# 3E-1's final review (M8) asks of one of them.
UNCONN_CLOCK = "clock is 'unconn'"

# The refusal classes of write_blif --vtr's error texts (3E spec §11), in E-order; the same
# patterns as blif-roundtrip's vtr_classes.
CLASS_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = tuple(
    (cls, re.compile(pattern)) for cls, pattern in (
        ("E1", r": falling-edge flip-flop; "),
        ("E1b", r": asynchronous set/reset flip-flop; "),
        ("E2", r": level-sensitive latch; "),
        ("E3", r": flip-flop without a clock; "),
        ("E3b", r": clock input .* is unconnected or x$"),
        ("E4", r": tri-state cell; "),
        ("E5", r": not a netlist cell \(run lower\)$"),
        ("E6", r"' is not a hard cell of a loaded tech library"),
        ("E7", r" inputs exceed K=[0-9]+ "),
        ("E8", r": cover mixes on-set and off-set rows$"),
        ("E9", r": inout port; VPR has no bidirectional pins$"),
        ("E10", r": hierarchy not flattened \(run flatten\)"),
        ("E11", r" cannot be recovered from BLIF formals$"),
        ("E12", r": net .* has [0-9]+ drivers \("),
        ("E13", r"^no top module"),
    ))
CLASS_ORDER = [cls for cls, _ in CLASS_PATTERNS] + ["?"]

_SUBCKT = r"Subckt instantiates model '.*', but no such model exists in the architecture file"
_BLACKBOX = r"Blackbox BLIF model '.*' has no equivalent architecture model"
_RISING = r"Only rising edge latches supported"
# §16.4's pairing table: the VPR message on G each refusal class pairs with. E1b pairs with
# `Subckt instantiates model` (plan point (e): an async flip-flop reaches a golden as a
# `.subckt`, never as a `.latch`). E4 and E9 cannot occur in a BLIF golden: any hit fails.
PAIRING: dict[str, re.Pattern[str]] = {
    cls: re.compile(pattern) for cls, pattern in (
        ("E1", _RISING),
        ("E1b", _SUBCKT),
        ("E2", _RISING),
        ("E3", r"Latch must have a clock"),
        ("E5", f"{_SUBCKT}|{_BLACKBOX}"),
        ("E6", f"{_SUBCKT}|{_BLACKBOX}"),
        ("E7", r"BLIF \.names input size \([0-9]+\) greater than \.names model input size"),
        ("E10", r"Found multiple models with primitives"),
        # The R16 assertion (c9_two_drivers), or the netlist check VPR's Release build stops at
        # on an Odin II multi-driver golden (elsif_both_defined, measured 2026-10-12).
        ("E12", r"Must be no existing net driver|Inconsistent block data sizes"),
        ("E13", r"No non-blackbox models found"),
    )}
# Refusals VPR accepts on G (an open hard-block clock pin; a mixed cover): intended refusals.
INTENDED = frozenset({"E3b", "E8"})
# E11 (parameters): VPR's verdict depends on the parameter; listed per file, any verdict.
LISTED = frozenset({"E11"})


@dataclass
class ReadCtx:
    """One run's settings. ``seq`` numbers the run directories ``<work>/<n>/``."""

    golden_root: Path
    arch: Path
    vpr: Path
    odin3: Path
    checker: Path
    work: Path
    techlib: Path = TECHLIB
    timeout: float = STEP_TIMEOUT_S
    seq: int = 0


@dataclass
class Row:
    """One golden's report line, plus why it failed (printed, not in the TSV)."""

    golden: str
    tool: str
    outcome: str = NOT_RUN
    classes: str = NOT_RUN
    vpr_w: str = NOT_RUN
    vpr_g: str = NOT_RUN
    checker_w: str = NOT_RUN
    checker_g: str = NOT_RUN
    verdict: str = VERDICT_FAIL
    why: list[str] = field(default_factory=list)
    unconn_clock: bool = False

    def tsv(self) -> str:
        return "\t".join((self.golden, self.tool, self.outcome, self.classes, self.vpr_w,
                          self.vpr_g, self.checker_w, self.checker_g, self.verdict))


@dataclass(frozen=True)
class Step:
    """A finished subprocess: its exit code (None on a timeout) and output files."""

    code: int | None
    stdout: Path
    stderr: Path

    @property
    def shown(self) -> str:
        return "timeout" if self.code is None else str(self.code)


def _no_core() -> None:
    """VPR's assertion failures abort; no core file lands in the run directory."""
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run_step(argv: Sequence[str], cwd: Path, name: str, timeout: float) -> Step:
    """Runs argv in cwd with stdout/stderr in ``cwd/<name>.out`` / ``.err``."""
    out, err = cwd / f"{name}.out", cwd / f"{name}.err"
    with out.open("wb") as fout, err.open("wb") as ferr:
        try:
            proc = subprocess.run(list(argv), cwd=cwd, stdin=subprocess.DEVNULL, stdout=fout,
                                  stderr=ferr, timeout=timeout, preexec_fn=_no_core, check=False)
        except subprocess.TimeoutExpired:
            return Step(None, out, err)
    return Step(proc.returncode, out, err)


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def refusal_classes(stderr: str) -> str:
    """The refusal classes in a write's stderr, comma-separated in E-order; `?` for an error line
    no class matches, or for no `write_blif: --vtr:` error line at all."""
    seen: set[str] = set()
    for line in stderr.splitlines():
        if not line.startswith(REFUSAL_PREFIX):
            continue
        text = line[len(REFUSAL_PREFIX):]
        if re.search(r"[0-9]+ more errors not shown$", text):
            continue
        seen.add(next((cls for cls, pat in CLASS_PATTERNS if pat.search(text)), "?"))
    return ",".join(cls for cls in CLASS_ORDER if cls in seen) or "?"


def vpr_message(step: Step) -> str:
    """VPR's verdict as text: "" when it accepted (exit 0); else its first `Message:` line (a
    vpr_throw or VPR_FATAL_ERROR, on stdout or stderr), its `Assertion '…' failed …` text (stderr,
    an abort), or its last output line."""
    if step.code == 0:
        return ""
    if step.code is None:
        return "timeout"
    stdout, stderr = _read(step.stdout), _read(step.stderr)
    for line in (stdout + "\n" + stderr).splitlines():
        if line.startswith("Message: "):
            return line[len("Message: "):].strip()
    for line in stderr.splitlines():
        at = line.find("Assertion '")
        if at >= 0:
            return line[at:].strip()
    lines = [ln.strip() for ln in (stderr + "\n" + stdout).splitlines() if ln.strip()]
    return lines[-1] if lines else f"exit {step.code}"


def pairing_ok(refusal: str, vpr_g_message: str) -> bool:
    """§16.4's pairing of a refusal (comma-separated classes) with VPR's message on G ("" when
    VPR accepted G), with plan point (e) (E1b pairs with `Subckt instantiates model`):

    - VPR accepted G: every class is an intended refusal (E3b, E8) or listed (E11);
    - VPR rejected G: some class's message matches, or E11 is among the classes;
    - a class the table rules out (E4, E9, `?`) fails whatever VPR says."""
    classes = [cls for cls in refusal.split(",") if cls]
    if not classes or any(cls not in PAIRING and cls not in INTENDED | LISTED for cls in classes):
        return False
    if not vpr_g_message:
        return all(cls in INTENDED | LISTED for cls in classes)
    return any(cls in LISTED or (cls in PAIRING and PAIRING[cls].search(vpr_g_message) is not None)
               for cls in classes)


def tool_of(golden: Path) -> str:
    """`parmys` or `odin` from the golden's name (`<leaf>.<tool>.blif`), else `-`."""
    for tool in ("parmys", "odin"):
        if golden.name.endswith(f".{tool}.blif"):
            return tool
    return NOT_RUN


def collect(golden_root: Path) -> list[Path]:
    """Every `*.blif` under golden_root whose sibling `.prov` has the line `status=ok`, smallest
    first (ties by path)."""
    found = []
    for blif in golden_root.rglob("*.blif"):
        prov = blif.with_suffix(".prov")
        try:
            lines = prov.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        if "status=ok" in (line.strip() for line in lines):
            found.append(blif)
    return sorted(found, key=lambda p: (p.stat().st_size, str(p)))


def _vpr(ctx: ReadCtx, blif: Path, run_dir: Path, name: str) -> Step:
    cwd = run_dir / name
    cwd.mkdir()
    argv = [str(ctx.vpr), str(ctx.arch), str(blif), "--circuit_format", "blif",
            "--exit_before_pack", "on"]
    return run_step(argv, cwd, name, ctx.timeout)


def _fresh_dir(ctx: ReadCtx) -> Path:
    ctx.seq += 1
    run_dir = ctx.work / str(ctx.seq)
    qor_heavy.safe_rmtree(run_dir, ctx.work)
    run_dir.mkdir(parents=True)
    return run_dir


def no_verdict(vpr: Step) -> bool:
    """VPR gave no verdict: a timeout, or killed by a signal other than its assertion abort."""
    return vpr.code is None or (vpr.code < 0 and vpr.code != VPR_ABORT)


def _written_verdict(row: Row, vpr_w: Step, checker_w: Step) -> None:
    if vpr_w.code != 0:
        row.why.append(f"VPR rejects W: {vpr_message(vpr_w)}")
    if checker_w.code != 0:
        row.why.append(f"blif-vtr-check --strict rejects W (exit {checker_w.shown})")


def _refused_verdict(row: Row, vpr_g: Step) -> str:
    message = vpr_message(vpr_g)
    if not pairing_ok(row.classes, message):
        shown = message or "accepted"
        row.why.append(f"refusal {row.classes} does not pair with VPR on G: {shown}")
        return VERDICT_FAIL
    classes = set(row.classes.split(","))
    if classes & LISTED:
        return VERDICT_LISTED
    return VERDICT_INTENDED if classes <= INTENDED else VERDICT_PASS


def _agreement(row: Row, vpr_g: Step, checker_g: Step) -> None:
    if no_verdict(vpr_g):
        row.why.append(f"VPR gives no verdict on G (exit {vpr_g.shown})")
    elif checker_g.code not in (0, 1) or (checker_g.code == 0) != (vpr_g.code == 0):
        row.why.append(f"checker on G (exit {checker_g.shown}) disagrees with VPR "
                       f"(exit {vpr_g.shown}: {vpr_message(vpr_g) or 'accepted'})")


def _write(ctx: ReadCtx, g: Path, run_dir: Path, row: Row) -> tuple[Path, Step]:
    """Step 1; sets row.outcome (written, refused or error) and row.classes."""
    w = run_dir / "W.blif"
    paths = (ctx.techlib, g, w)
    if any('"' in str(p) or "\n" in str(p) for p in paths):
        row.outcome = "error"
        row.why.append("a path holds '\"' or a newline; odin3 -p cannot quote it")
        return w, Step(None, run_dir / "write.out", run_dir / "write.err")
    techlib, gq, wq = (f'"{p}"' for p in paths)
    script = f"read_techlib {techlib}; read_blif {gq}; write_blif --vtr {wq}"
    write = run_step([str(ctx.odin3), "-p", script], run_dir, "write", ctx.timeout)
    if write.code == 0 and w.is_file():
        row.outcome = "written"
        row.unconn_clock = UNCONN_CLOCK in _read(write.stderr)
    elif write.code == 1 and not w.exists():
        row.outcome, row.classes = "refused", refusal_classes(_read(write.stderr))
    elif write.code == 1:
        row.outcome = "error"
        row.why.append("odin3 refused (exit 1) but left W behind")
    else:
        row.outcome = "error"
        row.why.append(f"odin3 exit {write.shown} (no W)")
    return w, write


def run_one(g: Path, ctx: ReadCtx) -> Row:
    """§16.4 on one golden: write, VPR on W and G, the checker on W and G, the verdict. The run
    directory is removed when the verdict passes."""
    try:
        rel = str(g.relative_to(ctx.golden_root))
    except ValueError:
        rel = str(g)
    row = Row(rel, tool_of(g))
    if g.name in EXCLUDED:
        row.outcome = row.verdict = VERDICT_EXCLUDED
        row.why.append(EXCLUDED[g.name])
        return row
    run_dir = _fresh_dir(ctx)
    w, write = _write(ctx, g, run_dir, row)
    verdict = VERDICT_PASS
    if row.outcome == "written":
        vpr_w = _vpr(ctx, w, run_dir, "vpr_W")
        checker_w = run_step([str(ctx.checker), "--strict", "--writer-log", str(write.stderr),
                              str(w)], run_dir, "checker_W", ctx.timeout)
        row.vpr_w, row.checker_w = vpr_w.shown, checker_w.shown
        _written_verdict(row, vpr_w, checker_w)
    vpr_g = _vpr(ctx, g, run_dir, "vpr_G")
    checker_g = run_step([str(ctx.checker), str(g)], run_dir, "checker_G", ctx.timeout)
    row.vpr_g, row.checker_g = vpr_g.shown, checker_g.shown
    if row.outcome == "refused":
        verdict = _refused_verdict(row, vpr_g)
    _agreement(row, vpr_g, checker_g)
    row.verdict = VERDICT_FAIL if row.why or verdict == VERDICT_FAIL else verdict
    if row.verdict in PASSING:
        qor_heavy.safe_rmtree(run_dir, ctx.work)
    return row


def vpr_lines(blif: Path) -> list[str]:
    """The texts of a corpus file's `# vpr:` lines (VPR's expected message)."""
    lines = _read(blif).splitlines()
    return [m.group(1) for m in (VPR_LINE.match(line) for line in lines) if m and m.group(1)]


def bad_one(blif: Path, ctx: ReadCtx) -> list[str]:
    """One negative-corpus file: VPR rejects it exactly when the checker does, and then with
    every `# vpr:` text in its stdout or stderr. Returns the reasons it fails (none: passes)."""
    run_dir = _fresh_dir(ctx)
    vpr = _vpr(ctx, blif, run_dir, "vpr")
    checker = run_step([str(ctx.checker), str(blif)], run_dir, "checker", ctx.timeout)
    expected = vpr_lines(blif)
    why = []
    if no_verdict(vpr) or checker.code not in (0, 1):
        why.append(f"VPR exit {vpr.shown}, checker exit {checker.shown}")
    elif (vpr.code != 0) != (checker.code == 1):
        verb = "rejects" if vpr.code != 0 else "accepts"
        why.append(f"VPR {verb} it (exit {vpr.shown}) but the checker exits {checker.shown}")
    if vpr.code != 0 and not no_verdict(vpr):
        output = _read(vpr.stdout) + "\n" + _read(vpr.stderr)
        if not expected:
            why.append("VPR rejects it but the file has no `# vpr:` line")
        why += [f"VPR's output lacks `{text}`" for text in expected if text not in output]
    elif vpr.code == 0 and expected:
        why.append(f"VPR accepts it, expected `{expected[0]}`")
    if not why:
        qor_heavy.safe_rmtree(run_dir, ctx.work)
    return why


def run_bad_corpus(corpus: Path, ctx: ReadCtx) -> int:
    files = sorted(corpus.glob("*.blif"))
    if not files:
        print(f"{PREFIX}no *.blif in {corpus}", file=sys.stderr)
        return EXIT_USAGE
    failed = 0
    for blif in files:
        why = bad_one(blif, ctx)
        failed += bool(why)
        print(f"{'FAIL' if why else 'ok  '} {blif.name}" + "".join(f"\n  why: {w}" for w in why))
    print(f"{PREFIX}bad corpus: {len(files)} files, {failed} failing: "
          f"RESULT: {'FAIL' if failed else 'PASS'}")
    return EXIT_FAIL if failed else EXIT_OK


def _summary(rows: list[Row], seconds: float) -> str:
    counts = Counter((r.tool, r.outcome) for r in rows)
    classes = Counter((r.tool, cls) for r in rows if r.outcome == "refused"
                      for cls in r.classes.split(","))
    verdicts = Counter(r.verdict for r in rows)
    parts = [f"{tool} {outcome} {n}" for (tool, outcome), n in sorted(counts.items())]
    parts += [f"{tool} {cls} {n}" for (tool, cls), n in sorted(classes.items())]
    parts += [f"{v} {n}" for v, n in sorted(verdicts.items())]
    failed = len(rows) - sum(verdicts[v] for v in PASSING)
    unconn = [r.golden for r in rows if r.unconn_clock]
    listed = "".join(f"\n{PREFIX}clock on 'unconn' (class U): {g}" for g in unconn)
    return (f"{PREFIX}{len(rows)} files in {seconds:.0f} s: " + ", ".join(parts) + listed
            + f"\nRESULT: {'FAIL' if failed else 'PASS'}")


def clear_stale_runs(ctx: ReadCtx) -> None:
    """Removes an earlier run's ``<work>/<n>/`` directories, so none outlives a rerun."""
    for child in ctx.work.iterdir():
        if child.name.isdigit() and child.is_dir() and not child.is_symlink():
            qor_heavy.safe_rmtree(child, ctx.work)


def run_goldens(ctx: ReadCtx, report: Path, limit: int | None) -> int:
    goldens = collect(ctx.golden_root)
    if limit is not None:
        goldens = goldens[:limit]
    if not goldens:
        print(f"{PREFIX}no status=ok golden under {ctx.golden_root}", file=sys.stderr)
        return EXIT_USAGE
    report.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    rows = []
    with report.open("w", encoding="utf-8") as tsv:
        tsv.write(HEADER + "\n")
        for g in goldens:
            row = run_one(g, ctx)
            rows.append(row)
            tsv.write(row.tsv() + "\n")
            tsv.flush()
            if row.verdict == VERDICT_EXCLUDED:
                print(f"EXCLUDED {row.golden}: {row.why[0]}", flush=True)
            elif row.verdict not in PASSING:
                print(f"FAIL {row.golden}" + "".join(f"\n  why: {w}" for w in row.why),
                      flush=True)
    print(_summary(rows, time.monotonic() - start))
    return EXIT_OK if all(r.verdict in PASSING for r in rows) else EXIT_FAIL


def _positive(text: str) -> int:
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return value


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    home = Path.home()
    parser = argparse.ArgumentParser(prog=PROG, description="VPR reads every golden-equivalent "
                                     "BLIF (3E spec §16.4)")
    parser.add_argument("--golden-root", type=Path, default=home / "odin3-ws" / "golden" / K6)
    parser.add_argument("--arch", type=Path, help=f"default $VTR_ROOT/{ARCH_REL}")
    parser.add_argument("--odin3", type=Path, default=DEFAULT_ODIN3)
    parser.add_argument("--checker", type=Path, default=DEFAULT_CHECKER)
    parser.add_argument("--work", type=Path, default=home / "odin3-ws" / "work" / PROG)
    parser.add_argument("--report", type=Path, help="TSV report (default <work>/report.tsv)")
    parser.add_argument("--limit", type=_positive, help="only the N smallest goldens")
    parser.add_argument("--bad-corpus", type=Path, metavar="DIR",
                        help="check VPR and the checker agree on DIR/*.blif instead")
    return parser.parse_args(list(argv))


def make_ctx(args: argparse.Namespace) -> ReadCtx:
    """The run's settings, or HeavyError(2) for a missing VPR, arch, tool or a --work outside
    ~/odin3-ws/work and /tmp/claude-1000."""
    vtr_root = os.environ.get("VTR_ROOT", "")
    if not vtr_root:
        raise HeavyError(EXIT_USAGE, "VTR_ROOT is not set (VPR is $VTR_ROOT/vpr/vpr)")
    vpr = Path(vtr_root) / VPR_REL
    arch = args.arch if args.arch is not None else Path(vtr_root) / ARCH_REL
    for what, path in (("VPR", vpr), ("odin3", args.odin3), ("checker", args.checker)):
        if not os.access(path, os.X_OK):
            raise HeavyError(EXIT_USAGE, f"no {what} at {path}")
    if not arch.is_file():
        raise HeavyError(EXIT_USAGE, f"no architecture file {arch}")
    root = args.bad_corpus if args.bad_corpus is not None else args.golden_root
    if not root.is_dir():
        raise HeavyError(EXIT_USAGE, f"not a directory: {root}")
    work = qor_heavy.check_out_dir(args.work, Path(pwd.getpwuid(os.getuid()).pw_dir))
    work.mkdir(parents=True, exist_ok=True)
    return ReadCtx(golden_root=args.golden_root.resolve(), arch=arch.resolve(), vpr=vpr,
                   odin3=args.odin3.resolve(), checker=args.checker.resolve(), work=work)


def main(argv: Sequence[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else list(argv)
    try:
        qor_heavy.reexec_under_lock(argv, script=SCRIPT, lock=qor_heavy.HEAVY_LOCK)
        args = parse_args(argv)
        ctx = make_ctx(args)
        clear_stale_runs(ctx)
        if args.bad_corpus is not None:
            return run_bad_corpus(args.bad_corpus.resolve(), ctx)
        home = Path(pwd.getpwuid(os.getuid()).pw_dir)
        report = (qor_heavy.check_out_dir(args.report, home) if args.report is not None
                  else ctx.work / "report.tsv")
        return run_goldens(ctx, report, args.limit)
    except HeavyError as exc:
        print(f"{PREFIX}{exc.message}", file=sys.stderr)
        return exc.code


if __name__ == "__main__":
    sys.exit(main())
