# 3A — `flatten`: design

Status: v1 (2026-10-14), for Peter's approval (CLAUDE.md rule 2). Phase 3, sub-project 3A (PHASE3
sub-projects table; light review; Opus `high`). Written by the lane-2 agent. Spec: `docs/DESIGN.md`
§5.1 ("hierarchy kept until an explicit `flatten` pass"), §6 steps 2 and 9, §12 (Phase 3 exit), §13
#6 (name style). Inputs: `docs/IR.md` IR-2, IR-3, IR-6, IR-7, IR-7b, IR-10, IR-12, IR-13, IR-15,
IR-16, IR-18, IR-18a, IR-19; the 2D spec (EL-4, EL-6, EL-9, EL-11, EL-33, EL-45, §3.1a, §3.6); the 2F
spec (OP-3, OP-14, OP-19); the 3E spec (E10); PHASE3 decisions 3, 7, 16, 17, 19, 20, 28 and the
2026-10-14 entries; the Phase 3 survey. **Oracle: Yosys 0.55 as pinned by VTR**
(`$VTR_ROOT/libs/EXTERNAL/yosys`, `60f126cd`; binary `$VTR_ROOT/build/bin/yosys`;
`passes/techmap/flatten.cc`, 438 lines), and Parmys's `synthesis.tcl` at VTR `3c9a4d23`. Decisions
are numbered **FL-n**. Where this file and `docs/IR.md` disagree, IR.md wins. Every default below
that is not Peter's is marked **agent default (Peter may override)**; §18 lists what is Peter's.

## 1. Purpose and scope (FL-1)

`flatten` turns a design whose top module instantiates other modules into a design whose top
module holds all their logic. After it, the top module has no live node whose cell type has
granularity `module`, except an instance Yosys would also keep (`keep_hierarchy`, §7). It is the
step between `opt` and `hardmap`/`lower` in the Phase 3 script (PHASE3 exit test, item 3):

```
read_verilog --nolatches; proc; opt; flatten; [wreduce;] hardmap; lower; abc; write_blif --vtr
```

3E's `--vtr` writer already refuses a hierarchical top (E10: "hierarchy not flattened (run
flatten); VPR reads one model"); `lower` (3B) and `hardmap` (3C) work on one module, and `abc` (3D)
reads one module's soft cone. 3A is the only producer of what they need.

**In scope.**
- The pass `flatten`, in `src/passes/flatten/`: expansion of module instances into the module that
  holds them, names, IR-2 aliases for the joined port nets, provenance (the instance path), black
  boxes, hard cells and library cells left alone, `keep_hierarchy`.
- One small additive IR function, `odin3_prov_derive_hier` (§6), so that the instance path is a
  field of a `DERIVED` record, as IR-12 already says it is. Peter reviews it (§18, 1).
- The evidence: unit tests, a differential against Yosys 0.55 (`flatten-diff`), simulator-equal
  before and after, fuzzing, determinism and out-of-memory tests (§14).

**Not in scope.**
- Removing the modules that become unused (§10: the IR has no way to; agent default is to leave
  them).
- Optimisation across the new boundaries. Yosys's `flatten` is followed by `opt -full` in Parmys; in
  Odin III that is the `opt` passes of 2F, run again after `flatten` (§2; Peter's question 3).
- `memory`/`memory_infer` (Phase 4), `hdlname`/`$scopeinfo` bookkeeping (replaced by provenance,
  §6), `-wb` (there is no whitebox concept in the IR; §13 row 9).
- Anything in `src/ir/` other than the one addition in §6.

## 2. Where it runs in the pipeline (FL-2)

**`flatten` runs after `proc`.** DESIGN §6 step 2 and `docs/IR.md` IR-19 already settle it: `flatten`
is a `REFUSE` pass (`odin3_pass_def.processes = ODIN3_PASS_PROCESSES_REFUSE`), so the pass manager
fails it, before it runs and without opening a pass run, when any module still holds a `$proc` node
(`pass 'flatten': module 'm' has processes; run proc first`). The reason is in the cells, not the
manager: a `$proc` node's body refers to its own pin positions (IR-19), and copying one into
another module would need the body to be re-packed with new pin numbering for no gain, because
`proc` deletes every such node and never runs after `flatten` in the script. So:

- **Order.** `proc` → `opt` → `flatten` (→ `opt` again, §2.1) → `wreduce`? → `hardmap` → `lower`.
- **All modules are checked, not only the reachable ones.** The manager looks at every module of the
  design; a library module with an unlowered process blocks `flatten` even if nothing reaches it.
  This is the manager's existing rule (`odin3_design_first_proc_module`), kept (§13 row 11).
- **Memories need nothing earlier.** Yosys's `memory -nomap` runs before `flatten` in Parmys because
  Yosys has `$memrd`/`$memwr` cells to collect first. Odin III's 2D emits one `$mem` node per array
  (EL-27, EL-39), with all its ports; `flatten` copies it like any word cell and rewrites its
  `MEMID` (§9). `hardmap` (3C) therefore sees every `$mem` of the design in one module.
- **Black boxes and hard cells** are not modules (IR-7b: granularity `blackbox`/`hard`), so they need
  nothing before `flatten` either.
- **Top required.** `flatten` takes the design's top (`odin3_design_top`, set by `hierarchy`,
  `read_blif`, `read_verilog --top` or the project). No top is `INVALID_ARG`: `flatten: the design
  has no top module (run hierarchy)`. Yosys flattens every module when no top is marked; Odin III
  has no use for that (§13 row 10), and 3E's E13 and `sim` make the same demand.

### 2.1 `opt` after `flatten` (agent default; Peter's question 3)

Parmys runs `flatten; opt -full` (2F spec §3, row 9). Without a second `opt`, three things Parmys
gets are lost: constants that now cross the old boundary (an input tied to `1'b0` at the instance
fold into the child's logic only after the merge), duplicate cells in two instances of one module
(`opt_merge`), and dead logic behind unused child outputs (`opt_clean`). 3A does not change the
script; it **recommends** `... proc; opt; flatten; opt; ...` for the O, O′ and Oprov legs and asks
Peter (§18, 3) because the script is quoted in PHASE3's exit test and in the 3H spec/plan (the
`read_techlib read_verilog proc opt flatten wreduce hardmap lower abc write_blif` strings).

## 3. What `flatten` does (FL-3)

### 3.1 Vocabulary

- **Root.** A module `flatten` rewrites in place. The roots are the top and every module that stays
  an instance somewhere (a `keep_hierarchy` module, §7), transitively. Yosys flattens the same set
  (`used_modules` + `flatten_module` on every module the topological sort reaches); a kept module is
  itself flattened internally.
- **Instance node.** A live node of a root whose cell type has granularity `module` (IR-7). Its
  module is found through a map from cell-type ID to module ID built once (as `sim/build_hier.c`
  does: `odin3_module_celltype`). A node of any other granularity (`word`, `bit`, `hard`,
  `blackbox`) is **not** an instance and is never expanded, whatever its name (§7).
- **Expansion** of instance `I` of module `C` inside root `R`: the contents of `C` are copied into
  `R` under `I`'s name, `C`'s port nets are joined to the nets on `I`'s pins, and `I` is deleted.

### 3.2 Why top-down, in place

Yosys flattens bottom-up: every module is flattened first, then copied whole into its parents, so an
object at depth *d* is copied *d* times. Odin III expands **top-down on the root only**: the nodes
an expansion copies include `C`'s own instance nodes, which are appended to a FIFO worklist and
expanded after. Each object of the final flat module is created exactly once, from the original
(unflattened) child, so cost is O(flat size) and non-root modules are never modified. The result
is the same netlist and the same names (a copy of `C`'s instance `b` named `a.b` expanded later
gives `a.b.x`, exactly the name Yosys's bottom-up copy gives).

### 3.3 Algorithm

**Pre-flight (no mutation; any failure leaves the design untouched).**
1. Build the module graph from the top: an edge `M → C` per live instance node in `M` of module `C`.
   A Kahn topological sort with an explicit queue (no recursion) detects cycles:
   `flatten: recursive instantiation: a -> b -> a` (the cycle, named by module; Yosys:
   `Cannot flatten a design containing recursive instantiations`). `ODIN3_ERR_INVALID_ARG`.
2. Per module in topological order, compute the flat sizes (live non-port, non-constant nodes; pins;
   nets; wires), saturating in `uint64_t`, by adding each non-kept instance's child once per
   instance. Compare with the caps (§12) and with the budget (`odin3_budget_check`, extra bytes =
   the estimate, §12). Failure: `flatten: the flat design would have N nodes (cap M)` or the
   budget message of `odin3_budget_message`.
3. Collect the roots (top + kept modules reached, §3.1) in module-ID order.

**Per root, in root order; per instance in FIFO order (initially the root's instance nodes in node-ID
order, then the instance nodes each expansion creates, in their creation order):**
1. Skip (keep) the instance if it or its module has `keep_hierarchy` (§7). Otherwise:
2. Record the instance's name, provenance, attributes and, per port bit, the net on its pin
   (`P[port][bit]`, or none). Begin a provenance operation (§6). Compute the instance path (§6) and
   the name prefix (§4).
3. Delete the instance node (`odin3_node_delete`). Its pins leave their nets; the nets stay.
4. **Net map.** `map[n]` for every live net `n` of `C`:
   - a net on a bit of one of `C`'s port nodes is **joined**: `map[n] = P[port][bit]` (§5);
   - a net on no port bit is **internal**: a fresh net in `R`;
   - a net driven by one of `C`'s constant nodes (`$_CONST0_/1/X/Z_`, IR-4) is **constant**: `map[n]`
     is `R`'s shared constant net of that value (found or created once per root); the constant
     nodes are not copied (§3.6).
5. **Nodes.** For each live node of `C` in node-ID order, except port nodes and constant nodes: a
   node of the same type in `R` (`odin3_node_create_connected`), parameters copied byte for byte
   (`MEMID` rewritten, §9), pins on `map[...]` of the original nets, name prefixed (§4), provenance
   derived (§6), attributes copied (§3.5). A new node of granularity `module` is appended to the
   worklist.
6. **Wires.** For each live wire of `C` in wire-ID order: a wire in `R` with the same range and
   sign, name prefixed, `nets = map[...]` of the wire's nets (`odin3_wire_create` with an explicit
   net vector: each bit is the primary of its net if the net has none yet, else an alias; IR-2,
   §5). This includes the port wires of `C`: they become ordinary wires `u1.p` over the joined
   nets.
7. **Check the joins** (§3.7).

### 3.4 Joining nets (FL-4)

The port `p` of `I` has width *w*; `I`'s pins `P[p][0..w−1]` are on `R`'s nets, and `C`'s port node
for `p` has *w* pins on `C`'s nets (IR-3). Bit *k* of the port joins `C`'s net `n` with `R`'s net
`P[p][k]`:

| case | action |
|---|---|
| `P[p][k]` is a net, `n` has no `map` yet | `map[n] = P[p][k]` |
| `P[p][k]` is none (unconnected) | `map[n]` = a fresh net (undriven if an input; an output with no reader). The same for an unconnected `inout` |
| `n` already has `map[n] = q` and `q` is not `P[p][k]` (the net of `C` is on two port bits: a feed-through `output y = a`, or two outputs on one net) | `odin3_net_merge(keep = q, drop = P[p][k])`; a small redirect table sends the dead net's ID to `q` for later bits |
| `n` is also driven by a constant node of `C` (an output tied to 0/1) | `odin3_net_merge(keep = shared constant net, drop = the joined net)` after checking §3.7 |
| two bits of one instance port, or of two ports, are on the same `R` net | no action: both `C` nets map to it |

The `$port_in`/`$port_out`/`$port_inout` nodes of `C` are never copied. The driver or sink they
represented is the instance pin's net on the `R` side, which the join makes the same net. So no
new buffer, no new cell and no `connect`/`assign` is made: the joined nets are **one net** with
several names (IR-2, §5). This is the main difference from Yosys, which keeps a `connect` between
two wires until `opt_clean`.

### 3.5 Attributes (FL-5)

- Every attribute of a copied node, and of a copied wire, is copied with `odin3_attr_foreach` /
  `odin3_attr_set` in the original's key order (IR-10, deterministic). `keep` and `init` therefore
  survive (Yosys: `addWire(name, other)` / `addCell(name, other)` copy attributes).
- **Not copied:** the BLIF reader's `blif_name` on a port wire (it names a *port* bit, and the copy is
  no longer a port; `ODIN3_BLIF_ATTR_PORT_NAME`), and a module's `blif_clock` (module attributes
  stay on the module; §13 row 12). The instance's own attributes, including `parameter:<NAME>`
  (EL-11), die with the instance (§8).
- **Nets** carry no attributes from the readers (attributes go on wires and nodes: 2D §10, BLIF
  attrs), so none are copied (agent default).

### 3.6 Constants (FL-6, agent default (Peter may override))

IR-4: front ends use one shared constant node per value per module. Copying `C`'s constant nodes
would give one per instance, and 100,000 instances would make 400,000 identical nodes. `flatten`
instead maps `C`'s constant nets to `R`'s own shared nets (creating the shared node in `R` the
first time), so the flat module keeps IR-4's invariant. Wires and names on the constant net
(`wire [3:0] k = 4'b0101;` in `C`) become wires/aliases of `R`'s shared constant net, as they are
aliases of `C`'s. A `$sop` with no inputs (BLIF's `.names x`) is **not** a constant node (IR-4); it is
copied like any cell.

### 3.7 Errors found while joining (FL-7)

- **Constant on an output** (Yosys: `Cell port %s.%s.%s is driving constant bits`): a joined output
  or `inout` net of `R` that is driven by a constant node of `R` (after deleting `I`), or an
  output port of `C` whose net is a constant of `C` joined to an `R` net that already has another
  driver. Error `<loc>: instance '<path>' of module 'C': output port 'y' drives net 'x', which is
  driven by a constant`. `ODIN3_ERR_INVALID_ARG`.
- **Multiple drivers** created by a join (agent default; Yosys leaves them for later passes and `check`
  reports them as rule 4): after the joins of one instance, for each joined `R` net with more than
  one driver where the drivers are not all `inout` or `tristate`-type (the same test as check rule
  4), an error `<loc>: instance '<path>': net 'x' has N drivers after joining port 'y'`. It costs
  O(joined bits) and means Release builds, which do not run `check`, still stop on the one defect
  `flatten` can introduce.
- Both are errors, not warnings; the pass returns at the first one (the 20-error cap of the
  readers is not needed: one is enough to stop a script). See §12 for what state the design is in.

## 4. Names (FL-8; feeds DESIGN §13 #6)

### 4.1 Style: Yosys's

An object of `C` that has a name gets, in the flat module, `<prefix><sep><name>` where `<prefix>` is
the **flat name of the instance** and `<sep>` is `.` (Yosys's default; the `--separator` option
replaces it, FL-16). Nested expansion composes: instance `b` inside instance `a` of the top is
created as a node named `a.b` and its contents are named `a.b.x`. Probed on Yosys 0.55
(`read_verilog; hierarchy -top top; proc; flatten`, instance `m1` of `mid`, whose instance `u` of
`leaf` has wire `t` and memory `mem`): wires `\m1.p`, `\m1.u.t`, `\m1.u.a`; memory `\m1.u.mem`;
private cell `$flatten\m1.\u.$and$t.v:2$1`; the kept module's instance keeps its name `\k`. Odin
III's names are the same strings without the RTLIL backslash (IR names carry no sigil, 2D §13):
`m1.p`, `m1.u.t`, `m1.u.a`, and the memory's `MEMID` `m1.u.mem`. Inside generate scopes the IR
name already carries the scope (`g[0].t`, 2D EL-21), so an instance in a generate block named
`g[1].u2` expands to `<path>.g[1].u2.<name>`, as Yosys.

**What gets a prefix:** named nodes, named nets, named wires, and the `MEMID` of `$mem`/`$memrd`/
`$memwr` (§9). **Port wires of `C` too**: `u1.p`, `u1.q`. They are not ports any more; their nets
are joined (§5).

**Unnamed objects stay unnamed** (agent default (Peter may override)). Yosys gives a private object
`$flatten<inst>.<private name>`, because every RTLIL object has a name. An IR object may have none
(IR-14: names are optional strtab IDs), and generated names (`$n<ID>`, `$c<ID>`) belong to writers.
So an unnamed cell or net of `C` is an unnamed cell or net in `R`; its lineage and instance path
are in its provenance (§6), where they cost nothing per name.

**An unnamed instance** (BLIF `.subckt` without `.cname`; the Verilog front end names every
instance) gives its children the prefix `$c<ID>` with `<ID>` the instance node's ID in `R` at
expansion time (the form the writers use for unnamed nodes). Deterministic (IDs are creation order,
FL-14), and no more than a label.

### 4.2 Uniqueness

IR-14 makes names unique per kind per module. A name that is taken in `R` (a Verilog escaped
identifier `\u1.x ` in the parent, or two unnamed instances that both received the same label)
gets `_1`, `_2`, … appended until free, per kind (Yosys's `Module::uniquify`: `%s_%d` from 1). A
`DEBUG` line names each, and the pass counts them (`renamed=N` in the run information, §11). A net's
alias names count (IR-14): a clash with an alias is a clash.

### 4.3 What this means for §13 #6

DESIGN §13 #6 asks for the name-style policy for VTR: "provenance names are long; measure VPR runtime
impact". 3A contributes the **short names** side: dotted instance paths in IR names, at Yosys/Parmys
length (the flat BLIF of Parmys uses names like `top.u1.x` in the same way). The **provenance names**
(`hier/path/cellname@file:line`, IR-12, written by `--name-style provenance`, 3E-6) get their path from
`flatten`'s records (§6). The measurement itself is 3E-6/3H's. 3A's answer for the Phase 3 exit: *names
in the IR are dotted instance paths; provenance carries the slash-separated path and the source line;
short names remain the writers' default*. Peter confirms the separator and the unnamed-object rule
in §18, 4.

## 5. Aliases (FL-9; IR-2)

IR-2 says it in so many words: "`flatten` joining `top.x` with `u1.p` … keep[s] every name". The
rules:

1. **Joined port nets are one net.** For every joined bit the `R` net is the net, and `C`'s port wire
   `p` is recreated as the wire `u1.p` over it (§3.3 step 6). `odin3_wire_create` makes `(u1.p, k)`
   the net's primary if the net has none, else an alias appended in insertion order. So a net the
   parent calls `top.x` and the child `p` has primary `(x, k)` (or `(p-of-parent, …)`; whichever
   wire came first) and the alias `(u1.p, k)`, and `odin3_module_find_wire(R, "u1.p")` finds the
   child's view. A writer that prints "net name first" (BLIF) or "primary wire bit first" (Verilog)
   prints the one IR-2 says; **which name a writer prints is the writer's policy, not the IR's**.
2. **A net's own name** (BLIF names every bit) is kept as a bare-name alias: for a joined net `n` of
   `C` that has a name, `flatten` makes a temporary net with the prefixed name and merges it into
   the joined net (`odin3_net_merge(keep = joined, drop = temporary)`, which appends the dropped
   net's name to the keeper's aliases, IR-15). A fresh internal net simply carries the prefixed
   name.
3. **Merging joined nets** (the feed-through case, §3.4) uses `odin3_net_merge`, which moves every
   pin, wire entry, name and alias of `drop` to `keep` in the documented order (`drop`'s name, its
   primary, its aliases). No name is lost.
4. **No assign cells.** `$port_in`→`$port_out` pass-through becomes one net. There is no `$pos`/
   `$buf`/`.names a b 1 1`; 3E's "buffer" for a net on several output ports (`vtr.c`) is the
   writer's business.
5. **IR-6.** `flatten` never compacts. The instance nodes and (via joins) the dropped nets stay dead
   until the script's `compact`; their tombstones keep the names the pins were on.

## 6. Provenance (FL-10; IR-12, IR-13)

IR-12: *"An IR module is shared by every instance of its specialization, so the instance path is not a
property of its objects; `flatten` adds it through its `DERIVED` records."* The records have the field
(`hier`, strtab ID of the hierarchical path) and the hash-consing identity includes it, but
`odin3_prov_derive` has no way to set it. 3A adds one:

```c
/* ir/prov.h — the one IR addition of 3A (Peter reviews). */
odin3_status odin3_prov_derive_hier(const odin3_pass_ctx *ctx, odin3_prov_list parents,
                                    uint32_t hier_str, odin3_prov_id *out);
```

Same as `odin3_prov_derive` but the record's `hier` is `hier_str` (a strtab ID, 0 = none, as the
origin's). Identity includes `hier`. `odin3_prov_derive(ctx, p, &id)` is `odin3_prov_derive_hier(ctx,
p, 0, &id)`. No record field, rule or check changes (rule 7 stays); the ABI is unchanged (the
function is internal, like `odin3_prov_derive`).

**What `flatten` records.**
- **One operation per expanded instance** (`odin3_prov_begin_op`, IR-12: "Decomposing one object into
  many is one operation; distinct operations never merge").
- **Each copied node, net and wire** gets the `DERIVED` record with `hier = <path>` and parents
  `[original record, instance record]`. The original record first, so "the first leaf of the backward
  walk in parent order" (IR-12, names in output) is the object's own source line; the instance's
  record second, so the instantiation line is reachable from every object it created and a forward
  query on it ("what did `u1` become") finds them all through the provenance index. A zero original
  record leaves just the instance record; both zero leaves provenance 0 (check rule 7: W per
  module, as before).
- **Path.** `hier = <root name>/<i1>/<i2>/…/<ik>[/<scope>…]`. `<i_j>` is the *instance's own IR name
  in the module that holds it* (for a generate-block instance, `g[1].u2`, as named); an unnamed
  instance is `$c<ID>`. `<scope>…` is the original record's `hier` **without its first component**
  (the child's module name, e.g. `sub$W=8`, which is the same for every instance and already in
  the parent record). So: top `top`, instance `u1` of `sub$W=8`, object in generate scope
  `genblk1` → `top/u1/genblk1`. This is IR-12's own example, `top/u1/gen[3]`. The root name is the
  root module's IR name.
- **Cost.** One record per (distinct original record, instance). A module with *r* distinct records
  instantiated *n* times costs *r·n* records (hash-consing merges equal ones). A record is about 48
  bytes plus its hash-index entry. The pre-flight estimate (§12) counts them. 3H measures it on the
  19 and reports (Task 5).
- **Nested expansion does not chain.** Contents are copied from the original child, so the parent of a
  copied object is the child's `SOURCE` record, not a `DERIVED` record of an outer flatten step.
- **Pass-run information** (3D-3's `odin3_passrun_set_info`): `roots=<n> instances=<n> kept=<n>
  nodes=<n> renamed=<n>`, so `dump`/`stats` can show what the run did.

**Fallback if Peter rejects the addition** (§18, 1): `hier` stays 0 on `DERIVED` records, the instance
path is only in names of named objects, and unnamed objects lose the path. Everything else in this
spec is unchanged.

## 7. Black boxes, hard cells, library cells, `keep_hierarchy` (FL-11)

**Not instances, never touched.** A node whose cell type has granularity `blackbox` (IR-7b: BLIF
`.blackbox`, a `(* blackbox *)` module, an implicit black box from `--auto-blackbox`), `hard` (the
tech library's `adder`, `multiply`, `single_port_ram`, `dual_port_ram`, from `read_techlib`) or any
`word`/`bit` cell is copied into the flat module as is, with its parameters, a prefixed name and
derived provenance. `flatten` looks only at the granularity: a name that matches a module is
irrelevant (2D already decided at elaboration which one an instance means, §3.1a "Precedence
against a hard cell"). A **black-box module** (no body) is not a `Module` at all (2D EL-4, EL-11),
so no node of it can be expanded. This is Yosys's `module->get_blackbox_attribute(ignore_wb)` test.

**`read_verilog --lib` units** are ordinary `Module`s once instantiated (2D §3.1a). They expand like
any user module, unless the instance meets the rule below. The marker attributes `odin3_lib*` are not
copied to the IR (2D), so nothing special survives.

**`keep_hierarchy`** (Yosys: `cell->get_bool_attribute(keep_hierarchy) ||
tpl->get_bool_attribute(keep_hierarchy)`):
- The instance node has attribute `keep_hierarchy`, **or** its module has it (a module attribute,
  `ODIN3_OBJ_MODULE`; 2D copies a `MODULE`'s `(* *)` attributes to the IR module). The value is true
  as Yosys `Const::as_bool`: an `INT` or `BITS` value that is non-zero, or a non-empty `STRING` (a
  front end stores a value-less `(* keep_hierarchy *)` as `INT 1`).
- A kept instance is copied into the flat module like a black box (§7 first paragraph), keeping its
  type, ports and parameters, with its pins on the mapped nets. It is **not** expanded, here or when
  it appears inside another expansion, and the log says `flatten: keeping 'top.k' (module 'kept',
  keep_hierarchy)` (INFO; Yosys logs `Keeping %s.%s (found keep_hierarchy attribute)`).
- A kept module is itself a **root** (§3.1): its instances are expanded in place. If the same module is
  also expanded at an instance without the attribute, the expansion copies from the module as it was
  before the in-place flattening (§3.2: copies come from the original child; order of roots does not
  matter).
- The result is then **not one flat module**, and `write_blif --vtr` says so (E10). That is
  correct: the user asked for it. `keep_hierarchy` on the **top** is ignored (nothing instantiates
  it).
- **The VTR flow.** Parmys sets `keep_hierarchy` on `single_port_ram`/`dual_port_ram` before `flatten`
  (`setattr -mod -set keep_hierarchy 1 …`, 2F spec §3). In Odin III those two are hard cells (the tech
  library, `lib/vtr.o3lib`), which `flatten` never expands, so the attribute has nothing to do and
  Odin III needs no equivalent of that `setattr`. The `vtr_primitives.v` copy of the same two modules
  carries `(* keep_hierarchy *)` (`tests/elab/lib/vtr_primitives.v`): read with `--lib`, it is used
  only when an instance's `#()` does not fit the hard cell (2D §3.1a case 2), and then `flatten`
  keeps that instance as a module, as Parmys does; 3E's E10 reports it. **Confirmed on the 19 by the
  Task 5 sweep, which counts `kept=` per design (expected 0).**

## 8. Parameters, specializations, generate scopes (FL-12)

- **Specializations are modules.** `sub#(.W(8))` is its own IR module and cell type (IR-7, EL-9); the
  expansion copies that module, whatever its name (`sub$W=8`, `sub$h<hash>`, `sub$2`). The module
  name never appears in an object name (instance names do), so the dollar-suffixed names never reach
  a writer.
- **No parameter is evaluated.** Elaboration has resolved every width, so `flatten` copies integer,
  bits and string parameter values byte for byte. A cell's `MEMID` is the one parameter that
  names something (§9).
- **`parameter:<NAME>` instance attributes** (EL-11: "source-level parameter values") are not
  propagated; they describe an instance that no longer exists. The specialization name (which
  contains the values, `sub$W=8`) stays visible through the original record's `hier`
  (the parent of every copied object). Yosys keeps them in `$scopeinfo`'s `cell_*`/`module_*` attributes
  (§13 row 3).
- **Generate scopes.** A name inside a generate scope already carries the scope (`g[0].t`, EL-21), so
  prefixing composes: `u1.g[0].t`. The `hier` of a record carries the scope too, as a `/`-separated
  tail (`genblk1`, `g_stage[2]`, EL-45), kept after the instance path (§6).
- **A parameterized black box** keeps its base name and per-instance parameters (EL-11): the copy
  carries the original node's parameters, so IR-8's table width rules still hold.

## 9. Memories and processes inside submodules (FL-13)

- **Processes.** None can exist: `flatten` is `REFUSE` (§2). A `$proc` node is never copied; `proc`
  must have run on every module. If it failed on some module, that module keeps its `$proc` nodes
  and the manager refuses `flatten` (IR-19, "a module with an error keeps all its `$proc` nodes").
- **`$mem`.** One node per array (2D EL-27/EL-39): ports for reads and writes, parameters `MEMID`
  (string), `SIZE`, `OFFSET`, `ABITS`, `WIDTH`, `RD_PORTS`, `WR_PORTS`, clocking bits and `INIT`.
  `flatten` copies it like any cell (pins on mapped nets) with
  `MEMID' = <prefix><sep><MEMID>`, the same name rule as §4. The `$memrd` and `$memwr` types
  (also registered, with their own `MEMID`) are treated the same. `MEMID`s must be unique in the
  flat module: a per-root set of `MEMID` strings (a `u64map` of strtab IDs) drives the `_N` suffix
  rule. Yosys does the same through `memory_map` and rewrites `MEMID` of `$mem*` cells.
- **`INIT`** is copied unchanged (a width-`SIZE·WIDTH` bits parameter).
- **Two instances of a module with a `$mem`** give two `$mem` nodes `u1.mem`, `u2.mem`: separate
  memories, as in Yosys and in the Verilog semantics.
- **Hard RAMs** (`single_port_ram`, `dual_port_ram`) are hard cells and are untouched (§7).
- **What flatten does not do** is merge, split or infer memories (`memory_*`; Phase 4).

## 10. Modules that become unused (FL-14, agent default (Peter may override); Peter's question 2)

Yosys deletes them (`Deleting now unused module`), keeping only the top, kept modules and black
boxes. **The IR has no way to remove a module**: IR-7 registers a cell type per module, the cell-type
table is append-only, and there is no `odin3_module_live` (3E's `vtr.c` notes it: "the IR has no way
to remove a module"). 29 files iterate `odin3_design_module_end`. So:

- `flatten` leaves them, and logs `flatten: N modules are no longer instantiated: a, b, … (the IR cannot
  remove modules)` at INFO (names capped at 8).
- Their memory stays (hierarchical + flat ≈ at most 2× peak, counted in the estimate, §12).
- **Consumers that matter pick the top**: `write_blif --vtr` (top + stubs only), `sim`, `hierarchy`.
  The plain `write_blif`, `write_rtlil`, `write_verilog`, `write_json` still write every module. A
  test or tool that wants the flat netlist alone reads **the top model** (the first `.model` of a BLIF; the
  `top` module of an RTLIL). `flatten-check` does (§14).
- **If a later phase wants removal**, the change is an IR addition (`odin3_module_retire`: mark
  dead, skip in iterators and writers, keep the cell type registered) with a 29-file blast radius.
  It is Peter's review, and not needed for the Phase 3 exit, so 3A does not propose it now.

## 11. Interface (FL-15, FL-16)

```
flatten [--separator <str>]
```

- **No `--top`:** the design's top is used (§2). `hierarchy --top` or the CLI's `--top` set it first.
- **`--separator <str>`** (Yosys: `-separator`; default `.`): non-empty, no blank, no `"`, `;` or `#`
  (a script word); applied to names and `MEMID`s. **Agent default (Peter may override)**, kept
  because some users read BLIF names with dots as hierarchy markers.
- **No other options.** Provenance is always on (it is the 3A deliverable); there is no `--no-prov`.
  Any other option is `unknown_option`.
- Spelling: Odin III's script words take `--long` options (2F). A single-dash alias of each is not
  provided (agent default).
- **Pass flag:** `processes = ODIN3_PASS_PROCESSES_REFUSE`; `wants_ast = false`.
- **Diagnostics (INFO unless noted):**
  - `flatten: top 'top': N instances expanded (M kept) in R roots; K modules unused`
  - `flatten: keeping 'path' (module 'm', keep_hierarchy)` (one per kept instance, capped at 20 + `N more`)
  - `flatten: renamed 'x' to 'x_1' (name taken)` (DEBUG)
  - `flatten: N modules are no longer instantiated: …` (§10)
  - Errors as §2, §3.3 (pre-flight), §3.7; all `<loc>` are the instance's SOURCE location when it has
    one, via `odin3_srcman` as the other passes do.

## 12. Caps, out of memory, check, determinism (FL-17)

**Caps** (named constants in `src/passes/flatten/flatten.h`, lowered in tests through a hidden
`flatten_test.h`, as 2D/2F do; **agent default (Peter may override)**):

| Cap | Value | Why |
|---|---|---|
| `ODIN3_FLATTEN_MAX_NODES` | 2^26 (67,108,864) | the flat module's live nodes; Titan's largest is 2M, the 19's largest ≈ 1M (`LU32PEEng`, `mcml`); 32× headroom, well inside `uint32_t` IDs |
| `ODIN3_FLATTEN_MAX_PINS` | 2^28 | likewise for pins (IR-5: IDs 32-bit; one cell may have 2^22 pins) |
| `ODIN3_FLATTEN_MAX_NETS`, `_WIRES` | 2^28, 2^26 | likewise |
| `ODIN3_FLATTEN_MAX_ROOTS` | 2^16 | `ODIN3_ELAB_MAX_SPECIALIZATIONS`; roots are modules |

No depth cap is needed: the Kahn sort rejects cycles, and the module count bounds the depth.

**Pre-flight estimate and the budget.** The estimate is the sum over roots of
`nodes·B_node + pins·B_pin + nets·B_net + wires·B_wire + (records)·B_rec + string bytes of the new
names` with constants **measured** in Task 2 (a fixture's `odin3_design_bytes_reserved` delta per
object, rounded up), plus the worklist and the net map. `odin3_budget_check(ctx, design, estimate)`
(60% of physical memory, shared with `proc` and the opt family, 2E §16) runs before the first
mutation and again every 2^16 created nodes (O(modules)), against the real `bytes_reserved`.
Failure: `ODIN3_ERR_PARSE` (the budget's status) with `odin3_budget_message`; the pre-flight failure
changes nothing.

**Out of memory and failure state.** Pre-flight (recursion, caps, budget) fails with the design
untouched. After the first mutation, an out-of-memory `NO_MEMORY` or a §3.7 error leaves the design
**partially flattened**: instances expanded so far are gone, the one in progress may be half
copied, and joins already made stay. The IR still satisfies check rules 1, 2, 3, 5, 6, 8, 9, 10, 11 (every
primitive keeps both sides of every relation; IR-15), but not necessarily 4 (a half-joined net can
have two drivers). The pass manager stops the script on the failure. **This is weaker than 2F's
refinement-on-failure contract (OP-13) and is an agent default (Peter may override):** making
each expansion transactional would need a rollback of `odin3_net_merge`, which the IR does not
have. The OOM test (§14) injects a failure at every allocation of a small fixture and requires: no
crash, no leak (ASan), `ODIN3_ERR_NO_MEMORY`, and rules 1, 2, 3, 5, 6, 8, 9, 10, 11 after it.

**Check.** `check` runs before and after the pass in Debug, as for every pass (CLAUDE.md rule 3). The
**post-conditions** `flatten` adds (asserted in Debug; tested): every root has no live module-type
node except kept ones; the number of live non-port, non-constant, non-instance nodes of a root
equals the pre-flight estimate; every wire of the root names live nets; every name map entry is
live.

**Determinism** (FL-14, PHASE3 risk 8, the exit's byte-identical reruns):
- the roots, the worklist (FIFO), the nodes, wires and nets of every copy go in ID order (IR-16);
  hash maps are used for lookup only, never iterated;
- names are pure functions of instance names and the `_N` rule, whose order is the creation order;
- the cell-type → module map is built in module-ID order;
- provenance operations are numbered in the worklist order;
- the output is independent of the order modules were created in, up to node IDs: the *set* of
  names, cell types and connectivity is invariant (tested by reordering the modules of a fixture).
Two runs give byte-identical `write_rtlil` and `write_blif` output.

## 13. Divergences from Yosys 0.55 (§17-style)

| # | Odin III | Yosys 0.55 / Parmys | Why / effect |
|---|---|---|---|
| 1 | order: top-down on the root, FIFO worklist; copies come from the original child | bottom-up: modules flattened first, then copied whole; `worklist.back()` order within a module | same netlist and names; O(flat size) not O(depth × size); node IDs differ (names, not IDs, are compared) |
| 2 | joined port nets are one net with several names (IR-2); no `connect` | `module->connect(...)` per port bit, merged later by `opt_clean`; the wire names stay | the IR's own model (IR-1, IR-2); the same set of public names in both |
| 3 | no `$scopeinfo` cells; the path and the module are in provenance (`hier`, parents). `parameter:<NAME>` attributes of instances die | `$scopeinfo` per public cell with `cell_*`/`module_*` attributes, `hdlname` on wires and cells, `scopename` with `-scopename` | IR-12 puts hierarchy into `DERIVED` records; `$scopeinfo` is not a registered cell and `opt_clean` removes it anyway |
| 4 | unnamed objects stay unnamed | private names `$flatten<inst>.<name>` | IR-14: names optional; writers generate names |
| 5 | an unnamed instance labels its children `$c<ID>` | n/a (RTLIL cells are always named) | BLIF `.subckt` has no name |
| 6 | a name clash gets `_1`, `_2`… per kind, and is counted | `Module::uniquify`: the same `%s_%d` | identical rule; per-kind namespaces (IR-14) |
| 7 | a constant net of a child maps to the parent's shared constant net (IR-4); constant nodes are not copied | constants are bits, no cells | keeps IR-4's one-node-per-value |
| 8 | a join that leaves two non-bus drivers on a net is an error at `flatten` (§3.7) | only constant-driven bits are an error; others are left for later passes | Release builds do not run `check`; cheap |
| 9 | no whitebox concept: `-wb` is absent; a `(* blackbox *)` module is a cell type and is never expanded | `-wb` ignores the `whitebox` attribute; a whitebox module is flattened with `-wb` | 2D reads `blackbox` only (EL-4); no Phase 3 design has a whitebox |
| 10 | a top is required | no top marked: every module is flattened | 3E's E13 and `sim` make the same demand; the Phase 3 script always has one |
| 11 | any unlowered `$proc` anywhere refuses the pass (manager rule) | `flatten` handles `RTLIL::Process` (copies and rewrites it) | IR-19 / 2D EL-33; `proc` always precedes |
| 12 | module attributes (`blif_clock`) of children are not merged into the root | n/a | BLIF `.clock` lists name a model's clocks; the root's list stands |
| 13 | unused modules stay in the design (§10) | deleted (`Deleting now unused module`) | the IR has no module removal |
| 14 | `--separator` default `.`; no `-noscopeinfo`, `-scopename`, `-nocleanup` | those options | rows 3 and 13 make them moot |
| 15 | recursion error text `flatten: recursive instantiation: a -> b -> a` | `Cannot flatten a design containing recursive instantiations.` | names the cycle |
| 16 | after a failure past pre-flight the design is partially flattened (§12) | `log_error` aborts the whole process | weaker than 2F's OP-13, stated |
| 17 | log text is Odin III's (§11) | Yosys's | — |

## 14. Evidence plan (FL-18)

Per CLAUDE.md rules 1–3 and 7: a unit test, a golden test and a `docs/PASSES.md` row; `check` before and
after in Debug; `/run-micro`. 3A is "light" review (PHASE3 table), but the evidence is the real
guard, because the pass's output is only as good as the comparison.

### 14.1 Unit tests (Unity, `tests/unit/test_flatten_*.c`; hand-built IR and `read_blif`)

Each is a named test; the plan's tasks list which task adds which.
- `flat_simple`: top with one instance of `sub` (input `a`, output `y = a & b`): no module-type node,
  the `$_AND_` is in top, wire `u1.a` exists, net of `x` has aliases `(u1.a, 0)`, names `u1.t` for a
  named internal net.
- `flat_alias_order`: the wire/alias list of a joined net is `[(x,0), (u1.a,0)]` in that order; merging
  of feed-through (`output y = a`) gives one net, aliases in `odin3_net_merge` order.
- `flat_nested_names`: three levels (`a.b.c`): names `a.b.c.x`, and a nested generate name
  `g[1].u2`.
- `flat_two_instances`: two instances of one module give disjoint names and independent nets.
- `flat_unconnected`: an unconnected input pin gives an undriven fresh net, an unconnected output a
  sink-less net; no crash.
- `flat_constants`: constants shared (one `$_CONST1_` node in the flat module for 100 instances); a
  child output tied to 1 joins a parent net, a net with a second driver is the §3.7 error.
- `flat_multidriver_error`, `flat_const_output_error`: the two §3.7 errors, text verbatim.
- `flat_bus`: an `inout` net with tristate drivers on two sides joins without an error.
- `flat_blackbox_kept`: `blackbox`, `hard` (`adder`, `single_port_ram` from the library) and a
  `(* blackbox *)` module's cell are copied untouched with parameters.
- `flat_keep_hierarchy_inst`, `_module`, `_value_kinds`: an instance attribute, a module attribute,
  INT 0 (not kept), STRING "" (not kept), STRING "x" and BITS 1 (kept); a kept module's own
  instances are expanded in place; kept inside an expansion stays an instance.
- `flat_mem_memid`: a `$mem` in `sub` instantiated twice: `u1.mem`, `u2.mem`, and the clash rule
  (a parent `u1.mem` forces `u1.mem_1`).
- `flat_params_copied`: widths, `LUT`, `INIT` bits byte-identical.
- `flat_attrs`: `keep`/`init` copied in key order; `blif_name` not.
- `flat_names_unique`: the `_N` rule for nets, nodes, wires; alias clash.
- `flat_unnamed_instance`: BLIF `.subckt` without `.cname`: children `$c<ID>.x`.
- `flat_recursion`: a → b → a (BLIF-built); error text; the design untouched.
- `flat_caps`: tiny caps (hidden hook) → the pre-flight error, design untouched; the budget hook
  (`odin3_budget_test_set_limit`) → the budget message.
- `flat_refuse_processes`: a module with a `$proc` → the manager's refusal, nothing run.
- `flat_no_top`: error text.
- `flat_prov`: every copied object's record is `DERIVED`, `hier` = the §6 path, parents = [original,
  instance]; the backward walk from a flat object reaches the child's source line first; a forward
  query on the instance's record finds the objects; `u1`'s and `u2`'s copies have distinct records.
- `flat_prov_nested`: a depth-3 path; no `DERIVED`-of-`DERIVED` chain.
- `flat_determinism`: two runs byte-identical (`write_rtlil`, `write_blif`); modules created in the
  reverse order give the same name/cell/connectivity set.
- `flat_oom`: failure injected at each allocation: no crash, no leak, `NO_MEMORY`, rules 1–3, 5, 6, 8–11 hold.
- `flat_check_post`: `check` FULL green after each of the above.
- `derive_hier_*` (Task 1): the IR function's contract.

### 14.2 Golden tests

- `tests/flatten/` (new): hand-written multi-model BLIF (`hand_body.blif` and `hand_ports.blif`
  already hold two bodies each; more are added for nesting and feed-through) and Verilog designs
  with hierarchy, generate and parameters. Each has a committed expected flat `write_blif` and
  `write_rtlil` (the **top model**; unused modules are written too, §10, and the test compares only
  the top). The CLI test `cli_flatten` runs the whole script `read_*; hierarchy; proc; flatten;
  write_blif`. PASSES.md's golden-test column names it.
- **There are no multi-model Odin II BLIF goldens.** PHASE3's "Order of work" says 3A is developed "on
  the hierarchical BLIF goldens (multi-model Odin II micros)". Scanned 2026-10-14: none of the 1,894
  `.blif` files under `~/odin3-ws/golden` has more than one `.model` with a body (the extra
  `.model`s are `.blackbox` stubs). The hierarchical inputs are therefore the hand-written fixtures
  above and Odin III's own elaboration of Verilog (§14.3, §14.5). This text in PHASE3 is amended
  (Task 5).

### 14.3 `flatten-diff`: the flat design against Yosys 0.55

`tools/flatten-check/flatten-diff` (Python 3 stdlib, `mypy --strict`, `unittest` under
`tests/tools/`; the shape of 2F's `opt-diff`). For each input Verilog file `F` with top `T`:

```
A: odin3 -p "read_verilog --nolatches F; hierarchy --top T; proc; flatten; write_rtlil a.il"
B: yosys 0.55 -p "read_verilog -nolatches F; hierarchy -top T; proc; flatten -noscopeinfo; write_rtlil b.il"
```

(B's `proc` output feeds on the same `hierarchy` and the same options as 2D's `elab-check`.)
Compare, on module `T` only:
1. **Public wire names and widths**: the set of `wire` names without `$` and their widths: exactly
   equal. (Our port wires `u1.p` appear in Yosys as `\u1.p`.)
2. **Cell names and types**: the multiset of `(type)` of public cells and the exact set of public
   cell names (`u1.add`), `$mem` `MEMID`s; the multiset of types of private cells (counts only).
3. **Instances that remain**: the exact list (type, name) of kept module instances.
4. **Connectivity equivalence**: both sides loaded into Yosys with the `equiv_make … equiv_simple
   -undef` script of 2F's `opt-equiv` (A as gold, B as gate; both flattened already; `equiv_induct`
   for sequential), result `equivalent` (the same result classes as `equiv-check`: identical,
   equivalent, unproven, differs).
5. **Exit codes** as `netlist-compare` (0 same, 1 differ, 2 error).

Corpus: `tests/micro/verilog` (10), `tests/flatten/` (designs with hierarchy), the 2D/2E corpus's
hierarchical files (`tests/elab`, `tests/verilog` ones with more than one module), and **the 19
VTR designs** (the large ones, with `LU32PEEng`, `mcml`, `bgm`, in the heavy leg through
`tools/heavy-run`, smallest first). Known expected differences are listed in
`tests/flatten/diff_expect.txt` with reasons (as `tests/opt/diff_expect.txt` does), never
suppressed silently.

### 14.4 Simulator-equal before and after (`flatten-sim`)

`odin3 sim` (1E) flattens hierarchy itself (`sim/build_hier.c`, an independent implementation with
its own naming); so the sim of the hierarchical design is the oracle for the sim of the flattened
one. `tools/flatten-check/flatten-sim <F> [--top T] [--vectors N] [--seed S]` runs
`sim` on `read_*; hierarchy; proc` and on `read_*; hierarchy; proc; flatten`, with the same random
vectors (seeded; reset rules of `sim-check`), and requires equal outputs on every cycle. It runs on:
every micro in `tests/micro` that has hierarchy and no RAM (1E has no `$mem` simulation: PHASE3 risk
5; RAM designs are covered by §14.3 only), the `tests/flatten/` fixtures, and the fuzz corpus. 1E
simulates the netlist view; the RTLIL-view word cells are simulated by the registry hooks (IR-8)
where they have them, else the case is `skipped` with the cell named (never `pass`).

### 14.5 Fuzzing (`fuzz_flatten`)

`tools/verilog-gen --flatten` (an option beside 2F's `--opt`, 2D's `--elab`, 2E's `--proc`) makes
random hierarchies: depth ≤ 6, fan-out ≤ 4 instances per module, parameterized children
(`#(.W(…))`), generate loops with instances, feed-through ports, an output tied to a constant,
one unconnected port, `inout` buses, and `(* keep_hierarchy *)` on random modules/instances.
`fuzz_flatten` runs each through §14.3 (names, cells) and §14.4 (sim) and `check`. Budget: 2,000
cases in `ctest` quick, 50,000 in the nightly leg by hand (PHASE3 #28: Peter does not run
nightlies; so the controller runs it via `tools/heavy-run` at PR time).

### 14.6 The VTR designs

Task 5's sweep runs, for each of the 19 (smallest first, `heavy-run`, PHASE3 #17): `read_verilog
--vtr-compat --nolatches; proc; opt; flatten; check` and records: wall time, peak RSS, flat nodes/
nets/wires, records created, bytes per object, `kept=` (expected 0), `renamed=`, and compares node
counts by cell type with Yosys 0.55's `flatten; stat` on the same input (equal up to the divergences
of §13). The numbers go into PHASE3 and calibrate §12's constants.

## 15. Files and interfaces (FL-19)

```
src/passes/flatten/flatten.h      internal: caps, options, odin3_flatten_run(ctx, design, opts)
src/passes/flatten/graph.c        cell type -> module map, Kahn sort, cycle report, size estimate (pre-flight)
src/passes/flatten/expand.c       the per-instance expansion (§3.3 steps 2-6), net map, joins, names
src/passes/flatten/names.c        prefixing, uniquify, MEMID set, `$c<ID>` labels
src/passes/flatten/prov.c         path strings, record derivation per object (uses odin3_prov_derive_hier)
src/passes/flatten/register.c     pass definition, option parsing, info lines, run information
src/passes/flatten/flatten_test.h hidden hooks: caps, budget
src/ir/prov.[ch]                  + odin3_prov_derive_hier (Task 1)
tools/flatten-check/{flatten-diff,flatten-sim,fuzz_flatten,flatten_common.py}
tests/flatten/                    fixtures, expected outputs, diff_expect.txt
tests/unit/test_flatten_*.c       §14.1
```

Every function stays within the gate (cyclomatic ≤ 15, ≤ 60 lines, ≤ 5 parameters): the expansion
state (`odin3_flatten_ctx`: the root, the net map, the worklist, the name table, the path string,
the provenance operation) is one struct passed by pointer. No recursion: worklist and Kahn queue
are explicit. No `malloc` of IR objects: scratch uses `util/vec`, `u64map`, `strbuf`, `arena`.

## 16. Review focus and risks (FL-20)

1. **Net joining is the whole pass.** The feed-through merge, the constant merge and the redirect of
   dead nets are where a bug hides; each has a unit test and the fuzz corpus builds them.
2. **Aliases through writers.** The flat module's nets carry several wires. `write_rtlil` and
   `write_verilog` must print them without dropping or duplicating (`connect` or a second wire).
   Task 3 checks it on the fixtures; any writer bug is fixed in a separate PR, not here.
3. **Memory.** Provenance records and the hierarchical copy kept beside the flat one (§10). The
   calibration in Task 5 is the evidence; if a design nears the budget, the response is a cap
   message, not a crash.
4. **The unused modules** (§10) mean `write_blif` (plain) of a flattened design is not one model;
   tests and tools must read the top.
5. **Weak failure state** (§12) — mitigated by a pre-flight that finds everything static.
6. **`opt` after `flatten`** (§2.1): without it the flat netlist is larger than Parmys's. The QoR bands
   (PHASE3 decision 3) sit in 3H; the O′/Oprov legs attribute any gap.
7. **A name with the separator in it** (an escaped Verilog identifier `\a.b `, or a BLIF net `x.y`)
   is ambiguous with a path. The names stay unique (§4.2); the path in provenance is exact.

## 17. Amendments to other documents (made by Task 5)

- `docs/IR.md`: IR-12 gains the `odin3_prov_derive_hier` sentence and its use by `flatten`; §11 lists
  "PHASE3 #29 (3A spec): `odin3_prov_derive_hier`"; §12's out-of-scope line "`flatten`" is removed.
- `docs/DESIGN.md`: §6 gets a line for `flatten` (after `opt`); §13 #6 notes 3A's contribution (§4.3).
- `docs/PHASE3.md`: the checklist item 3A; the stale "hierarchical BLIF goldens" sentence (§14.2); a
  decisions-log entry for this spec's agent defaults and Peter's answers.
- `docs/PASSES.md`: the row (Task 2 adds it, Task 5 completes its golden-test column).
- 3E spec/plan: E10's text is unchanged; a cross-reference to 3A is added to §13.

## 18. Decisions for Peter

Each has a recommendation; 1–3 change a rule or a document Peter owns, 4 confirms a policy.

1. **IR addition `odin3_prov_derive_hier`** (§6): the one new IR function; no field, rule or ABI
   change. Recommend **yes**. Without it the instance path is lost for unnamed objects (rejected
   alternative: an attribute per object, which costs far more memory).
2. **Unused modules after `flatten`** (§10): recommend **leave them** (no IR change; the writers that
   matter pick the top). The alternative is an IR change (`odin3_module_retire`) touching about 29
   files, for no Phase 3 exit need.
3. **Script text: `proc; opt; flatten; opt`** (§2.1): recommend **yes** for the O, O′ and Oprov legs
   (Parmys runs `flatten; opt -full`). This edits PHASE3's exit-test item 3 and the 3H spec and plan
   script strings; I make the edits in Task 5 if approved. If **no**, the flat netlist keeps
   cross-boundary constants and duplicates, and the QoR bands may be missed on LUT count.
4. **Name policy for DESIGN §13 #6 as far as 3A goes** (§4): dotted Yosys-style instance paths in the IR
   names (`--separator` can change them), unnamed objects stay unnamed (path in provenance),
   unnamed instances label as `$c<ID>`. Recommend **yes**; the final answer to §13 #6 (which style a
   writer prints) still waits for 3E-6/3H's measurement.

The remaining defaults (caps, FIFO order, constants sharing, multi-driver error, weak failure state,
`--separator`, no `--no-prov`, `$c<ID>` labels, no net attributes) are marked "agent default (Peter may
override)" where they occur and logged in PHASE3's decisions by Task 5.
