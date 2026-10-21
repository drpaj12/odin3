"""odin3_flow: take a design through Odin III, and optionally VPR, in one command (7.3, spec 7
section 2.1).

    odin3_flow.py design.v [more.v ...] --top TOP [--target vtr|lpm] [--arch ARCH.xml]
        [--family NAME] [--vpr | --no-vpr] [--analysis] [--odin3 PATH] [--out DIR]

Target ``vtr``: the Odin III script of the vtr-qor O leg (imported from tools/vtr-qor, never
copied) writes ``odin3/<top>.blif``. Target ``lpm``: the 5.1 LPM script (tests/lpm/lpm.script)
writes ``odin3/<top>.v`` and a read-back lint (``read_verilog -icells``) follows. Every step runs in
its own subdirectory of ``--out`` with its log. Exit codes: 0 ok, 1 a step failed, 2 usage error.
The module is importable: ``run_flow(FlowArgs) -> FlowResult``.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "vtr-qor"))

import qor_legs  # noqa: E402  (a stand-alone tool directory, put on sys.path above)

LPM_SCRIPT = ROOT / "tests" / "lpm" / "lpm.script"
LPM_LIB = ROOT / "lib" / "lpm.o3lib"
DEFAULT_ARCH = "k6"
DEFAULT_K = 6
STEP_TIMEOUT_S = 3600
TARGETS = ("vtr", "lpm")
_MODULE = re.compile(r"^\s*module\s+\\?([A-Za-z_][A-Za-z0-9_$]*)", re.M)
_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)


class UsageError(Exception):
    """A bad command line or input: exit 2."""


@dataclass(frozen=True)
class FlowArgs:
    designs: list[Path]
    top: str | None
    target: str
    arch: Path | None
    family: str
    vpr: bool
    analysis: bool
    odin3: Path
    out: Path


@dataclass(frozen=True)
class StepResult:
    name: str
    ok: bool
    log: Path
    seconds: float


@dataclass
class FlowResult:
    ok: bool
    steps: list[StepResult] = field(default_factory=list)
    summary: dict[str, object] = field(default_factory=dict)


def find_tops(designs: Sequence[Path]) -> list[str]:
    """The modules of ``designs`` that no text of the files mentions but their own header."""
    text = "\n".join(_COMMENT.sub("", p.read_text(errors="replace")) for p in designs)
    names = list(dict.fromkeys(_MODULE.findall(text)))
    def mentions(name: str) -> int:
        pattern = rf"(?<![A-Za-z0-9_$]){re.escape(name)}(?![A-Za-z0-9_$])"
        return len(re.findall(pattern, text))

    return [n for n in names if mentions(n) == 1]


def resolve_top(designs: Sequence[Path], top: str | None) -> str:
    """``top`` when given, else the one uninstantiated module; UsageError naming the candidates."""
    if top is not None:
        return top
    tops = find_tops(designs)
    if len(tops) == 1:
        return tops[0]
    if not tops:
        raise UsageError("no top module found: give --top")
    raise UsageError(f"more than one top module ({', '.join(tops)}): give --top")


def lpm_script(designs: Sequence[Path], out: Path, family: str) -> str:
    """The text of tests/lpm/lpm.script with its placeholders filled, as lpm-check fills them."""
    lines = [ln for ln in LPM_SCRIPT.read_text().splitlines()
             if ln.strip() and not ln.startswith("#")]
    fam = f'--family "{family}"' if family else ""
    text = lines[0].replace("@LIB@", str(LPM_LIB))
    text = text.replace("@DESIGN@", " ".join(str(d) for d in designs))
    return text.replace("@FAMILY@", fam).replace("@OUT@", str(out)).replace("  ", " ")


def run_step(name: str, argv: Sequence[str], step_dir: Path, env: dict[str, str] | None = None,
             log_name: str | None = None) -> StepResult:
    """Run ``argv`` in ``step_dir``; stdout and stderr go to its log. A timeout is a failure."""
    step_dir.mkdir(parents=True, exist_ok=True)
    log = step_dir / (log_name or f"{name}.log")
    start = time.monotonic()
    try:
        with log.open("w") as handle:
            handle.write("$ " + " ".join(argv) + "\n")
            handle.flush()
            proc = subprocess.run(list(argv), cwd=step_dir, stdout=handle, stderr=subprocess.STDOUT,
                                  env=env, timeout=STEP_TIMEOUT_S, check=False)
        ok = proc.returncode == 0
    except (OSError, subprocess.TimeoutExpired) as exc:
        log.write_text(f"{log.read_text()}\n{exc}\n")
        ok = False
    return StepResult(name, ok, log, time.monotonic() - start)


def lut_k(args: FlowArgs, vtr_root: Path | None, log: Path) -> int:
    """K from the architecture through VTR's own reader; 6 (stated in ``log``) without VTR."""
    arch = args.arch
    if arch is None and vtr_root is not None:
        arch = qor_legs.arch_xml_path(vtr_root, DEFAULT_ARCH)
    if arch is None or vtr_root is None:
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(f"NOTE: no VTR_ROOT or arch: LUT size K = {DEFAULT_K}\n")
        return DEFAULT_K
    python = vtr_root / ".venv" / "bin" / "python"
    return int(qor_legs.lut_size(vtr_root, python, arch))


def vtr_root_from_env() -> Path | None:
    value = os.environ.get("VTR_ROOT")
    return Path(value) if value else None


def odin3_command(args: FlowArgs, top: str, script: str) -> list[str]:
    return [str(args.odin3), "--diag-paths", "resolved", "--top", top, "-p", script]


def vtr_steps(args: FlowArgs, top: str, result: FlowResult) -> Path | None:
    """The odin3 step of target ``vtr``; the BLIF it wrote, or None when it failed."""
    step_dir = args.out / "odin3"
    vtr_root = vtr_root_from_env()
    k = lut_k(args, vtr_root, step_dir / "k.log")
    blif = step_dir / f"{top}.blif"
    design = " ".join(str(d) for d in args.designs)
    script = qor_legs.odin3_script("O", Path(design), blif, k, "short")
    step = run_step("odin3", odin3_command(args, top, script), step_dir)
    result.steps.append(step)
    return blif if step.ok else None


def lpm_steps(args: FlowArgs, top: str, result: FlowResult) -> Path | None:
    """The odin3 step and the read-back lint of target ``lpm``."""
    step_dir = args.out / "odin3"
    written = step_dir / f"{top}.v"
    script = lpm_script(args.designs, written, args.family)
    step = run_step("odin3", odin3_command(args, top, script), step_dir)
    result.steps.append(step)
    if not step.ok:
        return None
    back = f"read_techlib {LPM_LIB}; read_verilog -icells {written}"
    lint = run_step("readback", [str(args.odin3), "--check", "-p", back], args.out / "readback")
    result.steps.append(lint)
    return written if lint.ok else None


def run_flow(args: FlowArgs) -> FlowResult:
    """Run every step; stop at the first that fails. UsageError for a bad input."""
    if args.target not in TARGETS:
        raise UsageError(f"unknown target '{args.target}' ({', '.join(TARGETS)})")
    for design in args.designs:
        if not design.is_file():
            raise UsageError(f"no such design file: {design}")
    top = resolve_top(args.designs, args.top)
    result = FlowResult(ok=False)
    args.out.mkdir(parents=True, exist_ok=True)
    step_fn = vtr_steps if args.target == "vtr" else lpm_steps
    result.ok = step_fn(args, top, result) is not None
    return result


def default_odin3() -> Path:
    found = shutil.which("odin3")
    return Path(found) if found else ROOT / "build" / "release" / "odin3"


def parse_args(argv: Sequence[str]) -> tuple[FlowArgs, bool | None]:
    ap = argparse.ArgumentParser(prog="odin3_flow", description=__doc__.split("\n")[0])
    ap.add_argument("designs", nargs="+", type=Path)
    ap.add_argument("--top")
    ap.add_argument("--target", default="vtr", choices=TARGETS)
    ap.add_argument("--arch", type=Path)
    ap.add_argument("--family", default="")
    ap.add_argument("--vpr", action=argparse.BooleanOptionalAction, default=None)
    ap.add_argument("--analysis", action="store_true")
    ap.add_argument("--odin3", type=Path, default=None)
    ap.add_argument("--out", type=Path)
    ns = ap.parse_args(argv)
    out = ns.out
    if out is None:
        for design in ns.designs:
            if not design.is_file():
                raise UsageError(f"no such design file: {design}")
        out = Path("odin3_out") / resolve_top(ns.designs, ns.top)
    args = FlowArgs(ns.designs, ns.top, ns.target, ns.arch, ns.family, False, ns.analysis,
                    ns.odin3 or default_odin3(), out)
    return args, ns.vpr


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args, _vpr = parse_args(sys.argv[1:] if argv is None else argv)
        result = run_flow(args)
    except UsageError as exc:
        print(f"odin3_flow: {exc}", file=sys.stderr)
        return 2
    for step in result.steps:
        state = "ok" if step.ok else "FAILED"
        print(f"step {step.name}: {state} ({step.seconds:.1f} s) log {step.log}")
        if not step.ok:
            print(f"odin3_flow: step {step.name} failed; see {step.log}", file=sys.stderr)
    return 0 if result.ok else 1


if __name__ == "__main__":
    sys.exit(main())
