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
| `two_clock` | `.vtr.blif` | Two `.latch` lines on different clocks. Unchanged (Parmys's form; `stereovision3` has two clocks). |
| `ram_slice_unconn_addr` | `.vtr.blif` | Review Focus 2. This is a declared `single_port_ram` slice that uses 5 of the 15 address bits. `addr[5]` … `addr[13]` are on Parmys's `unconn` (class U) and `addr[14]` is on a net nothing drives (x). All of them are written `=unconn`, an open pin in VPR. The input's formals are unsorted; the output sorts them (§8.2). |
| `ram_one_bit_slice_scalar` | `.vtr.blif` | An undeclared 1-bit RAM slice read from `data[0]=`/`out[0]=`. It is written with scalar `data=`, `out=` and `we=`, in both the formals and the library stub (§8.2, §8.3; spec risk "scalar against indexed formals"). |
| `multiply_narrow` | `.vtr.blif` | An undeclared 8×8 `multiply`. Only `a[0]` … `a[7]` and `b[0]` … `b[7]` are written (bits beyond the node's width stay open). The stub has the instance's widths, in library order `a b out`. |
| `odin2_init3_cont` | `.vtr.blif` | Odin II's form: latch init `3` is written `2` (init `0` is kept). The input's `\` continuations, with no blank before the backslash, are joined by the reader. The writer rewraps them at 100 columns with ` \`. |
| `const_output` | `.vtr.blif` | Review Focus 4: constants on output ports, each with one driver. A zero-input `.names` on the port is kept as it is, with no buffer. The constant cover `.names a y1` / `- 1` becomes the generator `.names y1` / `1`. Parmys's buffer from `vcc` is unchanged. |
| `x_output` | `.vtr.blif` | An output nothing drives, and an output buffered from a net nothing drives. Both read x as `gnd` through a buffer, and the `gnd` generator is emitted. |
| `subckt_unconnected_outputs_named` | `.vtr.blif` | Two adders whose `cout` nobody reads. Each gets its own `$p<pin ID>` net, so every output bit is written and the block names are distinct (R12). |
| `neg_e1_fe` | `.err` | E1: an `fe` latch. |
| `neg_e2_ah` | `.err` | E2: an `ah` latch. |
| `neg_e3_untyped` | `.err` | E3: an untyped latch (`$_FF_`, no clock). |
| `neg_e5_word` | `.err` | E5: the word cell `$add`. |
| `neg_e6_implicit` | `.err` | E6: an implicit black box. |
| `neg_e7_wide` | `.err` | E7: a 7-input `.names` (K = 6). |
| `neg_e8_mixed` | `.err` | A cover that mixes on-set and off-set rows. **The BLIF reader refuses it first**, so the line is the reader's. E8 itself needs hand IR and is pinned by `test_blif_vtr` (`test_e8_mixed_cover`). |
| `neg_e10_hier` | `.err` | E10: the top instantiates another model. |
| `neg_e12_multidriver` | `.err`, `.debug.err` | E12: two `.names` drive `y`. A Debug build checks the IR after every pass (CLAUDE.md rule 3), so `check` refuses the input at `read_blif` before the writer sees it. The driver therefore also accepts `<name>.debug.err`, which holds that `check` line. CI's CTest runs the Debug build; the Release CLI gives the E12 line. |

`<name>.debug.err` is written by hand; `--write` never writes it. It exists only for this case.
