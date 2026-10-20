"""fsm-probe: what does Yosys 0.55's ``fsm`` find in a design? (4.8 spec section 12, T-1)

Usage::

    fsm-probe [--vtr] [--top T] [--yosys PATH] [--json] [--log FILE] FILE.v...

Runs the spec section 12 script on the files and prints one row per FSM that reached
``fsm_info`` (module, wire, states, state bits before and after recoding, encoding, reset state
index, control inputs and outputs, transitions), then the registers marked or rejected and the
extraction failures.  ``--vtr`` adds the ``setattr -mod -set keep_hierarchy 1`` lines of the
VTR designs for the RAM models.  ``--json`` prints the rows for ``fsm-diff``.  Yosys defaults
to ``$VTR_ROOT/build/bin/yosys`` (0.55, the oracle); the tool never writes under VTR_ROOT.

Exit codes: 0 done; 2 usage or tool error; 3 Yosys crashed or timed out.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import asdict
from pathlib import Path

import yosys_log

EXIT_OK = 0
EXIT_ERROR = 2
EXIT_HARD = 3

TIMEOUT_SECONDS = 600
ADDRESS_LIMIT_KIB = 8 * 1024 * 1024
RAM_MODELS = ("single_port_ram", "dual_port_ram")
PRE_PROC = "opt_expr; opt_clean; opt -nodffe -nosdff; proc -norom"


class ProbeError(Exception):
    """A usage or tool error (exit 2)."""


def probe_script(files: Sequence[str], vtr: bool, top: str | None = None) -> str:
    """The Yosys script of spec section 12, one statement per ``;``."""
    steps = ["read_verilog -nomem2reg +/parmys/vtr_primitives.v"]
    if vtr:
        steps += [f"setattr -mod -set keep_hierarchy 1 {ram}" for ram in RAM_MODELS]
    steps += [f"read_verilog -sv -nolatches {f}" for f in files]
    steps.append(f"hierarchy -check -top {top} -purge_lib" if top else
                 "hierarchy -check -auto-top -purge_lib")
    steps += [PRE_PROC, "fsm"]
    return "; ".join(steps)


def vtr_root() -> Path:
    default = Path.home() / "odin3-ws/external/vtr-verilog-to-routing"
    return Path(os.environ.get("VTR_ROOT", str(default)))


def default_yosys() -> str:
    return str(vtr_root() / "build/bin/yosys")


def run_yosys(yosys: str, script: str, log: Path) -> str:
    """Run ``script`` and return the log text; raises ProbeError / subprocess errors."""
    if shutil.which(yosys) is None and not Path(yosys).is_file():
        raise ProbeError(f"yosys not found at {yosys}")
    cmd = [yosys, "-q", "-l", str(log), "-p", script]
    prefix = ["prlimit", f"--as={ADDRESS_LIMIT_KIB * 1024}"] if shutil.which("prlimit") else []
    proc = subprocess.run(prefix + cmd, capture_output=True, text=True,
                          timeout=TIMEOUT_SECONDS, check=False)
    if proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, cmd, proc.stdout, proc.stderr)
    return log.read_text(errors="replace")


def rows_of(design: str, facts: dict[str, yosys_log.ModuleFacts]) -> list[dict[str, object]]:
    """One row per FSM, in module then log order."""
    rows: list[dict[str, object]] = []
    for module in sorted(facts):
        for fsm in facts[module].fsms:
            row: dict[str, object] = {"design": design}
            row.update(asdict(fsm))
            del row["codes"], row["table"]
            rows.append(row)
    return rows


def rows_json(rows: list[dict[str, object]]) -> str:
    return json.dumps(rows, indent=1, sort_keys=True)


def format_table(rows: list[dict[str, object]]) -> str:
    cols = ("module", "wire", "states", "bits_before", "bits_after", "encoding", "reset_index",
            "inputs", "outputs", "transitions")
    table = [list(cols)] + [[str(r[c]) for c in cols] for r in rows]
    widths = [max(len(row[k]) for row in table) for k in range(len(cols))]
    return "\n".join("  ".join(cell.ljust(w) for cell, w in zip(row, widths, strict=True)).rstrip()
                     for row in table)


def format_notes(facts: dict[str, yosys_log.ModuleFacts]) -> str:
    out: list[str] = []
    for module in sorted(facts):
        f = facts[module]
        out += [f"marked {module}.{w}" for w in f.marked]
        out += [f"not marked {module}.{w}: {'; '.join(r)}" for w, r in sorted(f.rejected.items())]
        out += [f"failed {module}.{w}: {r}" for w, r in f.failures]
    return "\n".join(out)


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="fsm-probe", description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("--vtr", action="store_true", help="add the RAM-model setattr lines")
    ap.add_argument("--top", help="top module (default: -auto-top)")
    ap.add_argument("--yosys", default=default_yosys())
    ap.add_argument("--json", action="store_true", help="print the rows as JSON")
    ap.add_argument("--log", type=Path, help="keep the Yosys log here")
    args = ap.parse_args(argv)
    with tempfile.TemporaryDirectory(prefix="fsm-probe-") as tmp:
        log = args.log or Path(tmp) / "yosys.log"
        try:
            text = run_yosys(args.yosys, probe_script(args.files, args.vtr, args.top), log)
        except ProbeError as exc:
            print(f"fsm-probe: {exc}", file=sys.stderr)
            return EXIT_ERROR
        except (subprocess.SubprocessError, OSError) as exc:
            print(f"fsm-probe: yosys failed: {exc}", file=sys.stderr)
            return EXIT_HARD
    facts = yosys_log.parse_log(text)
    rows = rows_of(Path(args.files[0]).stem, facts)
    if args.json:
        print(rows_json(rows))
    else:
        print(format_table(rows))
        notes = format_notes(facts)
        if notes:
            print(notes)
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
