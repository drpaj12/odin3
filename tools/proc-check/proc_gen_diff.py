"""proc-gen-diff: seeds of the ``--proc`` generator through ``proc-diff`` and the bridge (2E spec
§18.6, plan Task 12).

Usage::

    proc-gen-diff --odin3 O --yosys Y --equiv-check E --seeds A..B [--yosys-069 Y2]
                  [--no-trio] [--mem-cap C] [--work DIR]

For each seed N in A..B, ``verilog_gen --proc`` (``proc_profile.generate``) writes
``DIR/seedN/seedN.v`` (the module is always ``t``), then:

- ``proc-diff --top t`` in the default latch mode and, when the seed infers no latch, again with
  ``--nolatches`` (a latch is an error of ``read_verilog --nolatches`` on both sides);
- when it infers no latch, ``elab-equiv --pass proc --unmap-enables --async2sync`` (the bridge,
  ``--nolatches``; 2E Task 11's flags).

Both read the pre-``proc`` trio's input unless ``--no-trio`` is given or ``opt -nodffe -nosdff``
does not run yet (2F-5): the probe announces ``trio: skipped``. Every seed's files stay under
``DIR`` (default: a new directory under the system temporary one), so a failing seed's file and
each run's work directory are kept and named.

- A difference, or any error of ``proc-diff`` or the bridge, is a failure: ``seed N: FAIL ...``,
  the child's last lines and the kept file.
- ``proc-diff`` exit 3 is a ``compare tie`` (its netlist-compare tells a side from a reordered copy
  of itself, so a difference proves nothing): counted and shown, never a failure; the bridge on
  the same seed is the arbiter.
- ``elab-equiv`` exit 3 is an ``oracle crash`` (Yosys died of a signal): counted and shown, never a
  failure by itself; exit 77 is a skip.
- A seed with a tie, a crash or a skip is ``unverified``. The run fails (exit 1) when more than 10%
  of its seeds are unverified, or when all of them are; the summary prints the count.
- ``--yosys-069 Y2`` (the nightly drift job): every ``proc-diff`` run that Yosys 0.55 passes runs
  again with ``--yosys Y2 --allow-version 0.69``; a difference there is ``oracle drift``, reported
  and counted, never a failure. A 0.55 failure is a failure whatever 0.69 says.

Exit 0 every seed ok, 1 a seed failed or too many are unverified, 2 usage or setup error. Children run with the lock
descriptor of ``tools/heavy-run`` inherited and a per-child memory cap (``--mem-cap``, default 3G).
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

import proc_diff

TOOL = "proc-gen-diff"
HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
PROC_DIFF = HERE / "proc-diff"
ELAB_EQUIV = TOOLS / "elab-check" / "elab-equiv"
PROC_PROFILE_PY = TOOLS / "verilog-gen" / "proc_profile.py"

EXIT_OK, EXIT_FAIL, EXIT_USAGE = 0, 1, 2
BRIDGE_CRASH, BRIDGE_SKIP = 3, 77  # elab-equiv's oracle crash, CTest's skip
DIFF_TIE = 3  # proc-diff's tie: netlist-compare cannot order the netlist's tied cells
GIB = 1 << 30
DEFAULT_CAP = 3 * GIB
TAIL_LINES = 6
TOP = "t"  # every generated module
CHILD_TIMEOUT_S = 3600  # a child past an hour is that seed's failure
MAX_UNVERIFIED = 0.10  # a run whose unverified seeds exceed this share fails
DRIFT_VERSION = "0.69"
_RANGE = re.compile(r"(\d+)\.\.(\d+)")


class GenDiffError(Exception):
    """A usage or setup error (exit 2)."""


@dataclass(frozen=True)
class Config:
    odin3: str
    yosys: str
    equiv_check: str
    yosys_069: str | None
    trio: bool
    cap: int
    work: Path


@dataclass
class SeedResult:
    seed: int
    failures: list[str] = field(default_factory=list)  # one "<run>: <why>" per failed run
    drift: list[str] = field(default_factory=list)
    crashes: list[str] = field(default_factory=list)
    ties: list[str] = field(default_factory=list)  # proc-diff exit 3: netlist-compare ties
    skips: list[str] = field(default_factory=list)
    detail: list[str] = field(default_factory=list)  # the failing runs' last output lines


def parse_seeds(text: str) -> range:
    match = _RANGE.fullmatch(text.strip())
    if match is None or int(match.group(1)) < 1 or int(match.group(2)) < int(match.group(1)):
        raise GenDiffError(f"--seeds {text!r}: expected A..B with 1 <= A <= B")
    return range(int(match.group(1)), int(match.group(2)) + 1)


def memcap() -> ModuleType:
    """tools/elab-check/memcap.py: the heavy lock's descriptor and the trio's probe."""
    module = proc_diff.load_module("memcap", proc_diff.MEMCAP_PY)
    if module is None:
        raise GenDiffError(f"{proc_diff.MEMCAP_PY} is missing")
    return module


def child(argv: Sequence[str]) -> tuple[int, str]:
    """(exit code, stdout + stderr) of a tool, the heavy lock's descriptor passed on."""
    try:
        done = subprocess.run([str(a) for a in argv], capture_output=True, text=True,
                              errors="replace", check=False, pass_fds=memcap().lock_fds(),
                              timeout=CHILD_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return EXIT_USAGE, f"timed out after {CHILD_TIMEOUT_S} s: {argv[0]}"
    except OSError as exc:
        return EXIT_USAGE, f"cannot run {argv[0]}: {exc}"
    return done.returncode, (done.stdout or "") + (done.stderr or "")


def trio_available(odin3: str) -> bool:
    """True once ``opt`` exists: the trio's flags run on an empty design (2F-5)."""
    probe = memcap().TRIO_PROBE
    code, out = child([odin3, "-p", probe])
    if code != 0:
        reason = (out.strip().splitlines() or ["no output"])[-1]
        print(f"{TOOL}: trio: skipped ({probe} exit {code}: {reason})")
    return code == 0


def load_profile() -> ModuleType:
    module = proc_diff.load_module("proc_profile", PROC_PROFILE_PY)
    if module is None:
        raise GenDiffError(f"{PROC_PROFILE_PY} is missing")
    return module


def tail(out: str) -> list[str]:
    return out.strip().splitlines()[-TAIL_LINES:]


def diff_argv(cfg: Config, yosys: str, source: Path, keep: Path, nolatches: bool) -> list[str]:
    argv = [str(PROC_DIFF), "--odin3", cfg.odin3, "--yosys", yosys, "--top", TOP,
            "--mem-cap", str(cfg.cap), "--keep", str(keep)]
    if nolatches:
        argv.append("--nolatches")
    if not cfg.trio:
        argv.append("--no-trio")
    if yosys != cfg.yosys:
        argv += ["--allow-version", DRIFT_VERSION]
    return [*argv, str(source)]


def bridge_argv(cfg: Config, source: Path, keep: Path) -> list[str]:
    argv = [str(ELAB_EQUIV), "--odin3", cfg.odin3, "--yosys", cfg.yosys, "--equiv-check",
            cfg.equiv_check, "--mem-cap", str(cfg.cap), "--pass", "proc", "--unmap-enables",
            "--async2sync", "--top", TOP, "--keep", str(keep)]
    if not cfg.trio:
        argv.append("--no-trio")
    return [*argv, str(source)]


def run_diff(cfg: Config, result: SeedResult, source: Path, nolatches: bool) -> None:
    """proc-diff in one latch mode under Yosys 0.55 and, when it passes, under 0.69."""
    mode = "nolatches" if nolatches else "default"
    base = source.parent / f"proc-diff-{mode}"
    base.mkdir(exist_ok=True)
    code, out = child(diff_argv(cfg, cfg.yosys, source, base, nolatches))
    if code == DIFF_TIE:
        result.ties.append(f"proc-diff ({mode})")
        return
    if code != EXIT_OK:
        result.failures.append(f"proc-diff ({mode}): exit {code}")
        result.detail += tail(out)
        return
    if cfg.yosys_069 is None:
        return
    drift = source.parent / f"proc-diff-{mode}-069"
    drift.mkdir(exist_ok=True)
    code, out = child(diff_argv(cfg, cfg.yosys_069, source, drift, nolatches))
    if code != EXIT_OK:
        result.drift.append(f"proc-diff ({mode}): 0.55 identical, 0.69 exit {code}")


def run_bridge(cfg: Config, result: SeedResult, source: Path) -> None:
    keep = source.parent / "bridge"
    keep.mkdir(exist_ok=True)
    code, out = child(bridge_argv(cfg, source, keep))
    if code == BRIDGE_CRASH:
        result.crashes.append("bridge: " + (tail(out) or ["oracle crash"])[-1])
    elif code == BRIDGE_SKIP:
        result.skips.append("bridge")
    elif code != EXIT_OK:
        result.failures.append(f"bridge: exit {code}")
        result.detail += tail(out)


def run_seed(cfg: Config, seed: int, profile: ModuleType) -> tuple[SeedResult, Path]:
    made = profile.generate(seed)
    folder = cfg.work / f"seed{seed}"
    folder.mkdir(parents=True, exist_ok=True)
    source = folder / f"seed{seed}.v"
    source.write_text(made.text, encoding="utf-8")
    result = SeedResult(seed)
    run_diff(cfg, result, source, nolatches=False)
    if "latch" not in made.features:
        run_diff(cfg, result, source, nolatches=True)
        run_bridge(cfg, result, source)
    return result, source


def unverified(result: SeedResult) -> bool:
    """True when nothing proved the seed: a tie, an oracle crash or a skip left it unchecked
    (the bridge is the arbiter of a tie, and runs on latch-free seeds only)."""
    return bool(result.ties or result.crashes or result.skips)


def too_many_unverified(count: int, total: int) -> bool:
    """A run that verified nothing, or left more than ``MAX_UNVERIFIED`` of its seeds
    unverified, must not pass."""
    return count > 0 and (count == total or count > MAX_UNVERIFIED * total)


def report(result: SeedResult, source: Path) -> None:
    if result.failures:
        print(f"seed {result.seed}: FAIL {'; '.join(result.failures)}; file kept: {source}")
        for line in result.detail:
            print(f"  {line}")
        return
    notes = [*(f"oracle drift: {d}" for d in result.drift),
             *(f"oracle crash: {c}" for c in result.crashes),
             *(f"compare tie: {x} (netlist-compare cannot order the tied cells)"
               for x in result.ties),
             *(f"skipped: {s}" for s in result.skips)]
    print(f"seed {result.seed}: {'; '.join(notes) if notes else 'ok'}")


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n\n")[0]
                                 if __doc__ else None)
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--equiv-check", required=True, help="tools/equiv-check/equiv-check")
    ap.add_argument("--seeds", required=True, metavar="A..B", help="the seeds, e.g. 1..200")
    ap.add_argument("--yosys-069", metavar="Y2", help="also run proc-diff with this Yosys 0.69; "
                    "a disagreement is oracle drift")
    ap.add_argument("--no-trio", action="store_true", help="no pre-proc opt passes on side A")
    ap.add_argument("--mem-cap", type=proc_diff.parse_size, default=DEFAULT_CAP,
                    help="per child (default 3G)")
    ap.add_argument("--work", type=Path, help="keep every seed's files here (default: a new "
                    "directory under the temporary one)")
    return ap.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = parse_args(sys.argv[1:] if argv is None else argv)
        seeds = parse_seeds(args.seeds)
        profile = load_profile()
        memcap()
    except GenDiffError as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_USAGE
    except SystemExit as exc:  # argparse's usage error
        return exc.code if isinstance(exc.code, int) else EXIT_USAGE
    work = args.work or Path(tempfile.mkdtemp(prefix="proc-gen-diff-"))
    trio = not args.no_trio and trio_available(args.odin3)
    cfg = Config(args.odin3, args.yosys, args.equiv_check, args.yosys_069, trio, args.mem_cap,
                 work.resolve())
    print(f"{TOOL}: seeds {args.seeds}, files under {cfg.work}")
    failed = drifted = crashed = tied = unproven = 0
    for seed in seeds:
        result, source = run_seed(cfg, seed, profile)
        report(result, source)
        failed += bool(result.failures)
        drifted += bool(result.drift)
        crashed += bool(result.crashes)
        tied += bool(result.ties)
        unproven += unverified(result)
    print(f"{TOOL}: {len(seeds)} seeds, {len(seeds) - failed} not failed, {failed} failed, "
          f"{drifted} oracle drift, {crashed} oracle crashes, {tied} compare ties, "
          f"{unproven} unverified")
    if too_many_unverified(unproven, len(seeds)) and not failed:
        print(f"{TOOL}: FAIL: {unproven} of {len(seeds)} seeds unverified (a tie, an oracle "
              f"crash or a skip; the limit is {MAX_UNVERIFIED:.0%}, and never all)")
        return EXIT_FAIL
    return EXIT_FAIL if failed else EXIT_OK

if __name__ == "__main__":
    sys.exit(main())
