# tristate pass: OpenCores re-runs (2026-11-04)

Branch `feat/tristate-pass`. Command line of the OpenCores intake (`work/opencores-runs/bin/oc-run.sh`, k6_frac 40nm arch,
`--vtr-compat`, 6-LUT) with `tristate;` added after `flatten` (script: `work/tristate-runs/run.sh NAME`; outputs under
`work/tristate-runs/<NAME>/`). VPR seed 1, default channel-width search.

| design | before | after (stage reached) | .names (6-LUT) | latches | CPD (VPR, ns) |
|---|---|---|---|---|---|
| i2cslave | read_verilog stops: tri-state on `sda` of an inout port below the top (D3) | `write_blif --vtr` and VPR done | 199 | 139 | 2.59 |
| sdram_16bit | `write_blif --vtr`: `sdram_data_io` inout, VPR has no bidirectional pins | VPR done; `sdram_data_io` split to `sdram_data_io_i` (see note 1) | 229 | 133 | 4.30 |
| sdr_ctrl | read_verilog stops: `'sdr_dq' has two drivers` (`{DW{1'bz}}` not seen as z) | VPR done; `sdr_dq` split to `sdr_dq_i/_o/_oe` | 1757 | 1130 | 5.55 |
| a-z80 | read_verilog stops: internal tri-state nets (D3) | map stops in `lower` (see note 2) | - | - | - |

1. sdram_16bit takes the `SDRAM_TARGET == "XILINX"` branch, whose `IOBUF` is an implicit black box that
   `--auto-blackbox` infers with all ports as inputs; nothing reads or drives the data bus inside, so the split
   gives only the input `sdram_data_io_i` (WARNING says so) and the 16-bit data path is not in the netlist.
2. a-z80: the 8 internal data buses and the 8-bit pin `D` convert (59 WARNINGs, `D` split to `D_i/D_o/D_oe`), but the
   two buses `db1`/`db2` are joined by `data_switch` (`db_up`/`db_down`, each a tri-state of the other). Converting
   them to logic gives a combinational loop (tried: ABC reported cut points without end), so the pass leaves those
   tri-states with a WARNING (`not converted: its data is the output of another tri-state`) and `lower` refuses with
   `'$tribuf' '$c6655' (output net 'db2') ... tri-state bus`. A bidirectional switch needs a net-level (wired) model.

Rules chosen (agent defaults, Peter may override): a single driver drops the enable (Yosys `opt_expr -mux_undef`, so
Parmys); several drivers become one `$pmux` per net bit with A = x and the last driver first (`tribuf -logic`); the
enables are not proved exclusive and the WARNING says two enabled at once is a bus conflict (OR of the data in `lower`).
Parmys leaves several drivers on one net in this case, which the IR cannot hold, so the `$pmux` form is followed.
