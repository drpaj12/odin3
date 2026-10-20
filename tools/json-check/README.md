# json-check

`write_json`, `read_json`, `write_json` round trip (Phase 5.3, spec `docs/specs/2026-10-20-5.3-read-json-design.md`
R-1 to R-3, T-2, T-3). Needs Yosys 0.55 only for the equivalence legs.

    tools/json-check/json-check --level word,gate,lut [--yosys Y] [--expect tests/json/expect.txt] DESIGN.v...
    tools/json-check/json-check --level lut --yosys-dir tests/json/yosys     # the committed Yosys fixtures

Legs per design and level: read (with `check`), R-1 fixpoint (file 2 equals file 3), equivalence of the
read-back with what was written (RTLIL `Checker` at the word level, `equiv-check` on BLIF at the gate and
LUT levels), `netlist-compare` identical or equivalent-only. The level scripts are `tests/json/*.script`.
`--yosys-dir` reads the fixtures made by `tests/json/yosys/gen.sh` (Yosys 0.55 from the VTR tree, local
only). Exit codes: 0 ok, 1 failed or nothing compared, 2 usage, 3 crash, 4 unproven, 77 skipped. See the
docstring of `json_check.py`.
