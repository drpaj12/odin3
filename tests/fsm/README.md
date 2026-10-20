# tests/fsm

FSM micros and the Yosys 0.55 facts for 4.8 (spec `docs/specs/2026-10-20-4.8-fsm-design.md`).

- `facts_corpus.txt`: the designs `tools/fsm-check/fsm-facts` runs Yosys 0.55 on.
- `yosys_facts.json`: what Yosys 0.55's `fsm` finds on them (generated; `fsm-facts --check` says
  whether it is current). The ctest `test_fsm_probe` checks it against spec section 12.
- `tools/fsm-check/fsm-probe FILE...`: the same probe on any Verilog (`--vtr` for the VTR designs).
