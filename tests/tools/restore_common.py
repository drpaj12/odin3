"""Shared helpers of the V1.2.0 Task 9 restore tests: the micro set, the VTR flow script and the
CLI runner. The CLI links its own sanitizer runtime, so LD_PRELOAD and LD_AUDIT (which CTest
python_names sets for a sanitized libodin3) are stripped from its environment (CLI_ENV, as in
test_names_v2.py). Work directories are made under TMPDIR and left in place."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

from .helpers import REPO_ROOT

ODIN3 = os.environ.get("ODIN3_CLI")
CLI_ENV = {k: v for k, v in os.environ.items() if k not in ("LD_PRELOAD", "LD_AUDIT")}
MICRO_DIR = REPO_ROOT / "tests" / "micro" / "verilog" / "micro"
DATA = Path(__file__).resolve().parent / "data" / "restore"
UPDATE = os.environ.get("ODIN3_UPDATE_GOLDEN") == "1"
LIB = REPO_ROOT / "lib" / "vtr.o3lib"
FLOW = (
    f"read_techlib {LIB}; read_verilog --nolatches --vtr-compat {{ast}} {{src}}; proc; opt; "
    "flatten; opt -full; wreduce; hardmap; lower; "
)
# pair -> (writer command with {map} {out}, reader command with {map} {out_in} {out}).
WRITE = {
    "blif": "write_blif --vtr {names} {out}",
    "json": "write_json {names} {out}",
    "v": "write_verilog {names} {out}",
}
READ = {
    "blif": "read_blif {rmap} {src}; ",
    "json": "read_json {rmap} {src}; ",
    "v": "read_verilog -netlist {rmap} {src}; proc; ",
}
PAIRS = ("blif", "json", "v")


def micros() -> list[str]:
    """The names of the micro designs, in byte order (the smallest first is not needed here)."""
    return sorted(p.stem for p in MICRO_DIR.glob("*.v"))


def workdir(tag: str) -> Path:
    return Path(tempfile.mkdtemp(prefix=f"restore-{tag}-"))


def run_cli(script: str) -> subprocess.CompletedProcess[str]:
    """One `odin3 -p script` from the repository root."""
    assert ODIN3 is not None, "ODIN3_CLI is not set"
    return subprocess.run(
        [ODIN3, "-p", script],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
        env=CLI_ENV,
    )


def flow(micro: str, ast: str = "") -> str:
    return FLOW.format(ast=ast, src=MICRO_DIR / f"{micro}.v")


def ok(script: str) -> bool:
    return run_cli(script).returncode == 0


def rmap_arg(pair: str, names: Path) -> str:
    """The reader's name-map option for a pair."""
    return f"-name-map {names}" if pair == "v" else f"--name-map {names}"


def step0_passing() -> dict[str, set[str]]:
    """micro -> the pairs whose idempotence step 0 proved (tests/tools/data/restore/step0.txt)."""
    rows: dict[str, set[str]] = {}
    for line in (DATA / "step0.txt").read_text().splitlines():
        name, *cells = line.split()
        rows[name] = {c.split("=")[0] for c in cells if c.endswith("=pass")}
    return rows


# mode -> (read option, writer option): `constructs` reads with `full` and writes the smaller map.
MODES = {
    "refs": ("--prov-ast refs", "--prov-map refs"),
    "full": ("--prov-ast full", "--prov-map full"),
    "constructs": ("--prov-ast full", "--prov-map constructs"),
    "none": ("", "--prov-map"),
}


def write_script(micro: str, mode: str, outs: dict[str, Path]) -> str:
    """The VTR flow to `lower` and the three writers with the hash names and a version 2 map."""
    read, prov = MODES[mode]
    script = flow(micro, read)
    return script + "; ".join(
        WRITE[p].format(names=f"--name-style hash {prov}", out=outs[p]) for p in PAIRS
    )


def rewrite_script(pair: str, names: Path, src: Path, out: Path, mode: str) -> str:
    """Reads src with its map and writes it again with the same map mode."""
    prov = MODES[mode][1]
    return (
        f"read_techlib {LIB}; "
        + READ[pair].format(rmap=rmap_arg(pair, names), src=src)
        + WRITE[pair].format(names=f"--name-style hash {prov}", out=out)
    )
