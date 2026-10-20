# 4.2 Parmys probes (spec X-4, R-2; plan Task 1)

Micros that settle two questions about Parmys before `partial_map`'s oversize multiplier split and RAM depth
split are frozen. Each ran through Parmys into a scratch golden root (a directory under the agent's own scratch,
never `~/odin3-ws/golden`):

```
export VTR_ROOT=~/odin3-ws/external/vtr-verilog-to-routing
K6=$VTR_ROOT/vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml
COFFE=$VTR_ROOT/vtr_flow/arch/COFFE_22nm/k6_frac_N10_4add_2chains_depop50_mem20K_22nm.xml
G=$(mktemp -d)
tools/run-oracle.sh --tool parmys --golden $G --name mul_40x40 tests/partial_map/probes/mul_40x40.v $K6   # and $COFFE
tools/run-oracle.sh --tool parmys --golden $G --name ram_a13  tests/partial_map/probes/ram_a13.v  $COFFE
```

Counting, on `<micro>.parmys.blif`: `multiply` and `single_port_ram` are the `.subckt` lines of that type;
`adder` is the number of `.subckt adder` lines; a chain is the set of `adder` cells linked `cout -> cin`, and its
length is in cells. A chain of `n` bits has `n + 2` cells (head, one stage per bit, top stage; 3C A-3).

## Multipliers (X-4): `multiply` count, `adder` cells and chain lengths in cells

| Micro | Arch (W) | `multiply` | `adder` cells | chains (cells each) |
|---|---|---|---|---|
| `mul_40x40.v` | k6 (36) | 4 | 87 | 45, 42 |
| `mul_40x40.v` | COFFE (27) | 4 | 96 | 54, 42 |
| `mul_40x8.v` | k6 (36) | 2 | 13 | 13 |
| `mul_40x8.v` | COFFE (27) | 2 | 22 | 22 |
| `mul_8x40.v` | k6 (36) | 2 | 45 | 45 |
| `mul_8x40.v` | COFFE (27) | 2 | 36 | 36 |
| `mul_80x80.v` | k6 (36) | 9 | 424 | 125, 82, 73, 53, 46, 45 |
| `mul_80x80.v` | COFFE (27) | 9 | 460 | 134, 82, 80, 55, 55, 54 |

## RAM depth (R-2): `single_port_ram` blocks of `ram_a13.v` (13 address bits, 1 data bit)

| Micro | Arch (D) | `single_port_ram` |
|---|---|---|
| `ram_a11.v` | COFFE (11) | 1 |
| `ram_a13.v` | COFFE (11) | 4 |

## What they show

- **The multiplier counts match the spec (X-4).** Four `multiply` for 40x40, two for 40x8 and 8x40, and nine for
  80x80 (a piece wider than W is split again: 3x3 on both archs). The split is by W, not by a fixed 36.
- **The adder count differs from the spec's "three adds" (amendment, spec v1.1).** For both-operands-oversize
  Parmys builds two adder chains, not three: 40x40 gives 2 chains. The cross-term add and the final add are
  sized by the zero fill of the low bits, so the chains are narrower than the full product. For one oversize
  operand it builds one chain, and its length is not symmetric: 40x8 and 8x40 differ (k6: 13 against 45 cells;
  COFFE: 22 against 36). Task 14 rebuilds exactly these chain lengths; the table above is its test oracle.
  Adder cells per case are fixed by the table, so a rewrite that gives other counts changes, not the table.
- **R-2 holds.** The depth split uses D = 11 on COFFE: a 13-bit RAM becomes 2^(13-11) = 4 blocks, an 11-bit RAM
  stays one.
