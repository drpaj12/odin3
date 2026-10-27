"""Writes `r4census_reference.json`: the pair classes that Norns' own `scripts/r4census.py` gives
on this fixture (V1.2.0 Task 10 comparison). The Norns checkout is read only; run with `-B` so no
bytecode is written there.

    python3 -B make_reference.py PATH/TO/norns/scripts

The word labels are the ones the fixture's name map gives (n1 and n6 share a declarator): the
Norns round-2 `members.json` is replaced by that table, as the spec (3.6) says the labelling
differs. The architecture's output ports are the adder's.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORDS = {"n1": "w", "n6": "w"}


def main(scripts: str) -> None:
    sys.path.insert(0, scripts)
    import nornlib as nl  # type: ignore[import-not-found]
    import r3lib  # type: ignore[import-not-found]
    import r4census as rc  # type: ignore[import-not-found]

    def runs_dir(_design: str, *parts: str) -> Path:
        return HERE / "flow" / "odin3" / "why.blif"

    nl.runs_dir = runs_dir
    nl.arch_output_ports = lambda _arch: {"adder": {"cout", "sumout"}}
    rc.Netlist._words = staticmethod(lambda _design: dict(WORDS))
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
