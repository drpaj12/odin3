
## 13. As built (2026-11-08, branch feat/activity-pass)

Files: `src/passes/activity/{activity.c, act_model.c, act_prop.c, act_solve.c, act_out.c, act_vcd.c, act_int.h}`; test
`tests/tools/test_activity.py` (CTest `activity_unit`), reference `tools/activity/act_ref.py`, Monte Carlo `tools/activity/act_mc.py`,
comparison `tools/activity/act_check.py`, goldens `tests/activity/*.blif` with `.act.golden`. Deviations from the sections above:

- Feedback density (3.3) is computed as P(next != state) with the state held at 0 and at 1 and the combinational cone between state and
  next-state recomputed under that condition (a cone of up to 2048 cells; larger: next-state probability taken as independent of the state,
  counted as `cone_capped`). This is the one-frame rule made exact for a state bit that reaches its own next-state function through logic
  (a counter built from separate `.names` cells gets 1, 1/2, 1/4, 1/8). Probabilities are iterated first (Gauss-Seidel, Aitken
  extrapolation every 10 sweeps, damping when the change stops shrinking), densities are then computed once, so there is no density iteration.
- `--ff-start` takes a number only (no `init`). `--name-map` is not implemented: `--name-style hash` runs the same naming pass as `write_blif`,
  so the names are those of the name map `write_blif --name-style hash` writes. The `activity_source` attribute is not stored (the module attributes
  are integers); the label is the JSON `source` field. Module instances are opaque cells with a WARNING (flatten first), not an error.
- The hard `adder` is handled by name (sum = parity, carry = majority of a, b, cin), not by interpreting its `fn` lines; other hard cells are opaque.
  `--opaque-type NAME` (repeatable) forces a cell type opaque; ACE 2.0 reads an `adder` as a box with primary-input outputs, so the ACE comparison
  uses `--opaque-type adder`.
- Provenance (decision 1, option A): no change to the C code or the schema; `odin3-prov-steps` attaching `act_*` attributes is a follow-up in the tools.
