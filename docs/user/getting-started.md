# Getting started

Every command on this page was run from the repository root on Ubuntu 24.04; the output blocks
are cut to the lines that matter.

## Build

You need CMake 3.28 or later (the default `ODIN3_WITH_SLANG=ON` build needs it; configure
with `-DODIN3_WITH_SLANG=OFF` to build the core with CMake 3.25), Ninja, a C compiler (gcc or
clang), `bison`, `flex` and `libreadline-dev`. The ABC logic optimiser is a git submodule and
is built with Odin III.

```bash
git clone --recursive https://github.com/drpaj12/odin3.git
cd odin3
cmake --preset release
cmake --build --preset release
```

The binary is `build/release/odin3`. Put its directory on `PATH` for the rest of this page, or
pass `--odin3 build/release/odin3` to the flow script.

```bash
export PATH="$PWD/build/release:$PATH"
odin3 --version
```

```text
odin3 1.1.0 (ABI 6)
SystemVerilog via slang (adapters/slang, third_party/slang)
linked ABC a3001b72edc5de22442e942165487fddf150e3d0 (adapters/abc/NOTICE)
```

(The `debug` preset adds ASan and UBSan and is the one the test suite uses. The Python binding
needs the release build; see {doc}`python-api`.)

## Run one design

The micro benchmark `tests/micro/verilog/micro/bm_DL_2_4_encoder.v` is a 2-to-4 encoder written
as a `case` statement. The flow script `tools/flow/odin3-flow` takes it from Verilog to a BLIF
netlist for VPR. With `VTR_ROOT` unset it stops after Odin III and says so.

```bash
tools/flow/odin3-flow tests/micro/verilog/micro/bm_DL_2_4_encoder.v --analysis --out out/enc
```

```text
NOTE: VTR_ROOT is not set: VPR skipped (use --no-vpr to silence this)
step odin3: ok (0.1 s) log out/enc/odin3/odin3.log
step analysis: ok (0.0 s) log out/enc/analysis/analysis.log
  luts             4
```

The top module was found by itself (the one module nothing instantiates). The result:

```text
out/enc/summary.json
out/enc/odin3/bm_DL_2_4_encoder.blif      the netlist
out/enc/odin3/odin3.log                   the exact odin3 command and its log
out/enc/analysis/*.dot, *.odin3.json, locs.txt, levelize.txt
```

```text
.model bm_DL_2_4_encoder
.inputs W[0] W[1]
.outputs Y[0] Y[1] Y[2] Y[3]
.inputs En
.names W[0] W[1] En Y[0]
111 1
```

## What the script ran

The first line of `out/enc/odin3/odin3.log` is the `odin3` command line. It is the flow of
{doc}`flow`, one pass per stage:

```text
odin3 --diag-paths resolved --top bm_DL_2_4_encoder -p read_techlib lib/vtr.o3lib;
  read_verilog --nolatches --vtr-compat tests/micro/verilog/micro/bm_DL_2_4_encoder.v;
  proc; fsm; opt; flatten; opt -full; wreduce; hardmap; lower; abc --lut 6;
  write_blif --vtr --lut-k 6 out/enc/odin3/bm_DL_2_4_encoder.blif
```

(The log shows absolute paths and `;` separated on one line; the line breaks above are ours.)
You can run the same script yourself with `odin3 -p "..."`; the full list of passes and their
options is in {doc}`passes`.

## Other target: LPM Verilog

`--target lpm` writes structural Verilog with LPM cells (for an Altera-style flow) and checks it
by reading it back:

```bash
tools/flow/odin3-flow tests/micro/verilog/micro/bm_DL_2_4_encoder.v --target lpm \
    --family "Cyclone V" --out out/enc-lpm
```

```text
step odin3: ok (0.0 s) log out/enc-lpm/odin3/odin3.log
step readback: ok (0.0 s) log out/enc-lpm/readback/readback.log
```

## Next

- {doc}`flow` explains each stage.
- {doc}`odin3-flow` lists every option of the script and its exit codes.
- {doc}`analysis` shows how to look at the result as a graph and how to map it back to the HDL.
