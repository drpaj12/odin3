# 3C Parmys probes (spec 9.2, plan Task 1)

Micros that settle three questions about Parmys before `hardmap`'s rules are frozen. Each runs through
Parmys (`tools/run-oracle.sh --tool parmys --golden <scratch> --name X X.v
k6_frac_N10_frac_chain_mem32K_40nm.xml`, never into `~/odin3-ws/golden`) and then
`tools/hardmap-check/parmys-probe SRC.v TOP PARMYS.blif OUTDIR [--cycles N --warm N --mask port=0xH
--corner]`: the BLIF is wrapped as Verilog (hard blocks are `vtr_flow/primitives.v` instances, `unconn`
is the BLIF's own constant-0 `.names`), an iverilog testbench drives it and the source with the same
random inputs, inputs change after each clock edge and outputs are sampled before the next. `--warm 16`
writes 16 words first so a RAM holds defined data. The CTests `parmys_probe_*` (label `sweep`; need
`VTR_ROOT` and iverilog) run these legs through `tools/hardmap-check/probe-leg` and check the exit
code in the last column.

| Micro | What it probes | Probe command (options) | Result | Exit |
|---|---|---|---|---|
| `ramsplit.v` | R-7: 16-bit-address `single_port_ram`, depth split | `--cycles 2000 --warm 16 --mask addr=0x000f` | 468 mismatches in 2000 cycles, first at cycle 23 | 1 |
| `ramsplit.v` | control: `addr[0]` held at 0 | `--cycles 2000 --warm 16 --mask addr=0x000e` | 0 mismatches | 0 |
| `subeq.v` | control: result as wide as the operands | `--cycles 3000` | 0 of 3000 | 0 |
| `subwide.v` | A-8: 9-bit result of an 8-bit `$sub` | `--cycles 3000` | 4478 mismatches in 3000 cycles; cycle 0: `uy9` ref `180`, blif `080` (borrow bit lost); `sy9` ref `053`, blif `153` | 1 |
| `subprobe.v`, `subprobe2.v` | unequal operand widths, signed and unsigned | `--cycles 3000` | `subprobe` 4538 mismatches; `subprobe2` 5252 (cycle 0: `uy` ref `005`, blif `015`) | 1 |
| `addrmerge.v` | R-8 address merge (plan agent default d) | `--cycles 2000 --warm 16` | 56 mismatches in 2000 cycles, first at cycle 40 (`out` ref `b4`, blif `80`) | 1 |
| `addrmerge.v` | control: `we` held at 0 after the warm-up | `--cycles 2000 --warm 16 --mask we=0x0` | 0 mismatches | 0 |

## What they show

- **D4 is recorded (R-7).** In Parmys's BLIF for `ramsplit` the output is
  `(addr[0] & S.out) | (~addr[0] & H.out)` with the live `addr[0]`, while `out` is the RAM's registered
  read of the address at the last clock edge. When `addr[0]` changes after a read, the mux picks the
  other half. With `addr[0]` held constant there is no mismatch. `hardmap` registers the select
  (one `$dff` per split level and read port).
- **D2b is recorded (A-8).** `subeq` (result width equal to the operand width) is right: the top stage
  is only the most significant sum bit. With a wider result (`subwide`, `subprobe*`) Parmys's top stage
  is `adder a=unconn b=unconn`, so the borrow bit `Y[n]` is lost (`uy9` 0x180 against 0x080), and the
  stages above a narrower B take `b=unconn`. `hardmap` builds `a + ~b + 1` with `b = vcc` above the
  operand (A-5, A-8). Caveat: the probe reads `unconn` as 0, as ABC and `equiv-check` do. VPR treats it
  as an open pin; the hardware meaning of an open adder input is not checked here.
- **D12 stands (R-8, agent default d).** For `addrmerge` Parmys keeps the read address unregistered on
  the RAM (`addr1 = ra`, the RAM registers it) and does not model the transparency that Yosys's
  `memory_dff` gives an address-merged read next to a write on the same clock. The RTL reads the memory
  after the write of the same edge (new data); the read-first RAM returns the old word. The 56
  mismatches are the same-address write/read cycles (about 1 in 32); with `we` held at 0 there are
  none. So `hardmap` applies the R-8 address merge only when the `$mem` has no write port on the read
  clock; otherwise the read port stays asynchronous (R-9). The data merge is unaffected.

The probes' stimulus is `$random` with the default seed; the mismatch counts are reproducible for a
given iverilog. The first line of a mismatching run is `cycle N: <port> ref <v> blif <v>`.
