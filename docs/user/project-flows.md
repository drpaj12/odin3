# Flows in the project file

Version 1.4. A `.o3proj` file can describe named flows, their ordered steps and how each step's objects
relate to the previous step's (provenance relationships). The C core stores and dumps this description
(`dump_project`); it never runs a step. `odin3-flow` and the tools under `tools/` run flows.

## Schema version

The first statement of a file may be `schema 2`. A file without it is schema 1 and loads and dumps exactly
as before. The flow statements below need `schema 2` (else `syntax`: "needs `schema 2` as the first statement").
`schema` anywhere but first is `syntax`; a number above 2 is `unsupported_schema`. An Odin III 1.3 binary
reading a schema 2 file stops at line 1 with `unknown_key: schema`; it never silently ignores flow statements.

## Statements

```text
schema 2
tool openroad required openroad version-arg -version
tool klayout  required python   module klayout.db
tool pdk      required sky130A  env PDK_ROOT
flowdef silicon-sky130 description "Odin III to GDS, SKY130 hd"
step silicon-sky130 elab  stage ir     tool odin3
step silicon-sky130 map   stage gates  tool odin3    needs elab out netlist=gates.v
step silicon-sky130 place stage placed tool openroad needs map in netlist=clean.v out def=place.def
provlink silicon-sky130 map   from elab kind cone confidence medium
provlink silicon-sky130 place from map  kind name key instance
provlink silicon-sky130 place from place kind physical_origin scope inserted
flow-var silicon-sky130 clock_port Clock
```

| Key | Form | Meaning |
|---|---|---|
| `schema` | `schema <1\|2>` | project schema version |
| `tool` | `tool <id> required\|optional <exe> [version-arg A] [module M] [env VAR]` | an external tool a flow may need (recorded; never run by the core) |
| `flowdef` | `flowdef <name> [description <text>]` | opens a flow; unique name |
| `step` | `step <flow> <id> stage <stage> tool <tool\|odin3> [needs a,b] [in role=path]... [out role=path]... [args text]` | an ordered step; `needs` must be earlier steps of the flow; `tool` must be `odin3` or declared; paths are recorded as written (relative to the project file), not checked |
| `provlink` | `provlink <flow> <step> from <prev> kind <k> [key F] [scope S] [confidence C]` | how the objects of `<step>` relate to those of `<prev>`; several per step; `from` the step itself only with kind `physical_origin` |
| `flow-var` | `flow-var <flow> <name> <value>` | a driver parameter |

`stage` is one of `hdl ir gates luts packed placed routed gds extracted recovered`; kinds and confidences
are listed in {doc}`provenance-files`. Errors are located like every project error (`file:line:col`).
The existing `flow <script>` statement is unchanged.

## Dump

For a schema 2 file `dump_project` adds `"schema": 2`, `"tools"` and `"flows"` (name, description, `vars`,
`steps` with `id stage tool needs in out args provlinks`). A schema 1 file's dump is unchanged.

## Compatibility

| File | Reader | Result |
|---|---|---|
| schema 1 (no `schema`) | 1.4 | loads, dump byte-identical |
| schema 2 | 1.4 | loads, dump has `schema`, `tools`, `flows` |
| schema 2 | 1.3 | `unknown_key` at line 1 |
| `schema 3` | 1.4 | `unsupported_schema` |
