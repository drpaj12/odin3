# Proposed new main-paper outline: Odin III

Order requested in the brief. Odin III is a systems paper whose evidence is acceptance gates and checks; the register (hypotheses.md) says which are pre-registered (O1, O3, O16) and which reconstructed. Paper text is not edited in this step. Branched from origin/docs/paper-tree because odin3 #295 is open.

## New structure

0. Abstract: the plain-English story, the innovations with their USEFUL hypotheses, one sentence on the failed cost gate (O16) and the unproven items.
1. Introduction: problem in plain English (a Verilog front end for the open FPGA flow that keeps track of where every gate came from); the innovations in plain English (innovations.md 1 to 5); contributions = innovations with USEFUL hypotheses (O1, O2, O4, O6, O7, O8, O9, O13, O14, O17) and the honest INCONCLUSIVE and DISMISSED items (O3, O5, O15, O16).
2. Background (terms in dependency order, terms.md): FPGA, Verilog/HDL, netlist, LUT, CLB, flip-flop, packing/placement/routing, VTR/VPR, front end, elaboration, AST, Odin II/Yosys/Parmys, ABC, provenance, IR, oracle, golden.
3. The innovation (plain English, then technical): the front end and flow; oracle checking; the provenance IR; the ABI; the Dark Factory process.
4. Methodology: setup (laptop, WSL2, heavy lock, 15 designs, the five shipped out), metrics: area (LUTs, CLBs, channel width W), speed (CPD, routed WL; near-critical timing is a tool feature from v1.1.1, not a measured result), power (not measured); gates and checks (QoR gate 1.20 / 1.50, exit-code discipline, proof outcomes, byte equality); what is pre-registered versus reconstructed; data: the VTR benchmark designs and golden results, the micro suite, and provenance as the enabler for Mimir (see below).
5. Experiments, one subsection per USEFUL/INCONCLUSIVE/DISMISSED row kept in the main paper:
   - 5.1 Quality of results against Parmys (O1, O2; O3 as the parity decision).
   - 5.2 Equivalence and function (O4, O5, O6, O7, O8).
   - 5.3 Defects found in the Parmys flow (O9).
   - 5.4 Provenance exact at every granularity and what it costs (O14, O15, O16, O17).
   - 5.5 Process, time and token cost (O13).
6. Discussion and limits: not a lights-out factory; code not deeply read; five largest designs untested; Oprov not measured; parity retired; path-dependent hash names; no power.
7. Conclusion.
Appendix A (Part II of the current paper, shortened): system detail; research log with every row of hypotheses.md.

## Mapping of the current paper

| current location | content | new place | action |
|---|---|---|---|
| odin3.tex:46-74 abstract | | Abstract | rewrite |
| odin3.tex:76-105 Introduction | story, contributions, Yggdrasil tree | Introduction | rewrite; tree paragraph kept |
| odin3.tex:106-128 Background | Odin II/Yosys models, VTR, provenance, oracles | Background | keep, add terms in dependency order |
| odin3.tex:129-210 Architecture | flow, ABI, comparison | 3 | keep; ABI and comparison tables shrink |
| odin3.tex:211-252 Quality of results | Table tab:m2 | 5.1 | keep |
| odin3.tex:253-274 Verification evidence | | 5.2 | keep |
| odin3.tex:275-300 Defects found in the Parmys flow | | 5.3 | keep |
| odin3.tex:301-347 Process, time, token cost | | 5.5 | keep |
| odin3.tex:348-379 Discussion, Conclusion | | 6, 7 | rewrite |
| odin3.tex:380-1161 Part II: synthesis background, architecture in detail, features, verification | system description | Appendix A | keep, no change |
| odin3.tex:1162-1525 internal data structures | IR, provenance, ABI | Appendix A (and 3 for the provenance paragraph) | keep |
| odin3.tex:1526-1728 reverse engineering and graph analysis | RE vision, Gate-Breaker | Appendix A; Loki paper owns the evidence | shrink |
| odin3.tex:1729-1876 comparison with Yosys and Odin II | | Appendix A | keep |
| odin3.tex:1877-1967 development methodology | | 5.5 and Appendix A | merge |
| odin3.tex:1968-2160 v1.0 phases | | Appendix A | keep; decision log rows O3 |
| odin3.tex:2160-2202 measured results table | M1, M2, M3, 3B, 3C, 3H, 3D-7 | 5.2 and Appendix A (rows O10, O11, O12) | keep as appendix table |
| odin3.tex:2203-2226 known gaps at v1.0 | | 6 | move |
| odin3.tex:2227-2262 after v1.0 | 1.0.1 to 1.2.0 | Appendix A, 5.4 for 1.1.1 and 1.2.0 | keep |
| docs/log_cost_of_tokens_and_time.md | cost log | 5.5 | cite |

## Provenance as the enabler

Odin III is the trunk of Yggdrasil III; every downstream study rests on one property, that each netlist object keeps the chain of HDL constructs it came from. Mímir III (the circuit knowledge base) stores that link per placed object, which is what lets the Norns III paper ask why a structure landed where it did and the Loki III paper score recovery against an answer key. For this paper the enabling facts are: the version 2 name map (records, join labels, construct table; V1.2.0.md Tasks 3 to 6), hash names that no longer depend on the build, and (still open) names that depend on the design's directory. What Odin III does not give Mímir today: near-critical statistics per HDL object (only the critical path and its window, v1.1.1 `odin3-timing`), power, and the cost of provenance names in VPR (Oprov, not measured).
