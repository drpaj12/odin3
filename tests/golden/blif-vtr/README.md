# tests/golden/blif-vtr

Fixtures for `write_blif --vtr` (3E spec §16.2), run by CTest `blif_vtr_fixtures`:

```
tools/blif-vtr-check/vtr-fixtures --odin3 <odin3> --dir tests/golden/blif-vtr [--checker <blif-vtr-check>]
```

For each case the driver runs, from the input's directory under its basename (so `<loc>` lines
do not depend on the checkout),
`odin3 -p "read_techlib <repo>/lib/vtr.o3lib; read_blif <in>; write_blif --vtr <tmp>/<name>.vtr.blif"`.
It then compares the result with one expected file:

- `<name>.vtr.blif`: the written file, byte for byte. The header's first line is matched by
  pattern only (plan 3E point (s)), so a version bump does not change the fixtures.
- `<name>.err`: a refusal. The refusal must leave no file, and its `odin3: error:` lines must
  match this file. The pass manager's trailers (`pass <p>: …`, `-p: command …`) are left out.

A written case then runs the round trip of spec §16.3 step 1 (as the 3E-1 final review's erratum
amends it): `W2 = write_blif --vtr (read_blif W1)`. When the W1 write legalised no name (no
`names legalised` info line), W1 == W2 byte for byte. Otherwise W1 and W2 are compared under the
VB-10 decode (continuations joined, comment lines dropped, every name decoded to a fixed point),
and the byte comparison is not applied: VB-10 escapes the `%` of its own escapes again (`\` ->
`%5C` -> `%255C`). The driver prints a `note` line for such a case (`shiftx.parmys`).

Last, `blif-vtr-check --strict` runs on every expected `.vtr.blif`. This step needs `--checker`,
which 3E-2 adds. `--write` writes the expected file of a case that has none; it never overwrites
one. Every expected file in this directory must belong to a case. The inputs run smallest first.

Each expected file was checked by hand when it was added; see the rows below.

## Copied goldens (`sample.txt`, plan point (r))

These inputs stay in `tests/golden/blif/`, which is read-only for 3E. `<name>` is the basename
without `.blif`.

| case | expected | why |
|---|---|---|
| `adder_hard_block.parmys` | `.vtr.blif` | Review Focus 1. It is the golden itself once ` \` continuations are joined and the header is set aside: the `gnd`/`vcc`/`unconn` generators; the dummy chain heads `.subckt adder a=vcc b=gnd cin=gnd cout=… sumout=…`; every formal in the golden's order (`a b cin cout sumout`); and the stub as declared (`.inputs a cin b`). |
| `ansiportlist_2.parmys` | `.vtr.blif` | The `adder` and `multiply` stubs as declared, sorted `adder` then `multiply`. The `multiply` formals are `a[0..35] b[0..35] out[0..71]` with Parmys's `unconn` on the unused inputs. The rest is identical to the golden in the same sense. |
| `ff.parmys` | `.vtr.blif` | `.latch … re clk 2`. Identical to the golden. |
| `dffsre.parmys` | `.err` | E1b for both `$_DFFSR_PPP_` cells (point (e)): an async flip-flop arriving as a `.subckt` (Review Focus 5). |
| `pow.parmys` | `.err` | E5 for the word cell `$pow`. |

## Hand-written cases (`<name>.in.blif`)

| case | expected | why |
|---|---|---|
| `shiftx.parmys` | `.vtr.blif` | A copy of the Parmys k6 golden `regression/verilog/common/shiftx/shiftx.parmys.blif` (it cannot join `sample.txt`: `tests/golden/blif/` is not 3E's). One of the 25 Parmys k6 goldens with a mid-name `\` (`$2\buffer[7:0][k]`), which VB-10 rule 1 writes `%5C`: 8 names legalised. The expected file is the golden in that sense, with `unconn` kept on the `.names` inputs (the §9.2 warning). Its round trip is compared under the VB-10 decode, not byte for byte; `tests/tools/test_vtr_fixtures.py` pins both (spec §16.3 step 1, erratum). |
| `two_clock` | `.vtr.blif` | Two `.latch` lines on different clocks. Unchanged (Parmys's form; `stereovision3` has two clocks). |
| `ram_slice_unconn_addr` | `.vtr.blif` | Review Focus 2. This is a declared `single_port_ram` slice that uses 5 of the 15 address bits. `addr[5]` … `addr[13]` are on Parmys's `unconn` (class U) and `addr[14]` is on a net nothing drives (x). All of them are written `=unconn`, an open pin in VPR. The input's formals are unsorted; the output sorts them (§8.2). |
| `ram_one_bit_slice_scalar` | `.vtr.blif` | An undeclared 1-bit RAM slice read from `data[0]=`/`out[0]=`. It is written with scalar `data=`, `out=` and `we=`, in both the formals and the library stub (§8.2, §8.3; spec risk "scalar against indexed formals"). |
| `multiply_narrow` | `.vtr.blif` | An undeclared 8×8 `multiply`. Only `a[0]` … `a[7]` and `b[0]` … `b[7]` are written (bits beyond the node's width stay open). The stub has the instance's widths, in library order `a b out`. |
| `odin2_init3_cont` | `.vtr.blif` | Odin II's form: latch init `3` is written `2` (init `0` is kept). The input's `\` continuations, with no blank before the backslash, are joined by the reader. The writer rewraps them at 100 columns with ` \`. |
| `const_output` | `.vtr.blif` | Review Focus 4: constants on output ports, each with one driver. A zero-input `.names` on the port is kept as it is, with no buffer. The constant cover `.names a y1` / `- 1` becomes the generator `.names y1` / `1`. Parmys's buffer from `vcc` is unchanged. |
| `x_output` | `.vtr.blif` | An output nothing drives, and an output buffered from a net nothing drives. Both read x as `gnd` through a buffer, and the `gnd` generator is emitted. |
| `ram_dual_port_clock` | `.vtr.blif` | A `dual_port_ram`, whose formals are given in library order, is written sorted as Parmys sorts them: `addr1[*] addr2[*] clk data1 data2 out1 out2 we1 we2`. Its stub is in library order. A `single_port_ram` whose `clk` is the constant `gnd` is written `clk=gnd`; the writer warns about it, and the warning is not compared. |
| `subckt_unconnected_outputs_named` | `.vtr.blif` | Two adders whose `cout` nobody reads. Each gets its own `$p<pin ID>` net, so every output bit is written and the block names are distinct (R12). |
| `neg_e1_fe` | `.err` | E1: an `fe` latch. |
| `neg_e2_ah` | `.err` | E2: an `ah` latch. |
| `neg_e3_untyped` | `.err` | E3: an untyped latch (`$_FF_`, no clock). |
| `neg_e3b_ram_clk_open` | `.err` | E3b: a RAM's `clk` is on a net nothing drives. |
| `neg_e5_word` | `.err` | E5: the word cell `$add`. |
| `neg_e6_implicit` | `.err` | E6: an implicit black box. |
| `neg_e7_wide` | `.err` | E7: a 7-input `.names` (K = 6). |
| `neg_e8_mixed` | `.err` | A cover that mixes on-set and off-set rows. **The BLIF reader refuses it first**, so the line is the reader's. E8 itself needs hand IR and is pinned by `test_blif_vtr` (`test_e8_mixed_cover`). |
| `neg_e10_hier` | `.err` | E10: the top instantiates another model. |
| `neg_e12_multidriver` | `.err` | E12: two `.names` drive `y`. **The BLIF reader refuses it first** (PHASE3 #36), so the line is the reader's, in every build. E12 itself needs hand IR and is pinned by `test_blif_vtr` (`test_e12_one_cell_twice`). |

Four E-classes have no fixture, and `test_blif_vtr` pins each: E4 (no BLIF form: a tri-state
cell), E9 (an inout port: BLIF has none), E11 (a hard cell whose parameters its formals do not
give back: `read_blif` infers them from the formals, so only hand IR makes one) and E13 (no top:
`read_blif` always sets one, and the driver runs no `.o3` scripts on hand IR).

`<name>.debug.err` is written by hand; `--write` never writes it. No fixture has one now. When it
is the file that matches, the driver prints a `note` line saying so. Two cases never reach the
writer, and their E-classes are pinned by `test_blif_vtr` instead: `neg_e8_mixed` and
`neg_e12_multidriver` (the reader refuses both).

The driver's own negative test is `tests/golden/blif-vtr-corrupt/`, run by CTest
`blif_vtr_fixtures_detects_corruption`.
