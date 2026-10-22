# v1.0.5: core options for Loki III L1 (`-const` for `cone_check`/`raise`, `submod -bus`) (design)

Status: for Peter's review. Patch release v1.0.5 of Odin III (post-v1.0 core changes are the patch
releases v1.0.1 to v1.0.5; decisions go into `docs/V1.0.5.md`). Source of the need: Loki III L1
spec (`loki/docs/specs/2026-10-21-l1-hypotheses-design.md`) section 3.2 (gap 7 and the "new" row)
and section 17 open questions 4 and 5, which Peter decided on 2026-10-21: "the core options
`raise -const PORT=V` and vector ports for `submod` are later Odin III patch releases (after
v1.0.4), each with its own spec". This spec covers both; each is its own PR.

## 1. Goal

Two small options on existing Phase 7 passes, so that Loki III L1 can commit what it proves:

1. **Constant operands.** `cone_check` and `raise` take `-const PORT=V`: the template's input
   port `PORT` is tied to the constant `V` instead of being bound to boundary nets. A cone that
   computes `Q + 1`, `A == 13` or `A < 10` can then be proved against one word cell and raised to
   it. Today every template bit must be bound to a net (`cc_tmpl_bind`, `tmpl.c:290`), and the
   ABI cannot create cells (RE-ARCHITECTURE section 5 gap 7), so L1 commits such pieces only as
   submodules with a `loki_fn` text.
2. **Vector ports from an attribute.** `submod` takes `-bus KEY`: boundary nets whose attribute
   `KEY` reads `NAME[BIT]` become one vector port `NAME` of the new module. Today a vector port
   forms only from a named wire whose bits are all boundary nets of one direction (`full_bus`,
   `submod.c:432`); a BLIF-read netlist has scalar nets only, and nets cannot be renamed through
   the ABI, so L1's buses stay `loki_bus` attributes on scalar nets.

**Done when:** the unit tests, the extended `cone_check_micros`, `raise_micros` and
`submod_equiv_micros` golden tests and a new Python test `python_core_options` (the calls exactly
as Loki III makes them through `plugins/python/odin3.py`) pass; `docs/PASSES.md` rows, `CHANGELOG.md`
and `docs/V1.0.5.md` are updated. No C ABI change: `ODIN3_ABI_VERSION` stays as v1.0.4 leaves it.

## 2. Code facts this design rests on (read 2026-10-21, `origin/main` at `23e30292`)

1. `cone_check` and `raise` share the option parser `cc_parse_args` (`src/passes/cone_check/
   register.c:107`) and the option struct `cc_opts` (`cone_check_int.h:67`); `raise` only sets
   `timeout_ok = false` (`src/passes/raise/register.c`). A new option in `take_option`
   (`register.c:83`) reaches both passes.
2. The template is built by `cc_tmpl_build` (`tmpl.c:179`): type, parameters, then a flat
   numbering of all port bits (`first`, `ord`). `cc_tmpl_bind` (`tmpl.c:290`) requires every flat
   bit to be bound exactly once to a boundary net through `odin3:bind`.
3. The proof's temporary design (`miter.c`) makes one input port `i<k>` per template input bit
   (`make_ports`, `miter.c:61`), stores its node in `job->in_ids`, and wires template input bit
   `b` to `cc_port_net(job->mod, in_ids[ord[b]])` (`template_port`, `miter.c:249`).
   `cc_port_net` returns the net of a node's first pin, so any one-pin driver node works there.
4. The counterexample text is built by `cex_text` (`prove.c:313`) over every template input bit.
5. The proof record is four module attributes; `raise` compares `odin3:cone_cell` with
   `cc_cell_text(opts)` (`tmpl.c:331`: the cell type then ` NAME=VALUE` per `-param`, sorted by
   name) and `odin3:cone_hash` with `odin3_cone_hash` (group nodes, pins, nets and `odin3:bind`
   texts; `cone_check.h`). The template's options are therefore checked by the cell text, not the
   hash.
6. `raise` builds `bit_net` (one net per flat template bit) in `make_ports` (`raise.c:157`),
   creates the cell with `odin3_node_create_connected` (`create_cell`, `raise.c:214`), then
   deletes the group (`replace_group`, `raise.c:291`); a failure before the first delete removes
   the new cell again. `prepare` (`raise.c:269`) runs in both the check phase and the run phase,
   so it must not create anything.
7. Constants are cells (IR-4): `$_CONST0_` and `$_CONST1_` nodes with one output pin; IR.md says
   front ends share one per value per module, and `check` has no rule enforcing that. The ABC
   region reads any net through `odin3_net_const_value` (`odin3_abc_pin_literal`,
   `src/passes/abc/region.c`), so a `$_CONST*_` driver in the temporary design is an AIG
   constant. Every pass that needs a constant net has its own helper (`lw_const` in
   `lower/kit.c`, `odin3_opt_const_net` in `opt`).
8. `submod` classifies boundary nets as `SM_IN`/`SM_OUT` (`submod.c:26`), then adds ports in net
   ID order (`build_module`, `submod.c:522`): `add_boundary_port` (`submod.c:492`) makes a vector
   port when `full_bus` holds for the net's primary wire, else a scalar port. The group struct
   `odin3_submod_group` (`submod_int.h:10`) has `key`, `value`, `name` and is built
   positionally in `register.c:174` and `submod.c` (`odin3_submod_extract_ctx`). `analyze`
   (`submod.c`) runs every refusal before anything changes, and the pass checks every group of
   every module before it moves any.
9. Python: `Design.run_pass(name, args)` passes the option text unchanged; `Node.set_attr` and
   `Net.set_attr` set string attributes; `Module.attr` reads them; `Module.ports()` gives
   `ModulePort(node, wire)` and `Node.ports()` gives each port's `width`
   (`plugins/python/odin3.py`). Nothing in the binding needs to change.

## 3. `-const PORT=V` for `cone_check` and `raise`

### 3.1 Syntax

`cone_check -attr KEY=V -cell TYPE [-param P=V ...] [-const PORT=V ...] [-timeout S]` and
`raise -attr KEY=V -cell TYPE [-param P=V ...] [-const PORT=V ...]`.

- `PORT` names an **input** port of the template cell type; the whole port is constant (every bit).
- `V` is an unsigned decimal integer (`13`, at most 2^64 - 1) or a binary string `0b<bits>`
  (MSB first, characters `0` and `1`, any length). It is zero-extended to the port width; a value
  that needs more bits than the port has is an error. `x` and `z` are not accepted (the proof
  reads them as 0, as for `-param` BITS, `tmpl.c` `parse_bits`).
- `-const` may be given for several ports, each at most once.

### 3.2 Rules

| # | Rule |
|---|---|
| K-1 | A template bit of a `-const` port is bound to its constant, not to a net. A boundary net whose `odin3:bind` names such a bit is an error (`cone_check: net 'n': binding 'B[0]': port 'B' is constant (-const B)`), located as the existing binding errors are. Every other template bit is still bound exactly once (`cc_tmpl_bind` unchanged for them). |
| K-2 | Errors (logged, `INVALID_ARG`, nothing changed, before any proof): unknown port (`cell type '$eq' has no port 'C'`), an output or inout port (`port 'Y' of cell type '$eq' is not an input: only inputs can be constant`), `-const` twice for one port (`-const B given twice`), a value that does not parse (`-const B=1x: '1x' is not a constant (decimal or 0b binary)`), a value wider than the port (`value 16 does not fit port 'B' (4 bits)`). |
| K-3 | The proof: in the temporary design, each constant bit is driven by a `$_CONST0_` or `$_CONST1_` node instead of an input port `i<k>`; the template reads it, the group copy does not (the group has no boundary net for it). Everything else (lowering, the miter, `&cec`) is unchanged. |
| K-4 | The counterexample `odin3:cone_cex` lists only the template input bits that are bound to nets (constant bits are not inputs of the claim). |
| K-5 | The proof record: `odin3:cone_cell` is the cell text of fact 5 followed by ` -const PORT=V` per constant port, sorted by port name, `V` as given (for example `$eq A_WIDTH=4 B_WIDTH=4 Y_WIDTH=1 -const B=13`). `raise` compares the text, so it must be given the same `-const` options with the same spelling, as it already must for `-param`. Without `-const` the text is unchanged (old records stay valid). |
| K-6 | `raise`: the new cell's constant port bits are connected to the module's constant nets: the output net of a live `$_CONST0_` / `$_CONST1_` node that is not in the group (the lowest node ID), else of a new such node with a fresh net. New constant nodes get the raised cell's DERIVED provenance record. They are created only in the run phase, after every module has passed the check phase (all-or-nothing stays as it is); when the raise is taken back (a failed first delete, fact 6), the constant nodes this run made are deleted too. |
| K-7 | The log line of `raise` is unchanged; the info line of `cone_check` is unchanged. |

### 3.3 What it covers for Loki III

| L1 claim | Template |
|---|---|
| `eq_const` (`Y = A == C`), `ne` | `$eq` / `$ne` `-param A_WIDTH=k -param B_WIDTH=k -param Y_WIDTH=1 -const B=C` |
| comparisons with a constant (`lt le gt ge`) | the same type, `-const B=C` (or `-const A=C`) |
| incrementer `Q + 1` (free-running counter's next state) | `$add -param A_WIDTH=k -param B_WIDTH=1 -param Y_WIDTH=k -const B=1` |
| decrementer, `Q - C` | `$sub ... -const B=C` |

A modulo counter (`Q <= (Q == 9) ? 0 : Q + 1`) is two or three cells, not one: it is still
committed by `submod`, but its `$eq` and `$add` parts can each be raised first. A counter with an
enable keeps the existing form (`B[0] = en`, no constant).

## 4. `submod -bus KEY`

### 4.1 Syntax and value format

`submod [-attr KEY] [-name N] [-bus BUSKEY]`. A boundary net of a group whose STRING attribute
`BUSKEY` has the text `NAME[BIT]` is bit `BIT` of the vector port `NAME`. This is the format
Loki III L1 writes (`loki_bus = "h<k>[<bit>]"`, L1 spec 7.4 and 9.3). `NAME` is non-empty, does
not start with `$`, and holds no `[` or `]`; `BIT` is a decimal integer from 0 to 2^31 - 1 (the
port width limit of `odin3_port_spec`).

### 4.2 Rules

| # | Rule |
|---|---|
| B-1 | Per group, the boundary nets with a `BUSKEY` value are grouped by `(NAME, direction)`. A group whose bits are exactly `0 .. W-1`, each on one net, becomes one port `NAME` of width `W`, range `[W-1:0]` (`has_range`, not `scalar`), bit `i` on the net with `NAME[i]`, direction as the nets' (`SM_IN` input, `SM_OUT` output). |
| B-2 | The port takes its place in the port list at its lowest member net ID (the order rule of fact 8, as wire buses do). Its name is made unique among the new module's wires as every port name is (`unique_wire_name`: `NAME_1`, ...). |
| B-3 | A `(NAME, direction)` set with a missing bit, a bit on two nets, or bits of one `NAME` in both directions does not form a port: each of its nets becomes a scalar port as today, and one WARN per such set names it and why (`submod: group 'g': bus 'h3' has no bit 2: its nets stay scalar ports`). A bus bit that is an internal net of the group is not a boundary net and is simply not a port bit, so the set has a gap (B-3). |
| B-4 | A net whose `BUSKEY` text is not of the form `NAME[BIT]` is a located error (`INVALID_ARG`, IR unchanged; found in the check phase with the other refusals). An INT attribute under `BUSKEY` is the same error. |
| B-5 | With `-bus`, a net that carries a valid `BUSKEY` value is never taken into a wire bus (`full_bus` returns false for a wire with such a bit): the explicit attribute wins. Without `-bus` nothing changes. |
| B-6 | The vector port's wire gets no copied attributes (each bit's old net keeps its own, including `BUSKEY`, in the parent module); `-attr`'s key is dropped as before. The instance connects bit `i` to the old net of `NAME[i]`; `write_verilog` writes the connection as a concatenation (`src/backends/verilog/writer.c`, the vector runs and concatenations). |
| B-7 | `odin3_submod_extract` and `odin3_submod_extract_ctx` (public in `submod.h`) are unchanged and never group by attribute. |

## 5. How Loki III calls it (Python, unchanged binding)

```python
lib = odin3.default(); lib.load_abc()
# prove and raise A == 13 on a scratch copy (L1 spec 8, 9.3)
for node in group_nodes: node.set_attr("lk7", "1")
for i, net in enumerate(a_nets): net.set_attr("odin3:bind", f"A[{i}]")
y_net.set_attr("odin3:bind", "Y[0]")
args = "-attr lk7=1 -cell $eq -param A_WIDTH=4 -param B_WIDTH=4 -param Y_WIDTH=1 -const B=13"
design.run_pass("cone_check", args + " -timeout 60")
if design.top().attr("odin3:cone_check") == "equivalent":
    design.run_pass("raise", args)
# commit with vector ports (L1 spec 9.3 steps 2-4)
for i, net in enumerate(bus_nets): net.set_attr("loki_bus", f"h12[{i}]")
working.run_pass("submod", "-attr loki_sm -bus loki_bus")
```

Loki III gates both uses on `Odin3.version()` being at least `1.0.5` (the ABI version does not
change, so it cannot tell). After this release L1's spec 3.2 rows "7 no cell creation" and "new:
no net rename, scalar submod ports" become "use `-const`" and "use `-bus loki_bus`"; that edit is
Loki III work, not part of this release.

## 6. Tests

- Unit `test_cone_check`: an incrementer cone against `$add` with `-const B=1` is `equivalent`;
  an `A == 13` cone against `$eq -const B=13` is `equivalent`, against `-const B=12` `differs`
  with a counterexample over `A` bits only; `-const B=0b1101` equals `-const B=13`; every K-2
  error and the K-1 binding error; the K-5 cell text.
- Unit `test_raise`: the incrementer raised to one `$add` whose `B` bits sit on constant nets
  (`odin3_net_const_value`), `check` clean; a module with a `$_CONST1_` outside the group reuses
  it (no new node); a proof with `-const B=1` and a `raise -const B=2` is refused.
- Unit `test_submod`: the `NAME[BIT]` parser; input and output vector ports from the attribute;
  B-3 fallbacks (gap, duplicate, mixed direction) with their WARN; B-4 refusal leaves the IR
  unchanged; B-5 precedence over a wire bus; no `-bus`, no change; `-bus` without a value is a
  usage error.
- Golden: `cone_check_micros` and `raise_micros` gain the incrementer and `A == 13` micros (and a
  constant-mismatch refusal); `submod_equiv_micros` gains a bus micro: `submod -bus` then
  `flatten` is equivalent to the input (`equiv-check`) and the written Verilog declares the
  vector ports.
- Python `python_core_options` (`tests/tools/test_core_options.py`): section 5's calls on a BLIF
  read with `Design.read_blif`.

## 7. Agent defaults (Peter may override; logged as V105-1 to V105-10 in `docs/V1.0.5.md`)

1. **V105-1** One option name for both passes, `-const PORT=V`, parsed once in `cc_parse_args`
   (fact 1); `raise -const` exists because `raise` replays the proof's options.
2. **V105-2** Whole ports only. A partly constant port (`B[3:1]` constant, `B[0]` a net) is not
   supported; no L1 claim needs it (the enable counter binds `B_WIDTH=1`).
3. **V105-3** Value syntax: decimal or `0b` binary, zero-extended, no truncation, no `x`/`z`.
   Verilog sized literals (`4'd13`) are not accepted (the `'` would need quoting in scripts).
4. **V105-4** The constant spelling is part of `odin3:cone_cell` as given (K-5), like `-param`
   values; `raise` does not normalise `13` and `0b1101` to one text.
5. **V105-5** `raise` reuses a `$_CONST0_`/`$_CONST1_` node outside the group, else makes one
   (K-6). It never reuses a constant `$sop` (a BLIF zero-input `.names`), which may belong to
   another group that a later `raise` deletes.
6. **V105-6** cone_check and raise change in one PR (`feat/cone-const`) although rule 1 says one
   pass per PR: `raise` has no new code path that can be reviewed or tested without the
   `cone_check` change, and shipping one without the other leaves proofs `raise` refuses.
   `submod -bus` is its own PR (`feat/submod-bus`).
7. **V105-7** Option name `-bus BUSKEY` (short, matches `-attr KEY`).
8. **V105-8** A malformed `NAME[BIT]` text is an error (B-4) because the attributes come from a
   tool that should write them right; an incomplete bus is data, so it falls back with a WARN
   (B-3).
9. **V105-9** The new bus code lives in a new file `src/passes/submod/busattr.c` with its
   functions in `submod_int.h` (the `SM_*` net states move there from `submod.c:26`), so it has a
   unit test of its own and `submod.c` (746 lines) does not grow much.
10. **V105-10** No version bump in these PRs: `CHANGELOG.md` gets the entries under
    `[Unreleased]`; the release step of v1.0.5 (after v1.0.1 to v1.0.4) moves them and bumps
    `project(... VERSION)` and `pyproject.toml`.

## 8. Not in scope

Partial constant ports; constants on `cone_check` group boundary nets (a group reading a constant
net still binds it); sequential templates (gap 3); net renaming through the ABI; `submod` ports
from attribute buses whose bits are split across groups; changes to the Python binding or the C
ABI.

## 9. Open questions (Peter accepted every recommendation, 2026-10-21)

1. **One PR for `cone_check` and `raise`** (V105-6) against rule 1. Recommend one PR; Peter
   reviews it as an IR change (it is the first core pass after v1.0 that adds constant cells to a
   module it rewrites).
2. **Constant bits on the miter as cells or as AIG constants.** This design adds
   `$_CONST*_` nodes to the temporary design (K-3) so the prove step is unchanged. The other way
   is to skip those inputs and hand constants to the AIG builder. Recommend the cells: no change in
   `prove.c` beyond the counterexample text.
3. **Should `-bus` also accept a plain `NAME` (one-bit bus)?** Loki III always writes `NAME[BIT]`.
   Recommend no (B-4 error) until a user needs it.
