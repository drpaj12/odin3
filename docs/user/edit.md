# Editing a netlist and watching every net

Odin III can change a netlist after it is read, and simulate the changed netlist with every net
visible. Both are plain API calls (C ABI and the Python binding); no pass is involved. They exist so
that tools built on Odin III, such as Leman Russ III (hardware Trojan insertion), do not splice BLIF
text or write their own simulator. Spec: `docs/specs/2026-10-31-netlist-edit-sim-design.md`.

## Editing

```python
import odin3
design = odin3.Design()
design.read_blif("in.blif")
top = design.top()
net = top.add_net("t_q", tag="trojan:t1")
gate = top.add_sop(2, "11 1\n", "t_and", tag="trojan:t1")      # a BLIF .names
pins = gate.pins()                                              # A[0], A[1], Y
pins[0].connect(top.net("a"))
pins[1].connect(top.net("b"))
pins[2].connect(net)
victim_pin.rewire(net)       # move a sink onto the new net
gate = gate.set_cover("10 1\n", tag="trojan:t1")                # returns the twin node
ff = top.add_node("$_DFF_P_", "t_ff", "trojan:t1", params={"INIT": 0})   # starts at 0, no twin
gate.remove()                # refused if it drives a net with other pins; remove(allow_undriven=True)
```

- Every call checks its arguments first and changes nothing on failure (`Odin3Error`). Names are
  unique per module. An output pin may not join a net that already has a driver. Edits are refused
  while the design has an open simulation.
- Debug builds run the fast IR check after every edit. The check runs after the edit, so a
  failure (`ODIN3_ERR_CHECK`) leaves the edit applied; release builds never check.
- `set_cover` and `set_param_int` replace the node by a twin that takes its pins, name and
  attributes; the ID changes. The call returns the twin (C: the `twin` out parameter, set as soon
  as the old node is replaced, so it is valid even if a later step fails; Python: the exception's
  `.twin`).
- `remove_node` of a node that drives a net with other pins is refused, unless `allow_undriven`
  is set; the net is then left undriven and the Debug check may report it.
- `rewire` is a disconnect then a connect. If the connect fails the pin returns to its old net, at
  the end of that net's pin array.
- C structs `odin3_edit_new` and `odin3_simulation_run` start with `size` (set it to `sizeof` of
  the struct) and carry a `reserved` word that must be zero. `odin3_edit_new` accepts exactly the
  first release's size (56 bytes) or a size of at least the current struct; any size in between is
  refused. A non-zero `n_params` is refused by every call but `add_node`. A library from before the
  parameter fields would ignore them, so the Python binding checks for `odin3_edit_get_events` when
  `params=` is given and raises "libodin3 too old" otherwise. The ABI version stays 7.
- There is no undo, and module ports are not edited.
- The result goes through `write_blif`, `write_json` and every pass like any other design.

### Provenance of an edit

Every object an edit creates carries a provenance leaf that is distinguishable from HDL and netlist
origins: an IMPORTED record in a pass run named `edit` whose arguments are your tag. Read it with
`obj.edit_tag` (Python) or `odin3_edit_get_tag` (C). `None` means the object was not made by an
edit. Objects made under one tag share one record.

A twin made by `set_cover` or `set_param_int` keeps the old node's ancestry: its record is a
DERIVED record (class rewrite, in a new run `edit` with your tag) whose parents are the old node's
record, first, and the edit's IMPORTED record. Source locations and the construct chain of the
old node are still reached through it, and `edit_tag` still reads your tag.

### Edit events: moved pins, and export

Creation is not the only edit. Each node and net carries a list of edit events, `(kind, tag)`, oldest
first, read with `obj.edit_events` (C: `odin3_edit_get_events`):

| kind | recorded on | when |
| --- | --- | --- |
| `added` | node or net | an `add_*` call made it |
| `param` | the twin node | `set_cover` or `set_param_int` replaced the node |
| `rewired` | the node of the pin | `Pin.rewire` moved the pin to another net (or onto a net from none) |
| `removed` | the node of the pin | `Pin.disconnect` took a connected pin off its net |

`rewire` and `disconnect` take an optional `tag`; without one the event carries the tag `edit`. An
identical event is recorded once. `connect`, `remove_node` and `remove_net` record nothing (use
`rewire(net, tag=...)` to mark a connect on a clean node). A twin keeps the events of the node it
replaced. Tags may not contain a newline: `add_net`, `add_node`, `add_sop`, `set_cover` and
`set_param_int` refuse such a tag (they accepted it before the events existed). The events live in
an attribute whose name contains a blank (`odin3 edit`), so a user attribute (including one named
`odin3_edit`) never collides with them.

The events are not printed by default, so every default output is unchanged. To keep them visible
after export, ask for them:

- `write_json --odin3` adds `"odin3_edit": "added:trojan:t1,rewired:t2"` to each cell and netname
  that has events; `read_json` restores them (older readers skip the member).
- `write_blif --edit-tags` (plain dialect; the VTR dialect has no `.attr`) writes
  `.attr odin3_edit added:trojan:t1,rewired:t2` after the cell. It is Yosys extended BLIF: the VTR BLIF
  parser reads `.attr name value` with one value token, and VPR accepts it only in extended BLIF, like
  `--src-attr`. `read_blif` restores the events. BLIF has no net attributes, so a net's `added` event is
  carried by the cell that drives it; JSON carries net events too.
- The value is one token: each tag is percent-encoded for blanks, control bytes, `%`, `,`, `#`, `"`
  and `\`. After a read-back the provenance record is gone, so `edit_tag` returns the tag of the first
  `added` event (else the first event).

```python
pin.rewire(net, tag="trojan:t1")          # the pin's node gets "rewired trojan:t1"
design.run_script("write_json --odin3 out.json; write_blif --edit-tags out.blif")
```

## Setting the top before `read_verilog` (Python)

`Design(top="name")`, `design.run_script(text, top="name")` and `design.run_pass(name, args,
top="name")` set the requested top module name for the run, as the CLI `--top` does, so a script that
reads Verilog needs no separate step. The name is set around each call and the previous value is put
back afterwards. The C setting is process-wide, so every `run_pass` and `run_script` of the binding
holds one lock for the whole run: runs from several threads are serialised and never see each
other's top. The previous value is the one the binding last set; a host that calls
`odin3_pass_set_top_name` itself is not tracked.

## Simulating every net

`Simulation.signal(name)` resolves any live named net of the simulated module, with no
`(* keep *)`, for a design read from BLIF or edited here (`$`-prefixed names resolve by name too,
but are not listed). A pass such as `opt` or `abc` that removed a net still removes it.

```python
sim = design.simulate()
sim.set_toggle_counting()                 # track every signal known now
rows = sim.run_vectors(["en"], [[1], [1], [0], [1]], ["q", "d"])   # set, step 1 cycle, read
sim.toggles("d")                          # bit changes of d between cycle ends
```

- `run_vectors(inputs, rows, outputs)`: for each row set the inputs (each at most 64 bits), step one
  cycle, read the outputs; returns one list per row.
- Toggle counts compare each signal with its value at the end of the previous cycle (a clock input
  reads 0 because it is sampled once per cycle). `reset()` zeroes them. They are off by default and
  cost nothing when off. They are the energy proxy for tools such as Leman Russ III.
- The gate `tools/netlist-edit-sim/check_vs_oracle.py` compares every simulated net with an
  independent evaluator (Leman Russ III `blif_sim.py`, named by `$ODIN3_LEMANRUSS`) on random
  vectors, once on the design as read and once after a `set_cover` and a `rewire`, comparing against
  the written edited BLIF. That evaluator is combinational only, so sequential designs are covered
  by `check_vs_oracle.py --selftest` and the unit tests, which check `run_vectors` and toggle counts
  against hand-worked values. Leman Russ III is a private repository, so CI cannot run the oracle
  gate: it skips and prints a `SKIP: netlist-edit-sim oracle gate NOT RUN` line and a GitHub
  warning annotation. Run it locally with `ODIN3_LEMANRUSS` set before merging changes here.
