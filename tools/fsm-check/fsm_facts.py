"""fsm-facts: regenerate ``tests/fsm/yosys_facts.json`` from ``tests/fsm/facts_corpus.txt``.

Usage::

    fsm-facts [--corpus FILE] [--out FILE] [--yosys PATH] [--only NAME...] [--check]

The corpus has one design per line: ``NAME KIND PATH [TOP]``.  KIND is ``vtr`` (PATH is a file
name under ``$VTR_ROOT/vtr_flow/benchmarks/verilog``; the RAM-model setattr lines are added) or
``micro`` (PATH is relative to the repository root).  The JSON maps NAME to the facts Yosys 0.55
reports for it (``fsm_probe``'s rows plus the marked and rejected registers and the extraction
failures).  With ``--check`` nothing is written; the exit code is 1 when the file differs (the first
differing keys are printed).

Yosys names cells and wires after the source position (``$memwr$\\m$/abs/path/x.v:40$38_EN``), so
every absolute path is normalised before it is written or compared: the repository root is
stripped (the path becomes repo-relative) and the VTR root becomes ``$VTR_ROOT``.  The file is
therefore the same from any checkout.

``--marks FILE`` writes the detection facts of the micros (no Yosys run: derived from the JSON)
in the form the C unit test reads: ``PATH MARKED REJECTED`` per micro, MARKED the registers
fsm_detect marked as ``module.wire`` joined by commas, REJECTED ``module.wire:reason+reason``
(reasons port, users, init, selfreset) joined by commas, ``-`` for none. RAM write-enable
registers (``$memwr``) are left out: Odin III's proc makes none (the ``native-memwr`` lines of
diff_expect.txt name each micro and the count).

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

DIFF_LIMIT = 10
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


def _normalise_text(text: str, repo: Path, vtr: Path) -> str:
    return text.replace(f"{vtr}/", "$VTR_ROOT/").replace(f"{repo}/", "")


def normalise(value: object, repo: Path = REPO_ROOT, vtr: Path | None = None) -> object:
    """``value`` (nested JSON) with every absolute path in a string or key made portable."""
    root = fsm_probe.vtr_root() if vtr is None else vtr
    if isinstance(value, str):
        return _normalise_text(value, repo, root)
    if isinstance(value, list):
        return [normalise(item, repo, root) for item in value]
    if isinstance(value, dict):
        return {_normalise_text(str(k), repo, root): normalise(v, repo, root)
                for k, v in value.items()}
    return value


def describe_diff(old: object, new: object, limit: int = DIFF_LIMIT,
                  where: str = "") -> list[str]:
    """Up to ``limit`` lines ``key: ...`` naming where two JSON values differ."""
    out: list[str] = []
    if isinstance(old, dict) and isinstance(new, dict):
        for key in sorted(set(old) | set(new)):
            here = f"{where}.{key}" if where else str(key)
            if key not in new:
                out.append(f"{here}: only in file")
            elif key not in old:
                out.append(f"{here}: only in fresh output")
            else:
                out.extend(describe_diff(old[key], new[key], limit - len(out), here))
            if len(out) >= limit:
                break
    elif isinstance(old, list) and isinstance(new, list):
        for i in range(max(len(old), len(new))):
            here = f"{where}[{i}]"
            if i >= len(new):
                out.append(f"{here}: only in file")
            elif i >= len(old):
                out.append(f"{here}: only in fresh output")
            else:
                out.extend(describe_diff(old[i], new[i], limit - len(out), here))
            if len(out) >= limit:
                break
    elif old != new:
        out.append(f"{where}: file {old!r} != fresh {new!r}")
    return out[:limit]


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


def _strings(value: object) -> list[str]:
    """A JSON list of strings, checked."""
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise ValueError(f"expected a list of strings, got {value!r}")
    return [str(item) for item in value]


def _reasons(value: object) -> dict[str, list[str]]:
    """The JSON object register -> list of rejection reasons, checked."""
    if not isinstance(value, dict):
        raise ValueError(f"expected an object, got {value!r}")
    return {str(reg): _strings(reasons) for reg, reasons in value.items()}


def marks_text(facts: dict[str, dict[str, object]],
               corpus: Sequence[tuple[str, str, str, str | None]]) -> str:
    """The detection facts of the micros of the corpus, one line each."""
    lines = ["# PATH MARKED REJECTED (tools/fsm-check/fsm-facts --marks; do not edit)"]
    for name, kind, rel, _top in corpus:
        if kind != "micro":
            continue
        entry = facts[name]
        marked = [m for m in _strings(entry["marked"]) if "$memwr" not in m]
        rejected = []
        for reg, reasons in sorted(_reasons(entry["rejected"]).items()):
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
            new[name] = normalise(facts_of(args.yosys, kind, rel, top))
    except (fsm_probe.ProbeError, OSError, ValueError) as exc:
        print(f"fsm-facts: {exc}", file=sys.stderr)
        return EXIT_ERROR
    except subprocess.SubprocessError as exc:
        print(f"fsm-facts: yosys failed: {exc}", file=sys.stderr)
        return EXIT_HARD
    text = json.dumps(new, indent=1, sort_keys=True) + "\n"
    if args.check:
        if not args.out.exists():
            print(f"fsm-facts: {args.out} does not exist", file=sys.stderr)
            return EXIT_DIFFERS
        if args.out.read_text() == text:
            return EXIT_OK
        lines = describe_diff(json.loads(args.out.read_text()), new) or ["formatting only"]
        for line in lines:
            print(f"fsm-facts: differs: {line}", file=sys.stderr)
        return EXIT_DIFFERS
    args.out.write_text(text)
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
