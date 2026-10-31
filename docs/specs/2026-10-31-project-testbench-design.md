# Project testbenches (`testbench` key, qsf import, `run_testbenches`)

Status: v1 spec (2026-10-31), written before code. Brief: controller briefs/odin3-project-tb.md
(Peter 2026-10-31: "Is there a way in the project to define test benches and how they are run if
simulation is done", answer: not yet; approved: `.o3proj` key, qsf import, a pass).
Purpose: Leman Russ trigger testbenches, Loki behavioural checks, VTR benchmarks without a
testbench (testbench-gen), all described once in the project and run by one pass.
Measure: each testbench ends pass, fail or error; output equality is judged against iverilog
(oracle) on the golden project. Builds on the `sim_tb` simulator (spec
`2026-10-31-testbench-design.md`, merged in #308). Wyrd Bifrost testbench mode is a later lane.

## 1. The `.o3proj` key

```
testbench <name> file <path> [top <module>] [dut <inst-or-module>] [max-time <T>]
          [expect <path>] [on rtl|netlist|both]
```

One statement per testbench, several per project. After `<name>` come keyword/value pairs in any
order, each at most once; `file` is required. A statement is one line (the `.o3proj` rule).

- `<name>`: an identifier (`[A-Za-z_][A-Za-z0-9_$]*`), unique in the project. It names the
  testbench in the report and for `run_testbenches --only`.
- `file <path>`: the testbench Verilog source (the `sim_tb` subset), relative to the project file,
  must exist (`missing_file` like every path).
- `top <module>`: the testbench module in that file (`sim_tb --top`). Default (agent default PT-1):
  the first module of the file, as `sim_tb`.
- `dut <inst-or-module>`: documents and checks the device under test: the testbench must contain an
  instance whose instance name or module name equals this word, else the testbench ends `error`
  ("dut `x` not instantiated"). It does not select the design module (the simulator drives every
  instance of the testbench by its module name). Without `dut` there is no check. (PT-2)
- `max-time <T>`: a decimal count of testbench time units (the unit of the file's `` `timescale ``,
  `sim_tb --max-time`); default the `sim_tb` default (10,000,000). 0 is refused.
- `expect <path>`: a golden output file, must exist. The testbench passes only when its `$display`
  text equals the file line by line (section 4).
- `on rtl|netlist|both` (default `rtl`, PT-3): which design the testbench drives (section 3).

Errors are the project's located errors (`file:line:col: error: syntax: ...`): a missing `file`, a
repeated keyword, an unknown keyword (`unknown_option`), a bad `on` value, a duplicate name, a bad
count. Usage line: `testbench <name> file <path> [top <module>] [dut <inst-or-module>]
[max-time <T>] [expect <path>] [on rtl|netlist|both]`.

Record: `odin3_project.testbenches` (vec of `odin3_proj_testbench`: name, file, top, dut,
max_time, expect, on, location). Dump: a `"testbenches"` array between `overrides` and `flow`,
written only when the project has at least one testbench (PT-4: the 60 corpus records are
unchanged). Each element: `{"name", "file" (resolved), "top" (or null), "dut" (or null),
"max_time" (or null), "expect" (resolved or null), "on"}`.

## 2. qsf import

Quartus stores a simulation test bench as a group of assignments (the `-section_id` is the
test bench name):

```
set_global_assignment -name EDA_NATIVELINK_SIMULATION_TEST_BENCH tb1 -section_id eda_simulation
set_global_assignment -name EDA_TEST_BENCH_NAME tb1 -section_id eda_simulation
set_global_assignment -name EDA_TEST_BENCH_MODULE_NAME tb_top -section_id tb1
set_global_assignment -name EDA_TEST_BENCH_RUN_SIM_FOR "1 us" -section_id tb1
set_global_assignment -name EDA_TEST_BENCH_FILE tb.v -section_id tb1
set_global_assignment -name EDA_DESIGN_INSTANCE_NAME dut -section_id tb1
```

(No qsf in the repository or in `external/` holds these names; the forms above are the Quartus
Prime assignment names and the usual layout, written from the Quartus documentation as known to
the author; the unit tests use hand-written files of that shape. Agent default PT-5.)

Mapping, resolved after the file (the order of the lines does not matter):

| assignment | testbench field |
|---|---|
| `EDA_TEST_BENCH_NAME <n>` (any section) | declares testbench `<n>` |
| `EDA_TEST_BENCH_FILE <f>` (`-section_id n`) | `file` (path relative to the `.qsf`) |
| `EDA_TEST_BENCH_MODULE_NAME <m>` | `top` |
| `EDA_TEST_BENCH_RUN_SIM_FOR "<n> <unit>"` | `max-time`, unit fs/ps/ns/us/ms/s converted to units of the testbench's `` `timescale `` (read from the file; none found: 1 ns, WARNING); a non-integer result is rounded down with a WARNING; `0 <unit>` means no limit (default) |
| `EDA_DESIGN_INSTANCE_NAME <i>` and `EDA_TEST_BENCH_DESIGN_INSTANCE_NAME <i>` | `dut` (`NA` or empty: none) |
| `EDA_NATIVELINK_SIMULATION_TEST_BENCH <n>` | accepted; declares `<n>` when no `EDA_TEST_BENCH_NAME` did (Quartus says which test bench NativeLink runs; here every declared testbench is in the record) |

`on` is `rtl` (Quartus RTL simulation) and there is no `expect`. WARNINGs (not silent): a test
bench with no `EDA_TEST_BENCH_FILE` (dropped); a second `EDA_TEST_BENCH_FILE` for one test bench
(the first file is used, the extra ones are not run); an `EDA_TEST_BENCH_*` or
`EDA_NATIVELINK_SIMULATION_TEST_BENCH` assignment in a section that is no declared test bench;
every other `EDA_TEST_BENCH_*` name (for example `EDA_TEST_BENCH_ENABLE_STATUS`); a bad
`RUN_SIM_FOR` value. Warnings go through `odin3_log` (WARN) with the `.qsf` line. The names are
not added to the project's `ignored` list (they are reported as warnings instead). The test
bench record is added only when it has a file.

## 3. `run_testbenches` and the meaning of `on`

```
run_testbenches [--only <name>] [--json <file>] [--report <file>] [--seed N]
```

Needs a finished project (a `read_project` first; none: error "no project was read"). Runs every
testbench of the project in listing order, or the one named by `--only` (unknown name: error).
No testbench in the project: prints "run_testbenches: the project defines no testbenches" and
passes. Exit: the pass fails (CLI exit status nonzero) when any testbench is `fail` or `error`.
The pass does not change the design in hand.

`on` (stages stated):
- `rtl`: a scratch design is built from the project record, independent of the design in hand:
  `read_project <entry> [--revision r]; read_verilog; proc; opt; lower` (the RTL after the
  front end, `proc`, `opt`, and `lower`, the stage the `sim_tb` spec calls RTL). The testbench
  drives that design. The design in hand may be empty. (PT-6)
- `netlist`: the design in hand, at whatever stage the script reached when `run_testbenches`
  runs, which should be after synthesis (`abc`, mapping) or after `read_blif`. A design in hand
  without modules is an `error` ("no design in hand: run synthesis or read_blif first").
- `both`: run `rtl` and `netlist`, then compare the two output texts line by line (section 4).
  Pass only when both runs finish and the texts are equal (and equal to `expect` when given).

A testbench passes when the run finishes without error and every comparison it has (`expect`,
`both`) matches. A testbench with neither runs only for a clean finish (self-checking testbenches
report through their `$display` text; PT-7: the pass does not parse it for "FAIL").

## 4. Result record, comparison, report

Per testbench: `status` (pass, fail, error), `on`, `wall_ms` (wall time of all its runs),
`end_time` (simulation time of the last run, testbench units), `lines` (output lines of the first
run), and for a fail the first mismatch:

- `compared`: what differed (`expect` or `rtl vs netlist`, and for `expect` with `both` which side);
- `line`: 1-based line number of the first differing line (a missing line counts: one text ending
  early mismatches at the first line the other has);
- `time`: the first field of the expected (or first) line when it is a decimal integer (the
  generated-testbench convention `cycle out1 out2 ...`, the `testbench_gen` and `tools/tb-compare`
  rule), else null. This is the simulation time or cycle the line was printed at when the
  testbench prints it first;
- `field`: 0-based index of the first space-separated field that differs (named by position; a
  `$display` text carries no signal names, PT-8: this replaces the brief's "signal");
- `want`, `got`: the two lines (`want` = expect file or the rtl run; `got` = the tested side).

For an `error`: `message` (the first line of the error the failed step logged is not captured;
the record names the step, e.g. "parse tb.v", "elaborate project (rtl)", "run (netlist)", and the
status the step returned).

Report (stdout, always): one line per testbench, `name status on wall_ms`, a line `  mismatch:
<compared> line N [time T] field F: want "..." got "..."` for a fail, `  error: <step>: <status>`
for an error, then `run_testbenches: P passed, F failed, E errors`. `--report <file>` writes the
same text to a file. `--json <file>` writes the records:
`{"testbenches": [{"name", "status", "on", "wall_ms", "end_time", "lines", "mismatch": null |
{"compared", "line", "time", "field", "want", "got"}, "error": null | "<step>: <status>"}],
"passed": P, "failed": F, "errors": E}`. Output files are written atomically (`odin3_pass_write_text`).

## 5. Entry points

CLI: `odin3 -p "read_project p.o3proj; ...; run_testbenches"`; the exit status is the pass status.
Project flow step: a `flow <script.o3>` named by the project is stored but not run by Odin III
(the project record only stores it); a flow script that runs the project calls `run_testbenches`
as any pass. Python: `design.run("run_testbenches --json out.json")` (a pass is a script command,
the cffi binding needs no new function); it raises on a nonzero status.

## 6. Tests and gates

(a) Unit tests `test_project_testbench`: o3proj parse (all keywords, defaults, errors, dump
text), qsf mapping (every assignment of section 2, time conversion, each WARNING). (b) `tests/tb/project` golden project `tbproj` with a counter DUT and testbenches: `good` (an
`expect` file made by iverilog), `bad` (a deliberately wrong `expect` file: fail, mismatch line
reported), `both` (`on both`: the scratch RTL against the design in hand, which the script builds
with `read_verilog; proc; opt; lower; abc`), `err` (an unsupported construct: error).
`tests/tb/run_project.sh` (ctest `tb_project`) checks the exit status, the report lines and the
JSON, and compares the `good` and `both` outputs with `iverilog -g2005 && vvp` (the oracle).
(c) the JSON schema is checked inside `run_project.sh` (python3 json), not by a separate
unittest file (see section 7).

## 7. Agent defaults and deviations

PT-1 `top` optional. PT-2 `dut` is a check, not a selector. PT-3 default `on rtl`. PT-4 dump key
only when non-empty. PT-5 qsf names as in section 2, unverified against a real qsf. PT-6 RTL is
a scratch design built through the pass manager. PT-7 no output parsing for failures. PT-8
`field` index instead of `signal`. PT-9 `max-time` is a 64-bit count (not a `count` word).

Deviations as built (2026-10-31):
1. No `tests/tools/test_project_testbench.py`: the JSON fields are asserted in `run_project.sh`.
2. An `error` record holds `<step>: <status>` only (`run (rtl): ODIN3_ERR_INVALID_ARG`); the reason
   (for example `tb_err.v:2: unsupported: function`) is in the log lines before the report.
3. In a mismatch `want` is the left text: the `expect` file, or the rtl run for `rtl vs netlist`;
   `got` is the run text. A text that ends first shows `<end of output>`.
4. `both` with `expect`: the rtl text is compared with the netlist text; if equal, the rtl text
   is compared with `expect` (so a side is not named in `compared`).
5. `--seed N` is the one `$random` seed for every testbench of the run.
6. The golden `both` test builds its netlist side with `abc --lut 6` after `lower`.
7. The error `dut` check runs after parsing the testbench; a `dut` that is neither an instance
   nor a module name of the testbench is `error` with the log line naming it.
8. Files: `src/passes/run_testbenches.c`, `run_testbenches_report.c` (comparison and reports),
   `src/frontends/project/qsf_tb.c`; tests `tests/unit/test_project_testbench.c`,
   `tests/tb/run_project.sh` with `tests/tb/project/`.

Gate results: see the PR description.
