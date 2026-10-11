# Odin III IR

Status: v2, 2026-10-08. Written by the agent under PHASE1 decision #9 (Peter delegated the IR
decisions for Phase 1; he reviews afterwards and may override any of them); v2 applies a Fable
design critique (5 must-fix, 7 should-fix). Source of truth for everything in `src/ir/`. Spec:
`docs/DESIGN.md` §5, ADRs D1, D5, D7, D8′, D9. Each decision is numbered **IR-n** so a later
change can cite it.

When unsure about an invariant: stop and ask. Do not guess.

## 1. Shape of the IR

```
Design ─┬─ strtab (all names, file paths, pass names)            design-global
        ├─ prov store: records + pass-run table (§6)              design-global
        ├─ cell-type table (§5)                                   design-global
        └─ Module* ─┬─ nodes   (pagevec, AoS)                      module-local IDs
                    ├─ pins    (pagevec, AoS)
                    ├─ nets    (pagevec, AoS) ── each owns a pin-ID array: drivers first, then sinks
                    ├─ wires   (pagevec) ── named, ordered groups of nets
                    ├─ net aliases (side table: net → extra (wire, bit) memberships)
                    ├─ name maps (strtab ID → node / net / wire ID)
                    └─ attributes (side table); arena for small arrays
```

**IR-1 Every net is one bit.** A cell's port of width *w* is *w* consecutive pins, one per bit,
LSB first; a port is therefore a slice `{first pin, count}` of its node's pins, so word structure
stays cheap to see. Word-level and bit-level cells differ only in their cell type; the
connectivity model is the same (Odin II's). `lower`/`raise` are cell replacement with no net
splitting; slicing and concatenation are which nets a pin vector uses, so **`$concat` and
`$slice` are not cells** (spec §5.2 amended). Rejected: Yosys-style multi-bit wires with
SigSpec connections — every connection becomes a list of slices and every rewrite must split
them. The 2M-node spike (§10) shows the per-bit memory is a non-issue.

**IR-2 Wires are names, not connectivity.** A *wire* is a named, ordered vector of nets with a
range (`[msb:lsb]`, either direction) and a `signed` flag: the Verilog/RTLIL signal. Wires carry
names, ranges, signedness and port membership for front ends and writers; connectivity lives
only in pins and nets. A net has at most one **primary** `(wire, bit)` and any number of
**aliases** in the module's alias side table (usually empty; an alias is either a `(wire, bit)` membership or a bare net name): `assign b = a;`, `flatten` joining
`top.x` with `u1.p`, and `opt` merging equivalent nets all keep every name. A net may also carry
its own name (BLIF names every bit, e.g. `n~19`). Which name a writer prints is the writer's
policy (BLIF: net name first; Verilog: primary wire bit first), not the IR's. Aliases are kept
in insertion order, oldest first; `merge` appends drop's name, drop's primary, then drop's
aliases.

**IR-3 Module boundaries are port nodes.** Each module port is one node of a built-in cell type
`$port_in`, `$port_out` or `$port_inout` (granularity `port`) whose pins are the port's bits.
A `$port_in` pin drives its net; a `$port_out` pin sinks it; a `$port_inout` pin is both. So
every net driver is a pin and `check` has one driver rule. The module keeps its port nodes in
declaration order (the order of `.inputs`/`.outputs`, of instance pin vectors, of Verilog
headers). An output driven straight by an input is one net with a `$port_in` driver and a
`$port_out` sink; an output driven by a constant is a net driven by a constant cell.

**IR-4 Constants are cells.** Constant bits are driven by cells, never stored on pins. Front ends
use one shared node per value per module (`$_CONST0_`, `$_CONST1_`, `$_CONSTX_`, `$_CONSTZ_`,
one output pin each). The BLIF reader keeps a zero-input `.names` as the `$sop` cell it was
(round trip, PHASE1 #2). Cell types may supply a `const_value` hook (0, 1, x, z or "not
constant"), and `odin3_net_const_value(net)` asks the driver's type, so passes have one
predicate for "is this net constant" across `$_CONST*_` and constant `$sop`s.

## 2. IDs

**IR-5 Typed 32-bit IDs, 0 = none.** Each kind has its own struct type so the compiler rejects a
net ID passed as a node ID (and `bugprone-easily-swappable-parameters` stays quiet):

```c
typedef struct { uint32_t v; } odin3_node_id;     /* module-local */
typedef struct { uint32_t v; } odin3_pin_id;      /* module-local */
typedef struct { uint32_t v; } odin3_net_id;      /* module-local */
typedef struct { uint32_t v; } odin3_wire_id;     /* module-local */
typedef struct { uint32_t v; } odin3_module_id;   /* design-global */
typedef struct { uint32_t v; } odin3_celltype_id; /* design-global */
typedef struct { uint32_t v; } odin3_prov_id;     /* design-global */
typedef struct { uint32_t v; } odin3_passrun_id;  /* design-global */
/* names: uint32_t strtab IDs (0 = empty / no name), from util/str.h */
```

Slot 0 of every store is a reserved dummy so a zeroed field means "none". The C ABI (1D) hands
out the raw `uint32_t` plus the module handle.

**IR-6 IDs are stable until an explicit `compact`.** Deleting an object marks it dead; its slot
and ID stay, so IDs are stable across passes (spec §5.1) and dense in creation order. Iterators
skip dead objects. Because churn (opt; lower → abc → read-back loops) leaves stores mostly dead,
`compact` is part of the contract now:

- `odin3_module_compact(module) → old→new maps` per kind; renumbers live objects in ID order
  (so output order is unchanged) and frees dead slots;
- it invalidates every module-local ID held outside the IR, including C ABI handles; only the
  pass manager calls it, at named pipeline points (after `opt`, after each ABC read-back);
- before freeing, it appends one **tombstone** per dead node, net and wire (module, kind, cell
  type, name, prov; for a node also its parameters and the names of the nets its pins were last
  connected to, PHASE1 #14) to a design-global tombstone table, so history keeps pointing at what existed
  (§6).

Provenance never refers to object IDs, so compaction cannot break lineage.

## 3. Objects

Field lists are the contract; exact C layout is the implementer's, behind accessors (IR-15).

**Node** — cell type; name (str, optional); prov; first pin and pin count (a node's pins are
allocated together, contiguous IDs, in port order then bit order); parameter values (§5); flags
(dead). A node's pin count is fixed by its type and parameters at creation; changing a width
means replacing the node. `odin3_node_port(node, port) → {first pin, count}`.

**Pin** — owning node; port index and bit index within that port; net (0 = unconnected). A pin
stores no provenance: its provenance is its node's (pins are created and die with their node).
Direction comes from the cell type's port (`in`, `out`, `inout`).

**Net** — name (str, optional); prov; primary wire and bit (optional); flags (dead); the **pin
array**, partitioned: driver pins (`out`, `inout`, and pins of `tristate` types) in
`[0, driver_count)`, sink pins after. Removal swaps with the last element of its partition (and
moves the partition boundary when a driver leaves), so `odin3_net_driver(net)` is O(1) and
"fanout of net" is a contiguous slice. `inout` pins sit in the driver partition only; "readers"
of a net are its sinks plus its `inout` pins; `odin3_pin_reads(pin)` tests one pin. Pin arrays are blocks from a per-module size-class pool
(capacities 2, 4, 8, …; freed blocks go to a free list per class); append is amortized O(1).
Order within a partition is not significant and never reaches output.

**Wire** — name (str); prov; range (msb, lsb); `signed`; the ordered vector of its net IDs; port
node (optional, for wires that are module ports).

**Module** — name; the cell type that instantiates it (§4); the stores above; port nodes in
order; name maps; alias table; attributes; a per-module arena for small arrays (parameter
vectors, wire net vectors, attribute values); the count of its live processes (IR-19).

**Design** — strtab; prov store and pass-run table; tombstone table; cell-type table; modules in
creation order; top module (optional, 0 = none).

## 4. Hierarchy

**IR-7 Instances are nodes.** Creating a module registers a cell type of granularity `module`
whose ports mirror the module's port nodes (names, directions, widths, scalar flags, signs,
order). An instance is a
node of that type; pin *k* of port *p* corresponds to bit *k* of the module's *p*-th port node.
Hierarchy stays until an explicit `flatten` pass (spec §5.1). Changing a module's ports after
instances exist is an error in Phase 1 (`module_add_port` refuses once the module's cell type is
instantiated).

**Port range and sign** (2D IR change C: 2D spec EL-24, §6.2; PHASE2 #13). A port carries the
range and sign of its declaration, as IR-2 says every wire does. `odin3_port_spec` has
`has_range`, `msb`, `lsb` and `is_signed`:
- Without `has_range`, the port's wire is `[width−1:0]`, the Phase 1 form, and `msb`/`lsb` are
  ignored. Readers that know no range (BLIF) are unchanged.
- With `has_range`, the wire is `[msb:lsb]` in either direction (`[0:7]` stays ascending, `[-1:2]`
  keeps its negative bound), and the port's width is derived as `|msb − lsb| + 1`; `width` 0 asks
  for that width. A port has 1 .. 2^31 bits either way.
- Two refusals, both `INVALID_ARG` with nothing changed: a nonzero `width` that differs from the
  derived width, and `scalar` with a range wider than one bit (`scalar` with `[5:5]` is a one-bit
  port).
- `is_signed` applies in both forms: the port wire is signed, and the module's cell type records it
  in that port's `odin3_port_def.is_signed` (false on every other type), for writers and for the
  elaborator's output extension (2D EL-10).
- `scalar` with a one-bit range other than `[0:0]` (`[5:5]`) is accepted and keeps its range in
  the IR and JSON, but writers that honour `scalar` (Verilog, BLIF) write the port without an
  index, so the index does not survive a round trip through them. EL-24 sets `scalar` only for a
  port declared without a range, so the elaborator never makes one.
- Writers name a port's bits by declared index, as Yosys does: `input [0:7] a` is `a[7] … a[0]`
  in BLIF (`.inputs`, and the formals of an instance), and `output [-1:2] n` is
  `n[2] n[1] n[0] n[-1]`.

The sign is internal to the core: the ABI's `odin3_plugin_port`, whose layout `odin3.h` freezes,
does not carry it, and the ABI stays at v4. A port's range and sign are fixed at creation (2D
rejected a wire-range mutation after creation: it would leave the cell type's port record without
the sign). `check` rule 9 ties the cell type's port to its wire: same width, same sign, and a
scalar port is one bit (2D Task 2 review). Ranges are not compared: the cell type records none.

**Parameterized modules:** each distinct parameter specialization (`sub #(.W(8))`) is its own
Module and cell type (the front end names them deterministically, e.g. `sub$W=8`); the instance
node keeps the source-level parameter values as attributes so writers can print them.

**IR-7b Black boxes.** A black box (BLIF `.blackbox`, a `(* blackbox *)` module, a library cell
without a body) is a cell type of granularity `blackbox` with declared ports and no body. If a
reader meets a black-box declaration whose name is **already registered** (e.g. the golden
`.model adder .blackbox` when the 1G VTR library has registered `adder` as `hard`), it checks
the declaration for port compatibility and reuses the registered type; a mismatch is a reader
error. Compatibility (1G): the same port names, matched **by name in any order** (Yosys+Parmys
lists one model's ports in a different order from file to file), the same directions, and the
same widths, where a registered width may be **parametric**: each INT parameter that is the
width parameter of a port takes that port's declared width (the largest, if several ports name
it), the other parameters keep their defaults, and then every port's width rule (constant,
parameter, function or expression) evaluated with those values must equal its declared width
(`odin3_celltype_blackbox_match`, quiet, for readers that report the reason themselves). Either
way the design records that the file *declared* that model (declared-model list, the declaring
reader run is recorded from 1C on): the cell type, the parameter values the declaration implies
(`odin3_design_declared_model_params`), and the declaration as written — its ports in declared
order with their widths and scalar flags (`odin3_design_declared_model_decl`) — so the BLIF writer
reproduces the `.model … .blackbox` stanzas in their original order and spelling (PHASE1 #2).
Ports are scalar or vector: a reader groups formals `a[0] … a[w-1]` into vector port `a` of
width *w*, and the port records whether it was written with brackets so writers reproduce `a[k]`
versus `a` byte-for-byte (PHASE1 #2). Instances use the spelling of the declaration (Odin II
writes a width-1 library port as `cin[0]`, Yosys+Parmys as `cin`).

**Parameters of a `.subckt` (1G).** BLIF cannot give parameters, so a reader derives them: an
instance of a model the file declares gets the declaration's parameter values; an instance of a
registered type the file does not declare gets the values its formals imply — each port's width
seen as its largest bit index + 1 (`p[k]`; a built-in `$` word cell's bare `P`, Yosys's spelling
of a 1-bit connection, is `P[0]`), inferred as above (`odin3_celltype_infer_params`;
types whose port widths are not parameters keep their defaults). Only a parameter that is some
port's width parameter is inferred: one used only inside width expressions keeps its default, and
a declaration it contradicts is reported as a port-width mismatch. Readers cap the widths their
input alone decides at 2^20 bits per port (`ODIN3_READER_MAX_WIDTH`) and 2^22 bits per cell
(`ODIN3_READER_MAX_TOTAL_WIDTH`): a tech-library cell's ports, and the ports of an undeclared BLIF
`.subckt` of a registered type (inferred or constant); declared widths and modules of the file are
bounded by the file. A writer can therefore write a cell only when its parameters are the ones a reader would
derive from what it writes; it writes a declared type's formals in the declaration's port order.

## 5. Cell types (op registry)

**IR-8 One table entry per type, one file per built-in type** (`src/ir/cells/<name>.c`), as
spec §5.2. An entry declares:

- `name` (Yosys spelling where one exists: `$add`, `$_AND_`, …), `granularity`, flags
  (`tristate`: output pins may legally share a net with other tristate/inout drivers);
- ports: name, direction, scalar/vector, and a width rule — a constant, or the name of an
  integer parameter (`A_WIDTH`), or a function pointer for anything else, or (1G) a compiled
  width expression over integer parameters (`A_WIDTH + B_WIDTH`) supplied by the tech library:
  the IR calls its `check` hook at registration and its `eval` hook to size ports, never parses
  it;
- parameters: name, kind, default. Signedness of word-level operands is a parameter
  (`A_SIGNED`, `B_SIGNED`), as in Yosys;
- hooks, each may be NULL: `verify` (consistency beyond widths), `const_value` (IR-4),
  `simulate` (1E), writer hooks (1C/1F).
- per type, a local type may carry opaque tech-library data (1G: `odin3_celltype_set_lib`; the
  cell's `fn`/`seq`/`memory` functions, port modifiers, area/delay) for 1E and Phase 4.
- simulation flags (1E): `ODIN3_CT_SEQ_EDGE` (edge-triggered storage whose outputs depend only on
  its state, so it cuts the simulator's combinational graph: `$_DFF_P_`, `$_DFF_N_`, `$_FF_`),
  `ODIN3_CT_SEQ_LEVEL` (a level latch, transparent while enabled; it does *not* cut the graph, so
  a loop through one is a combinational loop in Phase 1: `$_DLATCH_P_/N_`), `ODIN3_CT_CLOCK_PIN0`
  (port 0 is the clock or enable pin: a scalar input of constant width 1). At most one SEQ flag;
  registration rejects both together, and CLOCK_PIN0 on a type whose port 0 is not such a pin.
  Only the clock pins of SEQ_EDGE cells must come from primary inputs; a latch enable may come
  from logic.

**The 1E hooks.** `simulate(const odin3_sim_cell *cell)` evaluates one cell; NULL means the type
cannot be simulated. Its contract is in `src/sim/cell.h`:
- values are 2-state, one byte per net bit;
- the view `odin3_sim_cell` has one index span per port, the node's parameters, `state` (one byte
  per output bit of a SEQ type), `event`, `type_data` and a shared `scratch` buffer;
- `type_data` is the type's tech-library data, looked up once per type (NULL for built-ins); it
  is how a hard cell's hook interprets its `fn` expressions;
- events: `COMB` in every settle; for SEQ types also `INIT` (once, state zeroed: set it from the
  parameters, INIT 1 starts at 1, 0/2/3 at 0) and `POSEDGE`/`NEGEDGE` (copy inputs into state and
  write no outputs, so visit order does not matter);
- a hook never allocates, logs or fails.

`sim_scratch_bytes(const odin3_sim_cell *sizing_view, uint32_t *bytes)` (may be NULL: no scratch)
tells the builder how much scratch one cell needs. It is called once per flat cell at build time,
with a sizing view: port spans carry widths but no indices, `params` and `type_data` are set, and
`values`, `state` and `scratch` are NULL. It may allocate (build time only) and returns a status:
ODIN3_ERR_INVALID_ARG (the builder reports "cannot simulate … too wide") or
ODIN3_ERR_NO_MEMORY. The builder allocates one scratch buffer, the maximum over all cells, so
hooks never allocate.

This signature replaced the planned `(params) → size` (agent decision, PHASE1 #9; for Peter's
review), for two reasons:
- the sizing view hands the hook the port widths the IR already computed from width rules and 1G
  width expressions, so a hook never re-derives widths from parameters;
- the status lets a type refuse a cell with a located message instead of clamping silently.

Layering: `src/ir/cells/cells.h` includes `sim/cell.h` and `sim/word.h`, because the built-in
types' simulate hooks live in their cell files. The IR layer therefore depends on these two
headers, which are value types and inline helpers only with no link dependency. `src/sim`
depends on the IR, not the other way round at link level.

**IR-9 Granularity tags:** `word`, `bit`, `hard`, `blackbox`, `module`, `port` (D1's four plus
the two structural tags; D1 amended). Views (spec §5.4): the *RTLIL view* allows `word`,
`hard`, `blackbox`, `module`, `port`; the *netlist view* allows `bit`, `hard`, `blackbox`,
`module`, `port`. Mixed is legal only inside `lower`/`raise`. Constant cells (`$_CONST*_`, flag `anyview`) are legal in every view: word-level
front ends and bit-level netlists both use them. The process cell `$proc` (granularity `word`,
IR-19) is legal in the RTLIL view and never in the netlist view, so `lower` cannot meet one: `proc`
(2E) lowers processes first.

**IR-10 Parameters vs attributes.** Parameters are typed values a cell type declares, stored per
node in declaration order. Attributes are free-form `name → value` on any object (Verilog
`(* … *)`, pragmas, reader extras such as BLIF `.cname`), kept in a per-module side table keyed
by (object kind, ID). Value kinds: `int` (int64), `bits` (4-state 0/1/x/z, any length),
`string`, `cover` (an SOP cover: rows of `0`/`1`/`-` input literals plus an output value,
kept as written), and (2D) `proc` (`ODIN3_VAL_PROC`: a process body, the `BODY` of a `$proc` node,
IR-19). A `proc` value is one blob in the module arena, like a cover's rows; copies, comparisons and
tombstones treat it as bytes, and only the IR reads it (`src/ir/proc.h`). Only the built-in `$proc`
declares a parameter of this kind (registration refuses it elsewhere, IR-19), and an attribute
never holds one (`odin3_attr_set` refuses it).
Reading: `odin3_attr_get(module, obj, key)` for one key; `odin3_attr_foreach(module, obj,
visit, ctx)` visits every attribute of one object in the order its keys were first set
(overwriting a key keeps its place; compact keeps the order), so writers emit attributes
deterministically. *(1F Task 4: agent-decided IR API addition, read-only; for Peter's review.)*

**BLIF cells (used by 1C).** `.names` → `$sop` (bit granularity; *N* inputs, 1 output;
parameter `COVER`; zero inputs allowed). `.latch` → `$_DFF_P_` (`re`) / `$_DFF_N_` (`fe`) with
clock, or `$_DLATCH_P_` (`ah`) / `$_DLATCH_N_` (`al`), each with an `INIT` int parameter 0–3
(BLIF: 0, 1, 2 = don't care, 3 = unknown; 2 and 3 both round-trip). A latch with no type or
clock (`.latch in out [init]`) is `$_FF_` (global clock); `as` is rejected in Phase 1 (no golden
uses it). `.subckt` → a node of the named type (IR-7b).

**IR-11 Registration.** Built-in and plugin cell-type *definitions* live in a process-global
registry (built-ins from a static table compiled from `src/ir/cells/`; plugins add theirs at load
through the C ABI, 1D). Each design's cell-type table instantiates every registered definition at
creation, then adds module and black-box types as they appear (IR-7, IR-7b). Names are unique per
design; registering a different type under an existing name is an error (black-box redeclaration
follows IR-7b).

**IR-19 Processes are `$proc` nodes** (2D IR change B: 2D spec EL-33, §8.1; PHASE2 #13). An
`always` block (and each process 2D builds for a continuous-context call or a mem2reg read) is one
node of the built-in word-level cell type `$proc` (`src/ir/cells/proc.c`): its connectivity is
ordinary pins and its decision structure (Yosys's `RTLIL::Process`) an opaque parameter. Nothing
in IR-3, IR-4, IR-6, IR-15 or check rule 4 changes.

- **Ports.** `I` (input, width `I_WIDTH`): every net the process reads and does not drive (action
  right sides, switch signals, compare operands, sync signals) and the constants it uses, as pins on
  the module's shared `$_CONST0_/1/X/Z_` nets (IR-4). `O` (output, width `O_WIDTH`): every net it
  drives. So `odin3_net_driver` of a process-driven net is its `O` pin, the sinks of a net it reads
  include its `I` pin, `odin3_net_const_value` and fanout walks work unchanged, and a second driver
  (another process, a cell) is rule 4's error as for any cell.
- **Each net on at most one `I` pin and at most one `O` pin of one `$proc`, never on both**
  (reading a net the process drives is `OUT | k` in the body, not a self-loop pin). This is a
  property of how 2D builds nodes (EL-35), not a check rule (2D plan review M22): the IR accepts any
  connection, and since a body refers to pins, a later merge that puts two of a node's pins on one
  net changes nothing the body says.
- **Parameters.** `I_WIDTH` (INT ≥ 0), `O_WIDTH` (INT ≥ 1) and `BODY` (kind `proc`, IR-10), with
  defaults 0, 1 and an empty `BODY`, which `verify` rejects, so a default node cannot exist. The only
  hook is `verify`; `const_value`, `simulate` and the writer hooks are NULL.
- **Flag and counter.** The type has `ODIN3_CT_PROCESS`, and the IR keeps a per-module count of live
  nodes whose type has it: creating one adds one; deleting one, replacing one (the old node) and the
  undo of a failed `odin3_node_create_connected` take one away; `compact` keeps it (only live nodes
  survive). `odin3_module_proc_count` is O(1); `odin3_design_has_processes` and
  `odin3_design_first_proc_module` (the first module in creation order with a process) are
  O(modules). Registration (`celltype.c`) refuses the flag, and any parameter of kind `proc`, on
  every definition other than the built-in `$proc` itself (identity, not shape: a copy of its
  definition is refused; 2D plan point (g), review I14). The ABI's plugin registration keeps its
  `cover` kind bound and flag mask, so a plugin can declare neither.
- **The body** (`src/ir/proc.h`). Positions (`odin3_pbit`): `IN | k` is the net on `I` pin k,
  `OUT | k` the net on `O` pin k, `ANY` (`-`, matches anything) appears only in compares. Positions
  are pin positions, never net IDs, so merging, renaming and compacting nets never touch a body.
  Spans `{first, len}` index the position array; an action is `lhs = rhs`; a case has its compares,
  actions and switches (case 0 is the root); a switch has a signal, its cases and the flags
  `full_case`/`parallel_case`; a sync has a type (posedge, negedge, level 0, level 1, always), a
  signal and its actions; actions, cases and switches carry their own provenance records (EL-45).
  `odin3_proc_body_pack` packs a body into one blob in the value's `bits`/`len`: six `uint32_t`
  counts (positions, compares, actions, cases, switches, syncs), then the six arrays as the C
  structs, each at an 8-byte-aligned offset, written field by field into zeroed arena memory, so
  padding is zero and equal bodies are equal bytes. `odin3_proc_body_view` reads a blob in place
  and refuses another kind, a payload that is not 8-byte aligned (no misaligned record reads) and a
  header whose arrays do not end exactly at `len`. A body is immutable once its node exists.
- **Reads of `OUT` positions** (2D re-review N6; 2E depends on it): in an action's right side, a
  switch signal or a compare, `OUT | k` is the *current* value of the net on that pin, exactly as
  an RTLIL process reads a wire it assigns: for a sync output the register's held value, for a
  temporary the value the case tree computed for it, never "the value being assigned in this
  evaluation". So `q <= q + 1` reads the register. Semantics are otherwise RTLIL's (2D spec §8.1).
- **Canonical layout** (2D plan point (e), review I3): case 0 is the root; the compares, actions and
  switches of the cases are contiguous per case and partition their arrays in case order (the cases'
  actions first; the syncs' actions follow, contiguous per sync in sync order, ending at the last
  action); the cases of the switches are contiguous per switch and partition `[1, n_cases)` in
  switch order; and every case of a switch has a higher index than the case that owns the switch.
  The cases and switches therefore form one tree under case 0 with each used once, and each action
  belongs to exactly one case or sync. 2D's process generator numbers breadth first.
- **`verify`** (run by check rule 5, FAST and FULL, as for any type): `I_WIDTH` and `O_WIDTH` in
  range, the view, then `odin3_proc_body_verify`: the canonical layout; every span inside the
  position array; `IN | k` with k < `I_WIDTH` and `OUT | k` with k < `O_WIDTH`; no compares on the
  root case; each compare as wide as its switch's signal; an action's two sides equally wide; an lhs
  of `OUT` positions only, none twice in one lhs; `ANY` only in compares (and with no index bits); a
  switch's flags only the two; a sync's type one of the five, and its signal one `IN` or `OUT`
  position for edge and level types (an `OUT` position is a block clocked by a net it assigns,
  `always @(posedge a) a <= ~a;`, which Yosys accepts; 2D plan point (h)) and 0 for always; every
  `OUT` position the lhs of some action (a pin the body never drives is rejected). It never
  allocates (review I3): the "twice" and "assigned" checks mark positions in a fixed 8192-bit bitmap
  on the stack, one pass per 8192 positions, so out of memory can never surface as a rejected body
  or a false rule-5 error. A rejection logs `$proc: BODY: <why>` (e.g. `action 0: lhs and rhs widths
  differ (1, 2)`). No new check rule exists.
- **IR operations**, all as for any cell, by construction. `odin3_node_create_connected` creates a
  node whole (2D builds the body off-IR, then calls once); `odin3_node_delete` removes it;
  `odin3_node_replace` replaces it by a node with the same port widths (different widths: delete and
  create). `odin3_net_merge` moves pins, and a body refers to pins: nothing is rewritten.
  `odin3_net_delete` needs a pinless net, so it never deletes a net a process uses.
  `odin3_module_compact` renumbers the node and its pins and copies `BODY` byte for byte (a body
  needs no map); a dead `$proc` gets the usual tombstone, its parameters and so its body included
  (IR-6; about 0.05 GB on lenet.v after `proc`, accepted for history: 2D re-review N7). The `always`
  block's attributes are the node's (`ODIN3_OBJ_NODE`); a switch's are its two flags.
- **Passes.** A pass that may meet a process must treat a `$proc` node as opaque (no constant
  propagation or CSE through it; its `O` pins are drivers like any cell's). The pass manager keeps
  every other pass away from a design with processes through the passes' `processes` flag (2D
  EL-33).
- **RTLIL spelling** (2D spec §13; plan points (r), (s)). `odin3_proc_sigspec` spells nets LSB
  first in, MSB first out: constants and `ANY` merge into `<n>'<bits>`, consecutive bits of a named
  wire into `\w [hi:lo]` (a whole wire `\w`, one bit `\w [k]`), another net is `\name` or `$n<ID>`,
  several chunks go in `{ … }`. `odin3_proc_print` writes a node as an RTLIL `process` block named
  `$proc$<file>:<line>$<ID>` from its SOURCE record (else `$proc$<ID>`), its attributes first. The
  body printer, `write_rtlil` and the ABI's text of `BODY` share these, so one text serves every
  test.

## 6. Provenance: the lineage DAG (PHASE1 #3, D9)

**IR-12 Every node, net and wire carries one `odin3_prov_id`** (pins inherit their node's).
Records are immutable, design-global, never deleted, and hash-consed (identical records stored
once, via `util/idindex`). A record holds:

| field | meaning |
|---|---|
| `kind` | `SOURCE` (from source text), `IMPORTED` (from a netlist file; structural only, spec §4.4), `DERIVED` (made by a pass) |
| `run` | the pass run that created it (pass-run table: pass name str + run number); readers and front ends are runs too |
| `op` | operation sequence number within that run (`DERIVED`: one per decompose/clump operation) |
| `locs` | zero or more source locations `{file str, line, col, end_line, end_col, loc}`, one per node the record covers; `loc` is the **raw** source-manager location of the node (any buffer, 0 = none; 2A AST-2). `locs[1..]` never hold chain entries, which `odin3_srcman_chain` derives from `loc` |
| `ast` | AST node ID in the elaborated (else parsed) store of the record's run, 0 = none, meaningful while that store is held (2A AST-15/16) |
| `hier` | str ID of the hierarchical path at creation, including generate scopes (`top/u1/gen[3]`) |
| `parents` | zero or more prov IDs (`DERIVED` only) |

**`hier` is module-relative** for hierarchical IR (2D EL-45): the module's own name (a
specialization's IR name), then each generate or named-block scope with its loop indices
(`top/g_stage[2]`, `sub$W=8/genblk1`). An IR module is shared by every instance of its
specialization, so the instance path is not a property of its objects; `flatten` adds it through its
`DERIVED` records. The example `top/u1/gen[3]` above is such a flattened path. No field changes.

**Why `run` and `op`:** without them, 100 `$add`s elaborated from one line of a `generate` loop
would share a record, and the 5,000 gates their lowering produces would be indistinguishable.
With them, each operation (lowering *one* `$add`) gets its own record, shared only by the pieces
*that operation* made; distinct operations never merge. `hier` separates generate iterations at
the source.

**Transformations add records and never edit them.** Clumping N objects into one (raise, merge,
CSE) is one operation: the new object gets a `DERIVED` record whose parents are the N objects'
records. Decomposing one object into many (lower, split) is one operation: every piece shares a
`DERIVED` record whose single parent is the original's. A pass that only rewires keeps the
existing records. Deleting an object leaves its records (and, after `compact`, a tombstone),
so history outlives objects ("nothing is cleaned up", PHASE1 #3).

**Navigation.**
- Backward: object → record → parents → … → `SOURCE`/`IMPORTED` leaves (their `locs`, `ast`).
  Iterative worklist with a visited mark (no recursion, §15.1). Primitives:
  `odin3_prov_parents(design, id)`, `odin3_prov_sources(design, id, callback)`.
- Forward ("everything that came from `foo.v:42`", "what did this `$add` become"): an index
  (`odin3_prov_index_build`) built by one sweep over **all** objects, live and dead, plus module
  records and tombstones, holding O(records + parent edges + objects): the objects carrying each
  record, each record's children, and the leaf records per (file, line).
  `odin3_prov_index_by_loc` / `odin3_prov_index_by_record` walk children breadth-first at query
  time without allocating; hits are flagged live, dead, or tombstone. The dead `$add` (slot kept by IR-6, or its tombstone) is therefore reachable
  from its gates and vice versa.
- Every step names its run, so the chain of passes that produced an object prints as history.

This replaces spec §5.3's `origin` field and D9's "pointers in-process": a record's `parents`
are the origin, and everything is reached by ID or hash (spec §5.3 and D9 amended).

**IR-13 Passes do not build records by hand.** A pass runs with a pass context that knows its
run; `odin3_prov_begin_op(ctx)` starts an operation and
`odin3_prov_derive(ctx, odin3_prov_list parents, &id)` makes its record. Identity for
hash-consing: every field including `run` and `op` for `DERIVED`; every field except `op` for
`SOURCE`/`IMPORTED`, so one origin in one run is one record. Creating any node, net or wire
requires a prov ID argument, so nothing is
created without lineage. Readers use `odin3_prov_source` / `odin3_prov_imported`.

**Names in output (spec §5.3).** An object's provenance name is
`hier/path/cellname@file:line`, from the record's first location (for `DERIVED`, the first leaf
of the backward walk in parent order). `--name-style` (writers) chooses provenance names or
short names; short names are the object's own name if set, else generated names
`$n<ID>` / `$c<ID>`, stable until the next `compact` (writers run after the last one). Writers make names unique across kinds where the target language has one
namespace (Verilog).

## 7. Names

**IR-14** Node, net and wire names are optional strtab IDs. Within a module, names are unique per
kind (two nets cannot share a name; a net and a node can); a net's alias names count, so a
merged net is found under every name it ever had. Names from a reader are kept
byte-for-byte (PHASE1 #2). Lookups go through per-module `u64map`s from str ID to object ID.
Renaming updates the map; a dead object's name leaves the map.

## 8. Mutation rules

**IR-15 All access goes through `src/ir` functions; no code outside `src/ir` writes an IR
struct field.** The API maintains both sides of every relation. Primitives:

- module: create; `module_add_port(name, dir, width | has_range [msb:lsb], scalar?, signed?,
  prov)` (creates the port node, its wire, appends to port order, updates the module's cell type;
  refused once instantiated; IR-7);
  `$port_*` types have one port of width parameter `WIDTH`;
- node: create (allocates pins); create-and-connect (type, params, per-port net vectors — the
  common case, so an `$add` is one call, not 97 `connect`s); delete (disconnects and kills its
  pins); replace by a node with the same port signature (pins re-attached by port/bit); delete
  and replace refuse port nodes (ports leave only through a module port API; none in Phase 1);
  replace does not move attributes;
- net: create; delete (must have no pins, no wire membership and no aliases); `merge(keep,
  drop)` — every pin of `drop` moves to `keep`; every wire vector entry that held `drop` now
  holds `keep`; `drop`'s name, primary `(wire, bit)` and aliases become aliases of `keep`, and the
  name map sends `drop`'s name to `keep`; `drop` dies;
- pin: connect / disconnect (updates the pin, the net's partitioned pin array and driver count);
  `connect` on a pin already on another net is an error (disconnect first); on its own net a
  no-op.
- wire: create (with its nets, or creating them), delete (a live non-port wire: its (wire, bit)
  memberships leave their nets, its name leaves the map; nets stay), add alias;
- queries: node port slice, net driver, net sinks slice, net const value, name lookups.

**IR-16 Iteration is in ID order (creation order), skipping dead objects.** Readers create
objects in file order, so writers iterating in ID order reproduce the input order (PHASE1 #2).
Objects created during an iteration are not visited by it (iterators capture the store length
at start); objects deleted during it are skipped. Hash-map iteration order never reaches
output. (New nodes from `replace` land at the end in ID order: fine for `netlist-compare`; a
readable-Verilog writer may sort by provenance.)

**IR-17 Single-threaded.** A design is used by one thread at a time.

## 9. Invariants (`check`)

`check` runs before and after every pass in Debug builds (spec §3, §10) and on demand. It
reports through `util/log` and returns `ODIN3_OK` or `ODIN3_ERR_CHECK` (new status). `check
--fast` runs rules 1–5 and 11; the full check runs all. Severity: **E** error, **W** warning.

1. **E** Every live pin belongs to a live node, and its port/bit indices are within that node's
   type's ports and widths.
2. **E** Pin ↔ net agreement: a pin with net *n* appears exactly once in *n*'s pin array, and
   every entry of *n*'s array is a live pin whose net is *n* (checked with a per-pin mark array,
   O(pins)).
3. **E** Partition: the first `driver_count` entries of a net's array are exactly its driver
   pins (`out`, `inout`, tristate-type outputs) and the rest are sinks.
4. **E** Two or more drivers that are neither `inout` nor of a `tristate` type (`inout` and
   tristate drivers are a bus: **W** when a bus net also has one ordinary driver). **W** a net with sinks and no driver (undriven); **W** a live net with no pins.
5. **E** A node's pin count equals the sum of its type's port widths for its parameters, and
   `verify` (if any) accepts it.
6. **E** Names are unique per kind per module, the name maps agree with the objects, and no dead
   object is in a name map.
7. **E** Every prov ID on a live object is a valid record; every `DERIVED` record has at least
   one parent and every parent has a smaller ID (a DAG by construction); every record's run
   exists. Phase 1: a live object with prov 0 is a **W** (one line per module); a nonzero ID that
   is not a record is **E**.
8. **E** Every wire's nets are live, and each either has this (wire, bit) as its primary or
   lists it in the alias table; every primary/alias entry is matched by the wire's vector.
9. **E** Port nodes appear in the module's port list once each and match the module's cell type
   (IR-7), and port pin *k* and port wire bit *k* hold the same net. The port wire's sign is its
   cell-type port's `is_signed`, and a scalar port is one bit wide.
10. **E** View check, when a view is asserted (IR-9).
11. **W** A pin left unconnected on a non-port node (dangling); **E** an unconnected `$port_out`
    pin. **E** A dead node with a live pin, or a dead pin with a net.

`ODIN3_ERR_CHECK` is added to `odin3_status` in `odin3.h` (an ABI change: ABI version 0 → 1,
done in 1B).

## 10. Memory layout (PHASE1 #5)

**IR-18 Array-of-structs in `util/pagevec`s per object kind, per module; nets own contiguous
pin arrays.** The throwaway spike (`work/spike-layout/`, 2M nodes × 4 pins, 2.2M nets, random
connectivity, -O2) measured: whole IR ≈ 260 MB AoS (budget 2 GB); fanout walk with intrusive
linked pin lists 2.8 s, with SoA link fields 0.85 s, with contiguous per-net pin arrays
0.24 s. The net→pin representation, not AoS vs SoA, decides traversal speed, so nets own
contiguous pin arrays (§3, Net) and everything else stays AoS behind accessors, where a hot
field can later move to SoA without touching callers. The 1B benchmark repeats this on the real
implementation and records the numbers in `docs/PHASE1.md`.

## 11. Spec amendments made with this document

Under PHASE1 #9: spec §5.2 (`$concat`/`$slice` are not cells, IR-1; six granularity tags and the
structural/BLIF cells, IR-4/IR-9/IR-10), §5.4 (both views also allow `module` and `port`), §5.3 (`origin` replaced by
the lineage DAG; IR-12), §15.1 wording ("arena owned by its Module" → per-module pagevecs and
arena; design-global strtab and provenance; IR-18); D1 (six granularity tags; IR-9); D9
("pointers in-process" → IDs and hashes; IR-12).

Under PHASE2 #7 (2A spec §13): `odin3_srcloc` gains `uint32_t loc`, the raw source-manager
location (part of a record's identity; not of the forward index's (file, line) key), and the
`ast` field is resolved by run and form (§6 table).

Under PHASE2 #13 (2D spec §19, IR change B): IR-19 (processes as `$proc` nodes: the cell, its
ports and parameters, the `proc` value kind, the `ODIN3_CT_PROCESS` flag and the per-module
counter); IR-9 (`$proc` in the RTLIL view only); IR-10 (the `proc` kind); IR-12 (`hier` is
module-relative). IR-3, IR-4, IR-6, IR-15 and check rule 4 are unchanged.

## 12. Out of scope for Phase 1

`flatten`, memories (`$mem*` types are registered but checked only for widths), the AST
(Phase 2), multi-threading, persistence of the IR to disk (BLIF/JSON writers serve).
