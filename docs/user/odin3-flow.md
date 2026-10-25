# odin3_flow.py

`tools/flow/odin3-flow` (a wrapper for `tools/flow/odin3_flow.py`) takes a design through Odin III
and, when VTR is available, VPR. It uses only the Python standard library.

```text
odin3-flow design.v [more.v ...] [--top TOP] [--target vtr|lpm] [--arch ARCH.xml]
           [--family NAME] [--vpr | --no-vpr] [--analysis] [--timing-attr] [--timing-paths N]
           [--odin3 PATH] [--out DIR]
```

| Option | Meaning |
|---|---|
| `designs` | Verilog files |
| `--top TOP` | top module; default is the one uninstantiated module. If the files hold more than one candidate the script stops with exit code 2 and names them |
| `--target` | `vtr` (BLIF for VPR, default) or `lpm` (Verilog with LPM cells plus a read-back lint) |
| `--arch ARCH` | VTR architecture XML (default `k6`); needs `VTR_ROOT`, without it K = 6 is assumed with a warning |
| `--family FAMILY` | LPM device family for `--target lpm` |
| `--vpr`, `--no-vpr` | run VPR after Odin III; default on when `VTR_ROOT` is set; `vtr` target only |
| `--analysis` | also write the dot, JSON, levelize and locations files; `vtr` target only |
| `--timing-attr` | attribute the critical path to HDL statements, modules and words: writes `timing/attribution.json` and `timing/summary.txt` (see {doc}`timing-attribution`); needs `--target vtr` and VPR |
| `--timing-paths N` | setup paths VPR reports and the attribution uses (default 100); implies `--timing-attr`. VPR gets `--timing_report_npaths N` only when N is not 100 |
| `--odin3 PATH` | the `odin3` binary; default `odin3` on `PATH`, else `build/release/odin3` |
| `--out DIR` | output directory; default `odin3_out/<top>` |

Every step runs in its own subdirectory of `--out` with its own log, and the first line of the
`odin3` log is the exact command. `summary.json` holds the LUT count and the step times (and the VPR
summary when VPR ran).

VTR routes once at 1.3 x the minimum channel width, and on a tiny device that width can be
unroutable while its neighbours route. When the minimum-width search finished and only that final
route failed, `odin3_flow` retries the route at fixed widths (relaxed + 2, relaxed + 4, then 2 x
minimum), each in `vpr/retry-<W>/`, and stops at the first success. It prints `NOTE: route at
W=<relaxed> failed (VPR); routed at W=<W>` and adds `route_retry` (`min_chan_width`,
`failed_width`, `used_width`) to `summary.json`; `min_chan_width` stays the true minimum. If every
width fails, the VPR step fails and its log lists the widths tried.

Exit codes: 0 all steps passed, 1 a step failed (the `timing` step too), 2 usage error.

`odin3-timing FLOW_DIR [--paths N] [--out DIR]` attributes a flow output directory that was made
with `--timing-attr` without rerunning anything (same exit codes); {doc}`timing-attribution` has the
outputs and the JSON schema.

The `vtr` target reuses the Odin III script of the QoR flow (`tools/vtr-qor/qor_legs.py`), so the
script here and the benchmark runs are one definition. The module can be imported as well:
`odin3_flow.run_flow(FlowArgs) -> FlowResult`, with `tools/vtr-qor` on `PYTHONPATH` (the wrapper
sets it).
