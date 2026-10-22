# tests/unmap

Inputs and expectations of the `unmap` pass (v1.0.4; spec `docs/specs/2026-10-21-unmap-design.md`,
plan `docs/plans/2026-10-21-unmap-plan.md`).

| File | What |
|---|---|
| `run.sh` | the command-line cases `cli_unmap_rtl` (round trip), `cli_unmap_golden` (RTLIL goldens), `cli_unmap_blif` (VTR BLIF without provenance, ambiguity cases) |
| `micros.txt` | the micros of the round trip: `<path> <comb\|seq>` |
| `expect.txt` | what may differ from equivalent and equal counts, one reason per line; a line that does not occur is stale and fails |
| `*.v` | the new micros: signed add and sub, the 40x40, 40x8, 8x40 and 80x80 multipliers, a signed multiplier, an 8-bit RAM and a 2r1w RAM |
| `expected/*.il` | `write_rtlil` after `hardmap; unmap; opt_clean` for the add, sub, 8x7 and 40x40 multiplier and spram micros |
| `blif/*.blif` | hand-made VTR netlists for the ambiguity cases (a branching carry, a carry tap, two RAMs with equal control nets) |

## Round trip

Per micro the Phase 3 script of `tools/hardmap-check/phase3_pre_abc.script` runs twice: with
`hardmap; unmap; opt_clean` and without `hardmap`; `tools/equiv-check/equiv-check` must exit 0 on
the two BLIFs. The count of `$add $sub $mul $mem` cells before `hardmap` and after `unmap` must be
equal, and for the combinational micros the `sim` vectors before `hardmap` and after `unmap` must
be byte-identical. Entries of `expect.txt` list the exceptions with a reason.

The explicit-instance micros (`spram`, `dpram`) have a black box in the reference, so equiv-check
cannot pair the two netlists: they are `equiv-skip` and compared by counts.
`common/adder.v` is not in the list: it instantiates a module through Parmys's flattened name
(`add_add_module`), which the Odin III reader does not resolve.
