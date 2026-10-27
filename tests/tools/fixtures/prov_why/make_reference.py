"""Writes `r4census_reference.json`: the pair classes that Norns' own `scripts/r4census.py` gives
on this fixture (V1.2.0 Task 10 comparison). The Norns checkout is read only; run with `-B` so no
bytecode is written there.

    python3 -B make_reference.py PATH/TO/norns/scripts

The word labels are read from the fixture's `words/members.json` (the real Norns file format,
`build_words.py`), and the output ports from the fixture's `arch.xml` by Norns' own
`arch_output_ports`.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main(scripts: str) -> None:
    sys.path.insert(0, scripts)
    import nornlib as nl  # type: ignore[import-not-found]
    import r3lib  # type: ignore[import-not-found]
    import r4census as rc  # type: ignore[import-not-found]

    def runs_dir(_design: str, *parts: str) -> Path:
        if parts and parts[0] == "words":
            return HERE / "words" / "members.json"
        return HERE / "flow" / "odin3" / "why.blif"

    real_ports = nl.arch_output_ports
    nl.runs_dir = runs_dir
    nl.arch_output_ports = lambda _arch: real_ports(HERE / "arch.xml")
    net = rc.Netlist("why")
    cen = rc.Census(HERE / "census.tsv")
    atoms_of_clb, cls = r3lib.clb_atoms_of(HERE / "norns" / "final.flat_place")
    groups = {c: sorted(a for a in atoms_of_clb[c] if a in net.kind)
              for c, k in cls.items() if k == "logic"}
    out: dict[str, object] = {"clbs": {}}
    for clb, atoms in sorted(groups.items()):
        stats = rc.pair_stats(net, cen.pin_crit, {clb: atoms})
        out["clbs"][clb] = {"members": atoms, "pairs": stats["pairs"], "counts": stats["counts"]}  # type: ignore[index]
    total = rc.pair_stats(net, cen.pin_crit, groups)
    out["total"] = {"pairs": total["pairs"], "counts": total["counts"]}
    (HERE / "r4census_reference.json").write_text(json.dumps(out, indent=1) + "\n")


if __name__ == "__main__":
    main(sys.argv[1])
