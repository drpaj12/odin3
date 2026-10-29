# odin3_flow.py

`tools/flow/odin3-flow` (a wrapper for `tools/flow/odin3_flow.py`) takes a design through Odin III
and, when VTR is available, VPR. It uses only the Python standard library.

```text
odin3-flow design.v [more.v ...] [--top TOP] [--target vtr|lpm] [--arch ARCH.xml]
           [--family NAME] [--vpr | --no-vpr] [--analysis] [--prov] [--timing-attr]
           [--timing-paths N] [--timing-legacy] [--odin3 PATH] [--out DIR]
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
| `--prov` | write the BLIF with hash names and the version 2 name map `odin3/<top>.blif.names` next to it (`read_verilog --prov-ast refs`, `write_blif --name-style hash --prov-map`): every net's records, full source spans and construct chain (the format and what the records mean: {doc}`provenance`). VPR reads that BLIF. A step `prov` checks the map; `vtr` target only |
| `--timing-attr` | attribute the critical path to HDL statements, modules and words: writes `timing/attribution.json` and `timing/summary.txt` (see {doc}`timing-attribution`); needs `--target vtr` and VPR. It uses the name map, so it implies `--prov`: no second BLIF is written. **Behaviour change since version 1.1.1:** VPR now reads the hash-name BLIF, so the net names in VPR's own files (`.net`, `.place`, `.route`, its reports and logs) are hash names (`h3k9...`) where they were the short names (`$n12`). The CPD, LUT count, channel width and wirelength do not change (the tests check the CPD and LUTs on both routes); only the names do. A script that reads net names out of VPR's files of a `--timing-attr` run must use `--timing-legacy` (the way back to the old names and the old second BLIF), or look the hash up in `<top>.blif.names` |
| `--timing-paths N` | setup paths VPR reports and the attribution uses (default 100); implies `--timing-attr`. VPR gets `--timing_report_npaths N` only when N is not 100 |
| `--timing-legacy` | with `--timing-attr`: the version 1.1.1 route (a short-name BLIF for VPR plus a second BLIF with provenance names, line-level spans, no chain) instead of the name map |
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

**Run record.** Every run writes `mimir-run.json` (`record_version` 1, kind `odin3-flow-run`) into
`--out`: the how-obtained data that Mimir III imports as a `full` flow configuration without
reconstruction. It holds the tool commits (Odin III, VPR from `VTR_ROOT`, and the ABC commit the
Odin III binary states) with how each was found and whether the tree was clean, the sha256 and
`--version` text of each binary, the exact command line of every step (paths as placeholders such as
`<out>`, `<vtr>` and `<top>`, so one setup has one command text for every design), every parameter as
`{value, conf, source}` (the Odin III pass options given in the script, the initial value of each
pass's other options cited `file:line` at the source tree's commit, VPR's own echo of its effective
options, the VPR default seed cited from `read_options.cpp`), the time and peak RSS of each step, and
the sha256 of the design files, the architecture and the artifacts. The host is the alias from
`$MIMIR_HOST_MAP` (`unknown` without the map; the real name is never written). A commit that the
binary cannot be shown to come from (the Odin III `--version` text names no commit) is recorded
`unknown`, so Mimir marks the configuration `partial` and names that parameter. A record that would hold an
absolute path, a home path, the user name or the host name is not written (a line on stderr names the
rule and the key). The format is Mimir's `docs/specs/2026-10-28-mimir-run-record.md`; the flow does
not import Mimir.

Exit codes: 0 all steps passed, 1 a step failed (the `timing` step too), 2 usage error.

`--keep-results` keeps VPR's `.net`, `.place` and `.route` in `vpr/run` (the flow deletes them by
default). `odin3-prov-post FLOW_DIR [--map FILE] [--blif FILE] [--run-dir DIR] [--net FILE] [--place
FILE] [--top NAME] [--census FILE [--census-net-sha256 HEX]] [--words FILE] [--arch FILE] [--attribution FILE] [--pack-objective TEXT]` then extends the version 2 name map of a `--prov --vpr --keep-results` run with
the BLE, CLB and site records of VPR's packing and placement, one `A` line per atom and the `Q` and `K` lines of
the logic CLBs (why they were joined, how critical; {doc}`outputs`; exit codes: 0
ok, 1 refused or failed, 2 usage error); see {doc}`outputs` and {doc}`provenance`. Give `--run-dir` when a route retry left
two `vpr/` directories with a `.net`.

`odin3-timing FLOW_DIR [--paths N] [--out DIR]` attributes a flow output directory that was made
with `--timing-attr` without rerunning anything (same exit codes); {doc}`timing-attribution` has the
outputs and the JSON schema.

**Names do not depend on where the design lives.** `odin3` runs with its working directory set to the
common parent directory of the design files, and each design is passed relative to it. The file names in hash
names and in the name map are therefore relative to that directory: one design in two different
directories gives byte-identical BLIF and name map. The line `name_root=. designs=...` in
`odin3/odin3.log` records the relative names. Output, architecture and log paths stay absolute.
If the designs sit under unrelated directories the common parent is `/`: the names then carry the
path minus its leading slash, so they depend on where the files live, and the flow prints a
`WARNING` (put the designs under one directory to avoid it).

The `vtr` target reuses the Odin III script of the QoR flow (`tools/vtr-qor/qor_legs.py`), so the
script here and the benchmark runs are one definition. The module can be imported as well:
`odin3_flow.run_flow(FlowArgs) -> FlowResult`, with `tools/vtr-qor` on `PYTHONPATH` (the wrapper
sets it).
