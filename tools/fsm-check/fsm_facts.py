"""fsm-facts: regenerate ``tests/fsm/yosys_facts.json`` from ``tests/fsm/facts_corpus.txt``.

Usage::

    fsm-facts [--corpus FILE] [--out FILE] [--yosys PATH] [--only NAME...] [--check]

The corpus has one design per line: ``NAME KIND PATH [TOP]``.  KIND is ``vtr`` (PATH is a file
name under ``$VTR_ROOT/vtr_flow/benchmarks/verilog``; the RAM-model setattr lines are added) or
``micro`` (PATH is relative to the repository root).  The JSON maps NAME to the facts Yosys 0.55
reports for it (``fsm_probe``'s rows plus the marked and rejected registers and the extraction
failures).  With ``--check`` nothing is written; the exit code is 1 when the file differs.

``--marks FILE`` writes the detection facts of the micros (no Yosys run: derived from the JSON)
in the form the C unit test reads: ``PATH MARKED REJECTED`` per micro, MARKED the registers
fsm_detect marked as ``module.wire`` joined by commas, REJECTED ``module.wire:reason+reason``
(reasons port, users, init, selfreset) joined by commas, ``-`` for none. RAM write-enable
registers (``$memwr``) are left out: Odin III's pipeline needs the VTR models to build them.

Exit codes: 0 done (or equal with --check); 1 differs (--check); 2 usage or tool error; 3 Yosys
crashed or timed out on a design.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import asdict
from pathlib import Path

import fsm_probe
import yosys_log

EXIT_OK = 0
EXIT_DIFFERS = 1
EXIT_ERROR = 2
EXIT_HARD = 3

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CORPUS = REPO_ROOT / "tests/fsm/facts_corpus.txt"
DEFAULT_OUT = REPO_ROOT / "tests/fsm/yosys_facts.json"


def read_corpus(path: Path) -> list[tuple[str, str, str, str | None]]:
    rows: list[tuple[str, str, str, str | None]] = []
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) not in (3, 4) or parts[1] not in ("vtr", "micro"):
            raise fsm_probe.ProbeError(f"{path}: bad corpus line: {raw!r}")
        rows.append((parts[0], parts[1], parts[2], parts[3] if len(parts) == 4 else None))
    return rows


def design_path(kind: str, rel: str) -> str:
    if kind == "vtr":
        return str(fsm_probe.vtr_root() / "vtr_flow/benchmarks/verilog" / rel)
    return str(REPO_ROOT / rel)


def _compact(fsm: yosys_log.Fsm) -> dict[str, object]:
    """The fields the facts file keeps (not the codes and the table, which are long)."""
    row = asdict(fsm)
    del row["codes"], row["table"]
    return row


def facts_of(yosys: str, kind: str, rel: str, top: str | None) -> dict[str, object]:
    with tempfile.TemporaryDirectory(prefix="fsm-facts-") as tmp:
        script = fsm_probe.probe_script([design_path(kind, rel)], kind == "vtr", top)
        text = fsm_probe.run_yosys(yosys, script, Path(tmp) / "yosys.log")
    mods = yosys_log.parse_log(text)
    return {
        "fsms": [_compact(f) for m in sorted(mods) for f in mods[m].fsms],
        "marked": [f"{m}.{w}" for m in sorted(mods) for w in mods[m].marked],
        "rejected": {f"{m}.{w}": r for m in sorted(mods) for w, r in mods[m].rejected.items()},
        "failures": [f"{m}.{w}: {r}" for m in sorted(mods) for w, r in mods[m].failures],
    }


REASON_CODES = {
    "Register is connected to module port": "port",
    "Users of register don't seem to benefit from recoding": "users",
    "Register has an initialization value": "init",
    "Circuit seems to be self-resetting": "selfreset",
}


def marks_text(facts: dict[str, dict[str, object]],
               corpus: Sequence[tuple[str, str, str, str | None]]) -> str:
    """The detection facts of the micros of the corpus, one line each."""
    lines = ["# PATH MARKED REJECTED (tools/fsm-check/fsm-facts --marks; do not edit)"]
    for name, kind, rel, _top in corpus:
        if kind != "micro":
            continue
        entry = facts[name]
        marked = [m for m in entry["marked"] if "$memwr" not in m]  # type: ignore[attr-defined]
        rejected = []
        for reg, reasons in sorted(entry["rejected"].items()):  # type: ignore[attr-defined]
            codes = "+".join(REASON_CODES[r] for r in reasons)
            rejected.append(f"{reg}:{codes}")
        lines.append(f"{rel} {','.join(marked) or '-'} {','.join(rejected) or '-'}")
    return "\n".join(lines) + "\n"


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="fsm-facts", description=__doc__.split("\n")[0])
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--yosys", default=fsm_probe.default_yosys())
    ap.add_argument("--only", nargs="*", default=[], help="regenerate only these names")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--marks", type=Path, help="write the detection marks of the micros here")
    args = ap.parse_args(argv)
    try:
        corpus = read_corpus(args.corpus)
        if args.marks is not None:
            args.marks.write_text(marks_text(json.loads(args.out.read_text()), corpus))
            return EXIT_OK
        old: dict[str, object] = json.loads(args.out.read_text()) if args.out.exists() else {}
        new: dict[str, object] = dict(old) if args.only else {}
        for name, kind, rel, top in corpus:
            if args.only and name not in args.only:
                continue
            new[name] = facts_of(args.yosys, kind, rel, top)
    except (fsm_probe.ProbeError, OSError, ValueError) as exc:
        print(f"fsm-facts: {exc}", file=sys.stderr)
        return EXIT_ERROR
    except subprocess.SubprocessError as exc:
        print(f"fsm-facts: yosys failed: {exc}", file=sys.stderr)
        return EXIT_HARD
    text = json.dumps(new, indent=1, sort_keys=True) + "\n"
    if args.check:
        return EXIT_OK if args.out.exists() and args.out.read_text() == text else EXIT_DIFFERS
    args.out.write_text(text)
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
