"""identity: the k6 identity gate of ``partial_map`` (4.2 spec T-2, plan Tasks 3 and 4).

Tier 1 is a wiring test, not a regression test: ``hardmap`` and ``partial_map`` are two pass rows
that share one entry point, so one binary compared with itself passes whatever a refactor did. It
fails only when the alias is unregistered or the two names parse their arguments differently.
Tier 2 (record against library) is a real check but needs VTR_ROOT. Tier 3 (two binaries, main
against the head) is the merge gate.

    identity --tier 1 [--odin3 BIN] [--list FILE]... [DESIGN.v[:TOP]]...
    identity --tier 2 [--odin3 BIN] [--arch-xml XML] [--list FILE]...
    identity --tier 3 --baseline BIN --candidate BIN [--list FILE]... [--vtr-list FILE
             --vtr-root DIR [--golden-root DIR]]

Runs Odin III's Phase 3 script up to and including ``lower; opt -fast; compact``
(tools/hardmap-check/phase3_pre_abc.script) twice per design and compares what ``write_blif
--vtr``, plain ``write_blif`` and ``write_rtlil`` write, byte for byte (lines starting with ``#``
are ignored). Scope: those three files; provenance, JSON, Verilog and run-info are not compared.
A pair that fails with the same exit status is ``same-failure`` only for a design named in the
allow-list (``--allow-same-failure``, default tests/partial_map/identity_known_failures.txt);
for it the tool also runs both legs up to ``write_rtlil`` before ``lower`` and compares that
RTLIL and the ``hardmap:``/``partial_map:`` log lines (prefix normalised). Any other design that
fails on both sides is DIFFERENT.

* tier 1 (CI): ``hardmap`` against ``partial_map``, both with ``read_techlib lib/vtr.o3lib``;
* tier 2 (local, needs VTR_ROOT): ``partial_map`` with the tech library against ``partial_map``
  with ``read_arch`` of the k6 XML;
* tier 3 (the controller, heavy): ``hardmap`` of the baseline binary against ``partial_map`` of the
  candidate binary, over the same designs (the micros of ``--list`` and the 19 of ``--vtr-list``).

A list line is ``path [top]`` (relative to the repository root); a ``--vtr-list`` line is a design
name under ``--vtr-root``, its top the first ``.model`` of its Parmys golden under
``--golden-root``. Exit codes: 0 no difference and at least ``--min-ok`` designs written the same BLIF;
1 a difference or too few; 2 usage or tool error; 3 a child crashed or timed out; 77 skipped.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import elab_sweep
import exit_check
import hardmap_diff as hd

TOOL = "identity"
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_HARD, EXIT_SKIP = 0, 1, 2, 3, 77
K6_ARCH = "vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml"
DEFAULT_INPUTS = REPO / "tests" / "partial_map" / "identity_inputs.txt"
DEFAULT_ALLOW = REPO / "tests" / "partial_map" / "identity_known_failures.txt"
TIMING = re.compile(r"\d ms$")  # the manager's "pass <name>: 0.3 ms" line
PASS_WORDS = re.compile(r"\b(?:hardmap|partial_map)\b")
OLD_NAME, NEW_NAME = "hardmap", "partial_map"


@dataclass(frozen=True)
class Design:
    key: str
    path: Path
    top: str | None


@dataclass(frozen=True)
class Leg:
    """One run: a binary, the pass name it runs and the command that loads the library."""

    odin3: str
    pass_name: str
    load: str


@dataclass(frozen=True)
class Outcome:
    rc: int
    vtr: list[str]
    plain: list[str]
    rtlil: list[str]
    tail: str
    log: list[str]


@dataclass(frozen=True)
class Outputs:
    """The files one run writes."""

    vtr: Path
    plain: Path
    rtlil: Path


def read_list(path: Path) -> list[Design]:
    """The designs of a ``path [top]`` list (``#`` comments), paths relative to the repo root."""
    designs: list[Design] = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        words = raw.split("#", 1)[0].split()
        if not words:
            continue
        if len(words) > 2:
            raise hd.ToolError(f"{path}:{number}: expected 'path [top]'")
        design = REPO / words[0] if not Path(words[0]).is_absolute() else Path(words[0])
        designs.append(Design(words[0], design, words[1] if len(words) == 2 else None))
    return designs


def read_vtr(path: Path, root: Path, golden_root: Path | None) -> list[Design]:
    designs: list[Design] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        name = raw.split("#", 1)[0].strip()
        if not name:
            continue
        source = root / f"{name}.v"
        top = None
        if golden_root is not None:
            golden = Path(exit_check.golden_path(golden_root, source))
            top = hd.first_model(golden) if golden.is_file() else None
        designs.append(Design(name, source, top))
    return designs


def script_for(leg: Leg, design: Path, outs: Outputs, pre_lower: bool) -> str:
    """The Phase 3 script of ``leg`` on ``design``.

    The full script writes the BLIFs and the RTLIL after ``compact``; with ``pre_lower`` it stops
    before ``lower`` and writes the RTLIL of the ``hardmap`` result only.
    """
    text = hd.script_text(hd.SCRIPT, hd.REPO / "lib" / "vtr.o3lib", design, outs.vtr, hd.ADDER_LIB,
                          rtlil=pre_lower)
    load = f"read_techlib {hd.REPO / 'lib' / 'vtr.o3lib'}"
    if text.count(load) != 1 or text.count("; hardmap; ") != 1:
        raise hd.ToolError("phase3_pre_abc.script no longer has one read_techlib and one hardmap")
    text = text.replace(load, leg.load).replace("; hardmap; ", f"; {leg.pass_name}; ")
    if pre_lower:
        return text.replace(f"write_rtlil {outs.vtr}", f"write_rtlil {outs.rtlil}")
    return f"{text}; write_blif {outs.plain}; write_rtlil {outs.rtlil}"


def body(path: Path) -> list[str]:
    """The lines of a file without the ``#`` header lines; empty when it was not written."""
    if not path.is_file():
        return []
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return [ln for ln in lines if not ln.startswith("#")]


def pass_log(text: str) -> list[str]:
    """The log lines of the pass (``hardmap:`` or ``partial_map:``), name normalised, timing out."""
    keep = [ln for ln in text.splitlines()
            if re.search(r"\b(?:hardmap|partial_map):", ln) and not TIMING.search(ln)]
    return [PASS_WORDS.sub("pm", ln) for ln in keep]


def run_leg(leg: Leg, design: Design, work: Path, timeout: float,
            pre_lower: bool = False) -> Outcome:
    stem = f"{leg.pass_name}.pre" if pre_lower else leg.pass_name
    outs = Outputs(work / f"{stem}.vtr.blif", work / f"{stem}.plain.blif", work / f"{stem}.il")
    script = script_for(leg, design.path, outs, pre_lower)
    argv = [leg.odin3, *(["--top", design.top] if design.top else []), "-p", script]
    proc = hd.run_child(argv, timeout)
    return Outcome(proc.returncode, body(outs.vtr), body(outs.plain), body(outs.rtlil),
                   hd.tail(proc.stderr), pass_log(proc.stderr + "\n" + proc.stdout))


def first_difference(left: list[str], right: list[str]) -> str:
    for number, (a, b) in enumerate(zip(left, right, strict=False), 1):
        if a != b:
            return f"line {number}: {a!r} against {b!r}"
    return f"{len(left)} lines against {len(right)}"


def judge(first: Outcome, second: Outcome, allowed: bool = False) -> tuple[str, str]:
    """(class, detail): identical, same-failure or DIFFERENT.

    Both runs failing with the same exit status is ``same-failure`` only for an allowed design
    (and then the ``hardmap`` log lines must match); otherwise it is DIFFERENT.
    """
    if first.rc != second.rc:
        return "DIFFERENT", f"exit {first.rc} against {second.rc}: {first.tail} | {second.tail}"
    if first.rc != 0:
        if not allowed:
            return "DIFFERENT", f"both exit {first.rc}, not in the allow-list: {first.tail}"
        if first.log != second.log:
            return "DIFFERENT", "pass log: " + first_difference(first.log, second.log)
        return "same-failure", f"both exit {first.rc}: {first.tail}"
    for label, left, right in (("write_blif --vtr", first.vtr, second.vtr),
                               ("write_blif", first.plain, second.plain),
                               ("write_rtlil", first.rtlil, second.rtlil)):
        if left != right:
            return "DIFFERENT", f"{label}: " + first_difference(left, right)
    if not first.vtr or not first.plain or not first.rtlil:
        return "DIFFERENT", "no BLIF or RTLIL written"
    return "identical", f"{len(first.vtr)} + {len(first.plain)} + {len(first.rtlil)} lines"


def judge_pre_lower(first: Outcome, second: Outcome) -> tuple[str, str]:
    """The ``hardmap``-only runs of an allowed failure: both must succeed and write the same."""
    if first.rc != 0 or second.rc != 0:
        return "DIFFERENT", f"hardmap-only run failed: exit {first.rc} and {second.rc}"
    if first.rtlil != second.rtlil or not first.rtlil:
        return "DIFFERENT", "hardmap-only write_rtlil: " + first_difference(first.rtlil,
                                                                            second.rtlil)
    if first.log != second.log:
        return "DIFFERENT", "hardmap-only pass log: " + first_difference(first.log, second.log)
    return "same-failure", f"hardmap-only identical, {len(first.rtlil)} RTLIL lines"


def legs_for(args: argparse.Namespace) -> tuple[Leg, Leg]:
    lib = f"read_techlib {hd.REPO / 'lib' / 'vtr.o3lib'}"
    if args.tier == 1:
        return Leg(args.odin3, OLD_NAME, lib), Leg(args.odin3, NEW_NAME, lib)
    if args.tier == 2:
        return (Leg(args.odin3, NEW_NAME, lib),
                Leg(args.odin3, NEW_NAME, f"read_arch {args.arch_xml}"))
    return Leg(args.baseline, OLD_NAME, lib), Leg(args.candidate, NEW_NAME, lib)


def compare(legs: tuple[Leg, Leg], design: Design, timeout: float,
            allowed: bool = False) -> tuple[str, str]:
    with tempfile.TemporaryDirectory(prefix="partial-map-identity.") as tmp:
        work = Path(tmp)
        runs = []
        for number, leg in enumerate(legs):
            leg_dir = work / str(number)
            leg_dir.mkdir()
            runs.append(run_leg(leg, design, leg_dir, timeout))
        cls, detail = judge(runs[0], runs[1], allowed)
        if cls != "same-failure":
            return cls, detail
        pre = [run_leg(leg, design, work / str(number), timeout, pre_lower=True)
               for number, leg in enumerate(legs)]
        cls, extra = judge_pre_lower(pre[0], pre[1])
        return cls, f"{detail}; {extra}"


def parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--tier", type=int, choices=(1, 2, 3), required=True)
    ap.add_argument("--odin3", default=str(REPO / "build" / "release" / "odin3"))
    ap.add_argument("--baseline", help="tier 3: the binary built from main")
    ap.add_argument("--candidate", help="tier 3: the binary of the PR")
    ap.add_argument("--arch-xml", help="tier 2: the k6 arch XML (default under $VTR_ROOT)")
    ap.add_argument("--list", type=Path, action="append", default=[],
                    help="a 'path [top]' list (default: tests/partial_map/identity_inputs.txt)")
    ap.add_argument("--vtr-list", type=Path, help="tier 3: names of the VTR designs")
    ap.add_argument("--vtr-root", type=Path, help="the directory holding <name>.v")
    ap.add_argument("--golden-root", type=Path, help="Parmys goldens, for the tops of --vtr-list")
    ap.add_argument("--allow-same-failure", type=Path, default=DEFAULT_ALLOW,
                    help="designs allowed to fail on both sides (default: "
                         "tests/partial_map/identity_known_failures.txt)")
    ap.add_argument("--min-ok", type=int, default=1,
                    help="the least number of designs that must write the same BLIF")
    ap.add_argument("--timeout", type=float, default=hd.DEFAULT_TIMEOUT)
    ap.add_argument("designs", nargs="*", metavar="DESIGN.v[:TOP]")
    return ap.parse_args(argv)


def gather(args: argparse.Namespace) -> list[Design]:
    designs: list[Design] = []
    lists = args.list or ([DEFAULT_INPUTS] if not args.designs and not args.vtr_list else [])
    for path in lists:
        designs += read_list(path)
    if args.vtr_list:
        if args.vtr_root is None:
            raise hd.ToolError("--vtr-list needs --vtr-root")
        golden = args.golden_root or elab_sweep.load_micro_top().DEFAULT_ROOT
        designs += read_vtr(args.vtr_list, args.vtr_root, golden if golden.is_dir() else None)
    for item in args.designs:
        name, _, top = item.partition(":")
        designs.append(Design(name, Path(name), top or None))
    if not designs:
        raise hd.ToolError("no designs")
    return designs


def prepare(args: argparse.Namespace) -> int | None:
    """Checks the options of the tier; an exit code when the run cannot start."""
    if args.tier == 3 and not (args.baseline and args.candidate):
        raise hd.ToolError("tier 3 needs --baseline and --candidate")
    if args.tier == 2:
        root = os.environ.get("VTR_ROOT")
        args.arch_xml = args.arch_xml or (str(Path(root) / K6_ARCH) if root else None)
        if not args.arch_xml or not Path(args.arch_xml).is_file():
            print(f"{TOOL}: skipped: tier 2 needs VTR_ROOT or --arch-xml", file=sys.stderr)
            return EXIT_SKIP
    return None


def run_all(args: argparse.Namespace, designs: list[Design]) -> int:
    legs = legs_for(args)
    allow = {d.key for d in read_list(args.allow_same_failure)}
    classes: Counter[str] = Counter()
    code = EXIT_OK
    for design in designs:
        try:
            cls, detail = compare(legs, design, args.timeout, design.key in allow)
        except hd.HardFailure as exc:
            cls, detail, code = "HARD", str(exc), EXIT_HARD
        classes[cls] += 1
        print(f"{TOOL}: {design.key}: {cls} ({detail})")
        if cls == "DIFFERENT" and code == EXIT_OK:
            code = EXIT_FAIL
    print(f"{TOOL}: tier {args.tier}: {len(designs)} designs: "
          + ", ".join(f"{n} {c}" for c, n in sorted(classes.items())))
    if code == EXIT_OK and classes["identical"] < args.min_ok:
        print(f"{TOOL}: only {classes['identical']} identical, --min-ok is {args.min_ok}",
              file=sys.stderr)
        return EXIT_FAIL
    return code


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        skip = prepare(args)
        if skip is not None:
            return skip
        return run_all(args, gather(args))
    except (hd.ToolError, OSError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
