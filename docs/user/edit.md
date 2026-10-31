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
gate.remove()
```

- Every call checks its arguments first and changes nothing on failure (`Odin3Error`). Names are
  unique per module. An output pin may not join a net that already has a driver. Edits are refused
  while the design has an open simulation.
- Debug builds run the fast IR check after every edit.
- `set_cover` and `set_param_int` replace the node by a twin that takes its pins and name; the ID
  changes and attributes are not carried over.
- There is no undo, and module ports are not edited.
- The result goes through `write_blif`, `write_json` and every pass like any other design.

### Provenance of an edit

Every object an edit creates carries a provenance leaf that is distinguishable from HDL and netlist
origins: an IMPORTED record in a pass run named `edit` whose arguments are your tag. Read it with
`obj.edit_tag` (Python) or `odin3_edit_get_tag` (C). `None` means the object was not made by an
edit. Objects made under one tag share one record.

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
  vectors. That evaluator is combinational only, so sequential designs are covered by the unit
  tests instead.
