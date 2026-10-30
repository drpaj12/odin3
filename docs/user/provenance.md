# Provenance

Every object Odin III makes keeps a **record** of where it came from. This page explains what a
record holds, how a name map exports the records, and how to read them back, ask questions of
them and extend them after VPR. The format is version 2 of the name map (`# odin3-names 2`); the
design is in `docs/V1.2.0.md` of the repository.

The examples on this page are not typed by hand. `docs/user/examples/provenance/make_examples.py`
runs the current build and writes every file the page includes; running it again gives
byte-identical files (see [Regenerating the examples](#regenerating-the-examples)). Where an
example is cut it says so with a line `... trimmed ...`.

## What provenance is

A provenance record is one of three kinds:

- **source** (`S`): the HDL text. It holds the file and the span (`line.column-line.column`) of the
  construct, and the AST construct it is.
- **imported** (`I`): a location read from a netlist (a BLIF, JSON or Verilog netlist) that has no
  AST behind it.
- **derived** (`D`): made by a pass from one or more parent records. It names the pass run, the
  **class** of the operation and the levels it goes between.

The records of a design form a directed acyclic graph: a derived record points at its parents, and
the sources are the leaves. The leaves reached from an object are where it came from; the steps on
the way are what was done to it. This is the **record chain**. The same graph is exported
(`R` lines) and read back (restore).

A second graph sits above the sources: the **construct table**. Each source record that came from
an AST node points at a construct (`C` line): a statement, a block or a module, with its parent
construct. Walking up gives the **construct chain** of an object: for example an `always` block
inside a module. Both graphs are provenance: the AST and the netlist.

### Levels

A level is the granularity of an object or a construct, ordered from coarse to fine:

| Level | Kind of thing |
|---|---|
| `module` | a module specialization |
| `block` | a process, continuous assign, generate construct or subprogram |
| `branch` | an `if` or `case` arm |
| `stmt` | a statement, declaration or switch |
| `expr` | an expression that made a cell |
| `word` | a word-level cell (also a wire and a port bit) |
| `gate` | a bit-level cell (`$_AND_`, ...) |
| `lut` | a `$lut` cell |
| `ble` | a basic logic element (written by `odin3-prov-post`) |
| `clb` | a cluster (written by `odin3-prov-post`) |
| `site` | a placed grid location and subtile (written by `odin3-prov-post`) |

There is no `design` level (nothing sits above a module) and no `bit` level (a bit is a position in
a word cell). Hard and black-box cells keep the level `word` and carry the cell type in the flag
`cell=<type>`. A net takes the level of its driver. An object that has no record has the level
`none`.

An instance is not a rung of this ladder. It is a separate axis: after `flatten` the record of a
flattened object holds the instance as a **context parent**, and the object's instance path is the
sequence of context parents. In a hierarchy that is not flattened there is no instance chain.

### Operation classes

Each pass declares the class of every derived record it makes. The class says how the record
relates to its parents and is the `dir` field of an `R` line (`<dir>:<from level>`):

| Class | Meaning | Passes |
|---|---|---|
| `origin` | a source or imported record; implicit | `read_verilog`, readers |
| `refine` | one parent, many pieces share the record | `proc`, `lower` |
| `rewrite` | 1:1, or a regrouping that blends nothing | `opt` rewrites, `hardmap`, `lpmmap`, `unmap`, `submod`, restored locations; `place` |
| `coarsen` | N to 1 | `opt` merges and FF merges, common-subexpression merges, `abc`, `raise`, `dfflibmap` (the shared inverter record); `pack` |
| `context` | flatten adds the instance | `flatten` |

### The construct table

The `C` lines list every construct some source record points at, with all its ancestors up to the
module. The kind is the AST kind (`MODULE`, `ALWAYS`, `IF`, `BLOCKING_ASSIGN`, `DECLARATOR`, ...),
or one of three synthesized kinds: `IF_THEN` and `IF_ELSE` (the arms of an `if`, which are not AST
nodes) and `CALL_SITE` (the call of an inlined function or task, so one body called twice has two
chains). Each construct has a level (above) from the AST kind:

| Level | AST kinds |
|---|---|
| `module` | `MODULE`, `UDP_DECL`, `CONFIG_DECL` |
| `block` | `ALWAYS`, `INITIAL`, `CONT_ASSIGN`, `NET_ASSIGN`, `GENERATE`, `GEN_FOR`, `GEN_IF`, `GEN_CASE`, `GEN_BLOCK`, `FUNCTION_DECL`, `TASK_DECL`, `SPECIFY_BLOCK` |
| `branch` | `CASE_ITEM`, `IF_THEN`, `IF_ELSE` |
| `stmt` | `IF`, `CASE`, the assignments, loops, timing statements, sequential and parallel blocks, declarations |
| `expr` | `BINARY`, `UNARY`, `TERNARY`, `SELECT`, `CONCAT`, `CALL`, `IDENT`, `NUMBER`, ... |
| `instance` (own axis) | `INSTANTIATION`, `INSTANCE`, `GATE_DECL` |

`UNIT`, `DIRECTIVE`, `LIST` and `ATTR` are transparent: their children attach to the nearest
ancestor that has a row.

How much of the table a map holds is the `ast=` word of its header, and is at most what the read
kept (`read_verilog --prov-ast`):

- `refs`: the nodes some record points at and their ancestors (the usual choice);
- `constructs`: the same, restricted to construct kinds (no `expr` rows; an expression resolves to
  its nearest construct ancestor);
- `full`: every node of the elaborated store;
- `none`: no `C` lines.

The command line keeps no AST by default (`--prov-ast off`); give `--prov-ast refs` to the read, or
use `odin3-flow --prov`, which does. The Python `Design` uses `refs`.

## Joins and their labels

Provenance can say that an object is the blend of several parts of the source. A **join** is a
`coarsen` record whose source leaves, taken as **anchors**, are at least two distinct statements (or
coarser constructs). The anchor of a leaf is the leaf's own construct when its level is `stmt` or
coarser, else its nearest `stmt`-or-coarser ancestor. Two anchors are distinct when neither equals
nor contains the other, so the `$mul` and `$add` cells of `x = a*b + c` have one anchor and are not
a join. A blend a record only inherits from one parent is not a new join. Records of class
`refine`, `rewrite` and `context` are never joins, and a leaf with no construct (a restored
location, an imported one) never makes one.

| Label (in `flags`) | Meaning |
|---|---|
| `join` | a join of distinct constructs; the `cons` field lists the reduced set `J` of anchors |
| `join=instance` | a join of instances: the same anchor reached through different context chains (two instances of one module merged) |
| `lossy=cap` | `J` has more than 16 members; `cons` lists the first 16 in key order, `n=` is the total |
| `lossy=bulk` | the pass declared the operation bulk (an `abc` over-budget record, or objects no anchor reaches); `n=` is the total |
| `n=<total>` | accompanies `lossy=`: the size of `J` |
| `chain=span` | some leaves reached have no construct (span-only leaves); the location is known, the construct is not |
| `capped` | the record holds more than 8 locations; the first 8 are written (`ODIN3_HDL_LOC_MAX`) |
| `call=c<n>` | the record is a call-site record (an inlined function or task body) |
| `cell=<type>` | the cell type of a hard or black-box object |
| `ctx=<n>` | the number of context parents at the end of the parent list (flatten); absent when 0 |
| `bulk` | the operation class was declared bulk |

The members of `J` are sorted by a stable key (file path, start line, start column, end line, end
column, kind, name, part) before the cap, so the first write and a write after a restore list the
same members. The records `odin3-prov-post` writes for packing use the same rule: a `clb` whose BLEs
have distinct anchors is a join, labelled by the `pack` step.

## The version 2 map, line by line

The map is one TAB-separated UTF-8 file with LF line ends, named `<netlist>.names` (or the file
given with `--name-map`). It is written only on request, with `--prov-map` (below). Escapes are
`%09 %0A %5C %25` and, in a version 2 line, also `%2C` and `%7C`, because list fields are
separated by `,` and `|`; `%23` escapes a leading `#` of the first field. A version 1 reader refuses
a version 2 file at the header; a version 2 reader ignores a line type it does not know, with a
located WARNING.

The header is `# odin3-names 2 hash=sha256 width=10 prov=1 ast=refs`.

| Type | Fields (after the type letter) | Meaning |
|---|---|---|
| `O` | `module kind short full hier file line recs level` | an object renamed by `hash`: the seven fields of version 1, then `recs` (the record `r<n>` that names it) and `level` |
| `N` | `module kind name recs level` | an object that keeps its name (a port bit, a generator) and has a record |
| `R` | `id kind level dir step parents origins cons flags` | one provenance record |
| `C` | `id kind level parent file span name` | one construct of the AST layer |
| `F` | `id path` | a file, once; `R` and `C` name it as `f<n>` |
| `T` | `id pass args info` | a pass run; `R.step` names it as `t<n>.<op>` |
| `A` | `atom recs stale` | after Odin III: an atom (a BLIF net name as VPR sees it) with its BLE, CLB and site records |
| `Q` | `id why objective gain pcrit rho slack near npaths members basis stale` | the why-classes, objective and criticality of a join (below) |
| `K` | `id member pcrit rho slack basis stale` | one member of a join with its own criticality |

The file has no other line type. The **foreign flags** that `odin3-prov-post` writes in the flags
of an `R` line (`x=`, `y=`, `sub=`, `layer=`) are not a line type and are not `X` lines: a restore
keeps them in a separate list of the same table, beside their record, and writes them again with it.

`kind` of an `O` or `N` line is `net`, `node` or `wire`. Lines come in the order header, `O`, `N`,
`R`, `C`, `F`, `T`; `odin3-prov-post` appends `T`, `R` and `A`, `Q`, `K` lines at the end.

### The `R` line

- `id`: `r<n>`, numbered from 1.
- `kind`: `S`, `I` or `D`.
- `level`: for `D`, the `to` level of its class; for `S`, the level of its construct.
- `dir`: `origin` for `S` and `I`; `<dir>:<from level>` for `D` (for example `coarsen:gate`).
- `step`: `t<n>.<op>`, the pass run (a `T` line) and the operation within it. Operations are
  numbered from 1 per `T` line over the ones referenced.
- `parents`: a comma list of `r<n>`. Always numbered before the child.
- `origins`: for `S` and `I`, the locations `f<n>:L.C-EL.EC`, separated by `|`, at most 8. A
  location with no file is `-:L.C-EL.EC`.
- `cons`: for `S`, the construct `c<n>` (empty for a record with no AST); for a join or a lossy
  record, the members of `J`, at most 16.
- `flags`: a comma list of the labels of the table above and of these:

| Flag | Written by | Meaning |
|---|---|---|
| `hier=<text>` | Odin III | the record's hierarchy string (escaped); absent when empty |
| `stale=t<n>` | restore | on an `R` record only: the post-Odin record was made stale by the pass run `t<n>` (see [Restore](#restore-from-a-map)); the last flag. An `A`, `Q` or `K` line has no such flag: it carries `t<n>` in its `stale` field |
| `x=<n>`, `y=<n>`, `sub=<n>` | `odin3-prov-post` | the grid column, row and subtile of a `site` record, from the `.place` |
| `layer=<n>` | `odin3-prov-post` | the layer, when the `.place` has the column |

The full flag list, then, is `join`, `join=instance`, `lossy=bulk`, `lossy=cap`, `n=<total>`,
`chain=span`, `capped`, `hier=<text>`, `call=c<n>`, `cell=<type>`, `ctx=<n>`, `bulk`, `stale=t<n>`,
`x=<n>`, `y=<n>`, `sub=<n>` and `layer=<n>`. (`stale=t<n>` is a flag of `R` records only; an `A`, `Q` or `K` line has the field `stale`, `-` when current, else `t<n>`.) The writers do not yet write `cell=<type>` (a record
does not know the cell type of the hard object it made).

### Examples of each line type

These come from the worked example below (`bm_DL_BCD_adder`, a 4-bit BCD adder, taken through
`odin3-flow --prov` and `odin3-prov-post`). The header, then the first `O` and `N` lines. The `level`
of the last field is the level of the naming record: the net `Cout` is a `gate`, the renamed nets
are `word`, `expr` or `stmt`:

```{literalinclude} examples/provenance/out/map_header_o_n.txt
:language: text
```

The `R` lines Odin III writes. `r9` is a source record: the expression `X + Y + Cin` (line 15, columns
7 to 18) of the construct `c11`; `r10` is derived from it by `hardmap` (`rewrite:word`, operation 2 of the
run `t4`) and names the adder net that the `O` line of the lookup below points at. `r12` is the `if`
statement (`c5`, level `stmt`); `r13` is made from it by `proc` (`refine:stmt`, to level `word`), and `r14`
by `lower` (`refine:word`, to level `gate`):

```{literalinclude} examples/provenance/out/map_r_odin.txt
:language: text
```

The `R` lines `odin3-prov-post` added. `r19` to `r21` are BLEs (`coarsen:lut`, step `t7.1`, the run
`pack`) made from the adder record `r7`; `r22` is the cluster of the three (`coarsen:ble`, `t7.2`); `r23`
is its place (`rewrite:clb`, step `t8.1`, the run `place`) with the `.place` coordinates. `r30` is a
cluster whose six BLEs come from the records of the adder (`r10`) and of the `if` (`r14`, `r16`): its
anchors are the `if` statement `c5` and the assignment `c10`, so it is a `join` and `cons` lists the two
constructs. Records `r24` and `r31` are the BLE and the site around it:

```{literalinclude} examples/provenance/out/map_r_post.txt
:language: text
```

The `C` lines, all of them: the module `c1`, the `always` block `c2`, the `@(...)` timing statement `c3`
and the `begin ... end` block `c4`, the `if` statement `c5` and its `else` arm `c6`, the assignments, the
expressions and the port declarations. `parent` points up to the module. `IF_ELSE` is a synthesized kind:

```{literalinclude} examples/provenance/out/map_c.txt
:language: text
```

The `F` and `T` lines. `t1` is the read, `t5` `proc` and `t6` `lower`; `t7` and `t8` are the `pack` and
`place` runs that `odin3-prov-post` added, with the `.net` and `.place` they read in `info`:

```{literalinclude} examples/provenance/out/map_f_t.txt
:language: text
```

The `A` lines: the atom as VPR sees it, then its BLE, CLB and site records, then `stale` (`-`, current).
An atom with no object in the map (`gnd`, `vcc`, an output pad `out:Cout`) still has its `A` line:

```{literalinclude} examples/provenance/out/map_a.txt
:language: text
```

The `Q` and `K` lines of the join `r30`. This is stock VPR (no census): `why`, `gain` and `pcrit` are empty
and `basis` says `-` for them. `objective` is the packer, found from the VPR log (`i`). `rho`, `slack`, `near`
and `npaths` come from `timing/attribution.json` (`r`). `rho` is 1.000 because a member sits on the critical
path; the slack, -4.619 ns, is the negative of this run's critical path delay (`cpd_ns` 4.61929 in
`timing/attribution.json`, whose `required_ns` is 0.0):

```{literalinclude} examples/provenance/out/map_q_k.txt
:language: text
```

### The numbering

The numbers `r`, `c`, `f` and `t` depend on the contents of the file only, so equal designs give
equal bytes. Objects sort by `(module, kind, name)`, `O` lines before `N` lines; the records are
numbered by a post-order walk from the objects' records, parents first, then the records the
`A`, `Q`, `K` lines name. Constructs are numbered when first reached, after their parent chain;
files and pass runs in order of first reference. Only the referenced pass runs are written.

### The `C`, `F`, `T` lines

`C`: `id` is `c<n>`; `kind` the AST kind; `level` as above; `parent` the parent construct (empty for a
module); `file` `f<n>`; `span` `L.C-EL.EC`; `name` the construct's name where it has one (a module,
an instance, a block label, a declarator). `F`: a file path, once. `T`: a pass run, with its
arguments and the information the pass recorded. `odin3-prov-post` adds the runs `pack` (operation 1
the BLE, operation 2 the CLB) and `place` (operation 1 the site).

### Writing a map

`--prov-map [refs|constructs|full]` on `write_blif --vtr`, `write_json` and `write_verilog` writes the
map in version 2 with that AST layer. It needs `--name-style hash` and is an error without it. A
mode above what the read kept is an error that names `read_verilog --prov-ast`. A bare `--prov-map`
is `refs` clipped to what was kept. It is given once; a word right after it that is `refs`,
`constructs` or `full` is the mode, so write a file of that name as `./refs`. A derived record
whose operation has no class stops the export (the error names the record, run and operation); a
failed export writes neither the map nor the netlist. The netlist is byte-identical with and without
the option. `odin3-flow --prov` does all of this for the VTR flow.

A BLIF that already carries provenance names (`--name-style provenance`) joins to the map of a hash
run of the same script by the `full` field of the `O` line, which is the same name.

### An Odin III join

`bm_DL_4_bit_comparator` has three `if` statements. After `abc --lut 6` two of the LUTs blend them. Each
is a `coarsen:gate` record at level `lut` with `join`, and `cons` names the statements it blends (`c5`,
`c7`, `c9`, below). Odin III writes the `Q` line for each, with the objective only: `depth`, derived from the
script (`i`). The `T` row of the run carries the script's hash:

```{literalinclude} examples/provenance/out/abc_joins.txt
:language: text
```

```{literalinclude} examples/provenance/out/abc_cons.txt
:language: text
```

## Why a join was made: `Q` and `K` lines

A join says that parts of the source were put together. A `Q` line says why and how critical it
was, and a `K` line gives the same for each member. Both are data, not prose. Odin III writes a `Q`
line for every join it makes; `odin3-prov-post` writes `Q` lines for every logic CLB and for every
join BLE, and `K` lines for the members. I/O, memory and multiplier clusters get none.

The fields of `Q`:

| Field | Meaning |
|---|---|
| `id` | the join record `r<n>` |
| `why` | `class:share` list, descending, shares to three decimals summing to 1.000 (largest-remainder rounding); empty if not computed |
| `objective` | the stated purpose of the step: the pass name (`opt`), a depth-or-area word for `abc`, the packer for `pack` |
| `gain` | `source:share` list: which term of the packer pulled the molecules in (seed, connectivity and timing, affinity group, transitive, high fanout, attraction group, RAM, unrelated, and `unknown` for a molecule with a source index outside this list); only with a census |
| `pcrit` | pack-time criticality, the maximum over the members, 0 to 1; only with a census |
| `rho` | post-route criticality: the maximum over the members of the largest arrival-time ratio of a path through them to path 1 (0 off every reported path) |
| `slack` | the smallest post-route slack in ns over the members |
| `near` | `5:<0\|1>,10:<0\|1>`: near-critical at the margin in percent; a path is near-critical at margin m when arrival >= (1 - m/100) of path 1 |
| `npaths` | `5:<n>,10:<n>`: the near-critical paths through the join; `>=<n>` when the report was capped |
| `members` | the member count |
| `basis` | `field=r\|i` over the eight fields above, `-` for an absent one |
| `stale` | `-` or `t<n>`: current, or made stale by that pass run |

The fields of `K`: `id member pcrit rho slack basis stale`. A logic CLB lists every member (its LUTs,
FFs and adders); another join lists at most 16, highest `rho` first (ties by name).

### Pair classes

The members of a CLB are classed in pairs (m(m-1)/2 of them), each by the first rule that applies. A
net is **local** when its fanout is at most 32, a **broadcast** net above that. The pack-time
criticality of an atom is the maximum of VPR's `setup_pin_criticality` over its non-clock inputs; the
threshold is 0.5.

| Class | Name | Rule |
|---|---|---|
| `SC` | series-critical | a local net is driven by one of the two atoms and the other is among its sinks, and that sink pin's pack-time criticality is at least 0.5 |
| `SN` | series-noncritical | the same direct local net, criticality below 0.5 |
| `SI` | shared-input | not SC or SN; both are sinks of one common local net |
| `OR` | orthogonal | not SC, SN or SI; the two atoms have the same word label, or are sinks of one common broadcast net (enable, reset, select) |
| `FL` | filler | none of the above |

A join can be a hybrid: `why` holds every class with a non-zero count, so a CLB is a mixture and not
one label. A class of a future packer is written `X-<name>`. A CLB of one member has no pair and an
empty `why`. The word label of `OR` is the word of `--words FILE` (a Norns `words/members.json`)
when given; otherwise it is the nearest `DECLARATOR` construct on the chain of the atom's first
leaf, and then the `why` letter of `basis` is `i`.

### Example

No Norns packer is installed with Odin III, so this example runs `odin3-prov-post --census` on the
hand-made flow directory of the tests (`tests/tools/fixtures/prov_why`: seven logic atoms in one
CLB with a pair of every class, and a census file). It is a real run of the current tool on made-up
input, not a VPR run. The first `Q` line is the CLB: 21 pairs, of which 16 are filler (0.762),
2 series-critical (0.095), 1 series-noncritical, 1 shared-input and 1 orthogonal. `gain` says which
packer terms pulled the molecules in, `pcrit` is 0.900, and `basis` says `why=i` because no
`--words` file was given (the word label came from the construct chain) and `gain`, `pcrit` are `r`.
The second `Q` line is a CLB of one LUT: no pair, so `why` is empty.

```{literalinclude} examples/provenance/out/why_q_k.txt
:language: text
```

### Recorded or inferred

`basis` marks each value `r` (every input was read as written by a tool: a pass-run field, a VPR
report, a census) or `i` (at least one input was inferred from netlist structure or a proxy). Where
each value comes from:

| Value | Stock VPR flow | Norns packer with `--census` | Odin III passes |
|---|---|---|---|
| pair classes `why` | absent (`-`): stock VPR does not write the pack-time criticality of a sink pin | `r` (`i` without `--words`) | not computed; empty |
| `gain` | absent | `r` | absent |
| `objective` | `i` for `pack` (the packer named in the VPR log) | `i` (`norns-pack`, implied by the census); `r` only with `--pack-objective` | `r` for a pass kind; `i` for an `abc` objective derived from the script |
| `pcrit` | absent | `r` | absent |
| `rho`, `slack` | `r` from `timing/attribution.json` | same | attached to `lut` records through the `A` lines; `r` |
| `near`, `npaths` | `r` | `r` | as `rho`; `r` |

The `abc` objective: a comment `# objective: <word>` in the script states the goal (`r`); otherwise
the last of `if`, `&if`, `map`, `&nf` decides: `area` when the letter `a` occurs an odd number of
times among its options, `depth` when even (`i`); a script with none of them gets an empty
objective, never a guess.

If the census names a `.net` whose SHA-256 differs from the `.net` read (the packing differs), the
census values are written as absent, with a WARNING, never as `r` or `i`. The SHA-256 is taken from
`--census-net-sha256`, else the `net_sha256` of the `metrics.json` next to the census, else a
`# net_sha256=` line of the census; a census with none is unchecked and treated the same way.

## `odin3-prov-post`

`odin3-prov-post FLOW_DIR` extends the version 2 map of an `odin3-flow --prov --vpr
--keep-results` run (the options are in {doc}`odin3-flow`). It parses VPR's `.net` and `.place` and
appends:

- `R` lines of level `ble` and `clb` (class `coarsen:lut` and `coarsen:ble`, step `pack`);
- `R` lines of level `site` (class `rewrite:clb`, step `place`, flags `x=`, `y=`, `sub=`, `layer=`);
- one `A` line per atom: its BLE, CLB and site records;
- the `Q` and `K` lines of the logic CLBs and of join BLEs and Odin III joins.

Every top-level block of the `.net` that has an atom or pad in the map is a cluster and gets a `clb`
record (an I/O cluster, a memory or a multiplier too, but those get no `Q` line); a cluster none of
whose atoms or pads is in the map gets no `clb` record. A BLE is a level-1 block that holds a primitive other
than a pad. A cluster of output pads alone has no parent record and gets no CLB, no site and no `A`
line. A `clb` whose BLEs have distinct anchors is a `join` labelled by `pack`. The map is rewritten
atomically; every existing line stays byte-identical. The tool refuses a map it has already
extended, a BLIF written with short names, and a version 1 map. `names --check` accepts the
extended map. The `.net` is trusted local VPR output.

## Restore from a map

`read_blif`, `read_json` and `read_verilog -netlist` rebuild the provenance of a design from a
version 2 map when given it with `--name-map` (`-name-map` for `read_verilog`). The design then
answers `Node.provenance()`, `Net.provenance()` and `chain()` as the design that wrote the map did.

**What is restored.** The pass runs (`T`), records with their classes (`R`), constructs (`C`), the
files (`F`), the objects bound to their records (`O`, `N`) and the post-Odin lines (`A`, `Q`, `K`),
which are held as text with their record ids rewritten. The records that only these lines reach
(the `ble`, `clb` and `site` records) are restored as well and written again. A restore creates no
restored-location records. An object the map does not list has record 0.

**The `restored` mark.** Each restored pass run is marked `restored`, so code that expects an `ast`
to be a node of the AST store (the elaborator, `dump_locs`) never reads a restored run's rows. A
restored source record points at a construct row by number, not by node.

**All or nothing.** The whole map is checked first (the checks of `names --check`, then the classes);
a map that fails restores nothing and the read fails with a located error, leaving the design as the
reader made it. A line with more fields than its type has, a line over 1 MiB, a `D` record with no
parent or no class, and a flag with a bad value are refused.

**Stale.** The `A`, `Q`, `K` lines and the post-Odin records describe the netlist as it was. After a
restore, the first pass run that is not a reader, a writer or one that only looks (`check`,
`stats`, `stat`, `hierarchy`, `names`, `dump_locs`, `dump_project`, `sim`, `preprocess`) is the
**mutating run** `t`. When the map is written again, every restored `A`, `Q`, `K` line carries `t<n>` in its `stale` field
(`-` when current) and every post-Odin `R` record carries the flag `stale=t<n>`. History is kept: the lines are
still written, never presented as current; treat a stale line as not current. A join made after the
restore gets a fresh `Q` line with stale `-` and only its objective, and the stale lines stay. A line
already stale keeps its first mutating run. Writing the map again after an unchanged read-back keeps
every line.

**Round trip.** The claims are scoped to what was proven; the counts below are those of `tests/tools/data/restore/roundtrip.txt`
(the cells match it for all four `ast=` modes). For each pair of writer and reader the
claim holds only if the reader reproduces its own writer's netlist first (Task 9, step 0).
`write_blif --vtr` with `read_blif` passes on 68 of 68 micros, `write_json` with `read_json` on 68 of 68,
and `write_verilog` with `read_verilog -netlist; proc` on 68 of 68. The results of the second write
(the table is `tests/tools/data/restore/roundtrip.txt`; the modes are the `ast=` word of the header):

| Pair | Micros proven by step 0 | Netlist identical on 2nd write | Map identical on 2nd write | Write 2 = write 3 | Known gap |
|---|---|---|---|---|---|
| `write_blif --vtr` and `read_blif` | 68 | 68 | 18 | 68 | a port vector keeps the record of its first bit, and the map lists objects the netlist does not mention, so write 2 has fewer `N` and `R` lines |
| `write_json` and `read_json` | 4 | 4 | 4 | 4 | none for these 4 |
| `write_verilog` and `read_verilog -netlist` | 23 | 0 | 0 | 0 | the Verilog reader binds no records to cells or modules, so write 2 loses the `// file:line` comments of the module and the `assign`, and its map holds new `I` records |

The rows are the same for the modes `refs`, `full`, `constructs` and `ast=none`.

Write 2 is the canonical form: for the proven pairs of the table, every later generation is byte-identical to it, and the chain, the
join flags and the `provenance()` answers of every object are unchanged by it. `constructs` mode
collapses equal-span expression rows into one construct, so for it only write 2 = write 3 holds.
What a restore does not bring back: records no listed object or extra line reaches, the AST node
IDs, the IR run numbers, and the records of objects the file does not mention (a net with a record
that no cell uses is listed by the writer and cannot be bound). `read_blif` binds nets only (a BLIF
cell has no record); `read_json` binds netnames and cells; `read_verilog -netlist` binds
module-level wires and nets. The cost is the size of the map: in the Task 9 log of `docs/V1.2.0.md` (release build,
medians of 3), a read with a map took 5.8 to 7.0 times as long as one without, and for `LU64PEEng`
(a map of 244 MB) added about 1.1 GB.

### Example

The map of the worked example, read back with `read_blif --name-map` and written again with no pass
between (`check`, a pass that only looks, runs between the read and the write), then read back and
written after `opt -full` (a pass that changes the netlist). In the table, `first` is the map the
flow wrote (and `odin3-prov-post` extended) and `second` is the map the write after the read-back
wrote. The `O` lines drop from 67 to 38, the `N` lines from 13 to 10 and the `R` lines from 62 to 38.
This is the known gap of the table above (the BLIF row): the map lists objects the netlist does
not mention (a BLIF cell is not a bound object), so what the reader cannot bind is not written
again. Task 9 gives no finer cause for these counts. The `A`, `Q` and `K` lines are all kept:

```{literalinclude} examples/provenance/out/restore_counts.txt
:language: text
```

After `opt -full` the pass run `opt` is recorded as `t8`, the mutating run, and the restored post-Odin
lines are marked stale with it. The `A` lines end in `t8`, the post-Odin `R` records have `stale=t8` as
their last flag, and the `Q` and `K` lines end in `t8`:

```{literalinclude} examples/provenance/out/restore_stale_t.txt
:language: text
```

```{literalinclude} examples/provenance/out/restore_stale_a.txt
:language: text
```

```{literalinclude} examples/provenance/out/restore_stale_r.txt
:language: text
```

```{literalinclude} examples/provenance/out/restore_stale_qk.txt
:language: text
```

## Looking things up: `names`

```text
names --map FILE --lookup SHORT   # the O line, its records (depth-first), its chain, the F and T lines
names --map FILE --chain SHORT    # the C lines of the chain and the F lines they name
names --map FILE --check          # the version 2 invariants
names --map FILE --reverse FULL   # full name to hash name
```

`SHORT` is the hash name of an `O` line, or the name of an `N` line. `--lookup` prints the object's
line, then its records, indented by their depth in the parent walk (the naming record first), then
the `C` lines of its chain (each source leaf's construct, then its ancestors to the module, each
once), then the `F` and `T` lines they name. The output is the map's own text, so the same parse
reads it. `--chain` on a version 1 map is an error; the other options work on both. An object with
no construct (`ast=none`, or an imported record) prints no chain and succeeds; a name that matches
nothing fails.

`--check` first prints the version 1 line, `entries=N bad_hash=N duplicates=N ignored=N` (the `O`
and `N` lines, bad hash names, duplicates, and lines the parse dropped), then
`records=N constructs=N files=N runs=N objects=N bad_id=N bad_ref=N bad_order=N bad_class=N no_record=N`,
and fails on any problem: an id that is not its line number, a reference (`r`, `c`, `f`, `t`) that
does not resolve, a parent numbered after its child (this is the "no cycle" check), a record with
no valid class, an object whose record is missing, a duplicate, a bad hash, or a line the parse
ignored. A lookup costs about two thirds of a check, because the cost is reading the file (Task 6 log of
`docs/V1.2.0.md`: 0.99 s against 1.51 s on `LU64PEEng`); a program
that asks many questions reads the map once.

## The Python readers

`odin3.read_name_map(path)` (`plugins/python/odin3.py`) reads a map without a design and restores
nothing. It returns a `NameMap` with `objects` (`MapObject`), `records` (`MapRecord`),
`constructs` (`MapConstruct`), `files`, `runs`, `atoms`, `whys` (`MapWhy`, the `Q` lines) and
`members` (`MapMember`, the `K` lines), and `ignored`, the number of lines the parse dropped.
`NameMap.find(name)` returns the `O` and `N` lines of a name, `provenance(obj)` the records (naming
record first, parents depth-first, each once), `chain(obj)` the constructs, `why_of(record)` and
`members_of(record)` the current `Q` and `K` lines of a record (stale `-` only; a stale line is
history and stays in `whys` and `members`). Hash names are not verified (that is
`names --check`). A bad reference in a line is a warning and the line is ignored.

`tools/flow/name_map_v2.py` is the copy of this parse for the flow tools, which cannot import
`odin3.py` (it needs `cffi`). A test loads both and compares them on every small micro map. It adds
`NameMap.atom(name, module)`, `leaf(obj)`, `location(record)` and the `SourceSpan` type, which the
timing attribution uses. In a design, `Node.provenance()`, `Net.provenance()` and `chain()` give the
same answers from the C readers ({doc}`python-api`).

## A worked example

The design is the micro `bm_DL_BCD_adder` (a 4-bit BCD adder, from `tests/micro`). Its source:

```{literalinclude} examples/provenance/src/bm_DL_BCD_adder.v
:language: verilog
:linenos:
```

**1. Run the flow with provenance.** `--timing-attr` implies `--prov` (`read_verilog --prov-ast refs`,
`write_blif --name-style hash --prov-map`); `--vpr --keep-results` runs VPR and keeps its `.net` and
`.place`:

```text
odin3-flow bm_DL_BCD_adder.v --timing-attr --vpr --keep-results --out flow
```

This writes `flow/odin3/bm_DL_BCD_adder.blif` (hash names for VPR) and the version 2 map
`flow/odin3/bm_DL_BCD_adder.blif.names`, whose header, `O` and `N` lines are shown above. At this point the map
holds the `O`, `N`, `R`, `C`, `F` and `T` lines and no others (the lines `A`, `Q`, `K` and the
`ble`, `clb` and `site` records come from the next step).

**2. Extend it with what VPR did.**

```text
odin3-prov-post flow
```

```{literalinclude} examples/provenance/out/prov_post_summary.txt
:language: text
```

The summary line says 36 atoms, of which 8 are not found in the map by name, and 12 bles, 16 clbs and 16
sites. The map gains `ble`, `clb` and `site` records (12, 16 and 16), `A` lines for the atoms and `Q` and `K` lines for the three logic
CLBs. One of them is a join by `pack`: the record `r30` above.

**3. Ask a question of one net.** The net `h5j5t26zpp3` is a net `hardmap` made for the adder of line 15. `names --lookup` prints
its `O` line, then its records (indented by depth: the naming record `r10`, then its parent `r9`), then
its construct chain, bottom to top, from the expression `X + Y + Cin` to the `module`, and the `F` and `T`
lines they name:

```text
odin3 -p "names --map flow/odin3/bm_DL_BCD_adder.blif.names --lookup h5j5t26zpp3"
```

```{literalinclude} examples/provenance/out/names_lookup.txt
:language: text
```

`--chain` prints the chain alone. For the output port bit `Cout`, which keeps its name, the chain starts at
the `if` statement (`c5`) and goes up through the sequential block, the timing statement and the `always`
block to the module:

```text
odin3 -p "names --map flow/odin3/bm_DL_BCD_adder.blif.names --chain Cout"
```

```{literalinclude} examples/provenance/out/names_chain.txt
:language: text
```

`--check` verifies the map. It counts the records, constructs, files and runs and each kind of problem
(all zero here):

```text
odin3 -p "names --map flow/odin3/bm_DL_BCD_adder.blif.names --check"
```

```{literalinclude} examples/provenance/out/names_check.txt
:language: text
```

**4. Read it from Python**, with the same answers (`tools/flow/name_map_v2.py`; `odin3.read_name_map` has the
same interface):

```{literalinclude} examples/provenance/out/python_reader.txt
:language: text
```

**5. Read it back.** `read_blif --name-map` with the extended map, then a pass, then a write: see
[Restore from a map](#restore-from-a-map) above for the output.

The text in the files above is cut with `... trimmed ...`; the files in the build are whole.

## Regenerating the examples

Every file under `docs/user/examples/provenance/out/` is written by

```text
python3 docs/user/examples/provenance/make_examples.py [--odin3 PATH] [--work DIR]
```

from a release build and `VTR_ROOT` with VPR. The script runs the tools above on the designs in
`docs/user/examples/provenance/src/` and on the test fixture, and writes only slices of what they
made. Since the fix after 1.2.0, `odin3-flow` runs `odin3` in the common
directory of the design files and passes them relative to it, so names use the path relative to
the design's common directory and do not depend on where the checkout lives (the run log has a
`name_root=` line). The committed examples were written before that fix and are regenerated with
the next release; until then the work
directory has a fixed default (`/tmp/claude-1000/pdoc-example`) and the committed files are the output for that
default, with the work directory written as `<work>` in every file; a run with another `--work` gives
other hash names. Running the script twice gives
byte-identical files. The work directory is never emptied.

## Limits: what the chain cannot give

- A record with no AST (a restored location, an imported one) has no construct; its `cons` is empty
  and `flags` has `chain=span`. A design read from a BLIF has `I` records and no chain at all; the map
  says so and does not guess.
- An unrolled loop's iterations share their body's records, so the construct of an iteration is the
  loop body; the loop index is in `hier` only when the scope is named.
- A design read back from a version 1 map has no chain. A version 2 map restores its chain.
- Odin III's own joins carry an objective and no pair class (a pair class needs a packed CLB); the
  `why` field of their `Q` line is empty.
- Stock VPR writes no pack-time criticality, so `why`, `gain` and `pcrit` are absent without a Norns
  census.
- `--prov-map` needs `--name-style hash`; plain, short and provenance names carry no map.
- `lossy=bulk` is not written by `odin3-prov-post` (the class flag is not in the map), so a BLE under a
  bulk `abc` record shows `lossy=cap,n=<|J|>` only.
- The Verilog reader binds no record to a cell or module, so a netlist read back with
  `read_verilog -netlist` keeps the reader's imported record for instances and cells.
