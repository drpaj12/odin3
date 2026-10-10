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
| `hand_writers.blif` | 1F writer-check cases found on the goldens: an undriven net buffered to an output (Odin II `no_input`), a constant buffered to an unread net, a 7-input cover with don't-cares, a constant input to a cover, a black box with one-bit vector formals `x[0]`/`o[0]` |
| `hand_wide.blif` | a 13-input cover (Yosys `read_blif` needs `-sop`) next to an identity buffer |

Error cases and one-off variants are written to a temporary file by the test itself, next to
their assertions in `tests/unit/test_blif_reader.c`.

## Writer gate

CTest `writer_check_fixtures` runs `tools/writer-check/writer-check --fixtures` over every
`*.blif` here (JSON through Yosys vs the golden through Yosys under `netlist-compare`; Verilog
through Icarus and Yosys + `equiv-check`; dot through Graphviz).
`writer_check_detects_corruption` checks that damaged JSON and Verilog fail it, and
`writer_check_bbox_not_excluded` that a black box only the Verilog side has counts as an error,
not an exclusion. All three are skipped when Yosys, Icarus or Graphviz is missing (CI).

Each Yosys-side normalization of the JSON step has a fixture that fails without it:

| normalization | fails without it |
|---|---|
| identity `$lut`s made connections (NORM) | `hand_writers.blif` (undriven net buffered to an output) |
| covers of at most 12 inputs as minterms | `hand_writers.blif` (7-input cover with don't-cares) |
| bit-0 names restored (JSON side) | `hand_writers.blif` (`x[0]`/`o[0]` formals) |
| `read_blif -sop` for wide covers | `hand_wide.blif` (Yosys rejects a 13-input `$lut`) |
| Yosys self-buffers `.names x x` dropped | `hand_ports.blif`, `hand_body.blif` (vector ports: netlist-compare refuses the self-loop as a second driver) |
| statements sorted | `ansiportlist_2.parmys.blif` (Yosys `read_json` cell order breaks a netlist-compare tie differently) |

## Round-trip gate

CTest `blif_roundtrip_fixtures` runs `tools/blif-roundtrip/blif-roundtrip --fixtures` over every
`*.blif` here (read, check FULL, write; normalized text identity; `netlist-compare`). No fixtures
were added for it; `elsif_both_defined.odin.blif` is on the script's multi-driver list. The full
golden run is `tools/blif-roundtrip/blif-roundtrip` with no arguments (see `docs/PHASE1.md`, 1C).
