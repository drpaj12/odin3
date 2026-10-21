# odin3_flow.py

`tools/flow/odin3-flow` (a wrapper for `tools/flow/odin3_flow.py`) takes a design through Odin III
and, when VTR is available, VPR. It uses only the Python standard library.

```text
odin3-flow design.v [more.v ...] [--top TOP] [--target vtr|lpm] [--arch ARCH.xml]
           [--family NAME] [--vpr | --no-vpr] [--analysis] [--odin3 PATH] [--out DIR]
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
| `--odin3 PATH` | the `odin3` binary; default `odin3` on `PATH`, else `build/release/odin3` |
| `--out DIR` | output directory; default `odin3_out/<top>` |

Every step runs in its own subdirectory of `--out` with its own log, and the first line of the
`odin3` log is the exact command. `summary.json` holds the LUT count and the step times (and the VPR
summary when VPR ran).

Exit codes: 0 all steps passed, 1 a step failed, 2 usage error.

The `vtr` target reuses the Odin III script of the QoR flow (`tools/vtr-qor/qor_legs.py`), so the
script here and the benchmark runs are one definition. The module can be imported as well:
`odin3_flow.run_flow(FlowArgs) -> FlowResult`, with `tools/vtr-qor` on `PYTHONPATH` (the wrapper
sets it).
