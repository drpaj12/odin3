# Provenance step files

Version 1.4. Every flow step that creates or changes objects writes a machine-readable provenance
manifest, so that a tool such as Loki can read provenance at every stage: from the HDL, through Odin III's
IR and netlists, to a placed, routed and extracted layout (silicon flow, {doc}`silicon-flow`) or to LUTs,
BLEs and sites (VTR flow, {doc}`odin3-flow` with `--prov`). The chains inside Odin III stay in the
version 2 name map ({doc}`provenance`); the step files add the links between stages and do not copy
the chains.

## Layout of a run directory

```text
OUT/prov/index.json                    the run: flow, design, tools, steps with sha256
OUT/prov/NN-STEP.json                  the header of one step
OUT/prov/NN-STEP.objects.jsonl         the objects of the step, one JSON object per line
```

`NN` is the step order. A check-only step (`equiv`, `lvs`, `netlist`) has a header and no objects file
(`objects_file` is null; the header has a `check`). The index is written last; its sha256 values let a
reader detect a partial run. No absolute path, home path or user name is ever written (the writer
refuses, as the run record does).

JSON Schema files: `docs/schemas/prov-index.schema.json`, `prov-step.schema.json`,
`prov-object.schema.json` (draft 2020-12). `python3 tools/silicon/schema_check.py OUT/prov` validates a
run against them, checks the sha256 values, that every link target is an object, and that an object with
`origin: hdl` has a link (stage `hdl` excepted).

## The index

`schema` (1), `kind` (`odin3-prov-index`), `flow`, `design`, `top`, `odin3_version`, `project`, `tools`
(`id`, `version`), `inputs` (design files with sha256), `steps`: `id`, `stage`, `file`, `objects`, `sha256`,
`objects_sha256`, `count`, `needs`, `tool`.

## The header

`step`, `stage`, `tool`, `objects_file`, `needs`, `inputs`, `outputs` (role, relative path, sha256),
`links` (how this step's objects relate to the previous stage: `from_step`, `kind`, `key`, `scope`,
`confidence`), `counts` (`objects`, `by_kind`, `by_origin`, `by_link_kind`), `census` (steps `place` and
`route`: per OpenROAD sub-step, instances kept, moved, resized, inserted by kind, removed, renamed),
`check` (`name`, `result`, `detail`), `map` (the name map file and sha256, when there is one).

## An object

```json
{"id":"gates:o3__c8","stage":"gates","kind":"cell","name":"o3__c8","type":"sky130_fd_sc_hd__nor2_1",
 "origin":"hdl","links":[{"to":"ir:$c8","kind":"cone","confidence":"medium"}],
 "src":[{"file":"bm.v","span":"12.6-12.12"}],"attrs":{"odin_name":"$c8","tie":""}}
```

- `id` is `STAGE:LOCAL`, unique in the run. Local ids: `ir` and `gates` use the object name; `placed` and
  `routed` the instance name; `gds` and `extracted` use `MASTER@X,Y` (no names: positions in nm);
  `recovered` the DEF component it joined to, or `unjoined:MASTER@X,Y`.
- `kind`: `module block stmt net port cell`, and for the FPGA stages `lut ble clb site`. `device` is
  reserved (below).
- `origin`: `hdl` (a chain to source exists), `physical:STEP-KIND` (inserted by the flow: `tap`,
  `cts_buffer`, `repair_buffer`, `hold_buffer`, `fill`, `decap`, `antenna`, `tie`), or `unknown`.
- `links`: to objects of the previous stage (a join may also reach back: `recovered` links to the
  `extracted` cell it is, and by `location_match` to the `routed` instance). Empty only for the first
  stage and for physical origins. `src`: HDL spans as `dump_locs` prints them. `recs`: record ids
  (`r12`) into the name map named in the header, for the FPGA stages.
- `attrs` by stage: `placed`, `routed`: `master`, `x`, `y`, `orient`, `status` (and `resized_from`);
  `gds`, `extracted`: `master`, `x`, `y`; `recovered`: `def_comp`, `join` (`matched` or `unmatched`).

## Link kinds

| Kind | Meaning | Confidence |
|---|---|---|
| `id` | the same object, by construction (a writer or a copy) | exact |
| `name` | same instance name in the previous stage | exact |
| `cone` | a mapped gate to the RTL objects whose spans it shares (gatemap G-2, coarse). In the silicon flow the target is the `hdl` statement object of each source span of the gate's chain; the `ir` cells of that span carry a `fine` link to the same object (one link per span, linear in gates) | medium |
| `fine` | per-object record chain (name map `R` and `C` lines) | exact |
| `join` | an optimisation join (the name map `Q` and `K` lines) | as recorded |
| `resize` | same name, other master | exact |
| `physical_origin` | no HDL parent; inserted by the named step | exact (a label) |
| `location_match` | name-free: same cell type and location as an object of the previous stage | high when unique |
| `structural_match` | reserved for the transistor recogniser | as stated |

Confidence is one of `exact high medium low ambiguous`.

## Stages

`hdl`, `ir`, `gates`, `luts`, `packed`, `placed`, `routed`, `gds`, `extracted`, `recovered`.
Device-level (transistor) objects are not modelled in v1.4: the `extracted` stage holds cells only (Magic's
hierarchical extraction, one object per standard cell). The `device` object kind and the
`structural_match` link kind are reserved for the transistor recogniser.

## FPGA flows

`odin3-flow --prov` writes the same files after the name map: stage `hdl` (the construct table, `C`
lines), stage `luts` (the `O` and `N` lines; link `fine` to the constructs of the record chain, `recs`
to the records). `odin3-prov-steps FLOW_DIR` (or `python3 tools/flow/prov_steps.py FLOW_DIR`) converts a past
`--prov` run. When `odin3-prov-post` has added `A` lines, one more stage `placed` is written (one `site`
object per atom, link `name` to the LUT object, `recs` from the atom). Stage `packed` (BLE, CLB), the
`Q`/`K` join links and an `ir` stage are not written in v1.4, and the index lists no design `inputs`
(the map and the BLIF are recorded as step `outputs` with their sha256). Source file names are made
relative to the design directory (base name when outside it). Without `--prov` no step files are written.

## The Loki contract

Loki reads `index.json` (`schema`, `flow`, `design`, `top`, `steps[].id/stage/file/objects/sha256/count`,
`tools[].id/version`), each header (`stage`, `links`, `census`, `check`, `map`) and each object (`id`,
`stage`, `kind`, `name`, `type`, `origin`, `links[].to/kind/confidence`, `attrs`, `src`, `recs`). Unknown
fields are ignored; a `schema` other than 1 is refused. `resolve(object)` in Loki walks `links` back to
stage `hdl` or `ir` and returns the chain with the weakest confidence on the way.

## Resolving an object to HDL

Follow `links` (the first link of an object with `origin: hdl`) until the stage is `hdl` or `ir`; then
`src` gives the file and span, and `recs` (FPGA stages) give the chain in the name map. An object with a
`physical:` origin has no HDL parent: the label says which step put it there.
