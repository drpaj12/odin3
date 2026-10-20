"""abc-determinism: the abc pass gives the same answer every time (3D Task 22; spec 13.8, AB-26).

Usage::

    abc-determinism --odin3 O [--odin3-debug D] [--via-blif] [--mode a|b|c|d]... [--lut K]
                    [--expect-fail F] [--keep DIR] [--timeout S] <file or dir>...

Every ``.blif`` under the directories (and the files) given is one design, smallest first (ties by
path); a design listed in ``--expect-fail`` (abc-equiv's file) is skipped.  The modes (``--mode``
repeats; the default is ``a``)::

    a  two processes of   read_techlib lib/vtr.o3lib; read_blif X; abc --lut K [--via-blif V];
                          write_blif OUT
       compare ``write_blif`` and, with ``--via-blif``, every ``abc<r>.in.blif``, ``.cmd`` and
       ``.out.blif`` of V (V is the same path in both runs, and run 1's files are moved aside
       first).  A ``.log`` and ``--log`` are never compared (they hold times), and neither is the
       ``# Benchmark "<name>" written by ABC on <date>`` line that ABC puts at the head of
       ``.out.blif`` (a deviation from "byte for byte": it holds the wall-clock date).
    b  one process: the first model of X three times (``<m>``, ``<m>__o3det2``, ``<m>__o3det3``)
       and its other models once, mapped by one ``abc`` call, so the engine opens three sessions
       in one process; the three copies must come out the same, for the default script and for
       ``strash; fraig; if -K K`` (the generators ABC reseeds at ``open``, I5).
    c  (a)'s first process, a sleep of 60 s, (a)'s first process again: the same.
    d  (a)'s command with ``--odin3`` (Release) and with ``--odin3-debug`` (Debug): the same
       ``write_blif``.  Micros only: a path under a ``vtr/`` golden directory is refused (exit 2),
       and so is a missing ``--odin3-debug``.

One line per design: ``<design> same`` or ``<design> differs <file>`` (``<design> error <why>`` when
odin3 does not finish).  The caller's environment is passed through unchanged.  A path under
``~/odin3-ws/golden`` first needs ``qor_heavy.require_heavy()`` (run through ``tools/heavy-run``).
Nothing is deleted.

Differences from the plan, both recorded in PHASE3 decision 53: mode (b) cannot run "A, then B, then
a fresh A" because ``read_blif`` refuses a second read into one design, so the repeated sessions
are three copies of the design in one process; and ``--mode`` may repeat.

Exit codes (the project's): 0 every design is the same, 1 a design differs, 2 a usage or tool
error.
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable, Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT / "tools/abc-check") not in sys.path:  # also under python -I
    sys.path.insert(0, str(REPO_ROOT / "tools/abc-check"))
import abc_equiv  # noqa: E402

TOOL = "abc-determinism"
EXIT_OK, EXIT_DIFFERS, EXIT_ERROR = 0, 1, 2
MODES = ("a", "b", "c", "d")
SLEEP_SECONDS = 60  # §13.8 (c)
COPIES = 3
FRAIG_SCRIPT = "strash\nfraig\nif -K {K}\n"
STAMP_HEAD, STAMP_MID = b"# Benchmark ", b" written by ABC on "
EXCHANGE_FILES = re.compile(r"^abc\d+\.(in\.blif|cmd|out\.blif)$")  # never a .log


class ToolError(Exception):
    """A run that did not finish, or an input that cannot be used (exit code 2)."""


@dataclasses.dataclass(frozen=True)
class Config:
    odin3: str
    odin3_debug: str | None
    lut: int
    via_blif: bool
    modes: tuple[str, ...]
    keep: Path
    timeout: int
    sleep: Callable[[float], None]


# --- one odin3 run ---------------------------------------------------------------------------


def script_text(design: Path, out_blif: Path, abc_args: str) -> str:
    """The odin3 ``-p`` script of one run."""
    return "; ".join([f"read_techlib {abc_equiv.TECHLIB}", f"read_blif {design}",
                      f"abc {abc_args}", f"write_blif {out_blif}"])


def run_odin3(cfg: Config, binary: str, script: str) -> None:
    """odin3 runs the script; ToolError when it fails, times out or cannot start."""
    try:
        proc = subprocess.run([binary, "-p", script], cwd=REPO_ROOT, env=dict(os.environ),
                              capture_output=True, text=True, timeout=cfg.timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise ToolError("odin3 timed out") from exc
    except OSError as exc:
        raise ToolError(f"cannot run {binary}: {exc}") from exc
    if proc.returncode != 0:
        log = (proc.stdout + proc.stderr).strip().splitlines()
        raise ToolError(f"odin3 failed: {log[-1] if log else 'no output'}")


def check_path(path: Path) -> None:
    """ToolError for a path odin3's ``-p`` script cannot carry."""
    if re.search(r"[;\s]", str(path)):
        raise ToolError(f"{path}: a blank or ';' in a path cannot go in an odin3 -p script")


# --- modes (a), (c) and (d): two runs of one command ------------------------------------------


def abc_args(cfg: Config, via: Path | None, script: Path | None = None) -> str:
    text = f"--lut {cfg.lut}"
    text += f" --via-blif {via}" if via is not None else ""
    return text + (f" --script {script}" if script is not None else "")


def exchange_files(directory: Path) -> dict[str, Path]:
    if not directory.is_dir():
        return {}
    return {p.name: p for p in sorted(directory.iterdir()) if EXCHANGE_FILES.match(p.name)}


def comparable(path: Path) -> bytes:
    """The bytes of a file, without ABC's own time stamp: ``write_hie`` heads ``.out.blif`` with
    ``# Benchmark "<name>" written by ABC on <date>``, which differs between any two runs."""
    kept = [line for line in path.read_bytes().splitlines(keepends=True)
            if not (line.startswith(STAMP_HEAD) and STAMP_MID in line)]
    return b"".join(kept)


def same_bytes(left: Path | None, right: Path | None) -> bool:
    if left is None or right is None:
        return left is right
    return comparable(left) == comparable(right)


def first_difference(runs: tuple[Path, Path], files: tuple[Path, Path]) -> str | None:
    """The first of ``write_blif`` and the exchange files that differ; None when all are equal."""
    out1, out2 = runs
    if not same_bytes(out1, out2):
        return "write_blif"
    left, right = exchange_files(files[0]), exchange_files(files[1])
    for name in sorted(set(left) | set(right)):
        if not same_bytes(left.get(name), right.get(name)):
            return name
    return None


def two_runs(cfg: Config, design: Path, out: Path, binaries: tuple[str, str]) -> str | None:
    """Two runs of the same command, one per binary; the first difference or None."""
    check_path(out)
    out.mkdir(parents=True, exist_ok=True)
    via = out / "exchange" if cfg.via_blif else None
    kept = out / "exchange-run1"
    for stale in (via, kept):
        if stale is not None and stale.exists():
            shutil.rmtree(stale)  # a reused --keep directory must not pass an old run off
    outs = (out / "run1.blif", out / "run2.blif")
    for number, binary in enumerate(binaries):
        outs[number].unlink(missing_ok=True)
        run_odin3(cfg, binary, script_text(design, outs[number], abc_args(cfg, via)))
        if number == 0 and via is not None and via.exists():
            shutil.move(str(via), str(kept))  # the same path again in run 2 (.cmd holds it)
    return first_difference(outs, (kept, via if via is not None else kept))


def mode_a(cfg: Config, design: Path, out: Path) -> str | None:
    return two_runs(cfg, design, out / "a", (cfg.odin3, cfg.odin3))


def mode_c(cfg: Config, design: Path, out: Path) -> str | None:
    where = out / "c"
    check_path(where)
    where.mkdir(parents=True, exist_ok=True)
    first = where / "run1.blif"
    second = where / "run2.blif"
    for number, target in enumerate((first, second)):
        target.unlink(missing_ok=True)
        run_odin3(cfg, cfg.odin3, script_text(design, target, abc_args(cfg, None)))
        if number == 0:
            cfg.sleep(SLEEP_SECONDS)
    return None if same_bytes(first, second) else "write_blif"


def mode_d(cfg: Config, design: Path, out: Path) -> str | None:
    if cfg.odin3_debug is None:
        raise ToolError("mode d needs --odin3-debug")
    return two_runs(dataclasses.replace(cfg, via_blif=False), design, out / "d",
                    (cfg.odin3, cfg.odin3_debug))


# --- mode (b): three sessions in one process --------------------------------------------------


def split_models(text: str) -> list[tuple[str, list[str]]]:
    """``(name, lines)`` per ``.model`` of a BLIF text; the lines include the ``.model`` line."""
    models: list[tuple[str, list[str]]] = []
    for line in text.splitlines():
        if line.startswith(".model"):
            words = line.split()
            models.append((words[1] if len(words) > 1 else "", []))
        if models:
            models[-1][1].append(line)
    return models


def three_copies(design: Path, target: Path) -> list[str]:
    """Writes the design with its first model three times; the names of the copies."""
    models = split_models(design.read_text(encoding="utf-8"))
    if not models or not models[0][0]:
        raise ToolError(f"{design}: no named .model")
    name, lines = models[0]
    names = [name] + [f"{name}__o3det{i}" for i in range(2, COPIES + 1)]
    out: list[str] = []
    for copy in names:
        out.extend([lines[0].replace(name, copy, 1), *lines[1:]])
    for _, other in models[1:]:
        out.extend(other)
    target.write_text("\n".join(out) + "\n", encoding="utf-8")
    return names


def body(models: dict[str, list[str]], name: str) -> list[str]:
    """The lines of model name after its ``.model`` line, without blank lines."""
    return [line for line in models.get(name, [""])[1:] if line.strip()]


def one_script(cfg: Config, design: Path, out: Path, script: Path | None) -> str | None:
    """Three copies through one abc call (one process); the first copy that differs."""
    label = "default" if script is None else script.stem
    three = out / f"{design.stem}-{label}.in.blif"
    names = three_copies(design, three)
    result = out / f"{design.stem}-{label}.out.blif"
    result.unlink(missing_ok=True)
    run_odin3(cfg, cfg.odin3, script_text(three, result, abc_args(cfg, None, script)))
    models = dict(split_models(result.read_text(encoding="utf-8")))
    for later in names[1:]:
        if body(models, later) != body(models, names[0]):
            return f"{result.name}:{later}"
    return None


def mode_b(cfg: Config, design: Path, out: Path) -> str | None:
    where = out / "b"
    check_path(where)
    where.mkdir(parents=True, exist_ok=True)
    fraig = where / "fraig.abc"
    fraig.write_text(FRAIG_SCRIPT.replace("{K}", str(cfg.lut)), encoding="utf-8")
    for script in (None, fraig):
        found = one_script(cfg, design, where, script)
        if found is not None:
            return found
    return None


MODE_FUNCS = {"a": mode_a, "b": mode_b, "c": mode_c, "d": mode_d}


# --- the designs ------------------------------------------------------------------------------


def judge_design(cfg: Config, design: Path, out: Path) -> str:
    """``same``, ``differs <file>`` or ``error <why>``; every mode runs, the first difference
    shows."""
    verdict = "same"
    for mode in cfg.modes:
        try:
            found = MODE_FUNCS[mode](cfg, design, out)
        except ToolError as exc:
            return f"error {exc}"
        if found is not None and verdict == "same":
            verdict = f"differs {mode}:{found}"
    return verdict


def refuse_golden_vtr(cfg: Config, designs: Sequence[Path]) -> None:
    """Mode d is for the micros: a path under a ``vtr/`` golden directory is refused."""
    if "d" not in cfg.modes:
        return
    root = abc_equiv.GOLDEN_ROOT.resolve()
    for design in designs:
        resolved = design.resolve()
        if root in resolved.parents and "vtr" in resolved.relative_to(root).parts:
            raise abc_equiv.UsageError(f"mode d is for the micros: {design} is a vtr/ golden")


def run_all(cfg: Config, designs: Sequence[Path]) -> int:
    same = differs = errors = 0
    for index, design in enumerate(designs):
        verdict = judge_design(cfg, design, abc_equiv.design_dir(
            abc_equiv.Config("", "", "", cfg.lut, None, None, False, cfg.keep, cfg.timeout),
            index, design))
        same += verdict == "same"
        differs += verdict.startswith("differs")
        errors += verdict.startswith("error")
        print(f"{design} {verdict}", flush=True)
    print(f"{TOOL}: {len(designs)} designs: {same} same, {differs} differs, {errors} error")
    if errors:
        return EXIT_ERROR
    return EXIT_DIFFERS if differs else EXIT_OK


def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog=TOOL, description="abc pass determinism (spec 13.8).")
    ap.add_argument("paths", nargs="+", metavar="FILE_OR_DIR")
    ap.add_argument("--odin3", required=True, metavar="O", help="the odin3 binary (Release)")
    ap.add_argument("--odin3-debug", metavar="D", help="the Debug odin3 binary (mode d)")
    ap.add_argument("--via-blif", action="store_true", help="use the BLIF exchange, compare files")
    ap.add_argument("--mode", action="append", choices=MODES, help="a mode (repeatable; a)")
    ap.add_argument("--lut", type=int, default=abc_equiv.DEFAULT_LUT, metavar="K")
    ap.add_argument("--timeout", type=int, default=abc_equiv.DEFAULT_TIMEOUT, metavar="S")
    ap.add_argument("--expect-fail", metavar="F", help="listed designs are skipped")
    ap.add_argument("--keep", metavar="DIR", help="where the runs' files are kept")
    return ap


def main(argv: Sequence[str] | None = None,
         sleep: Callable[[float], None] = time.sleep) -> int:
    args = _parser().parse_args(argv)
    try:
        abc_equiv.check_golden(args.paths)
        keep = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="abc-determinism."))
        cfg = Config(args.odin3, args.odin3_debug, args.lut, args.via_blif,
                     tuple(args.mode or ("a",)), keep, args.timeout, sleep)
        entries = abc_equiv.read_expect(args.expect_fail)
        designs = [d for d in abc_equiv.collect(args.paths)
                   if abc_equiv.listed_reason(d, entries) is None]
        if "d" in cfg.modes and cfg.odin3_debug is None:
            raise abc_equiv.UsageError("mode d needs --odin3-debug")
        refuse_golden_vtr(cfg, designs)
    except abc_equiv.UsageError as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_ERROR
    return run_all(cfg, designs)


if __name__ == "__main__":
    sys.exit(main())
