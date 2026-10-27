"""Writes the `why` fixture (V1.2.0 Task 10) next to this script: a hand-made flow directory
`flow/` (BLIF, version 2 name map, VPR-shaped `.net` and `.place`, attribution) and the Norns
census `census.tsv` with the flat placement `norns/final.flat_place` that `r4census.py` reads.

Run once; the output is committed. The design has seven logic atoms in one CLB (n1..n6 LUTs, q1
a FF), one LUT in a second CLB (o1) and an I/O cluster, chosen so the CLB holds a pair of every
class (precedence: n1 and n2 are both a direct local net and a shared input, which is SC):

    n1 = f(a, b)   n2 = f(n1, a)   n3 = f(n2, d)   q1 = FF(n3)   n4 = f(e, f)   n5 = f(e, g)
    n6 = f(h, i)   o1 = f(n4, x)

Pin criticality (census P lines): n2 reads n1 at 0.9 (SC), n3 reads n2 at 0.2 (SN), q1 reads n3 at
0.7 (SC); n4 and n5 share e (SI); n1 and n6 have the same word label (OR); the rest is FL.
Usage: python3 gen_fixture.py
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
FLOW = HERE / "flow"
LUTS = {"n1": ("a", "b"), "n2": ("n1", "a"), "n3": ("n2", "d"), "n4": ("e", "f"),
        "n5": ("e", "g"), "n6": ("h", "i"), "o1": ("n4", "x")}
INPUTS = ["a", "b", "c", "d", "e", "f", "g", "h", "i", "x", "clk"]
OUTPUTS = ["o1", "q1", "n5", "n6"]  # the atoms nothing reads: primary outputs, so VPR sweeps none
PADS = ["out:" + o for o in OUTPUTS]
CLB0 = ["n1", "n2", "n3", "q1", "n4", "n5", "n6"]
STAMP = "0.0"


def blif() -> str:
    out = ["# hand-made BLIF for the prov_why fixture (hash-style names)", ".model why",
           ".inputs " + " ".join(INPUTS), ".outputs " + " ".join(OUTPUTS)]
    for name, ins in LUTS.items():
        out += [f".names {' '.join(ins)} {name}", "11 1"]
    out += [".latch n3 q1 re clk 2", ".end", ""]
    return "\n".join(out)


def leaf(name: str, stem: str) -> str:
    return f'<block name="{name}" instance="{stem}[0]" mode="{stem}"/>'


def net() -> str:
    out = ['<?xml version="1.0"?>', '<block name="why.net" instance="FPGA_packed_netlist[0]">',
           f"<inputs>{' '.join(INPUTS[:-1])}</inputs><outputs>{' '.join(PADS)}</outputs>"
           "<clocks>clk</clocks>"]
    for top, atoms in (("n1", CLB0), ("o1", ["o1"])):
        out.append(f'<block name="{top}" instance="clb[{0 if top == "n1" else 1}]" mode="default">')
        for k, atom in enumerate(atoms):
            stem = "ff" if atom == "q1" else "lut6"
            out += [f'<block name="{atom}" instance="fle[{k}]" mode="n1_lut6">',
                    leaf(atom, stem), "</block>"]
        out.append("</block>")
    for k, name in enumerate([*INPUTS, *PADS]):
        stem = "outpad" if name.startswith("out:") else "inpad"
        out += [f'<block name="{name}" instance="io[{k}]" mode="{stem}">', leaf(name, stem),
                "</block>"]
    out += ["</block>", ""]
    return "\n".join(out)


def place() -> str:
    rows = ["Netlist_File: why.net Netlist_ID: SHA256:0", "Array size: 4 x 4 logic blocks", "",
            "#block name\tx\ty\tsubblk\tlayer\tblock number",
            "#----------\t--\t--\t------\t-----\t------------", "n1\t1\t1\t0\t0\t#0",
            "o1\t1\t2\t0\t0\t#1"]
    names = [*INPUTS, *PADS]
    rows += [f"{n}\t0\t{k + 1}\t0\t0\t#{k + 2}" for k, n in enumerate(names)]
    return "\n".join(rows) + "\n"


def names_map() -> str:
    atoms = [*INPUTS, "n1", "n2", "n3", "q1", "n4", "n5", "n6", "o1"]
    out = ["# odin3-names 2 hash=sha256 width=10 prov=1 ast=refs"]
    for k, atom in enumerate(atoms, start=1):
        out.append("\t".join(("N", "why", "net", atom, f"r{k}", "word")))
    row_of = {"n1": 2, "n6": 2}  # the same declarator: the same word label
    assigns = [x for x in atoms if x in CLB0 + ["o1"] and x not in row_of]
    for k, atom in enumerate(assigns):
        row_of[atom] = 3 + k
    for k, atom in enumerate(atoms, start=1):
        row = row_of.get(atom, 0)
        out.append("\t".join(("R", f"r{k}", "S", "stmt", "origin", "t1.1", "", f"f1:{k}.1-{k}.20",
                              f"c{row}" if row else "", "" if row else "chain=span")))
    out.append("\t".join(("C", "c1", "MODULE", "module", "", "f1", "1.1-40.10", "why")))
    out.append("\t".join(("C", "c2", "DECLARATOR", "stmt", "c1", "f1", "2.1-2.20", "w")))
    for k in range(len(assigns)):
        out.append("\t".join(("C", f"c{3 + k}", "CONT_ASSIGN", "block", "c1", "f1",
                              f"{3 + k}.1-{3 + k}.20", "")))
    out += ["F\tf1\twhy.v", "T\tt1\tread_verilog\t\t"]
    return "\n".join(out) + "\n"


def census(sha: str) -> str:
    def mol(cseq: int, idx: int, source: int, root: str) -> str:
        return f"M\t{cseq}\t{idx}\tlut\t{source}\t1\t0.5\t0\t0\t0\t-1\t{root}"

    rows = [f"# net_sha256={sha}"]
    plan = [(1, 0, 0, ["n1"]), (1, 1, 1, ["n2"]), (1, 2, 1, ["n3", "q1"]), (1, 3, 2, ["n4"]),
            (1, 4, 7, ["n5"]), (1, 5, 7, ["n6"]), (2, 0, 0, ["o1"]), (9, 0, 0, ["zz"])]
    for cseq, idx, source, atoms in plan:
        rows.append(mol(cseq, idx, source, atoms[0]))
        rows += [f"A\t{cseq}\t{idx}\t{a}\t0\t0\t0\t0" for a in atoms]
    rows += ["M\t3\t0\tlut\t0\t1\t0.5\t0\t0\t0\t-1\tn7", "A\t3\t0\tn7\t0\t0\t0\t0"]  # never closed
    pins = [("n2", "n1", 0.9), ("n2", "a", 0.1), ("n3", "n2", 0.2), ("q1", "n3", 0.7),
            ("n4", "e", 0.3), ("n5", "e", 0.4), ("n1", "a", 0.1), ("n6", "h", 0.1),
            ("o1", "n4", 0.6)]
    rows += [f"P\t1\t{a}\t{n}\t{c}" for a, n, c in pins]
    rows += ["K\t1\t6\t6\t0", "K\t2\t1\t1\t0", "K\t9\t1\t1\t0"]
    return "\n".join(rows) + "\n"


def flat_place() -> str:
    rows = []
    for atom in CLB0:
        rows.append(f"{atom} 1 1 0 0 clb0 {'ff' if atom == 'q1' else 'lut6'}")
    rows.append("o1 1 2 0 0 clb1 lut6")
    rows += [f"{n} 0 {k + 1} 0 {k} io{k} {'outpad' if n.startswith('out:') else 'inpad'}"
             for k, n in enumerate([*INPUTS, *PADS])]
    return "\n".join(rows) + "\n"


def attribution() -> dict[str, object]:
    def path(index: int, arrival: float, slack: float, atoms: list[str]) -> dict[str, object]:
        return {"index": index, "arrival_ns": arrival, "slack_ns": slack,
                "nodes": [{"atom": a} for a in atoms]}

    return {"schema_version": 1,
            "timing": {"paths_requested": 100, "paths_in_report": 4, "paths_kept": 4},
            "paths": [path(1, 10.0, -1.0, ["a", "n1", "n2", "n3", "q1"]),
                      path(2, 9.7, -0.7, ["e", "n4", "o1", "out:o1"]),
                      path(3, 9.2, -0.2, ["e", "n5"]), path(4, 5.0, 4.0, ["h", "n6"])]}


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def main() -> None:
    write(FLOW / "odin3" / "why.blif", blif())
    write(FLOW / "odin3" / "why.blif.names", names_map())
    net_text = net()
    write(FLOW / "vpr" / "run" / "why.net", net_text)
    write(FLOW / "vpr" / "run" / "why.place", place())
    write(FLOW / "timing" / "attribution.json", json.dumps(attribution(), indent=1) + "\n")
    write(HERE / "census.tsv", census(hashlib.sha256(net_text.encode()).hexdigest()))
    write(HERE / "norns" / "final.flat_place", flat_place())


if __name__ == "__main__":
    main()
