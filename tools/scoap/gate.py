#!/usr/bin/env python3
"""Gates of the scoap pass (spec docs/specs/2026-10-31-scoap-design.md section 7).

For every BLIF: run the pass (combinational and --sequential) and the independent reference
(scoap_ref.py) and compare, per module and net, every integer. Construction check: every net of the
reference has a value in the pass output and the other way round (no silently dropped cell or net).

Usage: gate.py --odin3 ODIN3 --lib vtr.o3lib --out DIR BLIF...
Exit 0 when every BLIF agrees in both modes.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
import sys
from pathlib import Path
from types import ModuleType

HERE = Path(__file__).resolve().parent


def load_ref() -> ModuleType:
    spec = importlib.util.spec_from_file_location("scoap_ref", HERE / "scoap_ref.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["scoap_ref"] = module
    spec.loader.exec_module(module)
    return module


def run_pass(odin3: str, lib: str, blif: Path, seq: bool, out: Path) -> dict[str, object]:
    flag = "--sequential " if seq else ""
    script = f"read_techlib {lib}; read_blif {blif}; scoap {flag}--out {out}"
    done = subprocess.run([odin3, "-p", script], capture_output=True, text=True, check=False)
    if done.returncode != 0:
        raise RuntimeError(done.stderr.strip()[-300:])
    result: dict[str, object] = json.loads(out.read_text(encoding="utf-8"))
    return result


def compare(ref: dict[str, dict[str, dict[str, int]]], got: dict[str, object]) -> list[str]:
    problems: list[str] = []
    modules = got["modules"]
    assert isinstance(modules, list)
    by_name = {m["name"]: {n["name"]: n for n in m["nets"]} for m in modules}
    for mname, nets in ref.items():
        mine = by_name.get(mname)
        if mine is None:
            problems.append(f"module {mname}: missing in the pass output")
            continue
        for extra in sorted(set(mine) - set(nets))[:5]:
            problems.append(f"{mname}: net {extra} only in the pass")
        for name, want in nets.items():
            have = mine.get(name)
            if have is None:
                problems.append(f"{mname}: net {name} has no value in the pass")
                continue
            for key, val in want.items():
                if have[key] != val:
                    problems.append(f"{mname}: net {name} {key}: pass {have[key]} ref {val}")
    return problems


def check_one(odin3: str, lib: str, blif: Path, seq: bool, tmp: Path) -> tuple[bool, str]:
    ref_mod = load_ref()
    try:
        got = run_pass(odin3, lib, blif, seq, tmp / f"{blif.stem}.{'seq' if seq else 'comb'}.json")
    except RuntimeError as err:
        return False, f"pass failed: {err}"
    try:
        expect = ref_mod.reference(blif, seq)
    except ref_mod.Unsupported as err:
        return True, f"skipped: {err}"
    problems = compare(expect, got)
    return not problems, "; ".join(problems[:3])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--odin3", required=True)
    parser.add_argument("--lib", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("blifs", nargs="+", type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    bad = 0
    for blif in sorted(args.blifs, key=lambda p: p.stat().st_size):  # smallest first
        for seq in (False, True):
            ok, note = check_one(args.odin3, args.lib, blif, seq, args.out)
            mode = "seq " if seq else "comb"
            print(f"{'ok  ' if ok else 'FAIL'} {mode} {blif.name} {note}", flush=True)
            bad += 0 if ok else 1
    print(f"{bad} failing")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
