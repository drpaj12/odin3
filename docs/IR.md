# Odin III IR

Status: v1, 2026-10-08. Written by the agent under PHASE1 decision #9 (Peter delegated the IR
decisions for Phase 1; he reviews afterwards and may override any of them). Source of truth for
everything in `src/ir/`. Spec: `docs/DESIGN.md` §5, ADRs D1, D5, D7, D8′, D9. Each decision is
numbered **IR-n** so a later change can cite it.

When unsure about an invariant: stop and ask. Do not guess.

## 1. Shape of the IR

```
Design ─┬─ strtab (all names, file paths, pass names)          design-global
        ├─ prov store (lineage DAG, §6)                         design-global
        ├─ cell-type table (builtins + plugins + modules, §5)    design-global
        └─ Module* ─┬─ nodes   (pagevec, AoS)                    module-local IDs
                    ├─ pins    (pagevec, AoS)
                    ├─ nets    (pagevec, AoS) ── each owns a contiguous pin-ID array
                    ├─ wires   (pagevec) ── named, ordered groups of nets
                    ├─ name maps (strtab ID → node / net / wire ID)
                    └─ attributes (side table), arena for small arrays
```

**IR-1 Every net is one bit.** A cell's port of width *w* is *w* consecutive pins, one per bit,
LSB first. Word-level and bit-level cells differ only in their cell type; the connectivity
model is the same. This is Odin II's model. It makes `lower`/`raise` pure cell replacement (no
net splitting), makes slicing and concatenation free (they are just which nets a pin vector
uses), and makes in-place rewiring for RE a pin-level operation. Rejected: Yosys-style
multi-bit wires with SigSpec connections — every connection becomes a list of slices, and every
rewrite must split them. Cost: a 32-bit `$add` has 97 pins; the 2M-node spike (§9) shows the
memory is a non-issue.

**IR-2 Wires are names, not connectivity.** A *wire* is a named, ordered vector of nets with a
declared range (`[msb:lsb]`, possibly descending), the Verilog/RTLIL notion of a signal. Wires
carry names, ranges and port membership for front ends and writers; connectivity lives only in
pins and nets. A net belongs to at most one wire (as bit *k*); a net may also carry its own name
(BLIF names every bit, e.g. `n~19`). Writers name a bit by its net name if set, else by
`wire[k]`, else by a generated name (§7).

**IR-3 Module boundaries are port nodes.** Each module port is one node of a built-in cell type
`$port_in`, `$port_out` or `$port_inout` (granularity `port`) whose pins are the port's bits.
A `$port_in` pin drives its net; a `$port_out` pin sinks it. So every net driver is a pin, and
`check` has one rule for drivers. The module keeps the port nodes in declaration order (that
order is the port order of `.inputs`/`.outputs`, of instance pin vectors, and of written
Verilog headers).

**IR-4 Constants are cells.** Constant bits are driven by cells, never stored on pins. Front ends
use one shared `$const` node per value per module (`$_CONST0_`, `$_CONST1_`, `$_CONSTX_`,
`$_CONSTZ_`, one output pin each). The BLIF reader keeps a zero-input `.names` as the `.names`
cell it was (round trip, PHASE1 #2); `opt` (Phase 2) may canonicalize.

## 2. IDs

**IR-5 Typed 32-bit IDs, 0 = none.** Each kind has its own struct type so the compiler rejects a
net ID passed as a node ID (and `bugprone-easily-swappable-parameters` stays quiet):

```c
typedef struct { uint32_t v; } odin3_node_id;   /* module-local */
typedef struct { uint32_t v; } odin3_pin_id;    /* module-local */
typedef struct { uint32_t v; } odin3_net_id;    /* module-local */
typedef struct { uint32_t v; } odin3_wire_id;   /* module-local */
typedef struct { uint32_t v; } odin3_module_id; /* design-global */
typedef struct { uint32_t v; } odin3_celltype_id; /* design-global */
typedef struct { uint32_t v; } odin3_prov_id;   /* design-global */
/* names: uint32_t strtab IDs (0 = empty / no name), from util/str.h */
```

Slot 0 of every store is a reserved dummy so a zeroed field means "none". The C ABI (1D) hands
out the raw `uint32_t` plus the module handle.

**IR-6 IDs are never reused.** Deleting an object marks it dead; its slot and ID stay. IDs are
therefore stable across passes (spec §5.1) and dense in creation order. Every iterator skips
dead objects. A future `compact` pass may renumber a module explicitly, returning an old→new
map; nothing does so implicitly. Provenance never refers to object IDs (§6), so compaction
cannot break lineage.

## 3. Objects

Field lists are the contract; exact C layout is the implementer's, behind accessors (IR-15).

**Node** — cell type; name (str, optional); prov; first pin and pin count (a node's pins are
allocated together, contiguous IDs, in port order then bit order); parameter values (§5); flags
(dead). A node's pin count is fixed by its type and parameters at creation; changing a width
means replacing the node.

**Pin** — owning node; port index and bit index within that port; net (0 = unconnected); prov.
Direction comes from the cell type's port (`in`, `out`, `inout`).

**Net** — name (str, optional); prov; wire and bit (optional); flags (dead); the **pin array**:
every pin attached to this net, drivers and sinks together, plus a cached driver count. Pin
arrays are contiguous blocks from a per-module size-class pool (capacities 2, 4, 8, …; freed
blocks go to a free list per class); append is amortized O(1); removal swaps with the last
element (order within a net's pin array is not significant and never reaches output).

**Wire** — name (str); prov; range (msb, lsb); the ordered vector of its net IDs; port node
(optional, for wires that are module ports).

**Module** — name; the cell type that instantiates it (§5); the stores above; port nodes in
order; name maps; attributes; a per-module arena for small arrays (parameter vectors, wire net
vectors, attribute values).

**Design** — strtab; prov store; cell-type table; modules in creation order; top module
(optional, 0 = none).

## 4. Hierarchy

**IR-7 Instances are nodes.** Creating a module registers a cell type of granularity `module`
whose ports mirror the module's port nodes (names, directions, widths, order). An instance is a
node of that type; pin *k* of port *p* corresponds to bit *k* of the module's *p*-th port node.
Hierarchy stays until an explicit `flatten` pass (spec §5.1). A black-box model (BLIF
`.blackbox`, a `(* blackbox *)` module, an arch `<model>` with no definition) is a cell type of
granularity `blackbox` with declared ports and no body.

## 5. Cell types (op registry)

**IR-8 One table entry per type, one file per built-in type** (`src/ir/cells/<name>.c`), as
spec §5.2. An entry declares:

- `name` (Yosys spelling where one exists: `$add`, `$_AND_`, …), `granularity`;
- ports: name, direction, and a width rule — a constant, or the name of an integer parameter
  (`A_WIDTH`), or a function pointer for anything else;
- parameters: name, kind (`int`, `bits`, `string`), default;
- `verify` (parameter/port consistency beyond widths; may be NULL);
- `simulate` (1E; may be NULL until then), and optional writer hooks (1C/1F).

**IR-9 Granularity tags:** `word`, `bit`, `hard`, `blackbox`, `module`, `port`. Views
(spec §5.4, D1): the *RTLIL view* allows `word`, `hard`, `blackbox`, `module`, `port`; the
*netlist view* allows `bit`, `hard`, `blackbox`, `module`, `port`. Mixed is legal only inside
`lower`/`raise`.

**IR-10 Parameters vs attributes.** Parameters are typed values a cell type declares, stored
per node in declaration order. Attributes are free-form `name → value` on any object (Verilog
`(* … *)`, pragmas, reader extras such as BLIF `.cname`); kept in a per-module side table
keyed by (object kind, ID), since most objects have none. Values are `int` (int64), `bits`
(4-state: 0, 1, x, z; any length) or `string`.

**IR-11 Registration.** Built-ins register when a design is created (a static table compiled
from `src/ir/cells/`). Plugins register through the C ABI (1D) before the design is read.
Modules and black boxes register themselves (IR-7). Cell-type names are unique per design;
registering a duplicate is an error.

## 6. Provenance: the lineage DAG (PHASE1 #3, D9)

**IR-12 Every node, pin, net and wire carries one `odin3_prov_id`.** Provenance records are
immutable, design-global, never deleted, and hash-consed (identical records are stored once,
via `util/idindex`). A record holds:

| field | meaning |
|---|---|
| `kind` | `SOURCE` (from source text), `IMPORTED` (from a netlist file: structural only, spec §4.4), `DERIVED` (made by a pass) |
| `pass` | str ID of the creating pass or reader (`read_blif`, `lower`, …) |
| `locs` | zero or more source locations `{file str, line, col, end_line, end_col}` |
| `ast` | AST node ID, 0 = none (Phase 2) |
| `hier` | str ID of the hierarchical path at creation (`top/u1/u3`) |
| `parents` | zero or more prov IDs (`DERIVED` only) |

**Transformations add records and never edit them.** Clumping N objects into one (raise, merge,
CSE) gives the new object a `DERIVED` record whose parents are the N objects' records.
Decomposing one into many (lower, split) gives each piece a `DERIVED` record whose single parent
is the original's. A pass that only rewires keeps the existing IDs. Deleting an object leaves its
records in place, so history outlives the objects (Peter: "nothing is cleaned up").

**Navigation.**
- Backward: object → record → parents → … → `SOURCE`/`IMPORTED` leaves (their `locs`, `ast`).
  Iterative worklist with a visited set (no recursion, §15.1). `odin3_prov_sources(design, id,
  callback)` and `odin3_prov_parents(design, id)` are the primitives.
- Forward: "everything that came from `foo.v:42`" — an index built on demand: one sweep over
  live objects, each record's set of leaf locations memoized, inverted into `loc → objects`.
- Every step names its pass, so the chain of passes that produced an object can be printed.

**IR-13 Passes do not build records by hand.** A pass runs with a pass context that knows its
name; `odin3_prov_derive(ctx, parents, n)` makes the record. Creating any object requires a
prov ID argument, so nothing is created without lineage. Readers use `odin3_prov_source` /
`odin3_prov_imported`.

**Names in output (spec §5.3).** The provenance name of an object is
`hier/path/cellname@file:line`, using the record's first location (or, for `DERIVED`, the first
leaf found by the backward walk in parent order). `--name-style` (1C/1F writers) chooses
provenance names or short names. Short names: the object's own name if set, else a stable
generated name `$n<ID>` / `$c<ID>`.

## 7. Names

**IR-14** Node, net and wire names are optional strtab IDs. Within a module, names are unique per
kind (two nets cannot share a name; a net and a node can). Names given by a reader are kept
byte-for-byte (BLIF round trip, PHASE1 #2). Lookups go through per-module `u64map`s from str
ID to object ID. Renaming updates the map; a dead object's name is released.

## 8. Mutation rules

**IR-15 All access goes through `src/ir` functions; no code outside `src/ir` writes an IR
struct field.** The API maintains both sides of every relation:

- create: module, node (allocates its pins), net, wire, all with a prov ID;
- connect / disconnect a pin and a net (updates the pin and the net's pin array and driver
  count);
- delete: a node (disconnects and kills its pins), a net (must have no pins), a wire;
- bulk helpers passes need: move every pin of net A onto net B (`merge`), replace a node by
  another with the same port signature (pins re-attached by port/bit).

**IR-16 Iteration is in ID order (creation order), skipping dead objects.** Readers create
objects in file order, so writers that iterate in ID order reproduce the input order (round
trip, PHASE1 #2). Hash-map iteration order never reaches output.

**IR-17 Single-threaded.** A design is used by one thread at a time.

## 9. Invariants (`check`)

`check` runs before and after every pass in Debug builds (spec §3, §10) and on demand. It
reports through `util/log` and returns `ODIN3_OK` or `ODIN3_ERR_CHECK` (new status). Rules, with
severity (**E** error, **W** warning):

1. **E** Every live pin belongs to a live node, and its port/bit indices are within that node's
   type's ports and widths.
2. **E** Pin ↔ net agreement: a pin with net *n* appears exactly once in *n*'s pin array, and
   every entry of *n*'s pin array is a live pin whose net is *n*.
3. **E** A net's cached driver count equals the number of `out`/`inout` pins in its pin array.
4. **E** A net with more than one driver, unless all its drivers are `inout`
   (tristate/multi-driver is representable, spec §5.1, and flagged). **W** a net with no
   driver that has sinks (undriven); **W** a live net with no pins.
5. **E** A node's pin count equals the sum of its type's port widths for its parameters, and
   `verify` (if any) accepts it.
6. **E** Names are unique per kind per module, and the name maps agree with the objects.
7. **E** Every prov ID on a live object is a valid record; every `DERIVED` record has at least
   one parent and parents precede it (records form a DAG by construction).
8. **E** A wire's nets are live and each records this wire and its bit index.
9. **E** Port nodes appear in the module's port list, once each, and match the module's
   cell type (IR-7).
10. **E** View check, when a view is asserted (IR-9).
11. **W** A pin left unconnected on a non-port node (dangling); **E** an unconnected
    `$port_out` pin.

`ODIN3_ERR_CHECK` is added to `odin3_status` in `odin3.h` (an ABI change, ABI version 0 → 1,
done in 1B).

## 10. Memory layout (PHASE1 #5)

**IR-18 Array-of-structs in `util/pagevec`s per object kind, per module; nets own contiguous
pin arrays.** The throwaway spike (`work/spike-layout/`, 2M nodes × 4 pins, 2.2M nets, random
connectivity, -O2) measured: whole IR ≈ 260 MB AoS (budget 2 GB); fanout walk with intrusive
linked pin lists 2.8 s, with SoA link fields 0.85 s, with contiguous per-net pin arrays
0.24 s. So the net→pin representation, not AoS vs SoA, decides traversal speed: nets own
contiguous pin arrays (§3, Net), everything else stays AoS behind accessors so a hot field can
move to SoA later without touching callers. The 1B benchmark repeats this on the real
implementation and records the numbers in `docs/PHASE1.md`.

## 11. Out of scope for Phase 1

`flatten`, `compact`, memories (`$mem*` types are registered but unchecked beyond widths), the
AST (Phase 2), multi-threading, persistence of the IR to disk (BLIF/JSON writers serve).
