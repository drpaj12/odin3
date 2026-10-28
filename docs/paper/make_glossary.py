#!/usr/bin/env python3
"""Write glossary.tex and terms.md from one list of terms (single source for both).

Usage (from docs/paper): python3 make_glossary.py [--tex odin3.tex]

glossary.tex is the Glossary subsection body of odin3.tex (one \\item per line, in dependency
order). Terms marked ``intro`` are defined where the Introduction first uses them (a line tagged
``% def:<key>`` in odin3.tex); the glossary repeats them untagged (``% ref:<key>``). terms.md is
rewritten with ``defined at`` read from the ``% def:<key>`` tags and ``first used at`` computed by
check_terms. Standard library only.
"""

# ruff: noqa: E501
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_terms as ct  # noqa: E402

JT = r"\cite{jamiesonthesis}"
LT = r"\cite{luuthesis}"

# key, name, definition (LaTeX), depends on (names), search regex, defined in the Introduction
TERMS: list[tuple[str, str, str, list[str], str, bool]] = [
    ("fpga", "FPGA", f"A chip of programmable logic blocks and wires onto which a digital design is loaded~{JT}.", [], r"\bFPGAs?\b", True),
    ("ff", "flip-flop", f"A one-bit storage cell that keeps its value between clock edges~{JT}.", [], r"flip-flops?", True),
    ("lut", "LUT", f"Look-up table: a small memory that implements any logic function of $K$ inputs ($K=6$ here). A LUT and a flip-flop form the basic logic element of most FPGAs~{JT}.", ["flip-flop"], r"\bLUTs?\b", True),
    ("clb", "CLB", f"Configurable logic block: a group of LUTs and flip-flops that is placed as one unit (a cluster in~{JT})~{LT}.", ["LUT"], r"\bCLBs?\b", True),
    ("hdl", "Verilog / HDL", f"The hardware description language (HDL) in which a designer writes a circuit; Verilog is the one used here~{JT}.", [], r"\bHDL\b|Verilog", True),
    ("netlist", "netlist", f"The list of the gates, LUTs and other cells of a design and the wires (nets) between them~{JT}.", ["Verilog / HDL"], r"\bnetlists?\b", True),
    ("front", "synthesis front end", f"The program that reads the HDL and produces a netlist for the rest of the flow~{JT}.", ["Verilog / HDL", "netlist"], r"front end", True),
    ("elab", "elaboration", f"The first step of a front end: turn the parsed HDL into a design graph by resolving parameters, generate loops and hierarchy~{JT}.", ["synthesis front end"], r"elaborat", True),
    ("abc", "ABC", f"The logic-synthesis program that maps the gates of a netlist onto LUTs (technology mapping)~{JT}.", ["LUT", "netlist"], r"\bABC\b", True),
    ("prp", "packing, placement, routing", f"Packing groups LUTs and flip-flops into CLBs; placement chooses a site on the chip for each CLB; routing chooses the wires between them~{JT}.", ["CLB", "LUT"], r"\bpack(ing)?\b|placement|\brout(ed|ing)\b", True),
    ("vtr", "VTR / VPR", f"The open-source FPGA CAD flow (Verilog-to-Routing, VTR) and its packing, placement and routing program (VPR)~{LT}.", ["packing, placement, routing", "synthesis front end"], r"\bVTR\b|\bVPR\b", True),
    ("odin2", "Odin II / Yosys / Parmys", "Odin II was the front end of VTR; Yosys is an open synthesis tool and Parmys the Yosys plugin that now serves VTR as its front end.", ["synthesis front end"], r"Odin~?II\b|Yosys|Parmys", True),
    ("prov", "provenance", "The link from each netlist object back to the HDL that produced it.", ["Verilog / HDL", "netlist"], r"provenance", True),
    ("dark", "Dark Factory", "A software process in which agents turn a human's specification into software, with no human writing the code.", [], r"Dark.?Factory", True),
    ("minw", "channel width", f"The number of routing tracks in each channel that the router needs to route a design (minimum channel width $W$)~{LT}.", ["packing, placement, routing"], r"channel width", True),
    ("wl", "wirelength", "The total length of the wires a routed design uses.", ["packing, placement, routing"], r"wirelength", True),
    ("cpd", "critical path / CPD", f"The slowest path between registers and its delay in ns (critical-path delay, CPD)~{LT}.", ["flip-flop", "netlist"], r"critical.path|\bCPD\b", True),
    ("near", "near-critical", "A path whose delay is within a stated window of the critical-path delay.", ["critical path / CPD"], r"near-critical", True),
    ("area", "area", "How much chip a design uses: the number of LUTs, the number of CLBs and the minimum channel width $W$.", ["LUT", "CLB", "channel width"], r"\barea\b", True),
    ("speed", "speed", "How fast the circuit runs: the critical-path delay and the routed wirelength, and where available the near-critical paths.", ["critical path / CPD", "wirelength", "near-critical"], r"\bspeed\b", True),
    ("power", "power", "The energy the circuit uses. This paper does not measure it.", [], r"\bpower\b", True),
    ("hyp", "hypothesis / verdict", "A hypothesis is a claim that an experiment can confirm or fail to confirm. The verdict is USEFUL (confirmed under the stated gate or check), POSSIBLY GOOD (confirmed but narrow or fragile), INCONCLUSIVE (neither confirmed nor failed) or DISMISSED (failed).", [], r"hypothes[ei]s|verdict", True),
    ("ir", "IR", "Intermediate representation: Odin III's one internal design graph, with a view of multi-bit words and a view of single bits.", ["netlist"], r"\bIR\b", True),
    ("oracle", "oracle", "A trusted reference tool whose output we compare against (Yosys with Parmys, and Odin II).", ["Odin II / Yosys / Parmys"], r"oracle", True),
    ("abi", "C ABI / plugin", "The application binary interface (ABI) is the stable C interface through which a plugin, a separately built program that adds passes or cell types, and AI tools drive Odin III without changing it.", ["IR"], r"\bABI\b|plugin", True),
    ("ast", "AST", f"Abstract syntax tree: the parsed form of the HDL text~{JT}.", ["Verilog / HDL"], r"\bAST\b", False),
    ("golden", "golden", "The stored result of an oracle for a design, kept so that later runs compare against it.", ["oracle"], r"golden", False),
    ("pmap", "partial mapping", f"Binding the parts of a design that fit the architecture's dedicated blocks (multipliers, adders, memories) to those blocks and leaving the rest as gates for ABC~{JT}.", ["IR", "LUT", "ABC"], r"partial.map|hardmap|hard.block", False),
    ("word", "word cell / bit cell / lower", "A word cell is a multi-bit operator in the IR; a bit cell is a one-bit gate; the pass lower turns word cells into bit cells.", ["IR"], r"\blower\b|word cell|bit cell|word-level|bit-level", False),
    ("passes", "proc, opt, fsm, wreduce, flatten", "The word-level passes re-implemented from Yosys 0.55: processes to multiplexers and flip-flops; clean-up; state-machine recoding; width reduction; inlining of the hierarchy.", ["IR", "word cell / bit cell / lower", "flip-flop"], r"\bproc\b|\bopt\b|wreduce|\bflatten\b|\bfsm\b", False),
    ("blif", "BLIF", "The netlist text format that VPR reads.", ["netlist", "VTR / VPR"], r"\bBLIF\b", False),
    ("lineage", "lineage DAG", "The append-only graph of source, imported and derived records, with labelled joins, that sits behind each provenance ID.", ["provenance"], r"lineage|\bDAG\b", False),
    ("geomean", "geomean ratio", "The geometric mean over designs of the ratio Odin III over Parmys; 1.0 means equal and below 1.0 is better for area and delay.", ["golden", "area", "speed"], r"geomean", False),
    ("leg", "leg (P, O, O$'$)", "One run of the quality-of-results harness: P is the Parmys flow (the baseline), O is Odin III with its linked ABC, and O$'$ is Odin III's netlist through VTR's own ABC stage.", ["VTR / VPR", "ABC", "oracle"], r"\blegs?\b", False),
    ("gate", "QoR gate", "The acceptance bound on quality of results: geomean ratio at most 1.20 and no design above 1.50 on the gated metrics.", ["geomean ratio", "leg (P, O, O$'$)"], r"QoR gate|1\.20 gate|gate of 1\.20|1\.20 and no", False),
    ("equiv", "equivalence (SAT, refinement)", "A proof, by SAT, that two designs give the same outputs, or that one refines the other; unproven when the design is past the size limit of the solver.", ["netlist"], r"equivalen|refinement|\bSAT\b", False),
    ("fcheck", "fcheck (functional check)", "VTR's random check: build the RTL and the netlist with Verilator and drive both with 50,000 random vectors.", ["netlist", "VTR / VPR"], r"fcheck|Verilator", False),
    ("det", "determinism", "Rerunning gives byte-identical output.", [], r"determinis|byte-identical", False),
    ("exit", "exit-code discipline", "A list of expected differences never turns an unproven result into a pass; tools report 0 holds, 1 differs, 2 tool error, 3 hard failure, 4 unproven, 77 skipped.", ["oracle"], r"exit-code", False),
]


def write_glossary() -> None:
    """Write glossary.tex."""
    lines = ["% Generated by make_glossary.py from the term list; do not edit by hand.\n",
             "\\begingroup\\small\n"]
    for key, name, definition, _deps, _rx, intro in TERMS:
        tag = f"% ref:{key}" if intro else f"% def:{key}"
        lines.append(f"\\par\\noindent\\textbf{{{name}.}} {definition} {tag}\n")
    lines.append("\\endgroup\n")
    (HERE / "glossary.tex").write_text("".join(lines), encoding="utf-8", newline="\n")


def tag_lines(tex: Path) -> dict[str, str]:
    """Map each ``% def:<key>`` tag to ``file:line`` over odin3.tex and the inputs."""
    found: dict[str, str] = {}
    base = tex.parent

    def walk(path: Path) -> None:
        rel = path.relative_to(base).as_posix()
        for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            m = re.search(r"%\s*def:(\w+)", raw)
            if m and m.group(1) not in found:
                found[m.group(1)] = f"{rel}:{number}"
            inp = re.search(r"\\input\{([^}]+)\}", raw)
            if inp and not raw.lstrip().startswith("%"):
                name = inp.group(1)
                target = base / (name if name.endswith(".tex") else name + ".tex")
                if target.is_file() and not name.startswith(("IMAGE/", "gen/")):
                    walk(target)

    walk(tex)
    return found


def write_terms(tex: Path) -> None:
    """Write terms.md."""
    tags = tag_lines(tex)
    lines = ct.flatten(tex)
    rows = []
    for key, name, definition, deps, rx, _intro in TERMS:
        term = ct.Term(name, definition, tags.get(key, "none"), "", deps, rx)
        used = ct.first_use(lines, term)
        first = f"{lines[used].file}:{lines[used].number}" if used is not None else "-"
        plain = re.sub(r"\\cite\{[^}]*\}|~", " ", definition).replace("\\ref{sec:eval}", "6.1")
        rows.append((name, plain.strip(), tags.get(key, "none"), first, "; ".join(deps) or "-", rx))
    head = [
        "# Terms for the Odin III paper (Introduction, Background and Methodology order)\n",
        "\n",
        "Generated by `make_glossary.py` (the same list writes `glossary.tex`). Ordered so that every "
        "term depends only on terms above it. `defined at` is the line tagged `% def:<key>` in the "
        "paper; `first used at` is where the term first occurs outside the abstract; `search` is the "
        "regular expression the checker uses.\n",
        "Check with `python3 docs/paper/check_terms.py --tex docs/paper/odin3.tex` "
        "(the abstract is exempt).\n",
        "| term | definition | defined at | first used at | depends on | search |\n",
        "|---|---|---|---|---|---|\n",
    ]
    body = [
        "| " + " | ".join(c.replace("|", "\\|") if i != 5 else f"`{c.replace('|', chr(92) + '|')}`"
                           for i, c in enumerate(r)) + " |\n"
        for r in rows
    ]
    (HERE / "terms.md").write_text("".join(head + body), encoding="utf-8", newline="\n")


def main() -> None:
    """Entry point."""
    ap = argparse.ArgumentParser()
    ap.add_argument("--tex", type=Path, default=HERE / "odin3.tex")
    args = ap.parse_args()
    write_glossary()
    write_terms(args.tex.resolve())


if __name__ == "__main__":
    main()
