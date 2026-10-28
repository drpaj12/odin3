# Innovations of the Odin III paper

Plain English first, then the technical paragraph, then the H-IDs (hypotheses.md) whose USEFUL verdict proves the value, then what is still unproven. Odin III is a systems paper; its proof is acceptance gates and checks against oracles, not statistical tests. Verdicts follow the register (hypotheses.md): O1, O11, O12 and O17 are POSSIBLY GOOD (narrow or with a stated gap).

## 1. A Yosys-class FPGA synthesis front end that plugs into VTR (comparable quality, not parity)

Plain English. A program that reads Verilog and hands a netlist to the open-source FPGA flow, so the rest of the flow (ABC, then VPR's pack, place and route) works as before and the result is about as good as with the Yosys-based front end.
Technical. C17 framework: two-stage elaboration, re-implementations of Yosys 0.55 `proc`, `opt`, `fsm`, `wreduce`, `flatten`, architecture-driven partial mapping, `lower`, linked ABC and VPR-ready BLIF (odin3.tex:129-210).
Proves its value, with a stated gap: O1 (POSSIBLY GOOD: all 15 designs route; geomeans against Parmys: LUT 1.041, CLB 1.012, W 0.985, CPD 0.986, WL 1.034 inside the 1.20 gate, but two designs differ in multiplier count and five lose adders), O2 (valid baseline), O6 (fcheck 12 of 15 pass, Parmys fails 8 of 15), O7 (byte-identical reruns 15 of 15), O10 (random generator for `lower`). POSSIBLY GOOD, stated with the fragility: O11, O12 (component checks).
Still unproven: parity per design (O3 INCONCLUSIVE, retired by decision 53); the five largest VTR designs (U2); power (not measured); the cost of provenance names in VPR (U1).

## 2. Checking against oracles instead of trusting them

Plain English. Every stage is compared against a trusted reference tool, and where the two disagree a probe decides which one matches the Verilog, so the reference's own mistakes surface.
Technical. Exit-code discipline (an unproven result never passes), refinement proofs by SAT (`lower-equiv`, `opt-equiv`), random generators against Yosys `techmap`, Verilator random checks, golden BLIFs (odin3.tex:253-300).
Proves its value: O4 (399 ok, 140 expected, 0 unexpected over 272 designs), O9 (six Parmys defects found: D2b, D4, D17, D19, `BUF_NODE`, `proc_mux` casez loss), O8 (144 of 156 VerilogEval references pinned, 12 refused with a stated reason).
Still unproven: O5 (14 small VTR designs: 8 or 9 proved, 5 unproven at the SAT size limit, 0 differ).

## 3. One provenance-tracked IR (the enabler of the suite)

Plain English. Every object in the netlist remembers which Verilog text made it, through every pass, and from v1.2.0 it remembers the full chain of constructs, including the places where two constructs were blended.
Technical. Append-only lineage DAG with one 32-bit ID per node, net and wire; record kinds SOURCE, IMPORTED and DERIVED (with parents; v1.2.0 adds labelled joins); granularity-tagged objects (word, gate, LUT); ABI 7 construct table and `odin3_prov_visit_*`; version 2 hash name map (lines `O N R C F T A`) read by `names --lookup/--chain` and extended after place and route by `odin3-prov-post` (odin3.tex:153-158, V1.2.0.md).
Proves its value: O14 (541 ctest cases, 0 fail; writer outputs byte-identical across modes on 539 micros), O17 (POSSIBLY GOOD, narrow: timing attribution to HDL does not change VPR's result on 3 designs; Norns round 5: attribution agreed with VPR's CPD in 999 of 999 runs). Downstream proof: Loki III's answer key (L0a, L0b) and the Norns III packer (Norns H8) are built on it.
Still unproven: O15 (round trip of exports: BLIF 68 of 68, JSON 4 of 68, Verilog 23 of 68); O16 DISMISSED (recording constructs at elaboration costs more than 5% time on 2 of 5 large designs, so the CLI default is off); path-dependence of hash names.

## 4. A versioned C ABI for plugins and AI tools

Plain English. A stable interface so other programs (Loki, Wyrd, Norns, Mimir) can drive and read Odin III without changing it.
Technical. C ABI (now 7) with sessions, IR readers, passes and construct/provenance queries; Python `odin3` package over cffi (odin3.tex:164-177).
Proves its value: none as a hypothesis here; the suite papers (Loki, Norns) use it, and the ABI bump from 6 to 7 required plugin rebuilds.
Still unproven: ABI gaps listed at odin3.tex:2221-2224 (strings-only attributes, module-local node IDs, no bulk readers).

## How it was built: a Dark Factory (method and context, not an innovation tested)

Plain English. The software was written by AI agents from a human's specification, with the human reviewing every day and automated checks against reference tools deciding what merges.
Technical. Specification with numbered decisions, a read-only critique, plan, implementer tasks on disjoint files, review before each pull request, CI gate (clang-tidy, cppcheck, complexity limits, sanitizers); transcripts logged (odin3.tex Methodology).
Status (Peter, 2026-10-28, decision 9): this is not a hypothesis and has no register row. The paper reports how the tool was built with its limits: daily human oversight, code not deeply read, a subset of scenarios tested, no comparison arm, so it does not show that the process generalises. Cost numbers come from docs/log_cost_of_tokens_and_time.md (v1.0 count frozen in the -v1.0 file).
