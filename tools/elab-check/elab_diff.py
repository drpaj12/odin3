"""elab-diff: seeds of the ``--elab`` generator through ``elab-equiv`` (2D spec 14.5, plan Task 28).

Usage::

    elab-diff --odin3 O --yosys Y --equiv-check E --seeds A..B [--yosys-069 Y2]
              [--mem-cap C] [--work DIR]

For each seed N in A..B, ``verilog_gen --elab`` (``elab_profile.generate``) writes
``DIR/seedN/seedN.v`` (the top is always ``gtop``), then ``elab-equiv --top gtop
--unmap-enables --nolatches``-style bridge runs on it (``elab-equiv`` reads both sides with
``--nolatches``; the generator initialises every register and assigns every combinational output
at the top of its block, so no latch or x source differs between the two sides).

- NOT equivalent (exit 1), or any error of ``elab-equiv`` (exit 2, a child that failed or
  crashed on 2D's side), is a failure: ``seed N: FAIL ...``, the child's last lines and the
  kept file.
- Exit 3 is an ``oracle crash`` (Yosys died of a signal, for example 0.55's segfault on a
  recursive instantiation where 2D must still give its recursion error): counted and shown,
  never a failure by itself. Exit 77 is a skip.
- A seed with a crash or a skip is ``unverified``. The run fails (exit 1) when more than 10% of
  its seeds are unverified, or when all of them are.
- ``--yosys-069 Y2`` (the nightly drift job): a seed that Yosys 0.55 passes runs again with
  ``--yosys Y2 --allow-version 0.69``; a difference there is ``oracle drift``, reported and
  counted, never a failure. A 0.55 failure is a failure whatever 0.69 says.

Exit 0 every seed ok, 1 a seed failed or too many are unverified, 2 usage or setup error. Every
seed's files stay under ``DIR`` (default: a new directory under the temporary one). Children
run with the lock descriptor of ``tools/heavy-run`` inherited and a per-child memory cap
(``--mem-cap``, default 2G).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from types import ModuleType

import memcap

TOOL = "elab-diff"
HERE = Path(__file__).resolve().parent
ELAB_EQUIV = HERE / "elab-equiv"
PROFILE_DIR = HERE.parent / "verilog-gen"

EXIT_OK, EXIT_FAIL, EXIT_USAGE = 0, 1, 2
EXIT_CRASH, EXIT_SKIP = 3, 77  # elab-equiv's oracle crash and CTest's skip
GIB = 1 << 30
DEFAULT_CAP = 2 * GIB
TAIL_LINES = 6
TOP = "gtop"  # every generated module
CHILD_TIMEOUT_S = 1800
MAX_UNVERIFIED = 0.10
DRIFT_VERSION = "0.69"
_RANGE = re.compile(r"(\d+)\.\.(\d+)")
_SIZE = re.compile(r"(\d+)([KMG]?)")
_UNITS = {"": 1, "K": 1 << 10, "M": 1 << 20, "G": GIB}


class DiffError(Exception):
    """A usage or setup error (exit 2)."""


@dataclass(frozen=True)
class Config:
    odin3: str
    yosys: str
    equiv_check: str
    yosys_069: str | None
    cap: int
    work: Path


@dataclass
class SeedResult:
    seed: int
    failures: list[str] = field(default_factory=list)
    drift: list[str] = field(default_factory=list)
    crashes: list[str] = field(default_factory=list)
    skips: list[str] = field(default_factory=list)
    detail: list[str] = field(default_factory=list)


def parse_seeds(text: str) -> range:
    match = _RANGE.fullmatch(text.strip())
    if match is None or int(match.group(1)) < 1 or int(match.group(2)) < int(match.group(1)):
        raise DiffError(f"--seeds {text!r}: expected A..B with 1 <= A <= B")
    return range(int(match.group(1)), int(match.group(2)) + 1)


def parse_size(text: str) -> int:
    match = _SIZE.fullmatch(text.strip().upper())
    if match is None:
        raise argparse.ArgumentTypeError(f"bad size {text!r} (want e.g. 2G, 512M)")
    return int(match.group(1)) * _UNITS[match.group(2)]


def load_profile() -> ModuleType:
    """tools/verilog-gen/elab_profile.py, imported from its directory."""
    path = str(PROFILE_DIR)
    if path not in sys.path:
        sys.path.insert(0, path)
    import elab_profile  # noqa: PLC0415

    return elab_profile


def child(argv: Sequence[str]) -> tuple[int, str]:
    """(exit code, stdout + stderr) of a tool, the heavy lock's descriptor passed on."""
    try:
        done = subprocess.run([str(a) for a in argv], capture_output=True, text=True,
                              errors="replace", check=False, pass_fds=memcap.lock_fds(),
                              timeout=CHILD_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return EXIT_USAGE, f"timed out after {CHILD_TIMEOUT_S} s: {argv[0]}"
    except OSError as exc:
        return EXIT_USAGE, f"cannot run {argv[0]}: {exc}"
    return done.returncode, (done.stdout or "") + (done.stderr or "")


def tail(out: str) -> list[str]:
    return out.strip().splitlines()[-TAIL_LINES:]


def equiv_argv(cfg: Config, yosys: str, source: Path, keep: Path) -> list[str]:
    argv = [str(ELAB_EQUIV), "--odin3", cfg.odin3, "--yosys", yosys, "--equiv-check",
            cfg.equiv_check, "--mem-cap", str(cfg.cap), "--top", TOP, "--unmap-enables",
            "--keep", str(keep)]
    if yosys != cfg.yosys:
        argv += ["--allow-version", DRIFT_VERSION]
    return [*argv, str(source)]


def run_seed(cfg: Config, seed: int, profile: ModuleType) -> tuple[SeedResult, Path]:
    """Writes seed's file and compares it; returns the verdicts and the file."""
    folder = cfg.work / f"seed{seed}"
    folder.mkdir(parents=True, exist_ok=True)
    source = folder / f"seed{seed}.v"
    source.write_text(profile.generate(seed).text, encoding="utf-8")
    result = SeedResult(seed)
    keep = folder / "bridge"
    keep.mkdir(exist_ok=True)
    code, out = child(equiv_argv(cfg, cfg.yosys, source, keep))
    if code == EXIT_CRASH:
        result.crashes.append("oracle: " + (tail(out) or ["oracle crash"])[-1])
    elif code == EXIT_SKIP:
        result.skips.append("bridge")
    elif code != EXIT_OK:
        result.failures.append(f"elab-equiv: exit {code}")
        result.detail += tail(out)
    elif cfg.yosys_069 is not None:
        drift = folder / "bridge-069"
        drift.mkdir(exist_ok=True)
        code, _ = child(equiv_argv(cfg, cfg.yosys_069, source, drift))
        if code != EXIT_OK:
            result.drift.append(f"0.55 equivalent, 0.69 exit {code}")
    return result, source


def unverified(result: SeedResult) -> bool:
    """True when an oracle crash or a skip left the seed unchecked."""
    return bool(result.crashes or result.skips)


def too_many_unverified(count: int, total: int) -> bool:
    """A run that verified nothing, or left more than ``MAX_UNVERIFIED`` unverified, fails."""
    return count > 0 and (count == total or count > MAX_UNVERIFIED * total)


def report(result: SeedResult, source: Path) -> None:
    if result.failures:
        print(f"seed {result.seed}: FAIL {'; '.join(result.failures)}; file kept: {source}")
        for line in result.detail:
            print(f"  {line}")
        return
    notes = [*(f"oracle drift: {d}" for d in result.drift),
             *(f"oracle crash: {c}" for c in result.crashes),
             *(f"skipped: {s}" for s in result.skips)]
    print(f"seed {result.seed}: {'; '.join(notes) if notes else 'ok'}")


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--equiv-check", required=True, help="tools/equiv-check/equiv-check")
    ap.add_argument("--seeds", required=True, metavar="A..B", help="the seeds, e.g. 1..200")
    ap.add_argument("--yosys-069", metavar="Y2", help="also run with this Yosys 0.69; a "
                    "disagreement is oracle drift")
    ap.add_argument("--mem-cap", type=parse_size, default=DEFAULT_CAP, help="per child (2G)")
    ap.add_argument("--work", type=Path, help="keep every seed's files here (default: a new "
                    "directory under the temporary one)")
    return ap.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = parse_args(sys.argv[1:] if argv is None else argv)
        seeds = parse_seeds(args.seeds)
        profile = load_profile()
    except DiffError as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_USAGE
    except SystemExit as exc:  # argparse's usage error
        return exc.code if isinstance(exc.code, int) else EXIT_USAGE
    work = args.work or Path(tempfile.mkdtemp(prefix="elab-diff-"))
    cfg = Config(odin3=args.odin3, yosys=args.yosys, equiv_check=args.equiv_check,
                 yosys_069=args.yosys_069, cap=args.mem_cap, work=work.resolve())
    print(f"{TOOL}: seeds {args.seeds}, files under {cfg.work}")
    failed = drifted = crashed = unproven = 0
    for seed in seeds:
        result, source = run_seed(cfg, seed, profile)
        report(result, source)
        failed += bool(result.failures)
        drifted += bool(result.drift)
        crashed += bool(result.crashes)
        unproven += unverified(result)
    print(f"{TOOL}: {len(seeds)} seeds, {len(seeds) - failed} not failed, {failed} failed, "
          f"{drifted} oracle drift, {crashed} oracle crashes, {unproven} unverified")
    if too_many_unverified(unproven, len(seeds)) and not failed:
        print(f"{TOOL}: FAIL: {unproven} of {len(seeds)} seeds unverified (an oracle crash or a "
              f"skip; the limit is {MAX_UNVERIFIED:.0%}, and never all)")
        return EXIT_FAIL
    return EXIT_FAIL if failed else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
