# S1: simulation API (per-cycle stepping in the C ABI and Python), `sim --vectors-in`, `sim --vcd`

Status: v1 (2026-10-21), draft for Peter's review of the ABI (decision 4). Post-v1.0 core change
(CLAUDE.md "Where things are going": "a per-cycle simulation call ... gets its own spec and PR").
Plan: `docs/plans/2026-10-21-s1-sim-api-plan.md`. Builds on 1E (`docs/specs/2026-10-09-1E-sim-design.md`).

## 0. Goal, consumers, decisions

Goal: let a program drive the existing netlist simulator one clock cycle at a time, through the
public C ABI and the cffi Python binding, and give the `sim` pass input-vector replay and VCD
waveforms.

Consumers: Wyrd W3 builds `wyrd.sim` (a Python session layer for the GUI simulation panel); W4
Verilog Town (pygame-ce) calls one simulation step per game tick on small sequential Verilog
designs (traffic-light controllers: car sensors in, lights out). Target: at least 60 steps per
second from Python on such designs, with a few signal reads and writes per step. Each step is one
C call; each read or write is one C call on a cached signal ID, so the target is met by a wide
margin (test `python_sim` `test_steps_fast_enough`).

Decisions (Peter, 2026-10-21):

| # | Decision |
|---|---|
| 1 | Per-cycle stepping in the C ABI (`include/odin3/odin3.h`) and the Python binding (`plugins/python/odin3.py`): open a simulation of one top, set inputs by name (and bit vectors), step one clock, read outputs and named internal nets, reset, close. Errors as `odin3_status`. Ownership rules for returned memory. |
| 2 | The `sim` pass also reads an input vector file in the `odin3-sim-vectors` format (`src/sim/vectors.h`); random PRNG inputs stay the default. |
| 3 | `sim --vcd out.vcd`: IEEE 1364 VCD output, signal names from the HDL, hierarchy as VCD scopes. |
| 4 | Opus writes the spec and the ABI; Peter reviews the ABI PR. |

Agent defaults taken in this spec (Peter may override; listed again in section 9):
D-1 one step ticks every clock of the top together (section 2); D-2 hierarchical names use `.`;
D-3 the public handle is a new type `odin3_simulation`, not the internal `odin3_sim`;
D-4 `ODIN3_ABI_VERSION` stays 4 (additive change, as 2D, 3D and 7.4b did);
D-5 passes are refused while a simulation of the design is open; D-6 no VCD through the ABI in S1.

## 1. What is simulated, and what the caller runs first

The simulator (1E, `src/sim/`) is a 2-state, cycle-based simulator over the **netlist view**. It
expands every module instance of the chosen top into flat cells over shared value slots (one byte
per net bit), levelizes once, and evaluates each cell through its type's `simulate` hook
(`src/sim/cell.h`). Hooks exist for: the bit gates, constants, `$sop`, `$lut`, the bit-level
storage cells (`$_DFF_P_`, `$_DFF_N_`, `$_DLATCH_P_`, `$_DLATCH_N_`, `$_FF_`, the `$_DFFE_*`,
`$_DFF_*` with reset, `$_SDFF*` families of `src/ir/cells/dffx.c`), the word cells `$and $or $xor
$not $xnor $logic_* $add $sub $mul $neg $eq $ne $lt $le $gt $ge $mux $pmux $reduce_*`, and
tech-library hard cells (`read_techlib`; the hook interprets the cell's `fn` expressions). There
is **no** hook for the word-level storage cells (`$dff`, `$adff`, `$sdff`, `$dlatch`, ... in
`src/ir/cells/ff.c`), memories (`$mem`, `$memrd`, `$memwr`), shifts, `$div`, `$mod`, `$pow`,
`$tribuf`, `$proc`, or black boxes without semantics.

So a Verilog design must reach the netlist view first. The caller runs:

```
read_verilog design.v; proc; opt; lower
```

(`read_verilog` elaborates and picks the top; `lower` turns every word cell, flip-flops and
memories included, into bit cells.) Verified on 2026-10-21 with the release build: `read_verilog;
proc; opt; sim` fails with "cannot simulate `$sdff` ... the cell type has no simulate hook";
adding `lower` simulates. A BLIF, JSON or gate-level netlist (`read_blif`, `read_json`) needs no
pass. `flatten` is not needed (the simulator expands the hierarchy itself) but is allowed.

Rules inherited from 1E, unchanged: values are 0/1 (constant x/z drivers read 0; latch INIT 2/3
start at 0); every clock pin of an edge-triggered cell must be driven by a top-level input port
(a derived or gated clock, such as a register used as a slow clock, is rejected at open with
"the clock of ... is net ..., which is not a primary input"; designs use clock enables instead);
an inout port of the top, a combinational loop or two ordinary drivers on a net are rejected; the
flattening budget is `ODIN3_SIM_DEFAULT_MAX_CELLS` (2^24).

**SC-1 Word-cell hint.** When the build rejects a cell of granularity `ODIN3_GRAN_WORD` for
having no simulate hook, the message ends "the cell type has no simulate hook; run `lower` first"
(was "... simulate hook"). Black boxes keep "a black box has no semantics"; other granularities
keep the old text.

## 2. What a step is

**SC-2 One step = one cycle of every clock of the top, ticking together.** This is exactly the
existing `odin3_sim_cycle`: settle the combinational logic with the inputs as set; raise every
clock input and send the rising edge to every edge-triggered cell; settle; lower every clock and
send the falling edge; settle. Outputs and internal values read after the step are those after the
last settle.

Justification (D-1): (a) it is what the `sim` pass, `tools/sim-check` and the 1E reference
testbenches already do, so a step sequence driven through the ABI produces the same values as the
vector file of the same inputs (`test_sim_abi` `test_matches_sim_pass_vectors` checks this); (b) Verilog Town designs have one clock;
(c) independent clocks need a per-clock edge set and a schedule of relative phases, which no
consumer needs yet. A later additive function (`odin3_simulation_step_clock(sim, signal)`) can
tick one named clock without changing anything defined here. A design with no clock input steps
too (the three settles run; useful for combinational designs).

**SC-3 Settle without a clock.** `odin3_simulation_settle` runs one settle only, so a caller sees
the combinational effect of inputs it just set without advancing state.

**SC-4 Reset is power-on.** `odin3_simulation_reset` returns the simulation to its state right
after open: every value and state bit 0 (inputs included, clocks low), every sequential cell gets
the INIT event again (`INIT` 1 starts at 1, others at 0), one settle, cycle counter 0. It is not
the design's reset input, which is an ordinary input the caller sets and steps.

## 3. The C ABI (decision 1)

All in `include/odin3/odin3.h`, inside the cdef block, a new group `--- simulation (S1) ---`
after the ABC engine group (the last one before `ODIN3_CDEF_END`), every function with its doc paragraph (purpose, ownership, failure
modes). Implemented in `src/api/sim_abi.c` over the internal simulator.

### 3.1 Types

```c
typedef struct odin3_simulation odin3_simulation; /* opaque */

typedef enum odin3_simulation_kind {
    ODIN3_SIMULATION_INPUT = 0,    /* a top input port (or one bit of it) the caller sets */
    ODIN3_SIMULATION_CLOCK = 1,    /* a top input port whose every bit is a clock: steps toggle it */
    ODIN3_SIMULATION_OUTPUT = 2,   /* a top output port (or one bit of it) */
    ODIN3_SIMULATION_INTERNAL = 3  /* any other wire or net, at any depth */
} odin3_simulation_kind;

typedef struct odin3_simulation_signal {
    const char *name;    /* full name: the scope's path, ".", the leaf; the leaf alone in scope 0 */
    const char *leaf;    /* the wire's or net's own name, or "name[i]" for a bit select */
    uint32_t width;      /* bits, >= 1 */
    uint32_t kind;       /* an odin3_simulation_kind value */
    int32_t msb;         /* declared range of the wire (a bit select: its index twice; a net: 0, 0) */
    int32_t lsb;
    uint32_t scope;      /* the scope it belongs to (0 = the top) */
    odin3_obj obj;       /* the wire (ODIN3_OBJ_WIRE) or net (ODIN3_OBJ_NET) in the scope's module */
    uint32_t first_bit;  /* vector position in obj of the signal's bit 0 (0 but for a bit select) */
} odin3_simulation_signal;

typedef struct odin3_simulation_scope {
    const char *name;    /* instance path from the top, "." separated; "" for scope 0 */
    const char *leaf;    /* instance name ("#<node ID>" when unnamed); the top module's name for 0 */
    uint32_t parent;     /* parent scope; 0 for scope 0 */
    uint32_t module;     /* the module this scope is an instance of */
    odin3_ref inst;      /* the instance node in the parent scope's module; {0, 0} for scope 0 */
} odin3_simulation_scope;
```

`obj` plus the existing `odin3_prov_visit_sources` gives a signal's HDL file and line; `inst`
gives the instance node. These keep the provenance and ID hooks of the graph and RE work intact.

### 3.2 Functions

```c
odin3_status odin3_simulation_open(odin3_design *design, uint32_t module, odin3_simulation **out);
void odin3_simulation_close(odin3_simulation *sim);
odin3_status odin3_simulation_get_scope_count(const odin3_simulation *sim, uint32_t *count);
odin3_status odin3_simulation_get_scope(const odin3_simulation *sim, uint32_t scope,
                                        odin3_simulation_scope *info);
odin3_status odin3_simulation_get_signal_count(const odin3_simulation *sim, uint32_t *count);
odin3_status odin3_simulation_get_signal(const odin3_simulation *sim, uint32_t signal,
                                         odin3_simulation_signal *info);
odin3_status odin3_simulation_find_signal(odin3_simulation *sim, const char *name,
                                          uint32_t *signal);
odin3_status odin3_simulation_set(odin3_simulation *sim, uint32_t signal, uint64_t value);
odin3_status odin3_simulation_set_bits(odin3_simulation *sim, uint32_t signal,
                                       const uint8_t *bits, uint32_t count);
odin3_status odin3_simulation_get(const odin3_simulation *sim, uint32_t signal, uint64_t *value);
odin3_status odin3_simulation_get_bits(const odin3_simulation *sim, uint32_t signal,
                                       uint8_t *bits, uint32_t count);
odin3_status odin3_simulation_settle(odin3_simulation *sim);
odin3_status odin3_simulation_step(odin3_simulation *sim, uint32_t cycles);
odin3_status odin3_simulation_get_cycle(const odin3_simulation *sim, uint64_t *cycle);
odin3_status odin3_simulation_reset(odin3_simulation *sim);
```

**SA-1 open.** Builds the simulation of `module` (0: the design's top). `*out` gets a handle the
caller owns and frees with `odin3_simulation_close`; NULL on any failure. Failures, all logged:
`ODIN3_ERR_INVALID_ARG` for a NULL design or out, a module ID that is not a module of the design,
module 0 when the design has no top ("odin3_simulation_open: the design has no top module"), a
design with processes (`odin3_design_refuse_processes(design, NULL)`: "module '<m>' has processes;
run proc first"), and every build rejection of section 1; `ODIN3_ERR_NO_MEMORY`. The design is
not changed. On success the simulation is in its power-on state (SC-4) and the design counts one
more open simulation (SA-11).

**SA-2 close.** Frees the simulation and every string it handed out; the design counts one fewer
open simulation. NULL is a no-op. It must be called before `odin3_design_destroy` of its design.

**SA-3 scopes.** One scope per expanded module instance, in expansion order: scope 0 is the top,
then each instance in the order the simulator expanded it (breadth first: a scope's instances in
node-ID order, appended after all scopes already listed). `get_scope` fills `*info`;
`ODIN3_ERR_INVALID_ARG` (logged) for a NULL argument or `scope >= count`.

**SA-4 listed signals.** Signal IDs start at 1 (0 is "none", as IR IDs). Signals 1..count are the
listed ones, in this order: the top's input ports in port order; its output ports in port order;
then for each scope in scope order: its module's live wires with a public name (named, not
starting with `$`, the rule of `src/passes/opt/clean.c`) in wire-ID order, skipping the top's own
port wires (listed already); then its module's live nets with a public name of their own and no
wire membership at all (no primary `(wire, bit)` and no wire alias), in net-ID order, width 1 (a
BLIF netlist names nets, not wires). The kind of a top input port is CLOCK when every bit of it
drives a clock, else INPUT. A child scope's port wires are INTERNAL. When two listed signals have
the same full name, `find_signal` returns the first.

**SA-5 find_signal.** `*signal` gets the ID of the signal named `name`, or 0 when the simulated
netlist has no such name (status `ODIN3_OK` in both cases, as `odin3_design_lookup_module`). The
lookup, in order:

1. a listed signal whose full name equals `name`;
2. a signal appended before (steps 3 to 5) under the same text: the same ID again;
3. **in the current scope** (scope 0 first), a bit select: `name` ends in `[<n>]` (`n` decimal,
   optional `-`), the text before it, the base, is a live wire of the scope's module, and the wire
   has a bit whose declared index (`odin3_wire_index`) is `n`: a new 1-bit signal, leaf
   `base[n]`, msb = lsb = n, `first_bit` that bit's vector position, and the kind of the bit: for
   a top input port INPUT (CLOCK when that bit is a clock), for a top output port OUTPUT, else
   INTERNAL;
4. **in the current scope**, a name: a live wire named `name` (any name, `$` included), else a
   live net whose name or bare alias name is `name` (`odin3_module_find_net`; IR-14: a merged net
   keeps every name it had). A new signal (a wire: its width and range; a net: width 1), kind as
   in step 3 for a top port wire, else INTERNAL;
5. a hierarchical name: for each `.` in the remaining text from the left, if the text before it is
   the name of a live instance node of the current scope's module that has a scope, enter that
   scope and resolve the text after the `.` by steps 3, 4 and 5 there. The first `.` that names an
   instance wins; the search never backtracks.

Steps 3 to 5 append the new signal (ID count + 1, + 2, ...) so later `get_signal`, `get` and
`set` take it; `get_signal_count` still returns the listed count. Bit selects come before names
so that a BLIF port bit net named `a[2]` resolves to the settable INPUT bit of port `a`. Because step 1 matches whole
names first, a flattened design (`flatten`, default separator `.`) and the unflattened one answer
the same names: `u0.inner` is a wire of the flat top or wire `inner` of scope `u0`.
`ODIN3_ERR_INVALID_ARG` (logged) for a NULL argument; `ODIN3_ERR_NO_MEMORY` when appending fails.

**SA-6 Names that are gone.** `lower` keeps every wire (it replaces cells and merges nets, and a
merge keeps the dropped net's names as aliases, IR-15), so every name of the RTL view is still
found after `lower` (verified on the fixtures: `state`, `timer`, `inner`, `mid` survive `proc;
opt; lower`). `opt` (opt_clean) deletes a public wire that nothing reads unless it has the `keep`
attribute (`(* keep *) reg r;`), and `abc` replaces logic regions; such a name is not found:
`find_signal` gives 0 (Python: `KeyError` with the hint of SP-3). The caller keeps a signal by
marking it `(* keep *)` or by simulating before the pass that removed it.

**SA-7 set / set_bits.** Sets an INPUT signal for the following settles and steps (it keeps its
value until set again). `set` takes the value LSB first in `value`, for width <= 64; `set_bits`
takes `count` bytes, `bits[i]` the value (0 or 1) of bit i, `count` equal to the width. A clock
bit inside an INPUT signal must be given as 0 and is left alone. `ODIN3_ERR_INVALID_ARG` (logged;
nothing changes) for: NULL arguments, signal 0 or unknown, a kind other than INPUT, width > 64
(`set`), a value with a bit set at or above the width (`set`), `count` not equal to the width, a
byte other than 0 or 1, a clock bit given as 1.

**SA-8 get / get_bits.** Reads any signal as of the last settle (open, reset, settle or step).
`get`: width <= 64, LSB first in `*value`, upper bits 0. `get_bits`: `count` equal to the width,
`bits[i]` = bit i. `ODIN3_ERR_INVALID_ARG` (logged; output untouched) for NULL arguments, an
unknown signal, width > 64 (`get`), a wrong count.

**SA-9 settle / step / get_cycle / reset.** `settle` runs SC-3. `step` runs `cycles` cycles of
SC-2 (0: nothing) and adds `cycles` to the cycle counter. `get_cycle` gives cycles stepped since
open or the last reset. `reset` runs SC-4. They allocate nothing and fail only with
`ODIN3_ERR_INVALID_ARG` (logged) for a NULL argument. Time per step is linear in the flattened
design.

**SA-10 Ownership.** The handle is the caller's (close it). Every `const char *` in a signal or
scope record is owned by the simulation, NUL-terminated, and valid until `odin3_simulation_close`
(appending signals never moves them). `set_bits` and `get_bits` use the caller's buffer only during
the call. Nothing else is returned.

**SA-11 The design while a simulation is open.** The simulation points into the design's module
storage (cell parameters) and names (string table), so the design must not change while it is
open. The library enforces it: the design counts its open simulations, and every pass and script
command (`odin3_design_run_pass`, `odin3_design_run_script*`, which reach the pass manager's
`run_def`) is refused with `ODIN3_ERR_INVALID_ARG`, logged "pass <name>: the design has <n> open
simulation(s); close them first (odin3_simulation_close)", until they are closed. Readers, string
attribute writes and `odin3_design_set_top_module` stay allowed (they move no module storage). A
copy (`odin3_design_copy`) starts with no open simulation. Several simulations of one design may
be open at once (different tops, or one top several times, such as one per Verilog Town
intersection); each has its own state. A design and all its simulations are used by one thread at
a time (IR-17). A refused call changes nothing (the keep level a `wants_ast` pass would raise
included). `odin3_design_destroy` needs every simulation closed first (a debug build asserts).

`odin3_ast_elaborate` and `odin3_design_new_ast` stay allowed while a simulation is open.
`odin3_design_new_ast` only adds an AST store, kept apart from module storage. `odin3_ast_elaborate`
(stage 2) only appends new modules (a taken name gets a `$2` suffix) and may set the design's top;
it never edits an existing module. An open simulation addresses modules by ID and reads strings
that never move (string table), so neither call moves anything it points to. A new top does not
change an open simulation: `odin3_simulation_open` fixed its module.

**SA-12 ABI version.** The change adds types and functions and changes no existing declaration,
so `ODIN3_ABI_VERSION` stays 5 (the gate-mapping ABI v5 already in this tree), following the
`(cont., 2D)`, `(cont., 3D)` and `(cont., 7.4b)` precedents; the header's history comment gains
"ABI v5 (cont., v1.0.3 S1): simulations (odin3_simulation_*): open a top, find signals, set inputs,
step, read values, reset, close; passes are refused while a simulation is open." The struct
`odin3_simulation_signal` ends in a zero-filled `reserved` word (size 56, no tail padding) and
`odin3_simulation_scope` is 32 bytes with no padding, so neither has a reserved slot. The refusal of SA-11 only affects programs that open a
simulation. The `abi_exports` test keeps the exported symbols equal to the cdef functions.

## 4. Python (decision 1)

**SP-1 Classes.** In `plugins/python/odin3.py`:

- `SignalInfo(NamedTuple)`: `id: int, name: str, leaf: str, width: int, kind: str` (`"input"`,
  `"clock"`, `"output"`, `"internal"`), `msb: int, lsb: int, scope: int, obj: tuple[int, int, int],
  first_bit: int`.
- `ScopeInfo(NamedTuple)`: `index: int, name: str, leaf: str, parent: int, module: int,
  inst: tuple[int, int]`.
- `Simulation(design: Design, top: str | None = None)`: opens (top by module name; None: the
  design's top; an unknown name is `ValueError`). Methods: `close()`, context manager, `signals()
  -> list[SignalInfo]` (listed), `scopes() -> list[ScopeInfo]`, `signal(name) -> SignalInfo |
  None`, `get(sig) -> int`, `set(sig, value: int) -> None` (`sig`: a name or a `SignalInfo`),
  `sim[name]` and `sim[name] = value` as `get` and `set`, `settle()`, `step(cycles: int = 1)`,
  `reset()`, property `cycle -> int`.
- `Design.simulate(top: str | None = None) -> Simulation`.

**SP-2 Values.** Python ints, unsigned, bit i of the int is bit i of the signal. Width <= 64 uses
`odin3_simulation_get`/`set`, wider uses the `_bits` calls. `set` with a negative value or one of
`2**width` or more raises `ValueError` before any C call. Strings are copied out at once.

**SP-3 Errors.** A failing C call raises `Odin3Error` (the library logged why). A name that
`find_signal` does not know raises `KeyError` whose message is "<name>: not in the simulated
netlist (a pass may have removed it: mark it (* keep *) in the HDL, or see Simulation.signals())".
Any method of a closed simulation raises `RuntimeError("simulation is closed")`.

**SP-4 Lifetime.** A `Simulation` holds its `Design`. Its C handle is wrapped by `ffi.gc` with a
destructor closure that also holds the design's `ffi.gc` pointer, so the design's
`odin3_design_destroy` can never run before the simulation's close, whatever order the garbage
collector frees them in. `Design.close()` first closes every simulation of the design still open
(the design keeps a `weakref.WeakSet` of them). `Design.run_pass` on a design with an open
simulation raises `Odin3Error` (SA-11).

**SP-5 Speed.** `signal(name)` results are cached per simulation (dict name -> SignalInfo), and
the `uint64_t *` out-parameter is allocated once per simulation, so a step and a cached read are
one C call each.

## 5. `sim --vectors-in` (decision 2)

**SV-1 Syntax.** `sim [--vectors-in <file>] [--vcd <file>] [<cycles> <seed>] [<file>|-]`. Options
may appear anywhere among the words, each at most once. Without `--vectors-in`, `<cycles>
<seed>` are required, as today. With it, they must be absent (cycles = the file's vector lines);
the last positional is the optional output vector file. Usage error (logged, `ODIN3_ERR_INVALID_ARG`):
"sim: expects <cycles> <seed> [<file>|-], or --vectors-in <file> [<file>|-]; option --vcd <file>
(decimal numbers)". "sim: --vectors-in replaces <cycles> <seed>" when both are given. An option
without its file name, or given twice, is the usage error.

**SV-2 Accepted file.** UTF-8/ASCII text, lines ended by LF (a CR before it is dropped), at most
2^28 bytes (`ODIN3_SIM_VECTORS_MAX_BYTES`). Blank lines (only blanks) are skipped anywhere.
A line starting with `#` is a header or comment line:

- the **title line**: the first `#` line, before any vector line, whose last four words are
  `seed <S> cycles <N>` (decimal, S <= UINT64_MAX, N <= UINT32_MAX) records S and N;
- **port lines**: `#` lines whose second word is `input`, `clock` or `output`; when the file has
  any, their sequence must equal, line for line, the port lines `odin3_sim_vectors_header` writes
  for the design (the title excluded; trailing blanks ignored);
- any other `#` line is a comment. After the first vector line every `#` line is a comment (the
  title is not looked for again), except that a port line there is refused, "<path>:<line>: a
  port line after the first vector line" (agent default S1-AD2).

A vector line is `<cycle> <inputs> [<outputs>]`, words separated by blanks or tabs: `<cycle>` is
the decimal count of vector lines before it (0, 1, 2, ...); `<inputs>` is every non-clock input
bit, port by port in port order, each port MSB first (exactly what the writer prints), as `0` and
`1`, or `-` when the design has no non-clock input bit; `<outputs>` is ignored (a file written by
`sim` replays as is). The header is optional, so a hand-written file needs only vector lines;
`sim 0 0 tmpl.vec` writes a header-only template.

Memory: the reader holds the whole file plus one byte per input bit (peak about 2x the file size
for a file of mostly input bits; the 2^28-byte limit bounds it). Error messages echo at most 80
characters of a word or line, then `...`.

**SV-3 Errors.** All `ODIN3_ERR_PARSE`, logged "<path>:<line>: <message>", nothing simulated and
no output written (the whole file is parsed before the first cycle):
"expected `<cycle> <inputs> [<outputs>]`" (no word, or more than three);
"expected cycle <k>, found `<word>`"; "expected <W> input bits of 0 and 1, found `<word>`";
"expected `-`: the design has no non-clock input bit"; "port line `<line>` does not match the
design: expected `<expected line>`"; "the header lists <a> port lines, the design has <b>" (at the
last port line); "the title says <N> cycles, the file has <M>" (at the last line). An unreadable
file is `ODIN3_ERR_IO`, "sim: cannot read `<path>`: <strerror>"; a file over the limit is
`ODIN3_ERR_PARSE`, "<path>: larger than 268435456 bytes".

**SV-4 Output.** The run drives line k's bits before cycle k. With an output vector file the
header is `# odin3 sim <top> seed <S> cycles <M>` (S from the title line, 0 without one; M the
vector lines), so `sim 32 5 a.vec; sim --vectors-in a.vec b.vec` makes `b.vec` byte-identical to
`a.vec` (CLI case `sim_vectors_in`). Without one, the summary is "sim: <top>: <M> cycles from <path>; last cycle:
<vector>".

## 6. `sim --vcd` (decision 3)

**SW-1 Content.** `--vcd <file>` writes an IEEE 1364 four-value VCD (values 0 and 1 only) of
every listed signal (SA-4) for the run, random or replayed, together with the vector output or
summary. `-` is refused ("sim: --vcd needs a file name, not `-`"). The file is written atomically
(`odin3_atomic_file_*`), and is a function of the netlist and the inputs (no date).

**SW-2 Header.**

```
$version odin3 sim $end
$timescale 1ns $end
$scope module <top module name> $end
$var wire <width> <code> <leaf>[ [<msb>:<lsb>]] $end      (one per signal of the scope)
$scope module <instance leaf> $end                         (child scopes, depth first)
...
$upscope $end
...
$upscope $end
$enddefinitions $end
```

Scopes nest as the instance tree (scope 0 outermost; children in scope-index order; built with an
explicit stack, no recursion). Within a scope, its signals in signal-ID order. The range is
written only for width > 1. Names (leaf of a signal or scope) are written with every byte outside
`!`..`~` replaced by `_`. Codes: the k-th listed signal (k from 0) gets k written in base 94 with
digits `!`..`~`, least significant digit first (`!`, `"`, ..., `~`, `!"`, ...).

**SW-3 Times.** One cycle is 10 time units. `#0` holds `$dumpvars` with every signal (the
power-on state, before any input is driven). Cycle c (from 0): the inputs are driven and settled
at `10c+2`; the rising edge and its settle at `10c+5`; the falling edge and its settle at
`10c+10`. So a clock reads 0 at 10c+2, 1 at 10c+5, 0 at 10c+10, as a testbench that drives inputs
2 units after the falling edge. Each time is written (`#t`) only when some signal changed;
then each changed signal: `0<code>` / `1<code>` for width 1, `b<bits MSB first> <code>` for wider
signals (all `width` digits).

**SW-4 Same values.** The VCD values at time `10c+10` equal the outputs the vector file prints on
line c (`sim_vcd_check` `test_vcd_matches_vectors`), and the cycle loop with VCD sampling computes exactly `odin3_sim_cycle`
(settle, rising edge + settle, falling edge + settle), so the vector file is byte-identical with
and without `--vcd`.

## 7. Internal structure

- `src/sim/eval.c`: `odin3_sim_settle`, `odin3_sim_clock_edge(sim, event)` (edge + settle) and
  `odin3_sim_reset` become internal API; `odin3_sim_cycle` = settle, edge(POS), edge(NEG).
- `src/sim/build.c`: the build keeps its frames and net map (final slots) in the simulator, for
  names.
- `src/sim/signals.{h,c}` (new): the scope and signal tables, listing (SA-4), lookup (SA-5),
  reads and writes (SA-7, SA-8). Shared by the ABI and the VCD writer.
- `src/sim/vectors.{h,c}`: the writer gains a line function (no drive, no cycle); the reader
  (SV-2, SV-3) is new.
- `src/sim/vcd.{h,c}` (new): SW-1 to SW-3.
- `src/api/sim_abi.c` (new): the ABI wrappers; `struct odin3_simulation` = design, simulator,
  signal tables, cycle counter.
- `src/ir/design.{h,c}`, `src/ir/ir_internal.h`: the open-simulation counter;
  `src/passes/manager.c`: the refusal (SA-11).
- `src/passes/sim_pass.c`: SV-1, SV-4, SW-1.

## 8. Tests (names; assertions in the plan)

Unit (C, `tests/unit/`): `test_sim_signals` (internal: listing, lookup, values, reset,
settle/edge equivalence, OOM), `test_sim_abi` (through `libodin3.so` only: SA-1 to SA-11),
`test_sim_vectors_in` (SV-2, SV-3), `test_sim_vcd` (SW-2, SW-3 text). CLI: `cli_sim_vectors_in`,
`cli_sim_vcd`. Python: `python_sim`, `sim_vcd_check`. Fixtures (new, `tests/sim/`): `traffic.v` (one clock, two
sensors, two 2-bit lights), `hier.v` (two instances of one stage, an internal wire, a register opt
deletes, a `(* keep *)` one), `netnames.blif` (named nets, no wires), `wide.v` (a 70-bit
register), `mixclk.v` (a 2-bit input port whose bit 0 is a clock). Reference values for `traffic.v` were taken from Icarus Verilog (section 10).
Independent check: `cli_sim_vcd` also compares the port values of our VCD with an Icarus run of a
generated testbench using the SW-3 timing when `iverilog` is found (skipped otherwise).

## 9. Decisions (Peter, 2026-10-21: yes to all; the plan follows them)

1. One step ticks every clock of the top together (D-1). Recommend: yes for S1; add
   `odin3_simulation_step_clock` when a multi-clock consumer appears.
2. ABI version (D-4): S1 alone is additive, but gate mapping (docs/specs/2026-10-21-gatemap-design.md)
   changes the ABC engine struct and needs 5. Decision: whichever of the two lands first sets
   `ODIN3_ABI_VERSION` to 5; the other keeps 5. The next release notes say ABI 5.
3. Passes refused while a simulation is open (D-5), rather than documenting "do not". Recommend:
   enforce; a pass that frees module storage under an open simulation is a use-after-free in
   Python.
4. VCD through the ABI (`odin3_simulation_vcd_open`) for Wyrd's "export waveform" (D-6).
   Recommend: not in S1; Wyrd records values per step. It is two additive functions over
   `src/sim/vcd.c` if wanted.
5. `sim --vectors-in --check` comparing the file's output field with the simulation. Recommend:
   not in S1; `tools/sim-check` already compares vector files.
6. Hierarchical separator `.` (D-2) rather than the provenance `hier` field's `/`. Recommend `.`:
   it is Verilog's, VCD viewers' and `flatten`'s default, so names agree before and after
   `flatten`.

## 10. Reference trace (`tests/sim/traffic.v`)

Icarus Verilog 2026-10-21, inputs set before each step, outputs after it (`light_ns light_ew`;
2 GREEN, 1 YELLOW, 0 RED). Odin III is 2-state, so right after open `state` = 0: `light_ns` = 2,
`light_ew` = 0 (Icarus shows x there).

| step | rst | car_ns | car_ew | light_ns | light_ew | state | timer |
|---|---|---|---|---|---|---|---|
| 1 | 1 | 0 | 0 | 2 | 0 | 0 | 0 |
| 2 | 0 | 0 | 1 | 1 | 0 | 1 | 1 |
| 3 | 0 | 0 | 0 | 1 | 0 | 1 | 0 |
| 4 | 0 | 0 | 0 | 0 | 2 | 2 | 3 |
| 5 | 0 | 0 | 0 | 0 | 2 | 2 | 2 |
| 6 | 0 | 0 | 0 | 0 | 2 | 2 | 1 |
| 7 | 0 | 0 | 0 | 0 | 2 | 2 | 0 |
| 8 | 0 | 1 | 0 | 0 | 1 | 3 | 1 |
| 9 | 0 | 0 | 0 | 0 | 1 | 3 | 0 |
| 10 | 0 | 0 | 0 | 2 | 0 | 0 | 3 |

## 11. Out of scope

4-state values, per-clock stepping, memories and other cells without a simulate hook (run
`lower`), forcing internal nets, VCD or vector replay through the ABI, `$dumpvars` scoping
options, timing.
