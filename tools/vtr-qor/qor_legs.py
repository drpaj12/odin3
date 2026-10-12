"""qor_legs: the one definition of every vtr-qor leg's commands (3H spec QH-3, §3.7).

Legs (spec §3): P (Parmys through VTR's default flow), O (Odin III alone), Oprime (Odin III's
front end, VTR's ABC), Pprime (the verified Parmys golden through Odin III's ABC), R (the 3E round
trip) and Oprov (O with ``--name-style provenance``). ``O'`` and ``P'`` are accepted on input
(plan point (s)).

Every VTR command runs ``$PY $V/vtr_flow/scripts/run_vtr_flow.py`` with ``cwd`` the job directory
``<J>``, its output in ``<J>/run/vtr_flow.out`` and ``PATH=$V/.venv/bin:$PATH``. The odin3 legs
first run the front end under ``/usr/bin/time -v`` into ``<J>/frontend.log``, then VTR on
``<J>/<d>.blif`` with ``-start vpr`` (O, Pprime, Oprov) or ``-start abc`` (Oprime, R).

3F's ``tools/vtr-flow`` imports ``odin3_script``; its signature is frozen (spec §3.7).
"""

from __future__ import annotations

import os
import pwd
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from qor_heavy import HeavyError

VTR_PIN = "3c9a4d23b27d187bd67c79dc01a18ff1ffc21d9b"
LEGS = ("P", "O", "Oprime", "Pprime", "R", "Oprov")
LEG_ALIASES = {"O'": "Oprime", "P'": "Pprime"}
# The repo whose lib/vtr.o3lib is $LIB (a module seam: tests point it at a temp repo copy).
REPO_ROOT = Path(__file__).resolve().parents[2]
LIB_REL = Path("lib") / "vtr.o3lib"
ABC_SCRIPT_REL = Path("lib") / "abc" / "vtr.abc"
# The golden tree (read only; spec §3.4), from the account's home, never $HOME.
GOLDEN_ROOT = Path(pwd.getpwuid(os.getuid()).pw_dir) / "odin3-ws" / "golden"
ARCH_BASES = {"k6": "k6_frac_N10_frac_chain_mem32K_40nm", "earch": "EArch"}
TIME_BIN = "/usr/bin/time"
# Spec §3 <T>: the VTR stage timeout (-timeout) per group (plan, Global Constraints caps).
STAGE_TIMEOUT_S = {"A": 7200, "B": 14400}
DEFAULT_SEED = 1
NAME_STYLES = ("short", "provenance")
# vtr_reg_qor_chain's script_params: the common parameters that enter the keys (point (f)).
QOR_CHAIN_PARAMS = ("-track_memory_usage", "-crit_path_router_iterations", "100")

_READ_VERILOG = (
    "read_verilog --nolatches {inp}; proc; opt; flatten; wreduce; hardmap; lower; "
)
_SCRIPT_BODY = {
    "O": _READ_VERILOG + "abc --lut {k}; ",
    "Oprov": _READ_VERILOG + "abc --lut {k}; ",
    "Oprime": _READ_VERILOG,
    "Pprime": "read_blif {inp}; abc --lut {k}; ",
    "R": "read_blif {inp}; ",
}
_VERILOG_PASSES = (
    "read_techlib", "read_verilog", "proc", "opt", "flatten", "wreduce", "hardmap", "lower",
)
LEG_PASSES: dict[str, tuple[str, ...]] = {
    "P": (),
    "O": (*_VERILOG_PASSES, "abc", "write_blif"),
    "Oprime": (*_VERILOG_PASSES, "write_blif"),
    "Pprime": ("read_techlib", "read_blif", "abc", "write_blif"),
    "R": ("read_techlib", "read_blif", "write_blif"),
    "Oprov": (*_VERILOG_PASSES, "abc", "write_blif"),
}
# -start for the VTR half of each odin3 leg (spec §3.2-3.6).
VTR_START = {"O": "vpr", "Oprov": "vpr", "Pprime": "vpr", "Oprime": "abc", "R": "abc"}
_PASS_LINE = re.compile(r"^  ([A-Za-z_][A-Za-z0-9_]*)(?=[ :]|$)")

# Point (x): K through VTR's own determine_lut_size, with python_libs put on sys.path by the
# snippet (``-I`` ignores PYTHONPATH). ``-B`` keeps the run from writing bytecode into VTR's
# read-only tree.
_LUT_SNIPPET = (
    "import sys\n"
    "sys.path.insert(0, {libs!r})\n"
    "from vtr.log_parse import determine_lut_size\n"
    "print(determine_lut_size(sys.argv[1]))\n"
)


@dataclass(frozen=True)
class LegContext:
    """What a leg's commands need: VTR's root and venv Python, the arch XML, the odin3 binary,
    the repo holding ``$LIB``, the job directory ``<J>``, the LUT size K, the design's group (A or
    B, which picks the stage timeout), the VPR seed and, for Pprime and R, the verified golden."""

    vtr_root: Path
    python: Path
    arch_xml: Path
    odin3: Path
    repo: Path
    job_dir: Path
    k: int
    group: str
    seed: int = DEFAULT_SEED
    golden: Path | None = None


@dataclass(frozen=True)
class Command:
    """One subprocess: ``argv`` run in ``cwd`` with stdout and stderr both to ``stdout``.
    ``env`` holds the overrides applied on top of the caller's environment (empty: unchanged)."""

    argv: tuple[str, ...]
    cwd: Path
    stdout: Path
    env: tuple[tuple[str, str], ...]


def normalise_leg(name: str) -> str:
    """The canonical leg name: ``O'`` → ``Oprime``, ``P'`` → ``Pprime`` (E-QH7 otherwise)."""
    leg = LEG_ALIASES.get(name, name)
    if leg not in LEGS:
        raise HeavyError(2, f"unknown leg '{name}' ({', '.join(LEGS)})")
    return leg


def arch_base(arch: str) -> str:
    """The arch basename for ``k6``/``earch`` or a basename itself."""
    if arch in ARCH_BASES:
        return ARCH_BASES[arch]
    if arch in ARCH_BASES.values():
        return arch
    raise HeavyError(2, f"unknown arch '{arch}' ({', '.join(ARCH_BASES)})")


def arch_xml_path(vtr_root: Path, arch: str) -> Path:
    """``<V>/vtr_flow/arch/timing/<arch base>.xml`` (spec §2)."""
    return vtr_root / "vtr_flow" / "arch" / "timing" / f"{arch_base(arch)}.xml"


def design_verilog(vtr_root: Path, design: str) -> Path:
    return vtr_root / "vtr_flow" / "benchmarks" / "verilog" / f"{design}.v"


def golden_blif(arch: str, design: str) -> Path:
    """The Parmys golden P' and R read: ``<golden>/<arch base>/vtr/<d>/<d>.parmys.blif``."""
    return GOLDEN_ROOT / arch_base(arch) / "vtr" / design / f"{design}.parmys.blif"


def stage_timeout(group: str) -> int:
    if group not in STAGE_TIMEOUT_S:
        raise ValueError(f"unknown group '{group}' (A, B)")
    return STAGE_TIMEOUT_S[group]


def key_params(seed: int = DEFAULT_SEED) -> list[str]:
    """The common parameters that enter both keys (point (f)): vtr_reg_qor_chain's, plus
    ``--seed N`` when N is not 1. No housekeeping (``-temp_dir``, ``-timeout``, ``-delete_*``)."""
    params = list(QOR_CHAIN_PARAMS)
    if seed != DEFAULT_SEED:
        params += ["--seed", str(seed)]
    return params


def common_params(job_dir: Path, group: str, seed: int = DEFAULT_SEED) -> list[str]:
    """Spec §3's common VPR parameters: ``key_params`` plus the harness's housekeeping."""
    return [
        *key_params(seed),
        "-temp_dir", str(job_dir / "run"),
        "-timeout", str(stage_timeout(group)),
        "-delete_intermediate_files", "-delete_result_files",
    ]


def _script(leg: str, design_v: Path | str, out_blif: Path | str, k: int, name_style: str,
            lib: Path | str) -> str:
    if leg == "P":
        raise ValueError("leg P runs no odin3 script")
    if leg not in _SCRIPT_BODY:
        raise ValueError(f"unknown leg '{leg}'")
    if name_style not in NAME_STYLES:
        raise ValueError(f"unknown name style '{name_style}' ({', '.join(NAME_STYLES)})")
    style = "" if name_style == "short" else f" --name-style {name_style}"
    body = _SCRIPT_BODY[leg].format(inp=design_v, k=k)
    return f"read_techlib {lib}; {body}write_blif --vtr --lut-k {k}{style} {out_blif}"


def odin3_script(leg: str, design_v: Path, out_blif: Path, k: int, name_style: str) -> str:
    """The odin3 script of an odin3 leg (spec §3.2-3.6; frozen for 3F).

    ``design_v`` is the leg's input: the ``.v`` for O, Oprime and Oprov, the verified golden BLIF
    for Pprime and R. ``$LIB`` is ``<repo>/lib/vtr.o3lib``. ``name_style`` ``short`` (the
    writer's default) adds nothing; ``provenance`` adds ``--name-style provenance``. Leg P runs
    no odin3: ValueError.
    """
    return _script(leg, design_v, out_blif, k, name_style, REPO_ROOT / LIB_REL)


def key_script(leg: str, k: int, name_style: str) -> str:
    """The script as it enters the keys: the leg's text with ``$LIB``, ``$IN`` and ``$OUT`` in
    place of the paths, so the leg key is shared by every design and job directory (point (f));
    the files' contents enter the key through their own SHA-256s."""
    return _script(leg, "$IN", "$OUT", k, name_style, "$LIB")


def leg_name_style(leg: str) -> str:
    return "provenance" if leg == "Oprov" else "short"


def _vtr_command(ctx: LegContext, circuit: Path, start: str | None) -> Command:
    scripts = ctx.vtr_root / "vtr_flow" / "scripts"
    argv = [str(ctx.python), str(scripts / "run_vtr_flow.py"), str(circuit), str(ctx.arch_xml)]
    if start is not None:
        argv += ["-start", start]
    argv += common_params(ctx.job_dir, ctx.group, ctx.seed)
    path = f"{ctx.vtr_root / '.venv' / 'bin'}:{os.environ.get('PATH', os.defpath)}"
    return Command(tuple(argv), ctx.job_dir, ctx.job_dir / "run" / "vtr_flow.out",
                   (("PATH", path),))


def _frontend_command(leg: str, design: str, ctx: LegContext) -> Command:
    if leg in ("Pprime", "R"):
        if ctx.golden is None:
            raise ValueError(f"leg {leg} needs the verified Parmys golden")
        source = ctx.golden
    else:
        source = design_verilog(ctx.vtr_root, design)
    script = _script(leg, source, ctx.job_dir / f"{design}.blif", ctx.k, leg_name_style(leg),
                     ctx.repo / LIB_REL)
    argv = (TIME_BIN, "-v", "-o", str(ctx.job_dir / "frontend.time"), str(ctx.odin3),
            "--diag-paths", "resolved", "-p", script)
    return Command(argv, ctx.job_dir, ctx.job_dir / "frontend.log", ())


def build_commands(leg: str, design: str, ctx: LegContext) -> list[Command]:
    """Every command of one job, in order (spec §3)."""
    leg = normalise_leg(leg)
    if leg == "P":
        return [_vtr_command(ctx, design_verilog(ctx.vtr_root, design), None)]
    return [
        _frontend_command(leg, design, ctx),
        _vtr_command(ctx, ctx.job_dir / f"{design}.blif", VTR_START[leg]),
    ]


def registered_passes(odin3_help: str) -> frozenset[str]:
    """The passes ``odin3 --help`` lists (point (t)): lines ``  <name>`` followed by a blank, a
    colon or the end; option lines (``  --…``) never match."""
    names = set()
    for line in odin3_help.splitlines():
        match = _PASS_LINE.match(line)
        if match:
            names.add(match.group(1))
    return frozenset(names)


def leg_available(leg: str, passes: frozenset[str]) -> str | None:
    """The first pass of the leg's script (in ``LEG_PASSES`` order) that ``passes`` lacks."""
    for name in LEG_PASSES[normalise_leg(leg)]:
        if name not in passes:
            return name
    return None


def lut_size(vtr_root: Path, python: Path, arch_xml: Path) -> int:
    """K = VTR's ``determine_lut_size(arch)`` (6 for both arches), read through VTR's own Python
    with ``-I`` (point (x)). Failure: HeavyError(2)."""
    libs = vtr_root / "vtr_flow" / "scripts" / "python_libs"
    argv = [str(python), "-I", "-B", "-c", _LUT_SNIPPET.format(libs=str(libs)), str(arch_xml)]
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, check=False)
    except OSError as exc:
        raise HeavyError(2, f"cannot run {python}: {exc.strerror}") from exc
    text = proc.stdout.strip()
    if proc.returncode != 0 or not text.isdigit() or int(text) <= 0:
        detail = (proc.stderr.strip().splitlines() or [f"exit {proc.returncode}"])[-1]
        raise HeavyError(2, f"cannot read the LUT size of {arch_xml}: {detail}")
    return int(text)


def vtr_commit(vtr_root: Path) -> str:
    """``git -C <V> rev-parse HEAD``; E-QH1 when it is not a VTR checkout."""
    argv = ["git", "-C", str(vtr_root), "rev-parse", "HEAD"]
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, check=False)
    except OSError:
        proc = None
    sha = proc.stdout.strip() if proc is not None and proc.returncode == 0 else ""
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise HeavyError(2, f"VTR not found at {vtr_root} (set VTR_ROOT or --vtr-root)")
    return sha
