# Netlist-edit gaps from Leman Russ III round 5 (design, 2026-10-31)

Status: spec first; code follows on branch `feat/edit-gaps`. Parent spec:
`docs/specs/2026-10-31-netlist-edit-sim-design.md` (the edit API, #309). Purpose: Odin III goal 3
(provenance to HDL) and goal 2 (research platform): an inserted Trojan, or any edit, stays visible
after export, so Loki III, Mimir III and detectors can tell edited cells from clean ones. The answer
key stays outside the netlist the detector sees (Leman Russ III writes `labels.json`); this work only
lets Odin III carry the edit provenance when asked. Source: `lemanrussIII` `docs/results-round5.md`
and `docs/specs/2026-10-31-htl-design.md` section 9.2. Measures: round trip equality of tags,
default output byte-identical, correctness of each gap against a test.

## 1. Gaps

1. Edit tags are not persisted by `write_blif` or `write_json --odin3`.
2. `disconnect` and `rewire` leave no mark on the nodes whose pins moved.
3. `add_node("$_DFF_P_")` creates INIT 3 (x); setting 0 needs `set_param_int`, which returns a twin.
4. `Design.run_script` (Python) cannot set the top before `read_verilog`; the CLI has `--top`.

## 2. Edit events (gaps 1 and 2)

### 2.1 In memory
An edit event is a pair (kind, tag). Kinds: `added` (the object was created by an edit), `param`
(set_cover or set_param_int: the node is a twin with a changed parameter), `rewired` (a pin of the
node moved to another net, or from no net onto one), `removed` (a pin of the node was disconnected
and left on no net). Events live in one STRING attribute `odin3 edit` (with a blank, see agent default 8) on the node or net: lines
`<kind> <tag>\n`, oldest first. An identical (kind, tag) line is not repeated. Attributes are copied
to the twin by set_cover/set_param_int (already so) and by flatten, so events survive both.
Rejected alternative: a new `ODIN3_PROV_*` kind or a mutable provenance record: records are
immutable and hash-consed (IR provenance invariant), and a pin move changes no object; an attribute
needs no IR invariant change.
Agent default 1: the attribute is the single source for events. `added` and `param` events are also
written when the object is made (so a BLIF round trip, which loses the provenance record, keeps
them). `odin3_edit_get_tag` keeps reading the provenance record first; when there is none it falls
back to the tag of the first `added` event, else of the first event (after a round trip the tag is
restored this way).
Tags: an event tag may not contain a newline (`ODIN3_ERR_INVALID_ARG`, logged), because the
attribute is line based. Everything else is allowed.
The attribute is bookkeeping: `odin3_wattr_classify` marks it SKIP, so the Verilog and JSON
user-attribute dumps and the RTLIL dump never print it. Only the opt-in exports below do.

### 2.2 API (additive, ABI stays 7)
```
odin3_status odin3_edit_rewire_tagged(design, pin, net, const char *tag);
odin3_status odin3_edit_disconnect_tagged(design, pin, const char *tag);
odin3_status odin3_edit_get_events(const odin3_design*, odin3_obj obj, const char **text);
```
`odin3_edit_rewire` and `odin3_edit_disconnect` (unchanged signatures) record with the tag `edit`
(agent default 2: an untagged call still leaves a mark, and the tag says it had no caller tag).
The tagged forms refuse an empty tag. `odin3_edit_connect` records nothing (agent default 3: it
binds a free pin, normally of a node made by an edit that already has an `added` event; to mark a
connect on a clean node use `rewire_tagged`, which also works from an unconnected pin). Removal of
a node or net leaves no object to mark (agent default 4: `remove_node` and `remove_net` record
nothing; the `removed` kind is the disconnect of a pin). `get_events` gives the raw text (lines
`kind tag`, NULL when none; owned by the design). Python: `obj.edit_events` -> list of
`(kind, tag)`; `Pin.rewire(net, tag=None)` and `Pin.disconnect(tag=None)` choose the tagged form
when a tag is given.

### 2.3 Export format (gap 1)
Both formats use the same event text, one token without blanks: events joined by `,`, each
`kind:tag` with the tag percent-encoded for every byte <= 0x20, 0x7f, `%`, `,`, `#`, `"`, `\`
(`%` + two uppercase hex digits). Example: `added:trojan:t1,rewired:trojan:t1`.

- JSON (`write_json --odin3`): a string member `"odin3_edit"` on each cell (node) and netname
  (net) that has events, next to `odin3_prov` and `odin3_level`. Gated by `--odin3` like them, so
  the default JSON is unchanged. `read_json` restores the member as the `odin3_edit` attribute
  (decoded); it already accepts unknown `odin3_` members, so older readers skip it.
- BLIF: opt-in `write_blif --edit-tags` (field `edit_tags` of `odin3_blif_write_opts`), PLAIN
  dialect only (the VTR dialect has no `.attr`; the flag there is refused as `--src-attr` is). It
  writes `.attr odin3_edit <text>` after the cell, in the cell's extras block. Why `.attr`: the
  reader already accepts Yosys extended BLIF `.attr`/`.param`/`.cname` lines, and the VTR BLIF
  parser (`libs/EXTERNAL/libblifparse`, `blif_parser.y`: `attr: DOT_ATTR STRING STRING`) takes
  `.attr name value` with one value token and no quotes needed; VPR accepts it only in extended
  BLIF (`read_blif.cpp`: ".attr Supported only in extended BLIF format"), the same status as
  `--src-attr`. The value is one token, hence the encoding. A sidecar file was rejected: it
  detaches from the netlist and needs a second path argument.
  `read_blif` restores a `.attr odin3_edit` line to the `odin3_edit` attribute (decoded) instead of
  a `blif.attr:odin3_edit` string, so a default (no flag) write after a read does not print it.
- Limit (stated): BLIF has no net attributes, so BLIF carries cell events only; the tag of an
  edit-made net is visible on the cell that drives it. JSON carries both. A node's `added` event
  survives through the cell; the provenance record itself is not exported (it never was).
- Default BLIF is byte-identical: the new code runs only under `--edit-tags`. Proof: the golden
  and `write_blif` suites pass unchanged, and a test compares default output of an edited design
  with and without the feature compiled in (same text as before the flag existed: no `.attr`).

## 3. Initial parameters on add_node (gap 3)
`odin3_edit_new` gets appended fields `const char *const *param_names; const int64_t *param_values;
uint32_t n_params; uint32_t reserved2;` (sizeof 56 -> 80). The library reads only what `size`
covers and treats the rest as zero, so a caller compiled against the 56-byte struct still works
(`size` must still be at least 56). `add_node` applies each (name, value) to the cell type's INT
parameter of that name over the default; an unknown name, a non-INT parameter, a duplicate name or
more than 8 parameters is `ODIN3_ERR_INVALID_ARG`, nothing changed. `set_param_int` keeps its twin
semantics. Python: `Module.add_node(cell_type, name, tag, params={"INIT": 0})`.

## 4. Python top before read_verilog (gap 4)
The C ABI already has `odin3_pass_set_top_name` (process-wide; the CLI `--top`). The Python
wrapper never called it. Add `top: str | None = None` to `Design.run_script` and `Design.run_pass`
and a `Design(top=...)` constructor argument kept on the design and used by every later run
(same meaning as `--top`: it wins over a file's own top for `read_blif`/`hierarchy`/`read_verilog`
elaboration). Agent default 5: the name is set around each call and the previous value restored afterwards,
under one module-level lock, because the C setting is process-wide and must not leak into another Design; the constructor value
is re-applied per call. Agent default 6: no new C symbol is needed.

## 5. Tests
- C unit `tests/unit/test_edit.c`: events for add, param, rewire, disconnect; tagged forms;
  newline tag refused; get_tag fallback; add_node with INIT=0 gives INIT 0 and no twin; old-size
  struct works; bad param name refused; BLIF `--edit-tags` writes the `.attr`, default does not;
  read-back restores events; JSON `--odin3` round trip.
- Golden: `tests/golden` has the existing write_blif goldens, unchanged (default output); a new
  golden `tests/golden/edit_tags` compares the `--edit-tags` BLIF and the `--odin3` JSON text.
- Python `tests/tools/test_python_edit.py`: events through the binding, add_node params, top
  before read_verilog (a two-module file where the default top differs from the named one).
- Docs: `docs/user/edit.md`, `docs/user/outputs.md`, `docs/PASSES.md` rows for `write_blif` and
  `write_json` changes, `include/odin3/odin3.h` paragraphs, `CHANGELOG.md`.

## 6. Agent defaults log
1. Events are an attribute (`odin3 edit`), not provenance records; no IR invariant change.
2. Untagged `rewire`/`disconnect` record the tag `edit`.
3. `connect` records nothing; `rewire_tagged` covers a connect on a clean node.
4. Node/net removal records nothing.
5. Python top is set per call and the previous value restored; the whole run holds a module-level
   lock (fix round 1; the C top name is process-wide, cffi releases the GIL, and the C API has no
   getter, so "previous" is the value the binding last set).
6. BLIF carries cell events only; nets only in JSON.
7. Percent encoding for the export token. A decoded NUL or newline is malformed: the reader keeps
   the text as a plain attribute and warns.
8. (Fix round 1.) The events attribute key is `odin3 edit` (with a blank), so no BLIF `.attr`, JSON
   member or HDL attribute can collide with it; the file name of the events stays `odin3_edit`. A
   writer treats the key as events (SKIP) only when its value is a well-formed event list; a
   malformed value is an ordinary attribute, and the opt-in exports warn and skip it instead of
   failing the write.
9. (Fix round 1.) `odin3_edit_new` sizes: exactly 56 (offsetof param_names) or at least the current
   size; the sizes between are refused. A non-zero `n_params` is refused outside add_node. ABI
   version: stays 7. Policy: this repo bumps for incompatible changes and has bumped for additive
   ones (D13, D14) once per release; ABI 7 is unreleased (v1.2.0 is not tagged) and the change
   is additive, so a bump would force a plugin rebuild for nothing. A new caller on an old library
   is caught by the Python probe of `odin3_edit_get_events` when `params` is given; C callers
   compile against one header.
10. (Fix round 1.) An edit step that fails after its object exists undoes it: add_net and
    add_node/add_sop delete the new net or node when the `added` event cannot be recorded, and
    rewire and disconnect put the pin back when the mark cannot be recorded. The Debug check
    after an edit is still not a pre-commit gate.
11. (Fix round 1.) Events copy through opt and lower: each gate that `odin3_kit_copy_attrs` makes
    from an edited cell carries the `added` event. This is intended: for Trojan provenance the
    gates of an inserted cell stay marked as inserted.
12. (Fix round 1.) A creation tag with a newline is refused by add_net, add_node, add_sop,
    set_cover and set_param_int (behaviour change, in the CHANGELOG and the API doc).

## 7. Progress and next step
Spec written. Next: C events module (`src/ir/edit_events.c`), edit.c recording, writers/readers,
tests, Python, docs.
