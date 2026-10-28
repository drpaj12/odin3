# Terms for the Odin III paper (Part I Background and Part II methodology order)

Ordered so that every term depends only on terms above it. `defined at` is the position (file:line of odin3.tex) where the paper explains the term now (`none`: the paper never defines it); `first used at` is where the term first occurs outside the abstract; `search` is the regular expression the checker uses.
Check with `python3 docs/paper/check_terms.py --tex docs/paper/odin3.tex` (the abstract is exempt).
Generated 2026-10-28 against origin/docs/paper-tree 5737610e (odin3 #295 still open).
| term | definition | defined at | first used at | depends on | search |
|---|---|---|---|---|---|
| FPGA | A chip of programmable logic blocks and wires that a design is loaded onto. | none | odin3.tex:81 | - | `\bFPGAs?\b` |
| Verilog / HDL | The hardware description language a designer writes; HDL is the general term. | none | odin3.tex:79 | - | `\bHDL\b\|Verilog` |
| netlist | A list of the gates, LUTs or cells of a design and the wires between them. | none | odin3.tex:79 | Verilog / HDL | `\bnetlists?\b` |
| LUT | Look-up table: a small memory that implements any logic function of K inputs (K = 6 here). | none | odin3.tex:111 | netlist | `\bLUTs?\b` |
| CLB | Configurable logic block: a group of LUTs and flip-flops that is placed as one unit. | none | odin3.tex:162 | LUT | `\bCLBs?\b` |
| flip-flop | A one-bit storage cell updated on a clock edge. | none | odin3.tex:144 | netlist | `flip-flops?` |
| packing, placement, routing | Pack LUTs into CLBs; choose a site for each CLB; choose the wires between them. | odin3.tex:117 | odin3.tex:96 | CLB; LUT | `\bpack(ing)?\b\|placement\|\brout(ed\|ing)\b` |
| VTR / VPR | The open-source FPGA CAD flow (VTR) and its pack, place and route program (VPR). | odin3.tex:117 | odin3.tex:79 | packing, placement, routing | `\bVTR\b\|\bVPR\b` |
| synthesis front end | The program that reads HDL and produces a netlist for the rest of the flow. | odin3.tex:79 | odin3.tex:79 | Verilog / HDL; netlist | `front end` |
| elaboration | Turning parsed HDL into a design graph: resolving parameters, generate loops and hierarchy. | none | odin3.tex:79 | Verilog / HDL | `elaborat` |
| AST | Abstract syntax tree: the parsed form of the source text. | odin3.tex:111 | odin3.tex:111 | Verilog / HDL | `\bAST\b` |
| Odin II / Yosys / Parmys | The older front end of VTR; the open synthesis tool; the Yosys plugin that now serves VTR. | odin3.tex:79 | odin3.tex:79 | synthesis front end | `Odin~?II\b\|Yosys\|Parmys` |
| ABC | The logic-synthesis and LUT-mapping program linked into the flow. | none | odin3.tex:81 | LUT; netlist | `\bABC\b` |
| provenance | The link from each netlist object back to the HDL that produced it. | odin3.tex:122 | odin3.tex:86 | Verilog / HDL; netlist | `provenance` |
| IR | Intermediate representation: Odin III's one internal design graph with word and bit views. | odin3.tex:86 | odin3.tex:86 | netlist | `\bIR\b` |
| oracle | A trusted reference tool whose output we compare against (Yosys with Parmys, Odin II). | odin3.tex:126 | odin3.tex:85 | Odin II / Yosys / Parmys | `oracle` |
| golden | The stored reference result of a reference tool for a design. | none | odin3.tex:96 | oracle | `golden` |
| partial mapping (hardmap) | Mapping the word cells that fit the architecture's hard blocks (adders, multipliers, RAMs) and leaving the rest for ABC. | none | odin3.tex:79 | IR; LUT | `partial.map\|hardmap\|hard.block` |
| word cell / bit cell, lower | A multi-bit operator cell; a one-bit gate cell; `lower` turns word cells into bit cells. | odin3.tex:148 | odin3.tex:113 | IR | `\blower\b\|word cell\|word-level` |
| proc, opt, fsm, wreduce, flatten | The word-level passes re-implemented from Yosys 0.55: processes to muxes and flip-flops; clean-up; state-machine recoding; width reduction; inlining of hierarchy. | odin3.tex:144 | odin3.tex:113 | IR; word cell / bit cell, lower | `\bproc\b\|\bopt\b\|wreduce\|\bflatten\b` |
| BLIF | The netlist text format that VPR reads. | none | odin3.tex:86 | netlist | `\bBLIF\b` |
| C ABI / plugin | The stable C interface through which plugins and AI tools call Odin III. | odin3.tex:164 | odin3.tex:79 | IR | `\bABI\b\|plugin` |
| lineage DAG | The append-only graph of source, derived and joined records behind each provenance ID. | odin3.tex:157 | odin3.tex:153 | provenance | `lineage\|\bDAG\b` |
| critical path / CPD | The slowest register-to-register path and its delay in ns. | none | odin3.tex:162 | netlist | `critical.path\|\bCPD\b` |
| wirelength, channel width | Total routed wire length; number of routing tracks per channel needed to route. | none | odin3.tex:162 | packing, placement, routing | `wirelength\|channel width` |
| geomean ratio | Geometric mean over designs of (Odin III / Parmys); 1.0 equals; below 1.0 is better for area and delay. | none | odin3.tex:162 | oracle | `geomean` |
| QoR gate | The acceptance bound: geomean O/P at most 1.20 and no design over 1.50 on the gated metrics. | odin3.tex:251 | odin3.tex:162 | geomean ratio | `1\.20 gate\|\bgate\b` |
| equivalence (SAT, refinement proof) | A proof that two designs give the same outputs (or that one refines the other), by SAT; unproven at the size limit. | odin3.tex:265 | odin3.tex:87 | netlist | `equivalen\|refinement\|\bSAT\b` |
| fcheck (functional check) | VTR's random check: build the RTL and the netlist with Verilator and drive both with 50,000 random vectors. | odin3.tex:269 | odin3.tex:269 | netlist; VTR / VPR | `fcheck\|Verilator` |
| determinism | Rerunning gives byte-identical output. | odin3.tex:271 | odin3.tex:87 | - | `determinis\|byte-identical` |
| exit-code discipline | A list of expected differences never turns an unproven result into a pass. | odin3.tex:263 | odin3.tex:263 | oracle | `exit-code` |
| Dark Factory | A software process in which agents turn a human's specification into software. | odin3.tex:81 | odin3.tex:81 | - | `Dark.?Factory` |
