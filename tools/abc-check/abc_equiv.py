"""abc-equiv: the abc pass against equiv-check, one design at a time, smallest first (3D Task 19).

Usage::

    abc-equiv --odin3 O --equiv-check E [--step abc|equiv|all] [--lut K]
              [--via-blif DIR | --exchange aig|blif] [--vtr] [--keep DIR]
              [--expect-fail F] [--timeout S] <file or dir>...

Every ``.blif`` under the directories (and the files) given is one design; all are sorted together
by size, smallest first (ties by path).  For each design (spec 13.4)::

    O -p "read_techlib lib/vtr.o3lib; read_blif X; write_blif before.blif;
          abc --lut K [--via-blif D]; write_blif after.blif"
                                    (and ``write_blif --vtr after.vtr.blif`` with --vtr)
    E --blackbox adder,multiply,single_port_ram,dual_port_ram --timeout S before.blif after.blif

then the sets of ``(model, occurrence, formal)`` pins on the net ``unconn`` of ``before.blif`` and
``after.blif`` (and ``after.vtr.blif``) must be equal: the hard-block pins Parmys left open stay
open.  ``--exchange`` sets ``ODIN3_ABC_EXCHANGE`` for that child (point (z)); everything else of the
caller's environment is passed through unchanged.  odin3 failing is detected from a missing output
file and its log, never from its exit status.

One line per design: ``<design> equivalent|differs|multiclock|timeout|error|unconn-changed|views
<s> s`` (``mapped`` with ``--step abc``), then a summary.  ``--expect-fail`` takes lines ``<path>
<reason>`` (``#`` comments): a listed design passes when it does not come out ``equivalent``; a
listed design that does is a failure (XPASS), and so is an unlisted one that does not.  A listed
reason ``views`` passes only when odin3 fails with the netlist-view error (point (j)): ``abc:
module 'm' is not in the netlist view``.

``--step`` (ruling I9; default ``all``): ``abc`` runs the odin3 step only and leaves
``before.blif``,
``after.blif`` (and ``after.vtr.blif``) in ``--keep DIR``, printing ``<design> mapped <s> s``;
``equiv`` runs equiv-check and the ``unconn`` check on the files already in ``--keep DIR`` (a
missing file is ``error``).  ``--step abc|equiv`` without ``--keep`` is a usage error.  A path under
``~/odin3-ws/golden`` first needs ``qor_heavy.require_heavy()`` (point (m)): run through
``tools/heavy-run``.  Nothing is deleted.

Exit codes: 0 every design passes, 1 any design fails, 2 usage error.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for _sub in ("tools/blif", "tools/vtr-qor"):
    if str(REPO_ROOT / _sub) not in sys.path:
        sys.path.insert(0, str(REPO_ROOT / _sub))
import blif  # noqa: E402
import qor_heavy  # noqa: E402

TOOL = "abc-equiv"
EXIT_OK, EXIT_FAIL, EXIT_USAGE = 0, 1, 2
GOLDEN_ROOT = Path.home() / "odin3-ws" / "golden"
BLACKBOX_MODELS = "adder,multiply,single_port_ram,dual_port_ram"
TECHLIB = "lib/vtr.o3lib"
DEFAULT_LUT = 6
DEFAULT_TIMEOUT = 600
EQUIV_SLACK = 120  # seconds beyond equiv-check's own --timeout before this tool gives up
PASSING = ("equivalent", "mapped")
VIEWS_TEXT = "is not in the netlist view"
MULTICLOCK_TEXT = "more than one clock domain"
UNCONN = "unconn"
_UNSAFE = re.compile(r"[^A-Za-z0-9_.-]+")


class UsageError(Exception):
    """A bad argument or input (exit code 2)."""


@dataclass(frozen=True)
class Config:
    odin3: str
    equiv_check: str
    step: str
    lut: int
    via_blif: str | None
    exchange: str | None
    vtr: bool
    keep: Path
    timeout: int


@dataclass(frozen=True)
class Outcome:
    status: str
    secs: float
    detail: str = ""


def collect(paths: Sequence[str]) -> list[Path]:
    """Every ``.blif`` of the arguments, smallest first (ties by path)."""
    found: list[Path] = []
    for arg in paths:
        path = Path(arg)
        if path.is_dir():
            found.extend(sorted(path.rglob("*.blif")))
        elif path.is_file():
            found.append(path)
        else:
            raise UsageError(f"{arg}: no such file or directory")
    unique = {p: None for p in found}
    return sorted(unique, key=lambda p: (p.stat().st_size, str(p)))


def read_expect(path: str | None) -> dict[str, str]:
    """``<path> <reason>`` lines of ``--expect-fail`` (``#`` comments and blank lines skipped)."""
    entries: dict[str, str] = {}
    if path is None:
        return entries
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as exc:
        raise UsageError(f"{path}: cannot read: {exc}") from exc
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, _, reason = line.partition(" ")
        if not reason.strip():
            raise UsageError(f"{path}:{number}: expected '<path> <reason>'")
        entries[name] = reason.strip()
    return entries


def listed_reason(design: Path, entries: dict[str, str]) -> str | None:
    """The reason of the expect-fail entry that names design (as given, or as a path suffix)."""
    text = str(design)
    for name, reason in entries.items():
        if text == name or text.endswith("/" + name.lstrip("./")):
            return reason
    return None


def design_dir(cfg: Config, index: int, design: Path) -> Path:
    return cfg.keep / f"{index:04d}-{_UNSAFE.sub('_', design.stem)}"


def child_env(cfg: Config, odin3: bool) -> dict[str, str]:
    """The caller's environment, with ODIN3_ABC_EXCHANGE added for the odin3 child under
    ``--exchange`` only."""
    env = dict(os.environ)
    if odin3 and cfg.exchange is not None:
        env["ODIN3_ABC_EXCHANGE"] = cfg.exchange
    return env


def odin3_script(cfg: Config, design: Path, out: Path) -> str:
    abc = f"abc --lut {cfg.lut}" + (f" --via-blif {cfg.via_blif}" if cfg.via_blif else "")
    parts = [f"read_techlib {TECHLIB}", f"read_blif {design}", f"write_blif {out / 'before.blif'}",
             abc, f"write_blif {out / 'after.blif'}"]
    if cfg.vtr:
        parts.append(f"write_blif --vtr {out / 'after.vtr.blif'}")
    return "; ".join(parts)


def run_odin3(cfg: Config, design: Path, out: Path) -> Outcome:
    """The odin3 step.  mapped, or views, timeout or error (a missing file is the failure)."""
    start = time.monotonic()
    wanted = [out / "after.blif"] + ([out / "after.vtr.blif"] if cfg.vtr else [])
    if re.search(r"[;\s]", str(design)) or re.search(r"[;\s]", str(out)):
        return Outcome("error", 0.0, "a path with a blank or ';' cannot go in an odin3 -p script")
    out.mkdir(parents=True, exist_ok=True)
    argv = [cfg.odin3, "-p", odin3_script(cfg, design, out)]
    try:
        proc = subprocess.run(argv, cwd=REPO_ROOT, env=child_env(cfg, True), capture_output=True,
                              text=True, timeout=cfg.timeout, check=False)
    except subprocess.TimeoutExpired:
        return Outcome("timeout", time.monotonic() - start, "odin3 timed out")
    except OSError as exc:
        return Outcome("error", time.monotonic() - start, f"cannot run {cfg.odin3}: {exc}")
    secs = time.monotonic() - start
    log = proc.stdout + proc.stderr
    if all(p.is_file() for p in wanted):
        return Outcome("mapped", secs)
    if VIEWS_TEXT in log:
        return Outcome("views", secs, log.strip().splitlines()[-1])
    return Outcome("error", secs, (log.strip().splitlines() or ["no output file"])[-1])


def unconn_pins(path: Path) -> set[tuple[str, int, str]]:
    """The ``(model, occurrence, formal)`` pins of the top model's ``.subckt`` cells on unconn."""
    top = blif.parse_file(str(path)).top
    seen: dict[str, int] = {}
    pins: set[tuple[str, int, str]] = set()
    for cell in top.cells:
        if isinstance(cell, blif.Subckt):
            seen[cell.model] = seen.get(cell.model, 0) + 1
            pins.update((cell.model, seen[cell.model], formal)
                        for formal, actual in cell.conns if actual == UNCONN)
    return pins


def unconn_changed(out: Path, vtr: bool) -> str:
    """A description of the first ``unconn`` set that differs from before.blif's, else ''."""
    want = unconn_pins(out / "before.blif")
    for name in ["after.blif"] + (["after.vtr.blif"] if vtr else []):
        got = unconn_pins(out / name)
        if got != want:
            lost = sorted(want - got)
            gained = sorted(got - want)
            return f"{name}: no longer on unconn {lost}; newly on unconn {gained}"
    return ""


def run_equiv(cfg: Config, out: Path) -> Outcome:
    """The equiv-check step and the ``unconn`` check."""
    start = time.monotonic()
    names = ["before.blif", "after.blif"] + (["after.vtr.blif"] if cfg.vtr else [])
    missing = [n for n in names if not (out / n).is_file()]
    if missing:
        return Outcome("error", 0.0, f"{out}: missing {', '.join(missing)}")
    argv = [cfg.equiv_check, "--blackbox", BLACKBOX_MODELS, "--timeout", str(cfg.timeout),
            str(out / "before.blif"), str(out / "after.blif")]
    try:
        proc = subprocess.run(argv, cwd=REPO_ROOT, env=child_env(cfg, False), capture_output=True,
                              text=True, timeout=cfg.timeout + EQUIV_SLACK, check=False)
    except subprocess.TimeoutExpired:
        return Outcome("timeout", time.monotonic() - start, "equiv-check timed out")
    except OSError as exc:
        return Outcome("error", time.monotonic() - start, f"cannot run {cfg.equiv_check}: {exc}")
    secs = time.monotonic() - start
    log = proc.stdout + proc.stderr
    if MULTICLOCK_TEXT in log:
        return Outcome("multiclock", secs, MULTICLOCK_TEXT)
    verdicts = {0: "equivalent", 1: "differs", 4: "timeout"}
    status = verdicts.get(proc.returncode, "error")
    if status == "differs" and "NOT equivalent" not in log:
        status = "error"
    if status != "equivalent":
        return Outcome(status, secs, (log.strip().splitlines() or [""])[-1])
    changed = unconn_changed(out, cfg.vtr)
    return Outcome("unconn-changed" if changed else "equivalent", secs, changed)


def run_design(cfg: Config, index: int, design: Path) -> Outcome:
    out = design_dir(cfg, index, design)
    if cfg.step == "equiv":
        return run_equiv(cfg, out)
    mapped = run_odin3(cfg, design, out)
    if cfg.step == "abc" or mapped.status != "mapped":
        return mapped
    result = run_equiv(cfg, out)
    return Outcome(result.status, mapped.secs + result.secs, result.detail)


def judge(outcome: Outcome, reason: str | None) -> tuple[bool, str]:
    """(passes, note): a listed design passes when it fails (a ``views`` entry only with the views
    error); an unlisted one passes when it is equivalent (or mapped)."""
    if reason is None:
        return outcome.status in PASSING, ""
    if outcome.status in PASSING:
        return False, f"XPASS: listed ({reason}) but {outcome.status}"
    if reason == "views" and outcome.status != "views":
        return False, f"listed as views but {outcome.status}"
    return True, f"listed: {reason}"


def check_golden(paths: Sequence[str]) -> None:
    """A path under the golden tree needs a heavy run (point (m)); UsageError otherwise."""
    root = GOLDEN_ROOT.resolve()
    for arg in paths:
        resolved = Path(arg).resolve()
        if resolved == root or root in resolved.parents:
            try:
                qor_heavy.require_heavy()
            except qor_heavy.HeavyError as exc:
                raise UsageError(exc.message) from exc
            return


def run_all(cfg: Config, designs: Sequence[Path], entries: dict[str, str]) -> int:
    counts: dict[str, int] = {}
    failed = 0
    for index, design in enumerate(designs):
        outcome = run_design(cfg, index, design)
        ok, note = judge(outcome, listed_reason(design, entries))
        counts[outcome.status] = counts.get(outcome.status, 0) + 1
        failed += 0 if ok else 1
        extra = " ".join(x for x in (note, outcome.detail) if x)
        print(f"{design} {outcome.status} {outcome.secs:.1f} s" + (f" ({extra})" if extra else ""),
              flush=True)
    shown = ", ".join(f"{name} {count}" for name, count in sorted(counts.items()))
    print(f"{TOOL}: {len(designs)} designs: {len(designs) - failed} passed, {failed} failed"
          + (f" ({shown})" if shown else ""))
    return EXIT_FAIL if failed else EXIT_OK


def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog=TOOL, description="abc pass against equiv-check.")
    ap.add_argument("paths", nargs="+", metavar="FILE_OR_DIR")
    ap.add_argument("--odin3", required=True, metavar="O", help="the odin3 binary")
    ap.add_argument("--equiv-check", required=True, metavar="E", help="the equiv-check tool")
    ap.add_argument("--step", choices=("abc", "equiv", "all"), default="all")
    ap.add_argument("--lut", type=int, default=DEFAULT_LUT, metavar="K")
    ap.add_argument("--via-blif", metavar="DIR", help="abc --via-blif DIR")
    ap.add_argument("--exchange", choices=("aig", "blif"),
                    help="ODIN3_ABC_EXCHANGE for the odin3 child")
    ap.add_argument("--vtr", action="store_true", help="also write after.vtr.blif (--vtr form)")
    ap.add_argument("--keep", metavar="DIR", help="where before/after files are kept")
    ap.add_argument("--expect-fail", metavar="F", help="listed designs that must not be equivalent")
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT, metavar="S")
    return ap


def _config(args: argparse.Namespace) -> Config:
    if args.via_blif and args.exchange:
        raise UsageError("--via-blif and --exchange exclude each other (--via-blif is BLIF)")
    if args.step != "all" and not args.keep:
        raise UsageError(f"--step {args.step} needs --keep DIR")
    keep = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="abc-equiv."))
    if not args.keep:
        print(f"{TOOL}: files in {keep}", file=sys.stderr)
    return Config(args.odin3, args.equiv_check, args.step, args.lut, args.via_blif, args.exchange,
                  args.vtr, keep, args.timeout)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        check_golden(args.paths)
        cfg = _config(args)
        entries = read_expect(args.expect_fail)
        designs = collect(args.paths)
    except UsageError as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_USAGE
    return run_all(cfg, designs, entries)


if __name__ == "__main__":
    sys.exit(main())
