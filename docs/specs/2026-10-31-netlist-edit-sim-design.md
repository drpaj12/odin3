# Netlist editing API and full-net simulation visibility (design, 2026-10-31)

Status: spec first; code follows on branch `feat/netlist-edit-sim`. Purpose: Odin III goal 2
(graph and reverse-engineering research platform) and goal 3 (provenance to HDL). Consumer: Leman
Russ III (Trojan insertion, toggle-count energy proxy), later Loki III. Measures: correctness
(equal to an independent evaluator on every net), invariants kept (`check` clean after each
edit), runtime. Peter 2026-10-31: add the editing and simulation abilities to Odin III as
features; tools consume them. Origin: `lemanruss/docs/specs/2026-10-31-htl-design.md` sec. 6
(Module.add_node, add_net, Pin.connect/disconnect, Node.set_table) and the two workarounds
`lemanruss/htl/insert_netlist.py` (BLIF text splice) and `lemanruss/htl/blif_sim.py` (pure Python
simulator, because the Simulation API drops unkept internal nets).

## 1. Findings from reading the core (2026-10-31)

- The IR already has every mutation primitive internally (`src/ir/module.h`:
  `odin3_node_create`, `odin3_node_delete`, `odin3_net_create`, `odin3_net_delete`,
  `odin3_pin_connect`, `odin3_pin_disconnect`, attrs). They are not in the public ABI.
  The wrappers add: name-string interning, cell-type lookup by name, open-simulation guard
  (SA-11), provenance, and the Debug `check`.
- A `$sop` (BLIF `.names`) has parameters WIDTH and COVER; its pin count follows from WIDTH and
  never changes (`node_create` doc). So "set table" replaces COVER at the same WIDTH; a different
  width is remove + add (the caller keeps the pin wiring).
- Provenance records are `SOURCE | IMPORTED | DERIVED` (`src/ir/prov.h`); a pass run is a row
  `{name, args}`. An edit record needs no new record kind (see 3.1).

## 2. Editing API (C ABI, additive)

All take the design and a module ID; objects are `odin3_ref` (module, id) as in the readers.
`tag` is the caller's edit tag (non-NULL, non-empty; e.g. "trojan:t1").

```
typedef struct odin3_edit_new { const char *name, *tag, *cell_type, *cover;
                                uint32_t n_inputs, reserved; } odin3_edit_new;
odin3_status odin3_edit_add_net(odin3_design*, uint32_t module, const odin3_edit_new*, odin3_ref*);
odin3_status odin3_edit_add_node(odin3_design*, uint32_t module, const odin3_edit_new*, odin3_ref*);
odin3_status odin3_edit_add_sop(odin3_design*, uint32_t module, const odin3_edit_new*, odin3_ref*);
odin3_status odin3_edit_set_cover(odin3_design*, odin3_ref node, const odin3_edit_new*);
odin3_status odin3_edit_set_param_int(odin3_design*, odin3_ref node, uint32_t index, int64_t v,
                                      const char *tag);
odin3_status odin3_edit_connect(odin3_design*, odin3_ref pin, odin3_ref net);
odin3_status odin3_edit_disconnect(odin3_design*, odin3_ref pin);
odin3_status odin3_edit_rewire(odin3_design*, odin3_ref pin, odin3_ref net);   /* move a sink */
odin3_status odin3_edit_remove_node(odin3_design*, odin3_ref node);
odin3_status odin3_edit_remove_net(odin3_design*, odin3_ref net);
```

- `cell_type` is any registered or built-in type name (`$sop`, `$lut`, `$_AND_`, `$_DFF_P_`, ...).
  `add_node` uses default parameters; a parameterised type is then set with `set_param_int` /
  `set_cover`. `cover` is the BLIF cover text: rows `"01- 1\n"` (inputs, space, output char), parsed
  by the same row rules as `$sop_verify`.
- `rewire` = disconnect then connect in one call, atomic (nothing changes on failure).
- Every call: refuses with `ODIN3_ERR_INVALID_ARG` (logged) while the design has an open
  simulation; checks arguments before changing anything; runs `odin3_check_module` (FAST) in Debug
  builds on success (and aborts the edit with `ODIN3_ERR_INTERNAL`, logged, if it fails); names are
  unique per module (a duplicate name is `ODIN3_ERR_INVALID_ARG`); IDs follow IR.md (new IDs at
  the end of the store, dead IDs not reused until `compact`). Undo: none.
- Driver rule: `connect` of an output pin to a net that already has a driver is refused (the
  `elab2` driver-claim rule), unless the net is the one the pin is already on.
- Port nodes cannot be removed (IR-7); module ports are not edited.

## 3. Provenance of edits

### 3.1 Agent default: no schema change
A new `ODIN3_PROV_*` kind would change the record schema (ADR needed, ABI bump). It is not needed:
an edit makes an `IMPORTED`-kind leaf record (no source locations, no parents) in a pass run
named `edit` whose arguments are the caller's tag. The record is distinguishable in the DAG by
`run name == "edit"`, and the tag is `run.args`. One record per (tag, run); every object inserted
under one tag shares it (hash-consing). Reading it: `odin3_obj_get_prov` then
`odin3_prov_get_info` (run, op) and the pass-run table. Public surface for convenience:

```
odin3_status odin3_edit_get_tag(const odin3_design*, odin3_obj obj, const char **tag); /* NULL tag: not an edit */
```
Rewire, connect and disconnect change no object, so they record nothing. `set_cover` and
`set_param_int` replace the node by a twin (IR-15 `odin3_node_replace`: the twin takes the pins and
the name; the ID changes; attributes are not carried) and the twin carries the edit record
(agent default: the old record is not kept as a parent; a DERIVED record needs an op class and
adds nothing the tag does not already say).

No IR invariant is changed: records stay immutable, hash-consed, parents have smaller IDs, nodes
and nets keep one record each.

## 4. Simulation visibility and sequences

Today `find_signal` resolves wires and nets by name, but `opt` and `abc` remove names, and BLIF
nets without an own name are not listed. Plan (read `src/sim/signals.c` and `build_names.c`
before coding):
1. A design read from BLIF or built by edits has every net named; make all live nets findable by
   `find_signal` without `(* keep *)` in the *simulated module as given* (no pass has run).
   `get_signal_count` lists them (the listing grows; ABI additive).
2. `odin3_simulation_get_net_value(sim, scope, net, *value)` is not needed: signals cover it.
3. Toggle counts: `odin3_simulation_set_toggle_counting(sim, bool)` and
   `odin3_simulation_get_toggles(sim, signal, uint64_t *count)` (bit transitions of the signal
   between consecutive settles, per bit summed, counted per half-cycle settle of `step`), reset by
   `odin3_simulation_reset`. Off by default (no cost when off).
4. Sequences: `odin3_simulation_run_vectors(sim, signals[], n_signals, values[], n_vectors,
   uint64_t *out_values (optional), uint32_t out_signals[], n_out)`: for each vector set the inputs,
   `step(1)`, record the outputs. Width <= 64 per signal.

## 5. Gate (independent evaluator)

`tools/netlist-edit-sim/check-vs-oracle.py` (a tools script, not vendored into the core) loads
`lemanruss/htl/blif_sim.py` (path via `--oracle`), simulates a BLIF with N random vectors through
both, and compares the value of every net each vector. Set: tests/micro netlists + small VTR BLIFs
available in goldens. The oracle is external; the gate skips with a clear message when the path is
missing.

## 6. Tests
- C unit (`tests/unit/test_edit.c`): each edit, each refusal, `check` clean, names unique, ID
  rules, tag provenance, SA-11 guard.
- Python (`tests/test_edit.py`): the wrapper, wrapper probes for missing symbols.
- Round trip: edit, then `write_blif`/`read_blif` and `write_json`/`read_json`, then equiv-check;
  a no-op edit sequence (add net + node, remove them; rewire to the same net) is equivalent.

## 7. Docs, ABI policy
User page `docs/user/edit.md`; `docs/PASSES.md` has no row (API, not a pass); API reference
paragraphs in `odin3.h`. ABI policy: additive, `ODIN3_ABI_VERSION` stays 7 (precedent: the
`odin3_graphlets_count` addition in #304). Python wrapper: `Module.add_net`, `Module.add_node`,
`Node.set_cover`, `Pin.connect/disconnect/rewire`, `Node.remove`, `Net.remove`; each probes its
symbol and raises a clear "built against an older libodin3" error.

## 8. Agent defaults log
1. No new provenance record kind; edit = IMPORTED leaf in run "edit" with tag (3.1).
2. ABI additive, version stays 7.
3. No undo; no module-port edits.
4. Oracle (blif_sim.py) is an external tools-script dependency, never vendored.

## 9. Progress and next step
Done: edit API (C + Python + tests), toggle counts and `run_vectors` (C + Python + tests), the
oracle gate script `tools/netlist-edit-sim/check_vs_oracle.py` and CTest `netlist_edit_sim_oracle`,
`docs/user/edit.md`. Agent defaults 5 to 8: creation calls take an `odin3_edit_new` struct (the
lint caps parameters at 5); the edit leaf record has no operation (op 0); toggle counts sample
once per cycle end (a clock reads 0); net visibility needed no core change (a BLIF-read or edited
design already lists and resolves every named net; the unit test proves it on an internal net).

Gate result: the oracle is combinational and `.names` only, so the gate covers combinational
designs: `hand_wide.blif`, and the micros `ansiportlist.v` and `binops.v` after
`read_verilog; proc; opt; lower; write_blif --gates-as-names`: 16 and 322 nets, 64 random
vectors, every net equal. Sequential micros and the VTR benchmarks are skipped by the script
(`.latch`) and reported as skipped; their toggle and sequence behaviour is covered by the unit
tests against hand-computed values. `LU8PEEng`, `ch_intrinsics`, `memory_controller`,
`mkPktMerge`, `stereovision0` fail to lower (not caused by this work); `CRC33_D264` lowers but the
oracle cannot resolve it.

Next step: PR; the controller merges after the critique review. Open follow-ups: a sequential
oracle (extend `blif_sim.py` with latches) to gate the VTR BLIFs; a DERIVED edit record that keeps
the HDL ancestry of a node changed by `set_cover`.
