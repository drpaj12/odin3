"""submod_equiv: `submod` then `flatten` against the input, one BLIF micro at a time.

Usage: submod_equiv.py ODIN3 EQUIV_CHECK OUT_DIR BLIF...

Per design (Phase 7 spec 3.1 S-6): read the BLIF, write it back as the reference and as JSON
(`write_json --odin3`); in the JSON mark every second logic cell (type starting with `$`) of every
module (by position, which is ID order) with attribute `submod` = `g_<module>`; black boxes stay
outside the group because equiv-check pairs them by instance name and flatten renames what it
copies; read the JSON back, run `submod` and `flatten`,
write BLIF and compare with the reference by equiv-check.  A second mode ("alt") puts the logic
cells alternately (by position) into two groups per module, so nets between the two groups cross a
boundary on both sides, and runs `submod` twice: the second run must move no group.  A micro
without a cell, or a module that ends up with fewer than the groups, still has to come out
equivalent.  Exit 0 when every design is equivalent, 1 otherwise, 2 on a usage error.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

BLACKBOXES = "adder,multiply,single_port_ram,dual_port_ram"
TECHLIB = Path(__file__).resolve().parents[2] / "lib" / "vtr.o3lib"
ALLOW_UNDRIVEN = ("hand_ports", "hand_writers")
# Designs submod must refuse (located error, text pinned): dffsre has a net with an inout pin.
EXPECT_REFUSED = {"dffsre.parmys.blif": "through an inout pin"}


def odin3(binary: str, script: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([binary, "-p", script], capture_output=True, text=True, check=False)


def mark(path: Path, out: Path, alt: bool) -> tuple[str, int]:
    """Writes `out` with every second cell marked (mode alt: all cells, two groups alternating);
    returns the first module's name and the number of marked cells."""
    design = json.loads(path.read_text(encoding="utf-8"))
    marked = 0
    for name, module in design["modules"].items():
        logic = [c for c in module.get("cells", {}).values() if c["type"].startswith("$")]
        for pos, cell in enumerate(logic if alt else logic[::2]):
            group = f"g_{name}_{pos % 2}" if alt else f"g_{name}"
            cell.setdefault("attributes", {})["submod"] = group
            marked += 1
    out.write_text(json.dumps(design), encoding="utf-8")
    return next(iter(design["modules"])), marked


def check(binary: str, equiv: str, blif: Path, out_dir: Path, alt: bool) -> str:
    """'' when the design passes, else the reason."""
    work = out_dir / (blif.stem + (".alt" if alt else ""))
    work.mkdir(parents=True, exist_ok=True)
    ref, plain, marked, result = (work / n for n in ("ref.blif", "a.json", "b.json", "out.blif"))
    first = odin3(binary, f"read_techlib {TECHLIB}; read_blif {blif}; write_blif {ref}; "
                          f"write_json --odin3 {plain}")
    if first.returncode != 0:
        return "odin3 could not read or write the design: " + first.stderr.strip()[-200:]
    top, count = mark(plain, marked, alt)
    second = odin3(binary, f"read_techlib {TECHLIB}; read_json --top {top} {marked}; submod; "
                           f"{'submod; ' if alt else ''}flatten; write_blif {result}")
    refused = EXPECT_REFUSED.get(blif.name)
    if refused is not None:
        text = second.stderr + second.stdout
        ok = second.returncode != 0 and refused in text and "submod: net" in text
        return "" if ok else f"expected a submod refusal ({refused}): " + text.strip()[-200:]
    if second.returncode != 0 or not result.is_file():
        return "odin3 submod/flatten failed: " + second.stderr.strip()[-300:]
    if count > 0 and "groups moved" not in second.stderr + second.stdout:
        return "submod printed no summary line"
    if alt and count > 0 and "submod: 0 groups moved" not in second.stderr + second.stdout:
        return "the second submod run still found a group"
    argv = [equiv, "--blackbox", BLACKBOXES, str(ref), str(result)]
    argv[1:1] = ["--allow-undriven"] if blif.stem.split(".")[0] in ALLOW_UNDRIVEN else []
    verdict = subprocess.run(argv, capture_output=True, text=True, check=False)
    if verdict.returncode != 0:
        return f"equiv-check exit {verdict.returncode}: " + verdict.stdout.strip()[-200:]
    return ""


def main(argv: list[str]) -> int:
    if len(argv) < 5:
        print(__doc__, file=sys.stderr)
        return 2
    binary, equiv, out_dir = argv[1], argv[2], Path(argv[3])
    designs = sorted((Path(a) for a in argv[4:]), key=lambda p: (p.stat().st_size, str(p)))
    failed = 0
    for blif in designs:
        for alt in (False, True):
            reason = check(binary, equiv, blif, out_dir, alt)
            failed += 1 if reason else 0
            verdict = "refused as expected" if blif.name in EXPECT_REFUSED else "equivalent"
            mode = "alt" if alt else "half"
            print(f"{blif.name} [{mode}] {verdict if not reason else 'FAIL: ' + reason}",
                  flush=True)
    print(f"submod_equiv: {len(designs)} designs x 2 modes, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
