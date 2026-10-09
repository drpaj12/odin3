# tests/golden/blif

BLIF fixtures for the 1C reader/writer tests (`tests/unit/test_blif_*.c`). Never hand-edit a
copied golden: re-copy it from `~/odin3-ws/golden` if it changes.

## Copied goldens

Verbatim oracle outputs from `~/odin3-ws/golden` (written by `tools/run-oracle.sh`), all
`status=ok`, VTR commit `3c9a4d23b27d187bd67c79dc01a18ff1ffc21d9b`, architecture
`vtr_flow/arch/timing/EArch.xml`. The sha256 of each file equals the `blif_sha256` of its
`.prov` in the golden tree.

| fixture | oracle | golden source | sha256 |
|---|---|---|---|
| `ff.odin.blif` | Odin II | `EArch/regression/verilog/micro/ff` | `dd3c90e8…d711fe0` |
| `ff.parmys.blif` | Yosys+Parmys | `EArch/regression/verilog/micro/ff` | `9eafff6e…7bedbc` |
| `adder_hard_block.parmys.blif` | Yosys+Parmys | `EArch/regression/verilog/micro/adder_hard_block` | `edcb8ee4…2e479f` |
| `ansiportlist_2.parmys.blif` | Yosys+Parmys | `EArch/regression/verilog/full/ansiportlist_2` | `408311c4…58dba` |
| `pow.parmys.blif` | Yosys+Parmys | `EArch/regression/verilog/common/pow` | `0447944d…4a3b719` |
| `dffsre.parmys.blif` | Yosys+Parmys | `EArch/regression/verilog/common/dffsre` | `d9bf07c5…b83b40b` |
| `elsif_both_defined.odin.blif` | Odin II | `EArch/regression/verilog/preprocessor/elsif_both_defined` | `0ca5a20b…ffa620b` |

`ansiportlist_2.parmys.blif` has three models: the top and the `.blackbox` models `adder`
(scalar formals) and `multiply` (vector formals `b[0..35]`, `a[0..35]`, `out[0..71]`).
`pow.parmys.blif` and `dffsre.parmys.blif` instantiate Yosys cells (`$pow`, `$_DFFSR_PPP_`) that
no `.model` declares (the reader makes them implicit black boxes). `elsif_both_defined.odin.blif`
drives one net from two `.names` (read as written; `check` reports rule 4).

## Hand-written fixtures

| fixture | covers |
|---|---|
| `hand_ports.blif` | port grouping (`v[0..2]` → vector `v`), non-consecutive bits staying scalar in order (`a[0] b a[1]`, Review Focus 2), bits not from 0 or out of order, a continuation inside `.inputs`, `.clock`, a `.blackbox` model with vector formals |
| `hand_body.blif` | `.names` covers (2-, 1- and 0-input, constant 0 and 1), every `.latch` form (`re` `fe` `ah` `al`, no type, with and without init), `.subckt` of a later model by bit formals across a continuation (Review Focus 1), `.subckt` of a black box with an unlisted formal, `.cname`, `.attr` (repeated key), `.param` with a multi-token value |

Error cases and one-off variants are written to a temporary file by the test itself, next to
their assertions in `tests/unit/test_blif_reader.c`.
