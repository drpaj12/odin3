# Reverse-engineering architecture (for the tool after v1.0)

Status: design note (Peter and the controller, 2026-10-21, Phase 7 brainstorm). Not built in
v1.0 except the core support of Phase 7 item 7.4 (`submod`, design copy, `cone_check`, `raise`).
The `unmap` pass (v1.0.4) gives the floor of F1 (VTR hard cells in a netlist) its word-level cells
with exact provenance, so the first hypotheses on adders, multipliers and RAMs have probability 1.

## 1. The problem

Input: an anonymous BLIF: no names, no comments, possibly restructured by ABC or another
obfuscator. Goal: rebuild the higher-level design: guess which nodes are data flow and which are
control, guess larger structures (adders, multiplexers, register files, counters, FSMs), and
build hierarchy and word-level operations from the guesses, phase by phase, with Bayesian
reasoning, small graphs (graphlets) and AI models.

## 2. The pyramid

1. **Ground truth.** The fine-grained netlist, read as Odin III IR (level 0). Speculation never
   edits it. Node and net IDs are stable `uint32` values, so everything above refers to them.
2. **Hypothesis overlay.** A guess is a cluster record: the node IDs it covers, a kind (adder,
   mux, register file, counter, FSM, control versus datapath), how its boundary nets map to the
   kind's ports, and a probability. Clusters nest (clusters of clusters), so levels form a lattice,
   not one tree. Competing hypotheses over the same nodes coexist with their probabilities: what
   a Bayesian search needs. The overlay is a data structure of the RE tool (Python), not of the
   core; it is cheap and fits graph libraries and models.
3. **Checking a guess is evidence.** "This cone is an 8-bit adder" is proved or refuted: cut the
   cluster and compare it with a template cell (`cone_check`, Phase 7.4c): equivalent, differs
   (with a counterexample) or unproven within a time limit. A strong likelihood term for the
   Bayesian update, beyond structural matching.
4. **Committing a level gives a real IR.** Copy the design (`odin3_design_copy`, 7.4b); replace
   each chosen cluster by a submodule (`submod`, 7.4a) or, when proved, a word-level cell
   (`raise`, 7.4d); each new object carries provenance to the fine nodes it came from. Level 1 is
   an ordinary Odin III design: every writer, `write_dot`, `levelize`, the graph export and any
   GUI work on it, and the next round of guesses runs on level 1. Any object traces down to the
   gates.

Why not the alternatives: editing one IR in place loses alternatives and cannot back out a wrong
guess; a full IR copy per hypothesis is too heavy for thousands of hypotheses; an e-graph (below)
is elegant but a second core data structure.

## 3. E-graphs (an option for step 3)

An e-graph (equality graph) holds many equivalent forms of a circuit at once: e-nodes are
operators whose inputs are e-classes; an e-class is a set of e-nodes proved equal. Rewrite rules
(`x*2 -> x<<1`, a ripple of full adders -> `add`) only add equal forms; equality saturation applies
them to a fixpoint or a budget, so rule order does not matter; extraction picks one form per
e-class by a cost (smallest, most word-level, most likely). Known through `egg` (Willsey et al.,
POPL 2021) and `egglog`; used for datapath optimisation and RTL rewriting.

Fit: rules that raise gates to word operations keep every interpretation side by side (the
"multiple IRs going up" in one structure), and extraction by "most word-level" or "most likely
under the model" picks the best. Limits: it holds proved equalities, not probabilistic guesses
("probably control"), so the overlay is still needed; sequential logic (registers, feedback) needs
extra work; it is research-scale. Later use: an e-graph engine inside step 3, raising proved
combinational cones to word operations, as a plugin (e.g. `egg` in Rust or `egglog`) with no core
change.

## 4. The loop

```
read_blif anonymous.blif            level 0, never edited
repeat:
    propose hypotheses              overlay: clusters, kinds, bindings, probabilities
    for promising clusters:
        mark nodes (attribute), bind boundary nets (odin3:bind)
        cone_check                  evidence: equivalent / differs / unproven
    update probabilities            Bayesian step
    commit a level:
        design_copy; submod / raise on chosen clusters
    next round on the new level
write_verilog / write_json --odin3  hierarchical, word-level, traceable to gates
```

`plugins/python/re_round.py` (Phase 7.4e) runs one round of this loop on a small example.

## 5. Gaps found by the 7.4e review

Read-only review of `include/odin3/odin3.h` and `plugins/python/odin3.py` against section 2 and
the loop of section 4, done while writing `re_round.py` (7.4e). One line each: gap, then the
suggested fix.

Fixed in 7.4e:

- The Python binding did not register the linked ABC, so `cone_check` and `abc` failed with
  `ENGINE` outside the CLI. Fix made: `Odin3.load_abc()` (loads `libodin3_abc.so` as a plugin).

Open:

1. Attributes are strings only, with no remove and no enumeration (`odin3_attr_get_string`,
   `odin3_attr_set_string`): a mark (`grp`, `odin3:bind`) cannot be cleared, and a tool cannot
   list the attributes of an object. Fix: `odin3_attr_remove` and `odin3_attr_visit`.
2. `cone_check` keeps one result per module (`odin3:cone_check` and friends): a second call
   overwrites the first, so a tool must read the result and `raise` before the next check. Fix:
   result attributes per key (`odin3:cone_check.<KEY=V>`), or an out-parameter in the ABI.
3. `cone_check` is combinational only (spec 3.3 C-3): clusters with registers (counters, FSMs,
   register files) cannot be proved. Fix: cut registers into template ports and compare per
   cycle (bounded sequential equivalence), as a later pass.
4. Reading is one cffi call per field, so a pass over 10^6 nodes from Python is slow. Fix: bulk
   array getters (`odin3_module_get_nodes`, a node's pins and nets in one call); until then use
   the JSON export and `plugins/python/odin3_graph.py` for whole-graph analysis.
5. Node, net and pin IDs are module-local (IR.md). After `submod` the moved nodes have new IDs in
   the new module, and a compacting pass (`abc`, `flatten`) renumbers: an overlay keyed by IDs
   is valid on one level only. Fix: an ID map output from `submod` and compaction, or key the
   overlay by provenance (`Node.sources()`), which survives.
6. `odin3_design_copy` does not copy held AST stores, the project or the architecture (spec
   3.2): a copy cannot run `write_verilog_ast` or architecture-aware passes. Fix: document in
   the ABI comment (done in `Design.copy`), copy the project when a later phase needs it.
7. No cell can be created or edited through the ABI: all mutation is a pass. A tool that wants a
   cell type other than those `raise` templates (`$add`, `$and`, ...) must add a pass. Fix: a
   generic `raise` with a plugin-supplied template, or `odin3_node_create` guarded by `check`.
8. The counterexample `odin3:cone_cex` is text (`PORT[i]=v,...`); the binding has no parser.
   Fix: `Module.cone_cex() -> dict[str, int]` once a tool needs it.
9. One linked ABC engine per process, passes run serially: scoring thousands of hypotheses is
   single-core. Fix: one process per worker over a `write_json` snapshot, or a re-entrant
   session in the adapter.
