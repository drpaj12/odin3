# 2D — Elaboration: design

Status: approved (agent default under Peter's overnight rule, 2026-10-11); v2 applied the Fable
review; v3 the re-review minors. (v2: `2D-review1`, 2 critical, 10 important, 17 minor, all
applied; F17 made moot by F1's redesign, F20 applied as divergence §17 #20. v3: `2D-review2`
approved v2 with minors N1–N7; N1–N3 and N5–N7 applied; N4 is not applied as asked — the review
says no `hierarchy` pass exists, but it is a 1D built-in (`src/passes/builtin.c`, the
`docs/PASSES.md` row), so it stays in EL-33's `OK` list, now with that citation. v4: two
amendments from the plan review (`2D-plan-review`, controller ruling C1 and minor M8), both
following Yosys 0.55: EL-39's write temporaries start each block as `addr = x`, `data = x`,
`en = 0` (was `en = 0` only: a path that never writes left `addr`/`data` unassigned, a
combinational loop after `proc`); EL-30 lowers `~&a`/`~|a` to `$logic_not` of the reduction, as
Yosys's parser does (was `$not`). v4.1: EL-14's `==`/`!=` and `**` text corrected to Yosys
0.55's `const_eq`/`const_pow`, as found by 2D Task 7's constant table and probes: `==` is 0 on any
0/1 mismatch even beside an x/z bit (was "x when any compared bit is x or z"), and `0 ** positive`
is 0 even when x/z bits read as 0 make the base or exponent so.) Decisions 1–13
ruled by the controller as agent defaults (§20). Phase 2, sub-project 2D (PHASE2 #1; review: Peter,
semantics). Spec: `docs/DESIGN.md` §4.0 (libraries, top), §4.1, §4.5, §5, §6 step 2, §12, §15.
Inputs: PHASE2 #3, #4 (exit bar), #5, #6 (coverage, D1–D5), #7/#8 (the AST and its carry-overs),
Q3 of #8 (two stages, the elaborated AST as the seam), #10–#12; the construct table in
`docs/specs/2026-10-09-2C-verilog-coverage.md` (rows cited as `L1`, `M1`, …); the AST in
`docs/specs/2026-10-09-2A-ast-design.md` (§4.3 slot table, §4.4 flags, §4.6 forms, §7
attributes, §8 symbol table) and `src/ast/*.h`; what 2C hands over
(`docs/specs/2026-10-10-2C-verilog-parser-design.md` §1, §3.6, §4.2, §7.3); `docs/IR.md`; the
op registry (`src/ir/cells/`); 1D (pass manager), 1E (simlib semantics). Oracles: Yosys 0.55 as
pinned by VTR (`$VTR_ROOT/libs/EXTERNAL/yosys`, the Yosys inside Parmys: `frontends/ast/
simplify.cc`, `genrtlil.cc`, `ast.cc`, `passes/hierarchy/hierarchy.cc`) and the Parmys script
(`vtr_flow/misc/yosys/synthesis.tcl`: `read_verilog -sv -nolatches`, `hierarchy -check
-auto-top -purge_lib`); Odin II (`ODIN_II/SRC`) is a read-only reference. **The semantic oracle
is Parmys's Yosys 0.55** (`$VTR_ROOT/build/bin/yosys`): every "verified" claim, the constant
table, the Yosys bridge and the exit test use it; the standalone Yosys 0.69 (ORACLES.md) is used
only where this file says so explicitly (§14.4's rerun of the fixtures' 0.69 oracles and §14.5's drift check), never mixed with 0.55 in one
comparison (ORACLES.md). IEEE 1364-2005 is not
in the workspace: clause numbers are from memory; where Yosys and the standard differ this file
says which it follows and why, and §17 lists every divergence. Each decision is numbered
**EL-n**. Where this file and the 2A slot table or `docs/IR.md` disagree, those win and this
file is corrected — except the three changes marked **IR change** (§7.2, §8.1, §6.2), ruled as
agent defaults (§20) and amending `docs/IR.md` when 2D-1 lands; Peter may still override them.

v4.2 (2026-10-11; the joint 2E/2F review `2EF-review1`, controller rulings T6 and T2). First (T6):
every action the process generator emits is **one action per chunk** of its left side, as Yosys's
`addChunkActions` emits them: the root defaults and sync actions (EL-35 steps 3–4), user assignments
(step 5) and the case temporaries' entries, defaults and exits (EL-36); temporaries are grouped by
the same chunks. Per-bit actions gave per-bit `$dff`s and `$mux`es after `proc` (an 8-bit `case`
became eight 1-bit `$pmux`es), and `fsm_detect` ignores a register whose `Q` is not a whole wire, so
identity with Parmys needs the chunks. Cost: one O(outputs) grouping per block. Second (T2; agent
default for Peter's review): the eight opt passes of 2F are `processes = OK` (EL-33), so Odin III
runs Parmys's `opt_expr; opt_clean; opt -nodffe -nosdff` before `proc` (2F spec OP-3, "mode P");
`$proc` stays opaque to them and `opt_muxtree`/`opt_clean` skip modules that still have processes.
Reason (Fable-verified): the opt family is not confluent, and the pre-`proc` opt order changes
netlists (`stereovision3` 812 versus 792 cells), so "`opt` only after `proc`" (v1–v4.1) could not
reproduce Parmys.

v4.3 (2026-10-11; the 2E/2F plan re-review): EL-35 step 7 fixes the order of the constant `I`
pins. They take their net-ID position among the other `I` pins, so a body that 2E decodes and
re-encodes keeps its pins (2E §17).

v4.4 (2026-10-11; the PR 2D-1 final review `final-review-2D1`, I1 and M1). First, §13 gains an
implementation note that describes the exact `write_rtlil` text 2D-1 ships
(`src/backends/rtlil/writer.h`). It lists the eight places where the text differs from §13's
bullets, and the note wins. 2E-1 and 2F-1 read it. The note also covers the `attribute \top 1`
line, which the writer now emits for the design's top. Second, EL-47 now says the ABI stays at
the unreleased v4 (ruling I1, plan point (c)). It no longer says "one bump", and it says that
IR change C has no ABI mirror (plan point (d)).

v4.5 (2026-10-12; the 2D Task 10 review, errata): definitions are located at their keyword, not
their name, which the parsed store does not locate (EL-6's taken names, §9.1's Hierarchy texts; Q7
and S2 were at their nodes already); EL-6 item 4's Q7 is at the `UDP_DECL`, as §9.1 says; the
taken-name check runs when a definition is reached, and names a built-in cell type with its own
text; EL-7 rejects a `(* blackbox *)` module given as the top; §17 #25 records `(* top *)`.

v4.6 (2026-10-12; 2D Task 11, agent defaults). (1) A C re-entry cap, `ODIN3_ELAB_MAX_REENTRY_DEPTH`
= 256, bounds the constant evaluations nested in C (a typer callback's run, a host's re-entry such
as a lazily evaluated parameter; §4.6, §11) apart from the call-depth cap, which still counts them
with the function frames: 1024 C levels needed 4–6 MB of stack under gcc ASan and overflowed under
clang ASan with fuzzer flags. Past it: `constant evaluation of <what> nested deeper than 256`. (2)
EL-9's key counts `UNSIZED` only for an unsigned value whose top bit is 1, the one case where it
changes how the value extends: `#(.W(8))` and `#(.W(32'sd8))` on `parameter integer W` share, while
`'hFFFFFFFF` and `32'hFFFFFFFF` on an untyped `P` are two specializations of one spelling (the
second takes `$2`). (3) A parameter is named in EL-9's name when an override applies to it and its
final value differs from what its default expression gives in that specialization. (4) Point (q)'s
taken names: a definition of another name, a specialization made so far, a registered cell type
(two libraries' definitions of one name: the second specialization takes `$2`). (5) Pending
overrides keep one entry per path, the one of highest precedence (the others can never win); the
key holds each one's path, source and value. (6) *Reversed by v4.7:* the top's specialization keeps
its definition's name (EL-9). (7) Locations: the defparam errors and warning at the
defparam's target (its `HIER_NAME`); the project texts at the project line; the localparam and
unknown-parameter errors at the `CONNECTION`; the mixing error at the first value of the other
form; `too many parameter values` at the first surplus value; the specialization cap at the
`INSTANTIATION`; the depth cap and the recursion error at the `INSTANCE` (a module instantiating
itself with the same parameters: `recursive instantiation of 'm'`, no "through"). An override of a
localparam has the localparam text from a `#()` or a `defparam`, the project text from a project
line. (8) Instances of one `INSTANTIATION` that land in different specializations are written as
one `INSTANTIATION` per run of consecutive instances sharing one (EL-4).

v4.7 (2026-10-12; the 2D Task 11 review, controller rulings I1 and I2, verified on Yosys 0.55).
(1) The top's specialization keeps its definition's name whatever its parameters and pending
overrides (EL-9), as Yosys's `chparam` and `hierarchy -chparam` do; v4.6 item (6) is reversed. (2)
Erratum to EL-16: an override value is sized before it is converted. Yosys passes `#()`,
`defparam` and `chparam` values into a `$paramod` as an `RTLIL::Const`, which has no `is_unsized`:
`m #(.P('hFFFFFFFF))`, `m #('hFFFFFFFF)`, `defparam a.P = 'hFFFFFFFF` and a ranged `[31:0] P`
overridden with it all give `64'h00000000ffffffff` in a 64-bit context, like `32'hFFFFFFFF`; only
a default `parameter P = 'hFFFFFFFF` gives all ones, and `#(.P(-1))` keeps its sign. (3) v4.6 item
(2) is corrected: the key keeps `UNSIZED` for an unsigned value whose top bit is not 0 (1, x or z),
since an unsized value repeats an x or z top bit where a sized one extends with 0 (Yosys: a default
`'hx0` gives `64'hxxxxxxxxxxxxxxx0`, an override `32'hxxxxxxx0` gives `64'h00000000xxxxxxx0`; two
specializations). With (2), `'hFFFFFFFF` and `32'hFFFFFFFF` overrides now share one. (4) Project
numbers are sized like every override (2) and keep the literal's sign, as the same text in a `#()`
would (`16` is `32'sd16`, `8'hFF` unsigned, `-3` the literal negated), so `param top.u1.W 3` names
`m$W=3`; `chparam -set W 3` makes an unsigned `RTLIL::Const` instead (`$paramod\leaf\W=32'…011`):
divergence §17 #26. (5) Override paths join their components with
the unit separator (0x1F), which no identifier holds, so an escaped `\a.b` is one component; names
and messages print `.`. (6) The `<what>` of a parameter's declared range is `range bound of
parameter 'P'`, so a nesting or constant error there names the parameter. (7) The forms of landed
kinds that later tasks land (ordered connections, instance arrays, a defparam reaching a cell
type, a body port, a specparam, a time or real variable, `tri0`/`tri1`/`trireg`, a hierarchical
call) have a table of their own beside point (l)'s, which the "tables empty" assertions of Tasks
16 and 24 also check.

v4.8 (2026-10-12; the 2D Task 12 review, controller rulings). (1) Black boxes are ports-only
`MODULE`s of the elaborated form (EL-4, EL-5, EL-11): a `(* blackbox *)` specialization's and an
implicit black box's, with a synthesized `ATTR` `blackbox`, their ports and (a `(* blackbox *)`
one) final parameter values; their instances connect by port like a module's; stage 2 registers a
black-box cell type from each (IR-7b). (2) A `SELECT` of any expression is an instance-array slice
read at the object's self-determined width (EL-4). (3) A cell type's ordered `#()` values are
written named, by the type's parameter at their position, in the type's parameter order (EL-4).
(4) EL-10 states that the lowest index gets the low bits for either range direction (Yosys 0.55);
§17 #29 records IEEE. (5) §9.1 gains the texts of Task 12's agent defaults; §17 #27 and #28 record
gate arrays and the output rule.

v4.9 (2026-10-12; 2D Task 13, the binder, and the Task 12 review's notes n1 and n2). (1) EL-4: a
black-box `MODULE`'s `PARAM_DECL`s are bare (no `RANGE`, `SIGNED` or `DT`: the value's `NUMBER`
carries width and sign), and its ports are always written as `wire` ports (`integer`: `signed
[31:0]`). (2) EL-4 lists the texts of the binder's store-shape checks, three of them agent defaults
for rows the v4.8 list left without one: a system task other than `$readmemh`/`$readmemb`, a range
bound that is not a `NUMBER`, a generate index that is not a 32-bit `NUMBER`. (3) EL-5 is made
exact (agent defaults): one `LIBRARY` per library, from each `MODULE`'s `FILE` buffer (`work` when
it has none), holding the `MODULE` symbols, which no expression name reaches; an `INSTANTIATION`
resolves to its `MODULE` symbol by name across libraries (names are unique in the `UNIT`, point
(q)), or to nothing for a registered cell type; a function's scope holds a `VAR` named like it, its
result, and a `CALL` from inside skips it; an `integer` is `signed [31:0]`; a localparam symbol is
`SIGNED` as its value; the resolution array also maps every declaring node to its symbol; a net
declaration assignment is a `CONTINUOUS` assignment; a `HIER_NAME` that ends in an indexed
component is E21; a `CALL` or `TASK_CALL` naming nothing is `'f' is not declared`; a range bound
outside int32 is the width error at the bound, an x or z bound `range bound has x or z bits`.
(4) The `IMPLICIT` flag is not set by the binder: the elaborated form has no marker for stage 1's
implicit-net `NET_DECL` yet (Task 15). (5) Fix round 1 (the Task 13 review): the binder also
rejects four rows that were carried (an `INSTANCE` with a `RANGE`, a `tri0`/`tri1`/`trireg` net, a
switch-level or pull gate, `vectored`/`scalared`), and a name that resolves to an instance,
function or task is `'u' is not a net, variable or parameter` (Yosys 0.55: "Failed to detect width
for identifier"); a block's name binds to its `BLOCK` symbol until Task 18 decides (Yosys makes it
an implicit net). 2A's symbol table gains O(1) chain appends and position lookups (2A §8
amendment).

v4.10 (2026-10-12; the PR 2D-3 final review, controller ruling I2 and minors m1, m3, m5). (1) EL-9,
EL-11, EL-4: a `(* blackbox *)` definition keeps its base name for every specialization (Yosys
0.55: `module \bb`, `cell \bb` with `parameter \W 8`), one ports-only `MODULE` from its default
specialization, its instances carrying their values; Task 19 declares one cell type under that
name and `write_blif --vtr` writes `.subckt <base>` (§17 #30). (2) EL-9's key text names each
pending entry's source. (3) EL-11: an implicit black box's width from an instance array's first
connection is one element's when N divides it. (4) EL-24: 2C rejects an empty non-ANSI port other
than a trailing comma, as Yosys does (§17 #31).

v4.11 (2026-10-12; 2D Task 15, agent defaults). (1) EL-4: the implicit nets' `NET_DECL` carries a
value-less `ATTR` `implicit` (synthesized, like `blackbox`); the binder sets `ODIN3_SYM_F_IMPLICIT`
from it (EL-5). (2) EL-24, EL-25: a `PORT_DECL` (the header's or the body's) and a `NET_DECL`/`VAR_DECL` of its name
merge in either order (Yosys 0.55 accepts `module m(output q); reg q;`); an `input` declared `reg`/`integer` is a net (an `integer` one a `signed [31:0]`);
a port expression in the header makes the body's unlisted-port errors moot (M6 is the one error);
`` `default_nettype `` and `` `resetall `` are read by stage 1 and dropped (the other directives stay
Task 16's). (3) EL-4: a `REPEAT` count below 0 is 0; an `always` without an event control, a delay
or event control inside a statement and a system task other than `$readmemh`/`$readmemb` in an
`initial` stay Task 16's (T6, T11, T12, S3). (4) §17 #35 records the rejections of what Yosys accepts.

v4.12 (2026-10-12; 2D Task 15 fix round 1). (1) EL-19, EL-4: a block, function or task local or
port hides a genvar of the same name (Verilog scoping). (2) EL-4: `$unnamed_block$<k>` is keyed by the
source block, so generate iterations share k. (3) EL-4: untyped function and task ports have
`NET` = 1 + `wire`; an `ATTR` `NUMBER` is unsized. (4) §17 #35 gains the Informational divergences.

v4.13 (2026-10-12; 2D Task 15b). (1) EL-4: `localparam` and `parameter` declarations in a function,
task or named block are accepted (Yosys 0.55 parity, Verilog-2005 block_item_declaration), written
as `PARAM_DECL[localparam]` in the locals LIST with their final values, evaluated in a scope of
their own under the enclosing one; a `parameter` there is a localparam. (2) §17 #35 loses the
`localparam` half.

v4.14 (2026-10-12; 2D Task 16). (1) §9.1/§9.2: stage 1 reports the REJECT and IGNORE rows in one
scan per definition (`elab1_diag.c`), in source order, over the whole body including generate
branches that are not selected; a definition whose scan reported an error is not expanded or
written. (2) Agent defaults, §17 #36: an intra-assignment or statement event control outside an
always's own is T12 (inside a function K12); `$readmemh`/`$readmemb` outside an `initial` is stage
1's error `$readmemh is supported only in an initial block`; an edge/level mix is located at the
`EVENT_CONTROL`; the arguments of an ignored system task are not scanned; a charge strength is
warned even on the `trireg` that N15 rejects.

v4.15 (2026-10-12; the PR 2D-4 final review, controller rulings C-1, I-1, I-2, I-3 and minors). (1)
EL-4, EL-24 (C-1): a net or variable declaration merged into a port keeps its value and its
attributes. The written port's `DECLARATOR` holds the value (`E?`: the net declaration assignment of
a net port, the initial value of a `reg`/`integer` port, the slot an ANSI `output reg q = 1'b1`
already used) and the `PORT_DECL` carries the attributes of both declarations (the later one wins a
name both give, as Yosys 0.55 does); the binder queues the `CONTINUOUS` assignment of a net port
that has a value. A value on both declarations is the error `port 'q' has a value in both of its
declarations` (§17 #37). (2) EL-22 (I-1): a path into a named `begin`/`end` block of an `always` or
`initial` (`blk.count`) is accepted by stage 1 and resolved by the binder. (3) I-2: a user function
call in a connection stage 1 must size has its declared result width (§17 #17). (4) §9.3, §17 #36:
a module whose scan found an error skips passes E and W, so its children are not reached; the info
`not instantiated under the top` is not logged once an error was. (5) §17 #35's columns are
corrected, #37 and #38 are new, and the changelog is in order.

v4.16 (2026-10-12; 2D Task 18, agent defaults). (1) EL-30: a ternary's `$mux` is as wide as its
branches as generated (an identifier at its own width) and the result is then extended to the
context, as Yosys's `genrtlil.cc` does (verified on 0.55: `assign y32 = c ? a : b;` is a `$mux` of
`WIDTH` 8 and 24 zero bits); the comparison row says operands are typed at their common width
(an identifier is still generated at its own width: `a < t` is `$lt` with `A_WIDTH` 8, `B_WIDTH` 4,
verified). (2) EL-32: the text of an out-of-range bit select, an x or z constant index (§17 #40)
and the generate-block name read as a value (§17 #39). (3) fix round 1: a self-determined operand's cell sign follows genrtlil (§17), plain decimals stay 32 bits (§17 #41), non-constant `+:`/`-:` reads are simpler than Yosys's (§17 #42).

v4.17 (2026-10-12; 2D Task 19, agent defaults). (1) EL-28: the driver table is per net and a second
table, the claims, is per (wire, bit): `assign y = a; assign y = b;` merges y's net into a's (which
its port drives), so only y's own claim tells A4 where y was first driven (3:10, not the port's
place); every driver of a wire bit (assign, instance output, gate, `$tribuf`) leaves one, and a
net entry says what drives the net itself (input ports, cell outputs, tri-state drivers). A bit
claimed by a merge is not a driver of the net it joined (`assign out = tmp; assign tmp = a & b;`
is legal). Instances and gates (step 4) are emitted before the continuous assignments (step 5) in
two walks of the items. (2) A4 for one lvalue naming a net twice (`{n4[0], n4[0]} = …`) is an error
at the later part (§17 #44); `'r' is a variable assigned by a continuous assignment; treated as a
net` is located at the lvalue. (3) EL-10: the resize warning is located at the `CONNECTION`, names
the module as the `INSTANTIATION` does (`sub`, `sub$W=8`), and is "extended" when the connection is
narrower than the port and "truncated" when wider, for outputs too (Yosys: "Resizing cell port
… from W bits to P bits"). (4) EL-11 (superseded by v4.19 (1)): Task 19 first failed closed on an
instance that gave a non-string parameter a value other than its default when a port had a range,
because the elaborated form carried only the default specialization's ports; v4.19 replaces that
error with exact widths. A registered type of the name is matched by port name and reused, its
widths inferred (IR-7b). (5) EL-11:
`parameter:<NAME>` is written for every parameter of a module instance's params LIST (the
elaborated form does not say which were overridden). (6) EL-26: a bit with one driver is merged
into the wire's net in step (7) (pin-based, IR-15) because the driver's net is only known then;
chains are word cells when every driver covers the whole wire, else one-bit cells. (7) EL-28:
`'w' is never driven` is for wires that are not ports (rule 4 of `check` covers a port), and a
self-assignment's warning `'q' and 'p' are connected only to each other and are never driven` names
the right side first, as the example does. (8) EL-30's `$tribuf` data and enable: the data is
generated at the lvalue's width, a one-bit enable is used as is, a wider one goes through
`$reduce_bool` (`$logic_not` when inverted).

v4.18 (2026-10-12; 2D Task 20 fix round 1, controller ruling I1). EL-27 rule 4 no longer treats an
array inside a concatenation lvalue as a complex lvalue: Yosys 0.55 splits concatenation lvalues
before mem2reg, so `{m[wa], t} <= {a, a};` with `assign y = m[ra];` keeps one memory and prints no
`replacing memory` warning (verified: `yosys -p "read_verilog t.v; proc; stat"`).

v4.19 (2026-10-12; Peter decision 3: exact black-box port widths per parameter set). (1) EL-11,
EL-4, EL-9: stage 1 writes, besides a `(* blackbox *)` definition's one ports-only `MODULE` (its
default specialization's ports and values, v4.10), a **carrier** `MODULE` for each other
parameter set an instance uses: a ports-only black-box `MODULE` named as a module's specialization
is (`bb$W=16`, `$2`.. when taken), whose `ATTR` `blackbox` holds the definition's name as a
`STRING` (the definition's own has none), with the ports evaluated under that set and one bare
`PARAM_DECL` per parameter holding the set's final values. An `INSTANTIATION` still names the
definition (v4.10): the carrier is named by nothing and only carries data. No carrier is written
for a definition whose name is a registered cell type (stage 2 reuses the type, IR-7b). Carriers
come after every specialization, before the implicit black boxes, in queue order. (2) Stage 2
(`elab2_bbox.c`): the definition's `MODULE` declares the one cell type under the base name (as
before, with the default widths in the declaration) and reads its carriers; each port whose width
differs in some set gets a **width expression** (`celltype.h`'s fourth width rule, produced here
as the tech library produces its own) that finds the cell's parameter values in a table with one
entry per set and returns the port's width there. The cell keeps its source parameters (no hidden
parameter, no per-cell type), so a cell takes the exact widths of its set as Yosys 0.55 gives its
connections (`hierarchy` derives the module, checks and resizes the connections against it and
drops it; `write_rtlil` shows `cell \bb \u1` with `parameter signed \W 16` and 16-bit `connect`s
beside `module \bb` with the defaults' widths); I4's resize and its warning apply to the exact
width. The `width of port … cannot be determined` error of v4.17 (4) is deleted. A string
parameter that no range reads changes nothing; a set whose values do not fit the parameter's kind
is left out (its instances get "value of parameter … is not an integer"). (3) New located error
(agent default): two sets whose values are equal as IR integers but whose literals differ in size
(`4'd8`, `5'd8`) and give a port two widths (`[(~W)>>2:0]`) cannot be told apart by the cell's
parameters: `black box 'bb': port 'p' has different widths in parameter sets with equal values`,
at the definition. (4) Remaining divergences are in §17 #46.

v4.20 (2026-10-12; 2D Task 21, agent defaults). (1) EL-35 step 7: the driver claims are made a chunk
at a time (the output chunks of step 1), so a chunk with a conflicting bit is refused whole and the
actions of all its bits leave the body (an error path: the stage fails either way); a bit of a
wired net is left out of the claim and its actions dropped (the pre-scan reported it). (2) EL-28
step (2): the second half of the pre-scan runs after the mem2reg decision and marks the word wires
of a register array that an `always` assigns (`odin3_elab2_prescan_words`: a NUMBER index marks that
word's wire, any other index every word's, a part select of a word the whole wire), because those
wires do not exist when the first half runs. (3) EL-41: T10 and T9 are reported by the process
generator in Task 21 (T10 is the driver claim of a process against a process, T9 a per-net record
of how each bit was first assigned, kept for the module), so the handover cases T9 and T10 pass
for real; Task 24 adds the memory and task-output cases. (4) `variable index in a procedural
assignment target` is a new point (l) feature, pending until Task 22 (the case method of EL-32); a
memory word write is `memory write` until Task 24. (5) EL-34: T3's analysis records the nets a
level-list block reads that no assignment of the block supplied; the warning is located at the
`always`.

v4.21 (2026-10-12; 2D Task 22, agent defaults). (1) EL-36 folding is by the AST alone: a condition
or selector and items that are literals and parameters (no variable, not even one holding a
constant) fold to the branch they take, and the scan applies it before the outputs are collected, so
a dead branch writes nothing and its outputs are not outputs, as in Yosys, which simplifies it
away first; a constant with an x or z bit that no wildcard covers is not folded (C9). Yosys keeps a
`switch` on the constant, so §17 #56. (2) EL-36: the added default case exists only when the
branches assign something by `=` (it holds the case temporaries), an item is `default`, or the
switch has `full_case` (v4.24, review C1); Yosys adds an empty one always, §17 #57. (3) D1's two warnings are located at the attribute (`(* *)` or the
metacomment) and given when the switch is built, not when a folded case drops it. (4) EL-32 writes:
the position temporary `p` is assigned the index in the **root** case (a conditional write must not
leave it unassigned: a latch), the switch on `p` has a case for every declared index value the
index expression's own width and sign can hold at which the slice lies at least partly inside the
target, and a case writes the bits of the slice that are inside it (v4.22), §17 #54; a select that can
reach nothing is the located warning `assignment to bits outside 'y' [7:4] is ignored`. A
non-constant select inside a concatenation target is an error, §17 #55. (5) Point (l): `if`,
`case` and the case method for a bit or part select landed; the feature `variable index in a
procedural assignment target` is renamed `variable index into a register array in a procedural
assignment target` and stays pending for a register array's word until Task 23 (EL-27's case over
the words). (6) The API: the statement entries are `odin3_pg_if` and `odin3_pg_case_stmt` (the
name `odin3_pg_case` is Task 21's primitive); a switch's blocking targets are collected once by the
scan (a sorted unique list per `if`/`case`), not once per nesting level.

v4.22 (2026-10-12 and 2026-10-13; 2D Task 23, agent defaults, items 1-4; 2D Task 22 fix round 1, controller
rulings, items 5-6). (1) EL-37: an unrolled loop's variable is bound for the
AST constant test while the loop runs, so a branch that reads it folds in every iteration at
generation (Task 22's carry); the scan still counts the outputs of every branch (§17 #62). The variable
is an output of the block only when the module reads it outside the loops it is the variable of
(EL-35, divergence 5; a read after the loop in the same block counts, as in Yosys). The loop frames
count on from the generate loops stage 1 unrolled (`odin3_elab_opts.gen_iters`, point (k)). Messages
added to §9.1: `for-loop initialisation is not constant (it reads 'x')`, `for-loop step is not
constant (it reads 'x')`, `task 't' has N arguments; M given`, `non-constant index into the
multi-dimensional register array 'm' is not supported`. (2) EL-38: a call frame gives the callee's
ports, locals and result unnamed IR wires for the call's duration (the symbols' `ir_wire` is set and
reset), so the walker and the lvalue code see them like variables; the frame's nets are plain
process-local temporaries (`O` pins with no sync action), not Yosys's `$0` temporaries with a sync
action that updates the wire to x (§17 #59). The interpreter is tried first; a call that stays is
inlined above the caller's frames (the walker, the statement stack and the lvalue scratch are
re-entrant). (3) EL-27: the words of a register array with a variable index are a case over the
words, inline in a process or a `$proc` of its own outside one; mem2reg rules 2 to 5 scan the bodies
of called functions and tasks, and `m[f(1)]` is a constant index when the interpreter folds the call
(Yosys, verified on 0.55). (4) The brief's `odin3_pg_comb_call`/`odin3_pg_comb_mem_read` take the
generator; the walker reaches them through `odin3_elab2_comb_call`/`odin3_elab2_comb_word`. (5) EL-32 writes: a part select
that straddles an end of the target writes the bits inside it (IEEE 1364-2005 4.2.1), at either end,
for `+:` and `-:`, in both declared directions and for a signed base (`y[s +: 2]`, s = -1, writes
`y[0]`): the switch has a case for every declared index value at which some bit of the slice lies in
the target, and a case writes only those bits. Yosys 0.55 agrees except for `-:` with an unsigned
base below the width minus one (§17 #54). (6) A constant index with an x or z bit as a procedural
target writes nothing, with the warning of any write outside the object (`assignment to bits
outside 'y' [3:0] is ignored`), §17 #58.

v4.23 (2026-10-13; 2D Task 24, agent defaults). (1) EL-39: the write port is built where the write
statement is executed (a loop or a call makes one set of `addr`/`data`/`en` per execution, which the scan
of the block cannot count), so the three variables of a statement join every `if`/`case` switch that is
open when it is built: each level gets case temporaries of its own, the entry action `t1 = x` (`en`: `0`)
in every case of the switch (the cases already built get it first in their lists, the cases to come get it
as they open, an added default is opened when a variable joined), and the exit action after the switch;
the root's `addr = x`, `data = x`, `en = 0` go to the front of the root, a later statement's before an
earlier one's, as Yosys inserts them (`current_top_block->children.insert(begin, defNode)`). The result is
the body a variable the scan had listed would have (§17 #64). The ports are added in the order the
statements are built (source order within a block, blocks in source order; two `always` blocks writing one
word in one cycle are a race, §17 #66). A constant index is kept at its self-determined width
(`m[2]` is a 32-bit integer: `ABITS` 32, as Yosys); an `addr` variable is the wider of the index and the
smallest address width that numbers the words. (2) EL-39: a write in a block that has not exactly one edge
event to an array that stayed a memory (`(* nomem2reg *)` exempts it from the mem2reg rules) is the
located error `array 'm' cannot be a memory (it is written in a block without exactly one edge) and (* nomem2reg *)
is set`, at the statement (Task 20's carry). A concatenation target with a word of a memory is split into one
write per part, the least significant first, the value taken once at the target's width (Yosys's
`$splitcmplxassign`); a word in a nested concatenation, and a non-constant bit or part select of a word
(`m[i][b] <= x`; Yosys shifts the data and the enable) are located errors (§17 #65). Messages added to
§9.1: `memory word of 'm' in a concatenation target is not supported`, `array 'm' cannot be a memory (it is
written in a block without exactly one edge) and (* nomem2reg *) is set`. (3) EL-40: the initial values are run after
the instances and the continuous assignments (end of emission step 5, still a step that may merge nets), not
at step 3, because the classification of a result needs the driver tables of those steps; the results are the
same. An `initial` block without locals is run statement by statement, so one statement that is not constant
does not take its neighbours with it; `$readmemh/b` is a statement of the block (inside an `if` or a loop it
is the located error `$readmemh is supported only as a statement of an initial block`, §17 #67). `$readmem`
reads as Yosys's `readmem` does (comments, `@hex`, words as sized literals, the default range the array's own);
a word past the array (an `@` address or a count) is not stored and the call warns once (§17 #68, fix round 1; a word wider than the memory is truncated with one warning, #73). The memory's `INIT` is a BITS value of
SIZE x WIDTH bits (word `k` at bit `k x WIDTH`), x where nothing was written. Messages added to §9.1: the
D2 warnings are as §8.8 says; the shape errors of `$readmem`'s arguments (`the second argument of $readmemh
must be a memory`, `$readmemh needs a one-dimensional array; 'm' has N dimensions`, `$readmemh takes 2 to 4
arguments; N given`, `the first argument of $readmemh must be a string`, `an address argument of $readmemh is
not constant`) and `assignment to a word outside array 'm' is ignored` (an `initial` write past the array).
(4) The pending feature `memory write` and the kind `INITIAL` are gone from the point (l) tables, which are
empty; `elab-equiv` gains `--unmap-enables` (`dffunmap` and `opt -nodffe -nosdff`: `memory_map` makes enabled
flip-flops, which `equiv-check` cannot read).

v4.24 (2026-10-13; PR 2D-6 fix round, agent defaults). (1) EL-36: under `full_case` (attribute or metacomment)
the empty default case is added even when no branch assigns by `=`, as Yosys does, so the unlisted values of
`<=` arms hold instead of becoming x (review C1; §17 #57 corrected). (2) EL-35: an output no action assigns
is dropped at emit (no hold, no sync update, no claim), so `y[i] = a[i]` in a loop over a half of `y` neither
latches the other half nor conflicts with a second block writing it (review I2; §17 #62). (3) The pre-scan
marks the actuals of a task's output and inout formals as assigned (review I1). (4) EL-40: a `$readmem`
address is hex digits only and at most 2^40, a located error otherwise (`bad address`, `address out of range`);
an over-wide word is truncated with one warning per call (§17 #73; reviews I3, I4). (5) Caps: the inline call
nesting cap 1024 (`ODIN3_ELAB_MAX_INLINE_DEPTH`, a heap cap since 2D-7, §17 #74, review I6) and, for endless unrolled loops, a
cycle check and an IR object budget per loop (`ODIN3_ELAB_MAX_LOOP_OBJECTS`, §17 #75, review M3). (6)
Documentation: §17 #70 states what Yosys does with `initial tk(7)`, rows 71 and 72 (reviews M1, M2), the
warnings of Task 24 are in §9.2, the duplicate v4.22 entry is merged. Messages added to §9.1: see §9.1.

v4.25 (2026-10-13; 2D Task 25 review fix, controller ruling I2, agent default Peter may override). §14.2:
the 54 "hand-written" `.elab` goldens are seeded from `elab-check --write` and accepted as regression
locks, not independent derivations; line 2 of each says so, and the 19 PARAM_DECL/GEN_BLOCK rows were
hand-checked against IEEE 1364-2005 (0 disagreements; the defparam order follows EL-8, not IEEE, as
documented). The expected-failure list is 14 rows (110 rows equivalent); its entries carry the expected
failure text.

v4.26 (2026-10-13; 2D Task 26, agent defaults). (1) §14.4: `project-fixtures compare --with-frontend --yosys Y
--equiv-check E` runs the bridge on every positive phase-2 Verilog case and format (odin3's `read_project;
read_verilog --nolatches; write_rtlil`, the shared Yosys back end, `equiv-check` against `ref.blif`). A
non-equivalent result reruns `ref.cmd` with the 0.55 given: if that disagrees with `ref.blif` the case is
reported `oracle drift`, if Yosys dies of a signal there it is `oracle crash`, neither is a failure; else it is
a 2D failure. (2) §14.5: `elab-sweep` (`tools/elab-check/elab-sweep`, library `elab_sweep`) is the one driver
of `verilog_micro_elab`, `elab_equiv_micro`, the VTR sweeps and `--bench`; the micros that fail before the
bridge are listed in `micro_expect_fail.txt` (71 entries: the 50 of 2C's list, the micros 2C parses but whose
elaboration is rejected, none of which has a Parmys golden), the ones the bridge finds not equivalent in
`micro_equiv_expect_fail.txt` (agent default: a separate file, since `equiv_expect_fail.txt` holds coverage-row
names). A reason that starts with `xl` marks a file for the oversized run (skipped by the 6G sweeps, run alone
with `--xl`). `elab-equiv` exits 3 (`oracle crash`) when Yosys dies of a signal on the source (0.55 segfaults
on recursive instantiation, where 2D reports "recursive instantiation of 'top'") and reports a child killed by
its cap as `out of memory (cap 4G)`. (3) `micro-top` follows nested golden groups (`keywords/assign/assign_wide`).

v4.27 (2026-10-13; 2D-7b, Peter 2026-10-13 "accept T6/D3/T9 with warnings", PHASE2 #20). (1) T6 is
accepted (§8.2, §9.2, §17 #76): an `always` with no event control is `always @*`, with a located
warning. The T6 row leaves §9.1. (2) T9 is accepted (§8.9, §9.2, §17 #77): `=` and `<=` on one
bit follow Yosys (every assignment is an action, the last on a path wins) with a warning that
names the variable, and on one control path adds that simulation and synthesis may differ. T10
stays an error. The T9 row leaves §9.1; `reject-2d` holds 23 cases and `ignore-2d` 12. (3) The hard `adder`'s positional width
(`adder #(W)`) stays a located error that now says why (§17 #78): the library cell is the 1-bit hard block;
Parmys's n-bit `adder` is a soft module (v4.29: it elaborates once `vtr_primitives.v` is read with `--lib`, §3.1a). (4) D3 (internal tri-state nets to mux logic, as `tribuf -logic`:
a `$pmux` of the data by the enables over an x default) is ruled accepted but not yet implemented; EL-29
and §17 #45 still describe the error until it lands.

v4.28 (2026-10-13; 2D-7b, D3 lands; PHASE2 #20). (1) EL-29 (§6.7): the `$tribuf` cells of nets that
reach no top-level port become logic at the end of each module, as Yosys's `tribuf -logic` does. The cells
that drive the same list of nets are one group, and a group is one `$pmux` (A = x, B the data with the last
driver first, S the enables); one warning per group names the net (§9.2). Cells on a net of a top-level output
or inout port keep their handling. A net with a tri-state driver and another kind of driver stays A4's
error. Four located errors replace the old one (§9.1). (2) §17 #45 is narrowed and #79 added (the
evidence, the several-enabled case, the single-driver case). (3) `elab-equiv --yosys-tribuf` runs
`tribuf -logic` after `proc; flatten` on side B; plain `proc` shorts the drivers, so a bus is not comparable
without it. Micro `syntax/multi_assignment` and `tests/elab/accept/D3_*.v` are `elab-equiv` equivalent; the
micro leaves `micro_expect_fail.txt`.

v4.29 (2026-10-13; 2D-7b library read, agent defaults; PHASE2 #20). (1) New §3.1a: `read_verilog
--lib` keeps a read's modules as library units for later reads, each elaborated where a later design
instantiates it and under the flags (`--nolatches`, `--nomem2reg`) of the read that supplied it; it is
not Yosys `read_verilog -lib` (no black boxes). (2) EL-6: a `--lib` unit is exempt from the "has the name
of cell type" error, and an instance resolves to the unit or to the hard cell by the parameter rule of
§3.1a. (3) EL-3 gains `--lib`; EL-4 gains the MODULE attributes `odin3_lib*`. (4) §17 #78 is rewritten
(the `adder #(W)` micros now elaborate, equivalent to Yosys with `vtr_primitives.v` read as a library) and
#80 added (the structural gap to the Parmys goldens). (5) `elab-equiv` and `elab-sweep` take `--lib FILE`;
`tests/elab/lib/vtr_primitives.v` is a copy of Parmys's file (trailing blanks stripped) for the micro sweeps.

v4.30 (2026-10-13; Peter's ruling on `initial x = <non-constant>`, PHASE2 #24). (1) EL-3 gains
`read_verilog --vtr-compat` (`odin3_elab_opts.vtr_compat`; a `--lib` read's units carry it as the
`MODULE` attribute `odin3_lib_vtr_compat`, `ODIN3_UNIT_VTR_COMPAT`). (2) EL-40: with the option, a
statement of an `initial` that is not constant and is a plain `=` or `<=` assignment to a variable
(or a constant bit or part select of one), after an optional delay, is `assign x = e;` when nothing
else drives the target bits; the warning is `initial value of 'x' is not constant: treated as a
continuous assign (VTR/Parmys compatibility)`. A target that an `always` block assigns, or a
continuous assignment, an instance, a gate or an input port drives, keeps the ignore, with `initial
value of 'x' is not constant and is ignored: 'x' is <also assigned in an always block | driven by
...> (VTR/Parmys compatibility does not apply)`. Without the option nothing changes. (3) §9.2 lists
both warnings; §17 #85 is the divergence from IEEE 1364-2005 (which leaves the variable at x until
the statement runs, at time 0, in simulation). (4) `elab-equiv`, `elab-sweep` (`--equiv`),
`sp-splice` and the `vtr-qor` odin3 legs pass `--vtr-compat`; the `elab_equiv_micro` sweep runs with it.

v4.31 (2026-10-14; Peter's ruling on Parmys behaviour in VTR mode, PHASE2 #27). EL-29 (§6.7): under
`read_verilog --vtr-compat`, a group of two or more tri-state drivers on an internal net whose data
is all input-port bits is resolved as Parmys does (enables ignored, per bit the input another cell
reads, else the lowest-named wire, else the highest bit), with a warning that names the net, the
data kept, the wires dropped and the IEEE reading (§9.2 D3; §17 #79). Micro
`syntax/multi_assignment` becomes the Parmys golden. Other tri-state groups, and every net without
the option, are unchanged.

## 1. Purpose and scope (EL-1)

2D turns the parsed-form AST of one read run (2C: one store, every `UNIT` of the run's Verilog
files) into word-level IR with processes (DESIGN §6 step 2), in two stages with the elaborated
AST as the seam (PHASE2 #8 Q3).

| Delivered by 2D | Where |
|---|---|
| **Stage 1** (Verilog only): parsed store → elaborated store. Module lookup, top selection, parameters (`#()`, `defparam`, project `param`), specialization, `generate`, constant evaluation including constant functions, implicit nets, non-ANSI port merging, the IGNORE warnings and the REJECT errors that need semantics | `src/frontends/verilog/elab.h` (the `odin3_elab_run` entry 2C calls), `elab1_*.c` |
| **Stage 2** (any front end): elaborated store → IR. Binding (the 2A symbol table), widths and signedness, expressions → word cells, continuous assignments and drivers, instances, memories, `always` → `$proc` process cells (EL-33), function and task inlining, loop unrolling, initial values | `src/frontends/elab/` (shared with the Phase 5 slang adapter) |
| The constant evaluator and statement interpreter used by both stages | `src/frontends/elab/ceval*.c` |
| IR additions (**ruled**, §20): the `$proc` process cell with an opaque body (IR-19), nine registry cells, port range and signedness; the pass flag that keeps processes away from passes that cannot handle them | `src/ir/cells/proc.c`, `src/ir/proc.[ch]`, `src/ir/value.c`, `src/ir/cells/*.c`, `src/ir/module.c`, `src/passes/manager.c` |
| `write_rtlil` (Yosys RTLIL text, processes included): the bridge to the Yosys oracle | `src/backends/rtlil/` |
| `read_verilog --nolatches --nomem2reg --auto-blackbox`; CLI `--elab-only`; ABI `odin3_ast_elaborate`, `odin3_ast_get_library_units_from`, process read accessors | `src/passes/builtin.c`, `src/cli/main.c`, `src/api/` |
| Tests: one case per coverage row (stage-1 golden, stage-2 golden, Yosys equivalence), the 2C handover (`reject-2d/`, `ignore-2d/`), micro and VTR sweeps, a differential fuzzer, the mcml.v and lenet.v benchmarks | `tests/elab/`, `tests/unit/test_elab_*.c`, `tools/elab-check/` |

Not in 2D: `proc` (2E: switches → muxes, syncs → `$dff*`, latch detection and its warning),
`opt` (2F), memory port analysis and mapping (Phase 4 `memory_infer`), `flatten`,
SystemVerilog (Phase 5 appends AST kinds; stage 2 handles the elaborated subset), and everything
the 2A/2B/2C specs own.

Success: every MUST and SHOULD row elaborates and is equivalent to Yosys on its coverage case
(§14.2); every REJECT row gives the located error of §9 and every IGNORE row the warning; every
micro and VTR benchmark elaborates under `--elab-only` (expected failures listed with reasons)
and, through `write_rtlil`, is equivalent to `read_verilog -sv -nolatches` in Yosys 0.55; the §15
targets are met on mcml.v and lenet.v; the gate passes.

## 2. Two stages and the seam

### 2.1 Pipeline and entry points (EL-2)

```
 read_verilog (2C)                       stage 1 (Verilog)                         stage 2 (any front end)
 files ─ 2B ─ 2C ─► parsed store ─► odin3_elab_stage1 ─► elaborated store ─► odin3_elab_stage2 ─► IR
                                    (lookup, top, params,  (+ nothing else:    (bind → check → emit:
                                     generate, constants)   EL-4 is the whole   wires, cells, instances,
 slang adapter (Phase 5): odin3_ast_* builders ───────────► contract)           processes, $mem)
                                                         └─ odin3_ast_elaborate (ABI) ─┘
```

```c
/* src/frontends/verilog/elab.h — the entry 2C's read_verilog calls (2C §4.2 step 8). */
odin3_status odin3_elab_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_ast *parsed);

/* src/frontends/elab/elab.h */
odin3_status odin3_elab_stage1(odin3_pass_ctx *ctx, odin3_ast *parsed,
                               const odin3_elab_opts *opts, odin3_ast **elab_out);
odin3_status odin3_elab_stage2(odin3_pass_ctx *ctx, odin3_ast *elab,
                               const odin3_elab_opts *opts);
```

`odin3_elab_run`: (1) read the options (EL-3); (2) `odin3_elab_stage1` → elaborated store (a
new store, form `ELABORATED`, the read's run, built with the 2A builders and sealed with
`odin3_ast_finish`); in Debug or under `--check`, `odin3_ast_check(elab, FULL)` (a violation is
`ODIN3_ERR_CHECK`, a 2D bug); (3) the parsed store follows AST-16 (destroyed unless the keep level
is `ALL`); (4) `odin3_elab_stage2`; (5) `odin3_design_set_top`; (6) the elaborated store and
its symbol table are handed to the design with `odin3_design_set_ast` when the keep level is
`ELABORATED` or `ALL`, else destroyed at the end of `read_verilog` (AST-16; SOURCE provenance
records keep their raw `loc` either way); (7) log `read_verilog: elaborated top 'T': M modules
(S specializations), P processes, C cells, N nets, t1 + t2 ms`.

`odin3_ast_elaborate(odin3_pass_ctx *ctx, odin3_ast *ast, const odin3_elab_options *opts)` (ABI)
runs steps 4–6 on a store an adapter built in the elaborated form: the slang seam. Stage 2 never
looks at a parsed store, and everything it needs is in EL-4, so a test can build an elaborated
store by hand and drive stage 2 alone (§14.1), exactly as slang will.

### 2.2 Options (EL-3)

```c
typedef struct odin3_elab_opts {
    uint32_t top;        /* strtab ID of "[lib.]mod" (CLI --top, else the project top); 0 = auto */
    odin3_loc top_at;    /* where the top was given (project line); 0 = CLI or none */
    bool nolatches;      /* Yosys -nolatches: unassigned paths of combinational blocks are x (EL-35) */
    bool nomem2reg;      /* never replace a memory by registers except where forced (EL-27) */
    bool auto_blackbox;  /* an undefined module becomes a black box, not unresolved_module (EL-11) */
    bool vtr_compat;     /* `initial x = <non-constant>` is `assign x = ...` (EL-40, §17 #85) */
} odin3_elab_opts;
```

`read_verilog` gains `--nolatches`, `--nomem2reg` and `--auto-blackbox` (all off by default) and
`--lib` (§3.1a: keep the modules as library units, elaborate nothing) and `--vtr-compat` (below);
`--top` is the 1D pass option (`odin3_pass_get_options().top`), which replaces the project top
(PHASE2 #9). Project parameters (`param top[.inst…].name value`, PRJ records) are read from the
design's project record by stage 1 (EL-8). The ABI mirror `odin3_elab_options` has the same
fields plus `uint8_t reserved_pad` (the byte after the three flags, no hidden padding) and
`uint32_t reserved[4]`, all zero.

The 2H exit harness passes `--nolatches` because Parmys does (`read_verilog -sv -nolatches`);
see Decisions for Peter, item 4.

**The VTR setting (`--vtr-compat`; Peter 2026-10-13, PHASE2 #24).** Nothing in the project record or
the passes said that a run targets the VTR flow: `flow` is a script path that is stored and never
executed (2B PRJ-16), `arch` names a tech library or XML, and `write_blif --vtr` only chooses the
output dialect, after elaboration. The setting that changes how Verilog is *read* is therefore an
option of `read_verilog`, beside `--nolatches`, which the VTR flow's scripts set: the Phase 2
scripts of `tools/opt-check/sp-splice` (Parmys's script in two halves), `elab-equiv --vtr-compat`
(and `elab-sweep --equiv --vtr-compat`, which the micro sweep uses), and the odin3 legs of
`tools/vtr-qor` (`read_verilog --nolatches --vtr-compat F; proc; ...; write_blif --vtr`). A
`.o3proj` that names a `flow` script puts the option on that script's `read_verilog`. The ABI
`odin3_elab_options` is unchanged (its `reserved` words stay zero): `odin3_ast_elaborate` has no
VTR setting. The option changes one behavior, EL-40's non-constant initial value.

### 2.3 Failure, lifetime and atomicity (EL-2, continued)

- **Stage 1** never touches the IR. Errors are collected (located, counted against the cap of
  EL-44); at the end of stage 1 any error destroys the elaborated store and `odin3_elab_run`
  returns `ODIN3_ERR_PARSE`. `ODIN3_ERR_NO_MEMORY` stops at once, frees the stage's scratch and
  the partial store, and returns.
- **Stage 2** creates IR as it goes. A user error is reported and the offending construct is
  skipped so the IR stays valid (the pass manager checks it after a failed pass, 1D); stage 2
  finishes the current module, continues with the next until the error cap, and returns
  `ODIN3_ERR_PARSE`. The modules it created stay in the design, the top is not set, and the
  failed pass stops the script. The IR has no module-delete primitive; a design after a failed
  `read_verilog` is fit only for destruction (documented in `odin3.h`; Decisions for Peter,
  item 10). Stage 2 checks every rule that can fail on user input before it attaches the object
  concerned (EL-28), so it never builds a multi-driven net, a cyclic hierarchy or an over-wide
  cell. "Valid" holds in the strong sense: every specialization's ports are created, with their
  port nodes connected to their nets, before any module body (EL-24), so an unfinished module has
  every `$port_out` pin connected (rule 11's E cannot fire) and at worst undriven nets, which
  `check` reports only as W (rule 4).
- The **elaborated store** owns the symbol table stage 2 builds (2A AST-14). A design that keeps
  it gets `odin3_symtab_remap` after each `compact` (PHASE2 #8 carry-over; 2D wires it into the
  `compact` pass and fixes the `docs/PASSES.md` row).

### 2.4 The elaborated form (EL-4) — normative

This is the contract between stage 1 (or the slang adapter) and stage 2. It refines 2A §4.6:
`odin3_ast_check` rule 7 enforces what 2A lists, and stage 2's binder (EL-5) checks the rest
before it emits anything. Kinds not listed keep their 2A meaning.

| Kind | In the elaborated form |
|---|---|
| `UNIT` | exactly one; name = the top specialization's name; tail = `MODULE`s, the top first, then every other specialization in the order a breadth-first walk of instances (in source order) first reaches it |
| `MODULE` | one per specialization (EL-9), name per EL-9; attributes kept (a specialization of a `read_verilog --lib` unit also carries the synthesized value-less `ATTR`s `odin3_lib` and, when its read had the flag, `odin3_lib_nolatches` / `odin3_lib_nomem2reg` / `odin3_lib_vtr_compat`: stage 2 uses them for that module instead of the run's options, v4.29, §3.1a); ports LIST of `PORT_DECL` (below); params LIST (absent when the source had none) of `PARAM_DECL[localparam]{IN_HEADER}` with the final values; items as below. **Black boxes** (v4.8): each `(* blackbox *)` definition reached (one `MODULE` for all its specializations, from its default one, v4.10) and each implicit black box (EL-11) is a ports-only `MODULE`: one `ATTR` `blackbox` without a value (synthesized, not the source's), named per EL-9 (an implicit one keeps its name), its ports LIST as for any `MODULE` but always written as `wire` ports (`NET` = 1 + `wire`, no `DT`: a `reg` port is a `wire`, an `integer` one `wire signed [31:0]`, v4.9; an implicit one's ports `inout`, at their EL-11 widths), the params LIST of a `(* blackbox *)` one with one bare `PARAM_DECL` per `parameter` holding its default value (none for an implicit one), and no items; `(* blackbox *)` ones in the `UNIT`'s breadth-first order where the default specialization is processed (queued after the first specialization with other values when no instance uses the defaults), then the **carriers** (v4.19: one more such `MODULE` per other parameter set an instance uses, named as a module's specialization, its `ATTR` `blackbox` valued with the definition's name as a `STRING`, its ports and `PARAM_DECL` values those of the set; none when the definition's name is a registered cell type), implicit ones after every specialization, in first-use order (their ports are known once every instance is seen); stage 2 registers a black-box cell type from the definition's (EL-11; the carriers give it its per-set widths), never an IR module |
| `PORT_DECL` | only in the port LIST, `IN_HEADER` set, exactly one `DECLARATOR`, in port order; direction sub; `DT` = `reg`, `integer` or none; `NET` field = 1 + the net kind for a net port (never 0: an untyped port is `wire`, EL-24); `SIGNED`; `RANGE` with `NUMBER` bounds or absent (scalar); merged from the non-ANSI declarations (EL-24): the `DECLARATOR`'s `E?` is the value of the port's own declaration or of the merged `wire w = e` / `reg q = v` (v4.15: a net port's `E?` is its net declaration assignment, which the binder queues as `CONTINUOUS` and stage 2 drives the port's net with; a `reg`/`integer` port's is its initial value, as for an ANSI `output reg q = 1'b1`; a value on both declarations is an error), and the `PORT_DECL`'s attributes are those of both declarations (the later declaration wins a name both give) |
| `PARAM_DECL` | sub `localparam` only; each `DECLARATOR` value is a `NUMBER` or a `STRING`: the `NUMBER` has the final value's bits and `SIGNED` flag, and its payload `width` is the value's width, or **0 (unsized, 2A §4.5) when the final value is still unsized** (EL-15, EL-16: Yosys keeps `is_unsized` on such a parameter, so stage 2 must extend it by repeating its top bit); `RANGE`, `SIGNED`, `DT` kept as declared (informational: the leaf already has the final type), except on a black-box `MODULE`, whose `PARAM_DECL`s are bare: no `RANGE`, `SIGNED` or `DT`, the value's `NUMBER` carrying the width and sign (v4.9) |
| `NET_DECL` | sub ∈ `wire tri wand wor triand trior uwire supply0 supply1`; no `STRENGTH`, no `DELAY`, no `VECTORED`/`SCALARED`; `RANGE`/unpacked `RANGE`s with `NUMBER` bounds; a declarator's `E?` is a net declaration assignment (N7) |
| `VAR_DECL` | sub `reg` or `integer`; a declarator's `E?` is a variable initialiser (N8: `initial name = e`, EL-40) |
| implicit nets | declared: one `NET_DECL[kind]` with one declarator per implicit net, `loc` = the first use, placed first among the `MODULE`'s items in first-use order; kind = the `` `default_nettype `` in effect (EL-25); marked by one value-less `ATTR` `implicit` (synthesized, v4.11) on the `NET_DECL`, from which the binder sets `ODIN3_SYM_F_IMPLICIT` (EL-5) |
| `GEN_BLOCK` | every selected generate block, named (the user's name or `genblk<n>`, EL-21), with a `NUMBER` index slot (32-bit signed) for loop iterations; nesting kept, since scopes name things (EL-21) |
| `CONT_ASSIGN` | no `STRENGTH`/`DELAY` slots |
| `ALWAYS` | sub `always` over `TIMING_STMT`[`EVENT_CONTROL`, S]; the event list is `STAR`, or all `EVENT_EXPR[posedge|negedge]`, or all `EVENT_EXPR[none]` (mixed lists are rejected, EL-34) |
| `TIMING_STMT` | only as the statement of an `ALWAYS`; elsewhere its `DELAY` was dropped with T11's warning and the statement kept |
| `BLOCKING_ASSIGN`, `NONBLOCKING_ASSIGN` | no intra-assignment control |
| `INITIAL` | kept (D2), delays dropped as above |
| `INSTANTIATION` | name = the specialization's name (EL-9; a `(* blackbox *)` specialization's too), the implicit black box's, or the registered cell type it instantiates (EL-11); one `INSTANCE` per instance (arrays expanded, element names `u[3]`); params LIST: for a module or a `(* blackbox *)` specialization, one named `CONNECTION` per `parameter` of the specialization in declaration order with its final value (2A §4.6); for a cell type, the values given (`#()`, `defparam`, project) one per parameter by EL-8's precedence, each named (an ordered one by the type's parameter at its position, v4.8), in the type's parameter order, converted to the parameter's kind; for an implicit black box, the overrides as given, values resolved, ordered ones with name 0 (2A §4.6: no definition names them) |
| `INSTANCE` | named; no `RANGE`; connections: for a module, a `(* blackbox *)` specialization or an implicit black box, one named `CONNECTION` per port of its `MODULE` in port order (`E` absent = unconnected; an implicit black box's ports are the union of every instance's, v4.8); for a cell type, as written |
| `GATE_DECL` | sub ∈ `and nand or nor xor xnor buf not bufif0 bufif1 notif0 notif1`; no `STRENGTH`/`DELAY`; gate arrays expanded; unnamed instances keep name 0 |
| `FUNCTION_DECL`, `TASK_DECL` | kept per specialization (and per generate scope that declares them); ranges with `NUMBER` bounds; ports LIST of `PORT_DECL`, one declarator each (non-ANSI inputs moved there in order; as for module ports, an untyped port's `NET` field is 1 + `wire`, never 0, v4.12); locals LIST (`VAR_DECL`s and, v4.13, `PARAM_DECL[localparam]` with the final value: a `parameter` there is a localparam, nothing overrides it; a read in a constant position folds in the function's or block's own scope, an expression read stays an `IDENT` that the binder gives the local's value; a local param hides a module parameter or genvar of the same name); body. A block, function or task local or port hides a genvar of the same name (v4.12) |
| `SEQ_BLOCK` | named, or unnamed with no locals; a block's locals LIST may hold `PARAM_DECL[localparam]` entries as a function's does (v4.13); an unnamed block with declarations is named `$unnamed_block$<k>` (k = 1, 2, … per module in the source order of the source block: every generate iteration of one block shares its k; Yosys's `-sv` name) |
| `FOR` | kept (stage 2 unrolls, EL-37) |
| `REPEAT` | count `NUMBER` ≥ 0 |
| `WHILE` | only inside `FUNCTION_DECL` bodies |
| `CASE` | kept; metacomment and `(* *)` attributes kept on it |
| `CALL{SYSTEM}` | only `$signed`, `$unsigned` (`$clog2` was folded to a `NUMBER`) |
| `TASK_CALL{SYSTEM}` | only `$readmemh`/`$readmemb` inside an `INITIAL` (S4); other system tasks were removed with S3's warning |
| `IDENT` | resolves to a declaration in scope (binder, EL-5); genvar reads are `NUMBER`s (32-bit signed) |
| `HIER_NAME` | only references into the module's own generate scopes and named blocks (G7), indices `NUMBER` |
| `SELECT` | `[m:l]` bounds and `+:`/`-:` widths are `NUMBER`s; bit indices and `+:` bases any expression; for an instance-array slice (EL-10) the object may be any expression (v4.8): it is read at its self-determined width, bit 0 its LSB, and the bounds are offsets into that value (Yosys's `extract` on the connection) |
| `REPLICATE` | count `NUMBER` ≥ 0 |
| `REAL` | absent (only delays held one) |
| `ATTR` | value absent, `NUMBER` or `STRING`; the `NUMBER` is unsized (width 0; Yosys keeps 32-bit signed), which is fine for a constant attribute (v4.12) |
| absent kinds | 2A §4.6's list (`DIRECTIVE UDP_DECL CONFIG_DECL SPECIFY_BLOCK DEFPARAM GENERATE GEN_FOR GEN_IF GEN_CASE GENVAR_DECL DELAY STRENGTH MINTYPMAX PROC_CONT_ASSIGN WAIT DISABLE EVENT_TRIGGER FOREVER PAR_BLOCK EVENT_DECL PORT_REF`) |

Expressions are otherwise kept as written: a `localparam` read stays an `IDENT` (the binder
gives it its value), constant subexpressions are not folded (stage 2 folds while it emits), and
locations are the parsed nodes' locations, so every diagnostic and provenance record of stage 2
points at source text. Every elaborated node that comes from one parsed node copies its
`{loc, end}`; nodes stage 1 creates (a genvar's `NUMBER`, a resolved `CONNECTION`) take the
range of the node that caused them.

The binder (EL-5) checks the rows that `odin3_ast_check` rule 7 does not, before anything reaches
the IR. A violation is a misuse of the seam, a located `elaborated form: <rule>` and
`ODIN3_ERR_INVALID_ARG` (counted against the error cap), where `<rule>` is (v4.9; the last three
are agent defaults): `'WHILE' outside a function`; `system function '$x' (only $signed and
$unsigned are allowed)`; `system task '$x' outside an initial block` (a `$readmemh`/`$readmemb`);
`edge and level events in one event list`; `'TIMING_STMT' outside an always`; `intra-assignment
control`; `a count that is not a NUMBER of 0 or more` (a `REPEAT` or `REPLICATE` count); `a select
bound or width that is not a NUMBER` (the object itself may be any expression, as the `SELECT` row
says); `instance 'u' does not connect every port of 'm' once, in port order` (a `MODULE` of the
store); `port 'p' without a net kind` (a module port that is neither a net nor a `reg`/`integer`
variable); `parameter 'P' is not a localparam with a NUMBER or STRING value`; `unnamed block 'b'
with declarations` (`b` its first declaration); `system task '$x' (only $readmemh and $readmemb are
allowed)`; `a range bound that is not a NUMBER`; `a generate index that is not a 32-bit NUMBER` (a
`GEN_BLOCK` index or a `HIER_NAME` index); and, from the fix round, `instance 'u' with a range`
(arrays are expanded); `net 'w' of kind 'tri0'` (`tri1`, `trireg`; a port's `NET` field too);
`switch-level or pull gate 'nmos'`; `'vectored' on a net declaration` (`'scalared'`).

### 2.5 Binding: the symbol table of an elaborated store (EL-5)

Stage 2 starts with `odin3_elab_bind(elab, &binding)`: one pre-order sweep that creates the 2A
symbol table (`odin3_symtab_create`) and a resolution array.

- **Scopes**: `DESIGN` → one `LIBRARY` (the library of the specialization's definition) →
  `MODULE` per `MODULE` node → `GENERATE` per `GEN_BLOCK` (index from the slot), `BLOCK` per
  named `SEQ_BLOCK`, `FUNCTION`, `TASK`. A black-box `MODULE` (EL-4, v4.8) has a scope holding
  its ports only, and an `INSTANTIATION` naming it resolves to it like any module's.
- **Symbols**: one per declarator (`NET`, `VAR`, `LOCALPARAM` with `odin3_symbol_set_value`),
  `INSTANCE`, `BLOCK` (named blocks and generate blocks), `FUNCTION`, `TASK`; `sub`, `flags`
  (`SIGNED`, `PORT`, `IMPLICIT`), `dir`, packed and unpacked dims (int32; a bound outside int32
  is EL-46's width error), `decl`/`declarator`.
- **Resolution**: `odin3_elab_binding { odin3_sym_id *sym_of; uint32_t n; odin3_u64map
  *scope_of; }` (`src/frontends/elab/elab_internal.h`), `sym_of` indexed by
  elaborated node ID (4 bytes per node), filled for every `IDENT`, `HIER_NAME`, `CALL`,
  `TASK_CALL` and `INSTANTIATION` by `odin3_symtab_lookup` from the node's scope outward. A
  name declared later in the same scope resolves (Yosys collects a scope's declarations first;
  IEEE requires declaration before use — divergence §17 #1).
- **Assignments**: `odin3_symbol_add_assign` for every `NET_ASSIGN` lvalue (CONTINUOUS) and
  every procedural lvalue outside function and task bodies (BLOCKING / NONBLOCKING), in source
  order, so T9, T10 and A4 report both places.
- **Checks** (each a located error, §9): an unresolved name ("'x' is not declared" — stage 1
  declares implicit nets, so a store from stage 1 never fails here; one from an adapter may); a
  duplicate declaration (M11, with both locations); a `HIER_NAME` that does not end in a
  generate scope or named block of the same module (E21); a `CALL` that names no function, a
  `TASK_CALL` that names no task.

Cost O(nodes) with one `u64map` probe per name. The binding is stage 2's (freed with it); the
symbol table belongs to the store.

Made exact by 2D Task 13 (v4.9, agent defaults):

- **Libraries and modules.** A `MODULE`'s library is its `FILE` buffer's (2A §3.1; `work` when the
  buffer has none). The `MODULE` symbols live in their `LIBRARY` scopes, so no name of an
  expression reaches them (a lookup stops at the `MODULE` scope). An `INSTANTIATION` resolves by
  name to the store's `MODULE` of that name, in any library (names are unique in the `UNIT`, point
  (q); a second `MODULE` of one name is M11), or to nothing: a registered cell type, which stage 2
  finds in the registry.
- **Symbols.** A module port is a `NET` (its `NET` field) or a `VAR` (`DT` `reg`/`integer`); a
  function or task port is a `VAR`. An `integer` is `signed [31:0]`. A localparam is `SIGNED` as its
  value's `NUMBER`, and a `STRING` value is stored as its bits (L10). A function's scope holds a
  `VAR` named like it, of its return type: its result, which a `CALL` of the function from inside
  it skips. Generate-loop iterations share the `BLOCK` symbol of the first. A black box's
  `PARAM_DECL`s are checked but not bound, and its items (none in the EL-4 form) are not walked.
- **Resolution array.** Besides references, every declaring node (`DECLARATOR`, `INSTANCE`,
  `MODULE`, `GEN_BLOCK`, named `SEQ_BLOCK`, `FUNCTION_DECL`, `TASK_DECL`) maps to the symbol it
  declares; `odin3_elab_scope_of` maps each scope-opening node to its scope.
- **Assignments.** A net declaration assignment (N7) is a `CONTINUOUS` assignment of its
  `DECLARATOR`; an lvalue's names are found through selects and concatenations, one record per
  name and assigning node. Task output arguments are not recorded here (Task 23).
- **References into scopes.** The first component is found from the current scope outward to the
  module (a `BLOCK` symbol), the others in the reached scope (EL-22). A component naming no block
  is E21; a block without that index is `'g[5].t' does not exist (generate block 'g[5]' was not
  elaborated)`; a last component with an index is E21; a last name not declared in the reached
  scope is `'blk.x' is not declared`. A `CALL` or `TASK_CALL` naming nothing is `'f' is not
  declared`.
- **Bounds.** A range bound outside int32 is EL-46's width error at the bound (`width |b| + 1`), a
  packed range wider than the cap the width error at the `RANGE`, an x or z bound `range bound has
  x or z bits`. Unpacked dimensions have no width cap here (memory sizes are EL-27's).
- **Names that are not values.** An `IDENT` or `HIER_NAME` that resolves to an `INSTANCE`,
  `FUNCTION` or `TASK` is `'u' is not a net, variable or parameter` (Yosys 0.55: "Failed to detect
  width for identifier"). One that resolves to a `BLOCK` binds to it: Yosys 0.55 makes a generate
  block's name an implicit net (`Identifier '\g' is implicitly declared`), which Task 18 (or
  Task 15) implements or records as a divergence.
- **Implicit nets** (v4.11). The `IMPLICIT` flag is set on the declarators of a `NET_DECL` carrying
  the value-less `ATTR` `implicit` (EL-4).

## 3. Hierarchy

### 3.1 Module lookup (EL-6)

Definitions are the `MODULE`, `UDP_DECL` and `CONFIG_DECL` nodes of the parsed store. **Design
units** are those in `UNIT`s `[0, library_units_from)` (all `UNIT`s when it is 0, 2C §4.2
step 3), **library units** the rest. A unit's library is the `library` of its `FILE` buffer
(2A §3.1; `work` by default). A name `N` instantiated in a module of library `L` resolves to,
in order:

1. a design-unit `MODULE` named `N` in `L`;
2. a design-unit `MODULE` `N` in another library, the first in project order (when several
   libraries define it, an info line names the one taken: `module 'N' found in libraries 'A',
   'B'; using 'A.N'`);
3. a library-unit `MODULE` `N`, the first in store order (2C: first file wins); a unit of a
   `read_verilog --lib` read is one (§3.1a), and when a hard or black-box cell type of the same name
   is registered, §3.1a's parameter rule decides between item 3 and item 5;
4. a `UDP_DECL` `N` → Q7's error, at the `UDP_DECL` (§9.1: at the definition, whether or not
   it is instantiated; a library unit's UDP when an instance resolves to it), not at the instance;
5. a registered cell type `N` of granularity `hard` or `blackbox` (a tech library read earlier,
   e.g. `single_port_ram`; a model a BLIF read declared) → an instance of that type (EL-11);
6. with `--auto-blackbox`, an implicit black box (EL-11);
7. otherwise `unresolved_module` (DESIGN §4.0), located at the `INSTANTIATION`'s module name.

Names beginning with `$` never resolve to built-in cell types (`$add` cannot be instantiated
from Verilog; Yosys allows it only under `-icells`). A `MODULE` carrying `(* blackbox *)` is a
definition of kind black box (EL-11).

**Names already taken in the design** (IR-11: a module registers a cell type under its own
name, and `odin3_module_create` refuses a name that is already a cell type). Stage 1 checks
every definition it is about to specialize, before stage 2 creates anything, so the IR call can
never fail with `INVALID_ARG` on user input:

- a design or `-v`/`-y` library `MODULE` (a `--lib` unit is exempt: §3.1a; not a `(* blackbox *)` one, which IR-7b reuses) whose name is a
  registered cell type of granularity `hard` or `blackbox` — a tech library's `adder`,
  `multiply`, `single_port_ram`, or a model a BLIF read declared — is the located error, at the
  `MODULE`'s keyword (the parsed store records no location for a name; 2C §4.2 step 4 locates
  `duplicate_module` there too): `module 'adder' has the name of cell type 'adder' registered by an earlier
  read (hard cell); rename the module` (`(black box)` for a declared model). The design unit
  does not shadow the cell type: an instance `adder u(…)` in the same design would otherwise mean
  two things. Parmys fails the same way ("Re-definition of module" against its
  `vtr_primitives.v`);
- a `MODULE` whose name is a module created by an earlier `read_verilog` run of the same design
  (a second read in one script) is `duplicate_module: module 'm' is already defined by an
  earlier read (run N)` at the `MODULE`'s keyword (2C's kind, F27). The check runs when the
  definition is reached from the top (the definitions stage 1 is about to specialize); a module
  named after a built-in cell type is `module '$add' has the name of built-in cell type '$add';
  rename the module` (v4.5).

### 3.1a Library reads: `read_verilog --lib` (2D-7b)

Two problems have one cause: a file of modules cannot be kept for a later design read. Parmys reads
`vtr_primitives.v` (the n-bit soft `adder`, `multiply`, the RAM wrappers) as real modules, with
`-nomem2reg` for that read only, and the design after it; and the hard `adder` of a tech library is
1-bit, so `adder #(W)` has no module to mean. This is **not** Yosys `read_verilog -lib` (which makes
black boxes): a library unit has a body and is fully elaborated where it is instantiated.

- **The read.** `read_verilog --lib [--nolatches] [--nomem2reg] FILE...` preprocesses and parses the
  files as a design read does (syntax errors are reported at this read), makes no IR, elaborates
  nothing (so a file of several uninstantiated modules is not `ambiguous_top`), and keeps its parsed
  store in the design (as `--parse-only` does) marked with the read's flags. `--auto-blackbox` is
  refused with `--lib` (it is an option of the design read). `--top` is not looked at.
- **Later reads.** A `read_verilog` (including another `--lib`) first reads again the files of every
  kept library store, in read order, after the design files and before the `-v`/`-y` files, as
  **library `UNIT`s** (EL-6 item 3: never a top candidate, shadowed by a design unit of the same name,
  the first definition wins, an already read file is skipped). Each such `UNIT` carries the
  `ODIN3_UNIT_*` flags of its read (`odin3_ast_unit_flags`). The store keeps each design file's
  **preprocessed stream** (`odin3_ast_keep_stream`), so the files are parsed again from exactly the
  text the `--lib` read's own defines, macros (a macro an earlier file of that read defined included)
  and include directories produced, never under the later read's (a `` `ifdef `` in a library file
  is decided once, by the read that kept it; PR 2D-7b review I1); a kept unit costs only that parse.
  **Limitation (re-review):** only a `--lib` read's design files are kept as streams; units it
  resolves through its own `-v`/`-y` library files are not, and are preprocessed again under the
  later read's macros. Pass such files to `--lib` directly when their `` `ifdef ``s matter.
- **Per-unit flags.** The flags of the *supplying* read replace, never add to, the design read's:
  a module defined by a `--lib` read elaborates under that read's `--nolatches`, `--nomem2reg` and `--vtr-compat`
  whatever the reading design says, and the design read's flags do not reach it. Stage 1 writes the
  elaborated `MODULE` of such a unit with the value-less attributes `odin3_lib` (the marker) and
  `odin3_lib_nolatches` / `odin3_lib_nomem2reg` / `odin3_lib_vtr_compat` when set; stage 2 (`odin3_elab2_enter`) reads them
  and uses them for that module in place of the run's options. `--auto-blackbox` is a design-read
  option and stays the run's. The attributes are not copied to the IR.
- **Unused units.** A library unit nothing instantiates is dropped without a message (the "not
  instantiated under the top" info line lists design units only).
- **Precedence against a hard cell.** A `--lib` unit may carry the name of a registered hard or
  black-box cell type (`adder`, `multiply`, `single_port_ram`): it is exempt from the "has the name
  of cell type" error of EL-6, because a design that reads both a tech library and
  `vtr_primitives.v` is exactly Parmys's situation. Which one an instance means is never silent, and
  is decided by the instance's `#()`:
  1. every value fits a parameter of the cell (no `#()`; ordered values no more than the cell's
     parameters; every named parameter one of the cell's): the **hard cell**, with one info line per
     name (`instance of 'adder' uses the hard cell 'adder': its parameters fit the cell, so the
     library unit of that name (read with --lib) is not used`). The 1-bit `adder u(...)` therefore
     stays the hard block the BLIF goldens instantiate;
  2. otherwise (`adder #(4)`, `adder #(.WIDTH(4))`) the **library unit**, with the warning `instance of
     'adder' uses the library unit 'adder' (read with --lib), not the hard cell of that name: its
     parameters do not fit the cell`, once per `INSTANTIATION`.
  The decision reads only the parameter list (not port names or widths). A design that wants one
  meaning everywhere reads no `--lib` file defining the name, or no tech library defining the cell.
  The unit's modules take the `$2` suffix on a clash with the cell type (EL-9), so
  `odin3_module_create` never sees the name; a shadowing unit cannot be the `--top` (`unknown_top:
  'adder' is a library unit with the name of a registered cell type, so it cannot be the top;
  rename it`).
- **Decided here (agent defaults).** The flag spelling `--lib`; precedence by parameter fit; the
  kept store is re-parsed rather than copied (the elaborated form has no way to merge two stores);
  nothing is elaborated before a design instantiates it (so an error inside a unused unit is never
  reported, unlike Parmys, which simplifies every module: §17 #2).

### 3.2 Top selection (EL-7)

The top is chosen before anything is elaborated, from the parsed store:

- **Given** (`--top`, else the project's `top`, `[lib.]mod`): the named design-unit or
  library-unit `MODULE`; not found → `unknown_top` (at `top_at`, unlocated for `--top`); found in
  several libraries without a `lib.` prefix → `ambiguous_top` listing `lib.mod` for each; a
  `(* blackbox *)` module → `unknown_top: 'm' is a black box ((* blackbox *)), not a module to
  elaborate` (v4.5: a black box is never a `MODULE` of the elaborated form, EL-4).
- **Automatic** (DESIGN §4.0, kept): the candidates are the design-unit `MODULE`s that are not
  black boxes and that no design-unit or library-unit `MODULE` instantiates, by name, anywhere
  in its body (every generate branch counts; the parsed form is all there is before
  elaboration — so a module instantiated only in a branch that is never selected is not a
  candidate here, while Yosys and IEEE, which look at the elaborated design, would count it:
  divergence §17 #20). One candidate → the top, with the info line `top module 'work.T' chosen: the
  only module no other module instantiates`. None → `no_top` (unlocated: `no top given and every
  module is instantiated by another`, or `no top given and no module is defined`). Several →
  `ambiguous_top` (unlocated), naming each candidate as `lib.mod` in definition order.

**Parmys differs.** `hierarchy -auto-top` scores every module by the depth of hierarchy under
it and takes the deepest, ties going to the module defined **last** (hashlib iterates in
reverse insertion order; verified: on micro `keywords/module_endmodule/multiple_topmodules.v`
Parmys picks `simple_op2`, on `common/adder.v` it picks `add_module`). The project corpus
requires DESIGN's rule (`neg_ambiguous_top`: two depth-0 candidates → `ambiguous_top`;
`neg_no_top`: a mutual instantiation → `no_top`), and the two micros are the same situation as
`neg_ambiguous_top`, so no single rule satisfies both. 2D keeps DESIGN's rule; the 2H micro
harness passes `--top` set to the first `.model` of the Parmys golden (Decisions for Peter,
item 6). Only modules reachable from the top are elaborated (EL-9); an info line lists the
others (`not instantiated under the top, not elaborated: 'a', 'b'`). Parmys parses and
simplifies every module at `read_verilog` time, so an elaboration error in an unused module
fails Parmys and not 2D (divergence §17 #2).

### 3.3 Parameter overrides and precedence (EL-8)

A `parameter` of an instance's module takes its value from, highest first:

1. a **project parameter** (`param top.W v` for the top, `param top.u1.u2.W v` for an instance
   path below it; DESIGN §4.0): the user's explicit override;
2. a **named** `#(.W(e))` of the instance;
3. a **`defparam`** that targets it — the earliest in stream order when several do;
4. an **ordered** `#(e1, e2, …)` value (the k-th value to the k-th `parameter` of the module,
   header parameters first, then body parameters, in declaration order);
5. the declared default.

Items 2–4 are Yosys's order, verified on Yosys 0.55: with `#(.W(5))` and `defparam u1.W = 7`,
W = 5; with `defparam u3.W = 8; defparam u3.W = 9;`, W = 8; Yosys rejects an ordered `#()`
together with a `defparam` on the same instance ("Can't find object for defparam `$1`"), where 2D
applies the order above. IEEE 1364-2005 §12.2 gives a `defparam` precedence over any `#()` and
lets the last `defparam` win; 2D follows Yosys because the exit bar is Parmys (Decisions for
Peter, item 5), and warns when a `defparam` loses (`defparam of 'u1.W' is overridden by the
named parameter assignment of 'u1'`).

Rules:

- **Values** are evaluated as constant expressions in the scope that wrote them (the
  instantiating module's specialization and generate scope for `#()` and `defparam`), then
  converted to the target's declared type (EL-16). A `localparam` cannot be overridden: "'P' is
  a localparam of module 'm' and cannot be overridden". An unknown name: "module 'm' has no
  parameter 'P'". More ordered values than parameters: "too many parameter values for module
  'm' (N given, M parameters)". Mixing ordered and named values in one `#()`: "parameter
  assignment of instance 'u' mixes ordered and named values".
- **`defparam` targets** are downward paths only: `a.b.W` from the scope that holds the
  `defparam`, whose first component is an instance or generate scope (with constant indices,
  `g[1].u.W`) of the same module. The path is resolved after the module's generate expansion
  (EL-9 pass E). The last component is the parameter, the one before it the target instance; the
  components between are pushed down: the defparam becomes a pending override `("b.W", v)` of
  instance `a`'s specialization, applied there in turn (Yosys pushes `PARASET`s the same way).
  A path that does not reach a `parameter` of an instance below: "defparam target 'a.b.W' does
  not name a parameter of an instance below this module". Upward or absolute paths (`top.u.W`
  from inside `top`'s children), and a `defparam` naming a parameter of its own module, are that
  error too (Yosys does not support them either).
- **Project parameters** are pending overrides of the top: `param top.W` overrides the top's
  `W`; `param top.u_wide.W` is the pending `("u_wide.W", v)` of the top, pushed down like a
  `defparam`. A path that never reaches a parameter is an error at the project line
  (`param 'top.u_wide.X' does not name a parameter`). Values are numbers (`16`, `-3`, `8'hFF`,
  through `odin3_ast_number_parse`, sized as a literal of that text and then sized like every
  override, its sign kept, v4.7) or strings.
- **Paths that end at a cell-type instance** (a `defparam` or project `param` whose instance is a
  hard or black-box cell, EL-11, such as `defparam ram.ADDR_WIDTH = 10` on a `single_port_ram`)
  set that node's parameter, converted to the type's parameter kind (INT: the value as a signed
  64-bit integer, an error when it does not fit or has x/z bits; BITS: the bits; STRING: the
  string); a name the type does not declare is "cell type 't' has no parameter 'P'". Nothing is
  pushed further down: a cell type has no hierarchy.

### 3.4 Specialization key and naming (EL-9)

A **specialization** is a definition plus the resolved values of all its `parameter`s plus its
pending downward overrides. Stage 1 computes the resolved values by evaluating the definition's
parameter declarations with the instance's overrides (EL-16) — a parameter's type may depend on
another parameter, so the key is the resolved vector, not the override list: `#(.W(8))` and
`#(.W(32'sd8))` on `parameter integer W` share a specialization, while on an untyped `W` they
differ (8 is 32-bit signed, `4'd8` 4-bit unsigned: different expressions inside). Key =
(definition, for each `parameter` in order: width, signedness, string flag, bits; the pending
list sorted by path, each entry as (path, source, value)). The source (`#()`, `defparam` or
project line) is part of the key because at its target a pending entry competes with the child's
own `#()` by EL-8's precedence, so two equal values from different sources can give different
results; an equal-valued pending from another source is another specialization, whose name takes
the `$2` suffix (`mid$u_leaf.W=4` and `mid$u_leaf.W=4$2`; v4.10). Hash: `odin3_hash_bytes`
(`util/hash.h`) over the key bytes; equality is exact. Two instances with equal keys share one
specialization and one IR module.

**Names** (IR-7): the definition's name when every `parameter` has its default value and nothing
is pending (Yosys derives a `$paramod` name as soon as any override is given, even one equal to
the default; 2D does not); otherwise `base$P=v$Q=v…` for each parameter whose value differs from
its default, in declaration order, then `$<path>=v` for each pending entry. A value prints as a
plain decimal when it is a 32-bit signed value without x/z (`W=8`, `W=-3`); otherwise
`<width>'<s>b<bits>` (`INIT=4'b10x0`, `S=8'sb11110000`); a string as `"text"` with `"` and `\`
escaped. A name longer than `ODIN3_ELAB_MAX_MODULE_NAME` (128 bytes) becomes
`base$h<16 hex digits of the key hash>`. The top's specialization keeps its definition's name
whatever its parameters and pending overrides, as Yosys `chparam` and `hierarchy -chparam` do
(verified on 0.55); VTR and BLIF `.model` expect it, and Task 17's `odin3_design_set_top` takes it.
A specialization of the top's definition with default values can then only come from recursion; it
takes `top$2` (v4.7). Every specialization of a **`(* blackbox *)` definition** takes the
definition's name, whatever its parameters (v4.10; Yosys 0.55 keeps `module \bb` and gives the
cell `parameter \W 8`, never a `$paramod`): its instances are told apart by their parameter
values, not by name (EL-11). A name already taken by another definition or
specialization, or by a registered cell type of the design (`odin3_celltype_find`; EL-6's
"names already taken"), gets `$2`, `$3`, … appended (checked against every definition name of
the store, every specialization made so far and the design's cell-type table), so names are
deterministic for a given input and `odin3_module_create` never sees a taken name.

**Algorithm** (stage 1, worklist, no recursion):

```
queue ← [top specialization]         (top: project params as its overrides)
while queue not empty:
    s ← pop front
    pass E (expand): walk s's definition with a fresh environment (EL-12): parameters, generate
        (EL-19, EL-20), declarations; record every generate decision (chosen branch, each loop
        value) in s's decision log; collect instances (scope path, name, module name, override
        values) and defparams (scope, path, value), all values evaluated where written
    resolve defparams against the expanded instance tree (EL-8)
    for each instance i: resolve its module (EL-6); for a module definition compute the
        child key (resolved parameters + pending); find or create the specialization c
        (create: depth(c) = depth(s) + 1, push back); record edge s → c
    pass W (write): walk again replaying the decision log, emitting the elaborated nodes
        bottom-up (AST-5) through the 2A builders; INSTANTIATION names are now known
check the specialization graph is acyclic (iterative DFS); emit the UNIT
```

Two passes are needed because an `INSTANTIATION` node must carry its specialization's name, and
a `defparam` later in the module (or in a generate block) can change it; the builders are
append-only, so nothing is patched. Pass W replays the log instead of re-evaluating generate
conditions. A specialization whose depth exceeds `ODIN3_ELAB_MAX_HIER_DEPTH` (1024) stops the
run ("instance hierarchy deeper than 1024 at 'u'; recursive instantiation of 'm'?"); a cycle in
the graph (a module that instantiates itself with the same parameters, `neg_no_top` under
`--top ping`) is "recursive instantiation of 'ping' (through 'pong')". Recursive instantiation
whose parameters change and stop (a tree built by a generate `if` on a shrinking `N`) is legal
and makes one specialization per level.

### 3.5 Instances and connections (EL-10)

- **Connections to a module** are resolved by stage 1 to one named `CONNECTION` per port in port
  order: ordered connections map to ports by position (an empty header position, EL-24, takes a
  position but no port: a connection to it is dropped with a warning `connection 3 of instance
  'u' goes to an unnamed port position and is ignored`); named connections by name. Errors: I2
  mixing ("instance 'u' mixes ordered and named connections"); an unknown name ("module 'm' has
  no port 'p'"); a port connected twice ("port 'p' of instance 'u' is connected twice"); more
  ordered connections than positions ("instance 'u' of 'm' has N connections but 'm' has M
  ports"). A missing or empty connection (`.p()`, `,,`, or a port not named) leaves the
  `CONNECTION` without an expression: an unconnected input is driven by x (`$_CONSTX_`), an
  unconnected output or inout gets fresh nets (EL-28). `u ()` on a port-less module is legal
  (M5).
- **Instance arrays** (I6, Q5): `m u[hi:lo] (…)` expands in stage 1 to instances `u[hi]` …
  `u[lo]` (Yosys's names; elements in declaration order of the range). Each connection
  expression of width W, against a port of width P, with N elements: W = P → every element gets
  the whole expression; W = N × P → element k gets bits `[(k − min(hi,lo)) × P +: P]` of it
  (`SELECT` nodes in the elaborated form); anything else → "connection of 'p' to instance
  array 'u' is W bits; expected P or N×P". The lowest index gets the low bits whatever the
  range's direction: Yosys normalizes the range (`simplify.cc` 2055–2058), names element *i*
  `[min + i]` and gives it `extract(P × i, P)` (`hierarchy.cc` 595); verified on 0.55, `ub[0:3]`
  on `bus[7:0]` gives `ub[0]` `bus[1:0]` (§17 #29 for IEEE). Gate arrays slice terminals the same
  way with P = 1 ("connection K of instance array 'g' is W bits; expected 1 or N"; §17 #27).
- **Port expressions** (I3): an input connection is any expression; an output or inout
  connection must be a net lvalue (identifier, constant selects, concatenation of those);
  anything else is "output port 'p' of instance 'u' must be connected to a net" (and a constant
  there is "output port 'p' of instance 'u' is connected to a constant", Yosys's error).
- **Widths** (I4), checked in stage 2 with Yosys's `hierarchy` rules: an input connection is
  generated at its self-determined width and sign; narrower than the port it is extended (sign
  extension when the expression is signed, else zero), wider it is truncated; an output
  connection narrower than the port leaves the extra port bits on fresh nets, wider has its extra
  high bits driven by the extension of the port's value (sign when the port is declared signed in
  the child, else zero; EL-28). A cell type or a black box has no port signedness in the IR
  (IR-7b), so its wider output connections are zero-extended, where Yosys would sign-extend a
  `(* blackbox *)` module's `output signed` port (divergence §17 #23). Every resize warns, except a constant input:
  `port 'p' of instance 'u' (module 'm') is P bits; the connection of W bits is extended` /
  `… is truncated`.

### 3.6 Black boxes, hard cells and undefined modules (EL-11)

- **`(* blackbox *)` modules** in the sources (Yosys's attribute): the body is ignored (an info
  line if it is not empty). A black box keeps its **base name** whatever its parameters (v4.10,
  Yosys 0.55; EL-9): stage 1 still specializes it per resolved parameter vector and checks each
  instance's connections against that specialization's ports (EL-10; Yosys's `hierarchy` derives
  the same per-parameter port widths for the check and then drops the derived module), but writes
  **one** ports-only black-box `MODULE` per definition, named after it, with the ports and
  `PARAM_DECL` values of its **default** specialization (Yosys's `module \bb`), queued once when no
  instance uses the defaults; every `INSTANTIATION` of the definition is named after it and carries
  its final values in its params LIST (EL-4: the cell's parameters). Stage 2 (Task 19) registers one
  black-box cell type per such `MODULE` (IR-7b; granularity `blackbox`, the definition's name, its
  ports in port order) instead of creating an IR module; each cell takes its `INSTANTIATION`'s
  values as cell parameters. Its declared port widths are the default specialization's, and each
  port that differs in another parameter set the instances use has a width expression over the
  cell's parameter values, so every cell has the exact widths of its own set (v4.19, §17 #46):
  stage 1 writes the other sets as carrier `MODULE`s for it (EL-4). `write_blif --vtr` (3E) therefore writes
  `.subckt <base name>`, which an architecture model of that name matches; no `$`-suffixed black-box
  name reaches a writer. When a cell type of that name is already registered (a tech library's
  `single_port_ram`), IR-7b's compatibility check by port name applies and the registered type is
  reused whatever the parameters; a mismatch is a located error naming the port.
- **Registered cell types** (EL-6 item 5): parameters by name must be parameters of the type
  ("cell type 't' has no parameter 'P'"); ordered values map to the type's parameters in its
  declaration order; parameters not given are inferred from the connections' widths
  (`odin3_celltype_infer_params`, IR-7b 1G) and otherwise keep their defaults. Connections by
  name match the type's ports; ordered ones go in the type's port order. Each port's connection
  is resized to the port's width as above.
- **Undefined modules** are `unresolved_module` (DESIGN §4.0; Parmys's `hierarchy -check` rejects
  them too). With `--auto-blackbox`, an undefined module instead becomes an implicit black box
  per distinct name: every instance must use named connections ("implicit black box 'm' needs
  named connections"), its ports are the union of the names used (every instantiation of the name
  in the sources, in source order), the first elaborated connection with an expression fixes each
  port's width (from an instance array of N elements, a width W that N divides fixes W / N, the
  element's slice, else W: EL-10's two forms, read with the slice preferred; later instances must
  agree, an array's connection being one element's width or N times it; 1 bit when no connection
  gives one; v4.10), all ports are
  `inout`, an info line names it, and it is written as a ports-only black-box `MODULE` (EL-4,
  v4.8).
  This is coverage row I7's black box (B §2.23.1) without guessing directions.
- Cell-type instances keep their parameter values as node parameters; module instances keep the
  overridden values as node attributes `parameter:<NAME>` (BITS or STRING; IR-7's "source-level
  parameter values as attributes").

## 4. Constant evaluation

### 4.1 Values and environments (EL-12)

```c
/* A 4-state constant: bits in VPI aval/bval planes (00 = 0, 10 = 1, 01 = z, 11 = x), LSB first. */
typedef struct odin3_cval {
    uint32_t width;           /* 1 .. ODIN3_ELAB_MAX_WIDTH */
    uint8_t flags;            /* ODIN3_CVAL_SIGNED | ODIN3_CVAL_UNSIZED | ODIN3_CVAL_STRING */
    uint64_t *aval, *bval;    /* ceil(width / 64) words each, in the evaluation arena */
} odin3_cval;
```

Words above `width` are kept zero. `UNSIZED` marks a value that came from an unsized literal and
has not been through an operator (EL-15); `STRING` a string literal or a parameter that holds one
(L10: printing and the `"…"` naming of EL-9; arithmetic ignores it). Conversions:
`odin3_cval_from_literal` (an `odin3_ast_literal`), `odin3_cval_to_value` (IR `odin3_value`
BITS, one byte per bit, IR-10) and `odin3_cval_to_number` (a `NUMBER` payload for the elaborated
store through `odin3_ast_number_new`). Values live in a per-evaluation arena that is reset after
each top-level evaluation; results that persist (parameter values, generate indices) are copied
into the run arena.

An **environment** (`odin3_elab_env`) answers name lookups during constant evaluation: in stage 1
it is the specialization's scope stack over the parsed store (parameters, localparams, genvars
with their current value, functions, and declared nets/variables that are *not* constants); in
stage 2 it is the binding (EL-5) plus the loop and call frames of the process generator. The
evaluator reaches names only through `odin3_ceval_host { lookup, function }` callbacks, so one
evaluator serves both stages and both stores.

### 4.2 Expression typing: widths and signedness (EL-13)

Widths and signedness follow IEEE 1364-2005 §5.4–5.5 (Table 5-22) exactly as Yosys implements
them (`AstNode::detectSignWidthWorker`, `genRTLIL`). Every expression is typed in two sweeps over
its subtree with explicit stacks (no recursion, CLAUDE.md):

1. **Self-determined** (post-order): each node's self width `w` and sign `s` from the table.
2. **Context** (pre-order): the root gets `(max(w, ctx_w), s && ctx_s)` where the context is the
   assignment target (width of the lvalue; Yosys's `genWidthRTLIL(lhs_width, true)`) or none;
   each *context-determined* operand inherits its parent's final `(width, sign)`; each
   *self-determined* operand keeps its own. A node's final width is `max(self, inherited)`; its
   final sign is the inherited sign (which is false whenever any context-determined operand in the
   same context is unsigned).

| Expression | Self width | Self sign | Operands |
|---|---|---|---|
| sized literal | its width | its `'s` flag | — |
| unsized literal | its stored width (2A §4.5: 32, or more when written) | signed for decimals and `'s` | — |
| string | 8 × length (8 for `""`) | unsigned | — |
| parameter / localparam | its value's width (or range) | its signedness; `UNSIZED` carried when the value is still unsized (EL-16), so it extends like an unsized literal | — |
| genvar (a `NUMBER` after stage 1) | 32 | signed | — |
| net / variable `x` | declared width (`integer`: 32) | declared `signed` (`integer`: signed) | — |
| `x[i]`, `x[m:l]`, `x[b+:w]`, `x[b-:w]`, memory word `m[i]` | 1, `\|m−l\|+1`, `w`, `w`, word width | **unsigned** (a select is unsigned even of a signed object); a memory word: the memory's sign | index, bounds, base: self-determined |
| `{a, b, …}` | sum | unsigned | self-determined |
| `{n{a, …}}` | n × sum | unsigned | self-determined; `n` constant |
| `-a`, `+a`, `~a` | `L(a)` | `s(a)` | context |
| `a + - * / % & \| ^ ~^ b` | `max(L(a), L(b))` | `s(a) && s(b)` | context |
| `a ** b` | `L(a)` | `s(a)` | `a` context; `b` self-determined with its own sign |
| `a << >> <<< >>> b` | `L(a)` | `s(a)` | `a` context; `b` self-determined, unsigned |
| `a == != === !== < <= > >= b` | 1 | unsigned | `a`, `b` sized together: `max(L(a), L(b))`, signed iff both signed |
| `a && \|\| b`, `!a`, `&a ~&a \|a ~\|a ^a ~^a` | 1 | unsigned | self-determined |
| `c ? a : b` | `max(L(a), L(b))` | `s(a) && s(b)` | `c` self-determined; `a`, `b` context |
| `$signed(a)`, `$unsigned(a)` | `L(a)` | true, false | self-determined |
| function call `f(…)` | the return width (1 bit by default, 32 for `integer`) | the declared `signed`/`integer` | arguments: context = the formal's width (assignment to the formal) |

A 1-bit-or-wider result of a comparison, reduction or logical operator is zero-extended to its
context. A context wider than an operand extends it **before** the operation: sign extension
when the operation is signed (all context operands signed), zero extension otherwise. An unsized
literal widened to its context repeats its top bit even when unsigned (Yosys
`bitsAsUnsizedConst`, 2A §4.5 amendment): `'hFFFF_FFFF` in a 64-bit context is all ones. A
concatenation operand that is unsized (E12: illegal in IEEE 1364-2005, a located error in
Odin II) is accepted as Yosys and Parmys accept it — silently there (verified on 0.55: `{a, 'h1}`
is `{a, 32'h00000001}`) — at its stored width (32 bits for a short literal, 2A §4.5), with the
warning "unsized constant in a concatenation is taken as N bits" (the exit bar is Parmys,
PHASE2 #4; divergence §17 #21). A width above `ODIN3_ELAB_MAX_WIDTH` (2^20) anywhere is an error
(EL-46). The typing pass costs O(nodes) of the expression.

### 4.3 Operator semantics on constants (EL-14)

Constant results equal Yosys's `RTLIL::const_*` functions (`kernel/calc.cc`), which are also the
semantics of the word cells (simlib, 1E), so folding in 2D and evaluation in a cell agree:

- **Arithmetic** `+ - * / % **` and unary `-`: two's complement at the result width; any x or z
  bit in an operand makes the whole result x. `/` and `%` by zero are all x. Signed `/` truncates
  toward zero, `%` takes the dividend's sign. `**`: `0 ** negative` is x, `0 ** positive` is 0,
  a negative exponent gives 0 except for bases 1 and −1 (`(−1) ** b` is ±1 by parity), and a
  positive exponent is computed by square-and-multiply modulo 2^width (`const_pow`); the
  exponent's own signedness decides whether it is negative. As in `const_pow`, the two zero-base
  cases are decided **before** the x/z check, with x and z bits reading as 0 (only a known-1 top
  bit makes a signed value negative): `8'b0000000x ** 3`, `8'bxxxxxxxx ** 3` and
  `0 ** 8'b0000001x` are 0, and `0 ** 8'sb1x00000x` (negative) is x; otherwise an x or z bit in
  either operand makes the result x (verified on 0.55, v4.1).
- **Bitwise** `& | ^ ~^ ~`: per bit with the IEEE 4-state tables (`0 & x = 0`, `1 | x = 1`, z
  reads as x).
- **Reductions** and **logical** `! && ||`: an operand is true if any bit is 1, false if all bits
  are 0, else x (`0 && x = 0`, `1 || x = 1`).
- **Comparisons** `== !=` (`const_eq`/`const_ne`): 0 (`!=`: 1) when any bit pair is a known 0
  against a known 1, **even beside an x or z bit**; otherwise x when any compared bit is x or z;
  else 1 (`!=`: 0). Verified on 0.55 (v4.1): `4'b1x01 == 4'b1000` is 0, `4'b1x00 == 4'b1000` is x.
  `< <= > >=`: x when any compared bit is x or z; else 0/1. `=== !==`: bit-exact 4-state
  comparison (constants only, EL-30 says what happens with a signal).
- **Shifts**: `<< <<<` fill 0; `>>` fills 0; `>>>` fills the sign bit when the left operand is
  signed in context, else 0; a shift amount with an x/z bit makes the result all x; an amount
  ≥ width gives all fill bits.
- **Conditional** `c ? a : b`: `c` true or false picks a branch; `c` x merges the branches bit by
  bit (equal bits kept, different bits x; IEEE Table 5-21).
- **Selects**: out-of-range or x/z indices read x bits (E17); constant out-of-range selects warn
  as Yosys does (EL-43).

Implementation: word-parallel on the aval/bval planes for widths ≤ 64; schoolbook loops over
words above (O(n²/64²) for `*`, `/`, `%`, `**` at width n ≤ 2^20 — at most ≈ 3 × 10^8 word
operations, the reason the width cap is 2^20). A fuzz test compares every operator against
Yosys (§14.5).

Declared divergences in constant functions (2D Task 9 fix round 1, rows for §17 by Task 27):
`casez`/`casex` treat z (x and z) on either side as wildcards as IEEE §9.5.1 does, where Yosys
0.55's `eval_const_function` never matches a wildcard; and arguments take the input's width as
context (§4.6), where Yosys's constant path evaluates them self-determined. Every other bit
follows Yosys's `==`, which leaves IEEE too: a plain `case` item, and any non-wildcard bit of a
`casez`/`casex` item or case value, matches only a known equal 0 or 1, so an x or z bit there
never matches (`case (4'b1x00) 4'b1x00:` and `casez (4'b1x00) 4'b1x00:` take neither branch;
IEEE's `===` would take both).

### 4.4 Literals, strings, x and z (EL-15)

- A `NUMBER` becomes a value through `odin3_ast_number_value` (2A §4.5, Yosys's sizing):
  sized literals at their width, unsized ones with `UNSIZED` set; the truncation warning is 2A's,
  once per literal.
- `UNSIZED` survives only until the value is extended to a context (then the top bit repeats,
  EL-13) or enters an operator (whose result is sized). A parameter keeps it in the cases EL-16
  lists (Yosys keeps `is_unsized` on the parameter's constant), and it crosses the EL-4 seam as a
  `NUMBER` whose payload `width` is 0. Verified on 0.55: `parameter P = 'hFFFFFFFF;` assigned to a
  64-bit output gives all ones.
- Strings (L10): 8 bits per byte, the first character most significant; `""` is 8'h00 (IEEE
  §5.2.3.3, Yosys `mkconst_str`). Comparisons and bit/part selects of a string parameter work on
  those bits.
- `x` and `z` in values are 4-state everywhere in the evaluator. Where a constant reaches a cell
  input or a process (stage 2), each bit becomes a pin on the module's shared `$_CONST0_`,
  `$_CONST1_`, `$_CONSTX_` or `$_CONSTZ_` net (IR-4) — a `$proc` node included (EL-33), so there
  is one constant representation; x is a don't-care for later passes, z means tri-state only
  under EL-29.
- A `REAL` in an expression is L9's error ("real number in an expression is not supported");
  in a delay it was dropped with the delay's warning.

### 4.5 Parameters and localparams (EL-16)

Typing of a `parameter` or `localparam` (IEEE §12.2; Yosys `simplify` for `AST_PARAMETER`):

| Declared as | Value |
|---|---|
| untyped (`parameter P = e`) | the value of `e` (self-determined), its width and sign, and `UNSIZED` when `e` is an unsized literal (or a parameter that is) |
| `signed` without a range | `e`'s width, signed |
| `[m:l]` (with or without `signed`) | `e` evaluated self-determined, then extended (by `e`'s own signedness) or truncated to `\|m−l\|+1` bits; signed iff `signed` is written; sized — **except** when `e` is unsized and its stored width already equals `\|m−l\|+1`: Yosys only rebuilds the constant when the widths differ (`simplify.cc` 2381–2390), so `UNSIZED` survives (verified on 0.55: `localparam [31:0] Q = 'hFFFFFFFF;` into 64 bits is all ones; divergence from IEEE, §17 #22) |
| `integer` | 32 bits, signed (`e` converted as for a range `[31:0] signed`) |
| `real`, `realtime`, `time` | P4's error ("parameter type 'real' is not supported") |

An override (EL-8) is converted the same way, so a ranged or `integer` parameter keeps its type
and an untyped one takes the override's type; the override's value is sized first, its sign kept
(v4.7: Yosys passes overrides as `RTLIL::Const`, without `is_unsized`), so only a default keeps
`UNSIZED`. Evaluation is lazy and in declaration order: a
parameter whose value reads a later parameter evaluates that one first (allowed, as in Yosys); a
cycle is "parameter 'P' depends on itself" at `P`'s declaration. A parameter value that reads a
net or variable is "parameter 'P' is not constant (it reads 'x')". In a generate block (and in
an `INSTANTIATION` override list) `parameter` declarations behave as `localparam`s
(IEEE 1364-2005 §12.4: they cannot be overridden). Stage 1 writes every final value as a
`localparam` with a `NUMBER` (unsized only in the cases above) or a `STRING` (EL-4).

### 4.6 Constant functions and the interpreter (EL-17)

A call `f(a, …)` in a constant context (a parameter value, a range bound, a generate condition,
a replication count, an attribute value, a `for`-loop bound in stage 2, …) is evaluated by the
**interpreter** (`odin3_ceval_call`): IEEE §10.4.5 constant functions, which Yosys evaluates in
`AstNode::eval_const_function`.

- **Frames**: an explicit stack of `odin3_ceval_frame { decl, statement cursor stack, locals }`;
  a call pushes a frame, `return` (end of body) pops it. No C recursion, so recursive constant
  functions (K8) work up to `ODIN3_ELAB_MAX_CALL_DEPTH` (1024) frames. The cap also counts every
  evaluation nested in C inside another: a typer callback's (a replication count, a bound, a
  return range) and a host's re-entry (a lazily evaluated parameter read by another, §4.5), and
  those C nestings alone are bounded by `ODIN3_ELAB_MAX_REENTRY_DEPTH` (256, v4.6), so a 2000-link
  `localparam` chain stops with `constant evaluation of parameter 'P257' nested deeper than 256`
  instead of exhausting the C stack.
- **Locals**: the function's ports, its result variable (named like the function; width from the
  return range, 32 bits for `integer`, 1 bit by default; sign from `signed`/`integer`), its local
  declarations and named-block declarations, each a value of its declared width initialised to x.
  `integer` locals are 32-bit signed.
- **Statements**: blocking assignment (to a local, with bit/part selects with constant indices),
  `if`, `case`/`casez`/`casex` (constant; matching as §4.3's declared divergences say), `for`,
  `while` (F2: only here), `repeat`, named and unnamed `begin … end`, calls of other functions
  (constant or not: the callee is interpreted too). `$display`-family tasks are ignored silently inside a function (as
  Yosys); any other system task, a task call, a nonblocking assignment, a timing control, a read
  of a module-level net or variable, a hierarchical name, or an assignment to anything but a local
  makes the call non-constant: "function 'f' is not a constant function: <reason>" when the
  context requires a constant, otherwise stage 2 inlines the call (EL-38).
- **Arguments** are evaluated in the caller's context and assigned to the inputs as by a blocking
  assignment (context = the input's width).
- **Budget**: `ODIN3_ELAB_MAX_EVAL_STEPS` (2^26) statements and expression nodes per top-level
  evaluation ("constant evaluation of 'f' exceeds 67108864 steps"), and each loop's
  `ODIN3_ELAB_MAX_LOOP_ITERATIONS` (EL-46), so a `while (1)` stops with a located error.

A function may be called before its declaration (K7); functions are looked up in the scope chain
(a function declared in a generate block is visible inside it). In stage 2, a call whose
arguments are all constant and whose body is constant is folded by the interpreter (Yosys does
the same before inlining); otherwise it is inlined (EL-38).

### 4.7 System functions (EL-18)

| Function | Meaning |
|---|---|
| `$clog2(e)` | constant only: `e` evaluated self-determined (unsigned); the result is the number of bits needed to hold `e − 1`, 0 for `e` ≤ 1 (Yosys: the index of the highest 1 bit of `e − 1`, plus one), as a 32-bit signed value; stage 1 folds every `$clog2` to a `NUMBER`, a non-constant argument is "$clog2 needs a constant argument" |
| `$signed(e)`, `$unsigned(e)` | change the sign, not the bits (EL-13) |
| any other `$name(…)` in an expression | E20: "system function '$name' is not supported" |

## 5. Generate

### 5.1 Genvars and generate loops (EL-19)

A `GEN_FOR` (`for (i = init; cond; i = step) body`) is expanded by stage 1 pass E:

- `i` must be declared as a `genvar` in the module or an enclosing generate region ("'i' is not a
  genvar"); the init and the step must assign the same genvar ("generate loop must assign genvar
  'i' in its initialisation and step"); a genvar used by an enclosing active loop cannot drive
  this one ("genvar 'i' is already used by an enclosing loop").
- `init`, then repeatedly `cond` and `step`, are constant expressions with `i` bound to its
  current value; the genvar holds a 32-bit signed value (Yosys's width for a genvar). `cond` must
  be constant ("generate loop condition is not constant (it reads 'x')"); x counts as false.
- Each iteration whose `cond` is true instantiates `body` as a `GEN_BLOCK` with the loop block's
  name and the index slot set to the current value; every read of `i` in it becomes a 32-bit
  signed `NUMBER`. A value seen twice in one loop is "generate loop of 'i' repeats the value N"
  (IEEE; a non-progressing loop is caught here before the iteration cap). Values may be negative
  or descending (G4: `g[-1]`, `for (i = 3; i >= 0; i = i - 1)`).
- Iterations are capped per loop (`ODIN3_ELAB_MAX_LOOP_ITERATIONS`, 2^20) and per run
  (`ODIN3_ELAB_MAX_TOTAL_ITERATIONS`, 2^24, generate and procedural together).
- The body may hold any module item (G3): declarations, `localparam`s, `assign`, `always`,
  `initial`, instances, gates, nested generates, functions and tasks, `defparam`, another
  `genvar` declaration. A `PORT_DECL` in a generate block is "port declarations are not allowed
  in a generate block".

### 5.2 if-generate and case-generate (EL-20)

- `GEN_IF`: the condition is constant (else "generate condition is not constant (it reads
  'x')"); true selects the then-branch, false or x the else-branch (or nothing).
- `GEN_CASE`: the case expression and the item expressions are constant; an item matches when
  Yosys's `const_eq(expr, item)` at `max` width, signed iff both signed, is 1 (so an x/z bit in
  either never matches); the first matching item in order wins, else `default`, else nothing.
  Several expressions per item (G6) are tried in order.
- The selected branch is one generate block (EL-21); nothing of an unselected branch is
  elaborated, so declarations there do not exist (errors in them are not reported, as in Yosys and
  IEEE: they are never elaborated).

### 5.3 Generate scopes and their names (EL-21)

Every branch of a `GEN_IF`, every item of a `GEN_CASE` and every iteration of a `GEN_FOR` is a
generate block and a scope, **whether or not it is written with `begin … end`** (Yosys wraps a
bare item in an unnamed `GENBLOCK`). Unnamed blocks are named with Yosys's `label_genblks`
(1364-2005 §12.4.3 numbering), which 2D reproduces exactly:

- In each scope (the module, or a generate block), a counter starts at 0 and increments once per
  `GEN_IF`, `GEN_FOR` or `GEN_CASE` met in a pre-order walk of that scope's items (a construct
  inside a branch is inside that branch's block, where the counter restarts).
- Every unnamed block **of one construct** gets the same name, `genblk<counter>` of that
  construct: the then-block and the else-block of a `GEN_IF` (only one is ever selected), every
  item of a `GEN_CASE`, the body of a `GEN_FOR` (indexed per iteration). If that name was **seen
  earlier in the pre-order walk of the scope** as a user identifier starting with `genblk`, zeros
  are inserted (`genblk01`, `genblk001`, …) until it is not; a user `genblk1` declared *after*
  the construct does not pad it (Yosys inserts a name into its set only when the walk reaches it;
  verified on 0.55 that `wire genblk1;` after the first `if` coexists with block `genblk1`).
- Inside a block the counter restarts at 0 with a copy of the outer scope's set of seen names.
- `if (A) … else if (B) …`: the else branch is a block of its own (Yosys wraps the bare nested
  `if`), named like the then-block, and the nested `GEN_IF` inside it is the first construct of a
  fresh scope. Verified on 0.55 with this as the module's first construct, `A` = 0, `B` = 1: the
  wire is `genblk1.genblk1.x` (and a bare `if (B) assign y = a;` consumes a counter value; a
  following `for` was `genblk3[0].w`). IEEE §12.4.2 does not make a directly nested conditional a
  scope (`genblk1.x`); 2D follows Yosys (divergence §17 #3).

Hierarchical names of objects inside generate scopes join the scope path with `.` and loop
indices as `[i]`: `g[0].t`, `genblk1[3].u_reg`, `named.v`. These strings are the IR names of the
wires and instances (EL-23), matching Yosys's `\g[0].t`. In the elaborated store the scope stays
a `GEN_BLOCK` with name `g` and index 0 (2A §4.6: distinct from an escaped identifier spelled
`g[0]`). A generate block name that clashes with another declaration in its scope is M11's error.

### 5.4 References into scopes (EL-22)

A `HIER_NAME` whose first component resolves, from the current scope outward, to a generate
block or a named block of the **same module** is resolved component by component
(`odin3_scope_child(st, scope, name, index)` for `g[2]`) and must end at a declaration in the
reached scope (G7: `g[0].t`, `blk.count`). Any other hierarchical name — through an instance,
upward, or into a function — is E21: "hierarchical reference 'u.w' is not supported". Indices
must be constant (after stage 1 they are `NUMBER`s). A reference into a block that was not
selected is "'g[5].t' does not exist (generate block 'g[5]' was not elaborated)". A named
`begin`/`end` block here means one of an `always` or `initial` of the same module scope (a block
inside a function or task is not reachable by this path, E21): stage 1 accepts a path whose first
component names one (v4.15: `blk.count`, before or after the block, inside a generate block too) and
leaves the member to the binder ("'blk.nope' is not declared", §17 #38); a constant position that
reads one is E21.

## 6. Nets, variables and ports

### 6.1 Declarations and IR wires (EL-23)

Every net and variable declarator of a specialization becomes one IR wire (IR-2) with fresh nets,
one per bit, unless it is a memory (EL-27):

- **Name**: the declarator's name prefixed by its scope path (EL-21: `g[0].t`, `blk.count`,
  `$unnamed_block$1.tmp`); a top-level module object keeps its plain name.
- **Range**: `odin3_wire_spec {msb, lsb}` as declared, direction kept (`[0:3]` stays ascending,
  `[-1:2]` keeps its negative bound, N2); no range → `[0:0]`; `integer` → `[31:0]`. Bit k of the
  wire is the declared index `lsb + k` (descending) or `lsb − k` (ascending), IR's rule, which is
  also Yosys's (`start_offset`, `upto`). A bound outside int32 is EL-46's width error.
- **Signed** from `signed` (N5) and `integer` (N4).
- **Provenance**: a SOURCE record at the declarator (EL-45); attributes of the declaration copied
  to the wire (2A §7: every declarator of an annotated declaration).
- Net kinds `wire`/`tri` are plain wires; `wand triand wor trior uwire supply0 supply1` add driver
  rules (EL-26). `reg` and `integer` are variables: assigned procedurally, or by an `initial`
  (EL-40); a `reg` assigned by a continuous assignment is accepted with Yosys's warning
  ("'r' is a variable assigned by a continuous assignment; treated as a net"), and a net assigned
  in an `always` block likewise ("'w' is a net assigned in an always block; treated as a
  variable") — both are illegal in 1364-2005 and accepted by Parmys (`-sv`), D5's spirit.
- A **net declaration assignment** `wire w = e;` (N7) is `assign w = e;` at the declaration. A
  **variable initialiser** `reg r = v;` (N8) is `initial r = v;` at the declaration (EL-40;
  Yosys's parser does the same).
- **Declarations in an unnamed block** (T18 outside 1364-2005; Parmys reads `-sv`) are accepted
  with "declaration in an unnamed block is a SystemVerilog extension; the block is named
  '$unnamed_block$1'".
- **Duplicates** (M11): a second declaration of a name in one scope is "'x' is already declared
  (first at f.v:3:5)", except the legal non-ANSI pair (EL-24).

### 6.2 Ports (EL-24) — includes IR change C

- **ANSI** headers give the ports directly. **Non-ANSI** headers (`module m(a, b);`) list names;
  each must be declared with a direction in the body ("port 'a' is not declared"), and a direction
  declaration of a name not in the list is "'x' is declared as a port but is not in the port
  list". A later `wire`/`reg`/`integer` declaration of a port name (M1: `output q; reg [3:0] q;`)
  merges into the port: the type comes from it; ranges must agree when both are written ("port
  'q' is declared with ranges [3:0] and [7:0]"), else the written one wins; `signed` on either
  makes it signed (IEEE §12.3.3). The merge keeps everything the second declaration says (v4.15):
  its value (`output w; wire w = ~a;` drives `w`; `output q; reg q = 1'b1;` initialises `q`) goes
  into the port's `DECLARATOR` (EL-4's `PORT_DECL` row) and its attributes (`(* keep *) wire q;`)
  join the port's own, in either order and for an ANSI port redeclared in the body. A value on both
  declarations is "port 'q' has a value in both of its declarations" (§17 #37).
- **Untyped ports** are `wire` (also under `` `default_nettype none `` — Yosys does not complain,
  IEEE arguably does; follow Yosys). `output reg`/`output integer` are variables (M3).
- **`input reg`/`input integer`** (M12, D5): accepted as a net with "input 'a' is declared as a
  variable ('reg'); it is treated as a net".
- **`inout`** (M7) ports are `$port_inout` (EL-29 says what may drive them).
- **Empty positions**: only a trailing comma (`(a, b, )`, 2C §3.6) reaches 2D; 2C rejects
  `(a, , b)` and `(, a)` with a located syntax error, as Yosys 0.55 does (2C §3.1 (k); §17 #31;
  v4.10). The trailing one is kept as a position for ordered connections (EL-10) and is not a port.
  **Port expressions** `.x(a)`, `{a, b}`, `a[3:0]` in a
  header are M6: "port expression in a module port list is not supported".
- **Order**: header order. Stage 2 creates the ports with `odin3_module_add_port` in that order,
  for every specialization before any module body (IR-7: a module's ports must exist before
  instances of it), `scalar` set for a port declared without a range.

**IR change C (ruled, §20 item 3).** `odin3_module_add_port` creates the port's wire as
`[width−1:0]`, unsigned (`src/ir/module.h`), so a port `input signed [0:7] a` loses its range
and signedness in the IR, which IR-2 says every wire carries. `odin3_port_spec` gains
`bool has_range; int32_t msb, lsb; bool is_signed;`. Without `has_range`, the port is
`[width−1:0]` as today, so existing callers are unchanged. With it, `width` is derived as
`|msb − lsb| + 1` and a caller-supplied `width` that differs is `INVALID_ARG`, as is `scalar` with
a range of more than one bit; `is_signed` applies in both cases. The module's cell type records
the sign too, for writers and for EL-10's output extension, **only internally**: the field is
added to the core's `odin3_port_def` (`src/ir/celltype.h`), never to the ABI's
`odin3_plugin_port`, whose layout `odin3.h` freezes (no reserved slots); if plugins ever need the
sign, an accessor arrives behind a reserved `odin3_plugin_celltype` slot (the re-review's N3).
Writers and `check` are otherwise unaffected; no invariant changes. (A wire-range mutation after creation was rejected:
it would leave the cell type's port record without the sign.)

### 6.3 Implicit nets and `` `default_nettype `` (EL-25)

- The setting in effect for a module is the last `DIRECTIVE[default_nettype]` before the
  module's `MODULE` node in project stream order (UNITs in order, directives inside earlier
  modules included), `wire` at the start of the run and after `` `resetall `` (L20). A
  `` `default_nettype `` inside a module warns "`default_nettype inside a module takes effect at
  the next module" (1364-2005 §19.2 allows it only outside modules).
- **Where implicit nets arise** (M10): any identifier in a module or generate scope that resolves
  to nothing — Yosys creates an implicit 1-bit wire for every undeclared identifier, in any
  expression, statement or connection, not only the net lvalues and port connections IEEE §4.5
  allows. 2D follows Yosys (Parmys accepts designs that rely on it; Decisions for Peter, item 11)
  with the warning "'x' is implicitly declared" (one per net, at its first use; Yosys:
  "Identifier `\x' is implicitly declared."). Exceptions: an identifier in a constant context is
  "'x' is not declared"; a name used as a function, task or module is never an implicit net.
- The net is scalar, of the default net type's kind (`wire`/`tri` → `wire`; `wand`/`triand` →
  `wand`; `wor`/`trior` → `wor`; `uwire` → `uwire`); `tri0`, `tri1`, `trireg` as default net type
  make the first implicit net N15's error. It is declared at **module** scope (Yosys adds
  autowires to the module even when the use is inside a generate block), named as written.
- `none` (L19): each would-be implicit net is "'x' is not declared (`default_nettype is none)".

### 6.4 Net kinds with driver rules (EL-26)

- **`wand`/`triand`, `wor`/`trior`** (N14, D4): every driver of the net (continuous assignment,
  gate or instance output) drives its own fresh nets; at the end of the module each bit of the net
  is driven by an `$and` (wand) or `$or` (wor) chain over its drivers, in source order (k drivers:
  k − 1 two-input word cells of the net's width when all drivers cover the whole net, else per
  bit). One driver: no gate. A procedural assignment to a wired net is "'w' is a wired net (wand)
  and cannot be assigned in an always block".
- **`uwire`** (N15): a second driver is "'u' is a uwire and has a second driver (first at …)".
- **`supply0`/`supply1`** (N16): driven by `$_CONST0_`/`$_CONST1_`; any other driver is A4's
  error.
- **`tri0`, `tri1`, `trireg`**: N15's error ("net type 'tri0' is not supported").
- `tri` is `wire` (its several drivers are legal only under EL-29).

### 6.5 Arrays and memories (EL-27)

An array is any declarator with unpacked dimensions (N9–N12). Multi-dimensional arrays
(`reg [7:0] m [0:3][0:1]`, N12) are flattened row-major into one dimension, as Yosys does
(`m[i][j]` is word `i × 2 + j` after offsetting each index by its range's low bound; ascending
dimensions index from their left bound). A select `m[i][3:0]` (N13) is a word select followed by
a part select of the word.

**mem2reg.** Each array becomes either one **memory** or one **register per word**, by Yosys's
`mem2reg_as_needed` rules (`simplify.cc`; `read_verilog` without `-nomem2reg`, as Parmys runs).
The array is replaced by words when any of these holds (computed per specialization over the
elaborated module by stage 2 before it emits anything):

1. **forced**: the array is declared as a net (`wire [3:0] w [0:3]`, N11 — Yosys's `!is_reg`), or
   carries `(* mem2reg *)`, or its module does;
2. **written in an asynchronous block**: assigned in an `always` whose event list does not have
   exactly one edge event (combinational, or with an asynchronous reset);
3. **read after a blocking write in the same block** (Yosys `MEM2REG_FL_EQ2`);
4. **complex lvalue**: the array appears inside the index of an lvalue (a concatenation lvalue
   is split into its parts first, as Yosys does: `{m[wa], t} <= x` is judged by rules 2, 3 and 5
   on the part `m[wa]`, not by this rule);
5. **constant writes only**: every write outside `initial` blocks uses a constant index (Yosys
   `CONST_LHS` without `VAR_LHS`);
6. **initial-only init with `nomeminit`**: written in `initial` and elsewhere while `nomeminit` is
   set (never set by 2D; listed for completeness).

`(* nomem2reg *)` on the array exempts it from 2–6. `--nomem2reg` disables mem2reg entirely,
rule 1 included, as Yosys's `-nomem2reg` does (its whole `mem2reg_as_needed` block is guarded by
the flag, `simplify.cc` 1072); an array that then cannot be a memory — written by a continuous
assignment, or in a block without exactly one edge event — is "array 'w' cannot be a memory
(it is written in a combinational block) and --nomem2reg is set" (Yosys fails later, in
`genrtlil`, on such designs; Parmys uses `-nomem2reg` only on `vtr_primitives.v`).
Rules 2–5 warn as Yosys does ("replacing memory 'm' with list of registers; see f.v:12, f.v:20",
the places that triggered it); rule 1 is silent.

- **Registers per word**: words `m[k]` (k the declared index, Yosys's names) are variables of the
  word's width and signedness. A constant index selects the word (out of range: x on read, no
  write, with EL-43's warning). A variable index on read is a `case` over the index (each in-range
  value selects its word, `default` gives x) and on write a `case` whose items assign one word
  each (no default: an out-of-range write changes nothing) — Yosys's `mem2reg_as_needed_pass2`.
  In a process the case is inline (EL-35); outside one, a read becomes a combinational process of
  its own whose result is a temporary (Yosys's `$mem2reg_rd` block).
- **Memories**: one `$mem` cell per array (EL-39), created at the end of the module's stage 2
  with every access as a port: `MEMID` = the array's IR name; `WIDTH`; `SIZE` = the number of
  words; `OFFSET` = the lowest declared index of a one-dimensional array (`[1:4]` and `[4:1]` →
  1, `[-2:1]` → −2), 0 for a flattened multi-dimensional one; `ABITS` = the width of the widest
  address expression (at least 1); read ports asynchronous (`RD_CLK_ENABLE` 0, `RD_CLK` = x,
  `RD_EN` = 1, `RD_TRANSPARENT` 0), one per read in source order; write ports clocked
  (`WR_CLK_ENABLE` 1, `WR_CLK_POLARITY` from the edge), one per write statement in source
  order, so a later write port wins a same-address conflict as a later statement does; `INIT` =
  the initial contents (EL-40), empty when there are none (all x). The address on each port is
  the **raw index** expression, generated at its self-determined width, with no subtraction:
  `$mem` has simlib's semantics, which subtract `OFFSET` themselves (`memory[RD_ADDR − OFFSET]`,
  `simlib.v` `$mem`), and Yosys's front end puts the raw index on its ports (verified on 0.55:
  `reg [7:0] m [1:4]` gives `OFFSET 1` and `RD_ADDR \i`). The only address arithmetic 2D emits is
  a multi-dimensional array's row-major flattening (`(i − low_i) × n_j + (j − low_j)`, with
  `OFFSET` 0). The
  `memory_infer` pass of Phase 4 later merges flip-flops into synchronous read ports and maps
  the cell. `SIZE × WIDTH` above `ODIN3_ELAB_MAX_MEMORY_BITS` (2^30) is an error.

This mirrors Yosys's genrtlil (`$memrd`, `$memwr`, `$meminit` cells plus a memory record) after
its `memory_collect`; 2D collects at elaboration because the IR has no memory record and the
registry's `$mem` already describes the collected form (Decisions for Peter, item 8).

### 6.6 Continuous assignments and drivers (EL-28)

`assign lhs = rhs;` (A1): the right side is generated (EL-30) at width `max(L(lhs), L(rhs))`,
sign from the right side, then truncated or extended (sign or zero, by its sign) to `L(lhs)`.
The left side is a net lvalue: identifiers, constant bit and part selects, concatenations; a
non-constant select is "continuous assignment to 'y' with a non-constant index is not supported"
(IEEE §6.1.2; Yosys rejects it too). Bits of an lvalue that select outside the net are dropped
with Yosys's warning ("assignment to bits outside 'y' [7:0] is ignored").

**Driving a net** without a buffer cell: the IR has no word-level buffer, so each lhs bit
becomes the right-side bit by construction —

- when the right side is the output of cells 2D creates for this assignment, those cells are
  created with the lhs nets as their outputs (the common case: `assign y = a + b;` makes one
  `$add` whose Y pins are y's nets);
- otherwise (the right side is another wire's net, a port input, a constant net, an instance
  output) the two nets are **merged** (`odin3_net_merge`, IR-15): `assign b = a;` keeps both
  names (IR-2), the constant case aliases the wire bit into the module's shared constant net.

A merge is pin-based (IR-15) and therefore safe at any time, `$proc` nodes included (their
reads and writes are pins, EL-33); 2D still merges only in this phase, so the nets it hands to
processes and cells are final. When both sides are already **the same net** (`assign a = b;
assign b = a;`, a legal loop of wires; Yosys makes a self-connection), the assignment is a no-op:
no merge (which would be `INVALID_ARG`, keep == drop), no driver, and the warning "'a' and 'b'
are connected only to each other and are never driven" when nothing else drives the net.

**Driver accounting.** Stage 2 keeps, per net it has created, the driver attached so far (kind:
port input, constant, cell output, instance output, gate, continuous assignment, process; and
the AST node), and checks **before** attaching a new driver:

- a second driver of a plain net bit is A4's error with both places ("'y[3]' has two drivers
  (also driven at f.v:9:12)"), and the new driver is not attached (the IR stays valid);
- a second process driving a bit is T10's error (EL-41);
- wired nets collect drivers (EL-26); tri-state drivers follow EL-29 (those on internal nets become
  `$pmux` logic at the end of the module, step 7b).

So no multi-driven net is ever built. A net with no driver is legal (x; IR check rule 4 warns
in `check`, and 2D warns once per wire that has sinks and no driver: "'w' is never driven").

**Order of emission per module** (each step completes before the next):
(1) ports and wires; (2) the mem2reg decision and the always-assigned bit set (a pre-scan of
every `always` block's lvalues); (3) initial values (EL-40), whose constant drivers merge nets (run after steps (4) and (5), v4.23);
(4) instances and gates; (5) continuous assignments; (6) processes (EL-35); (7) wired-net
gates (EL-26); (8) `$mem` cells (EL-27). Steps 3–5 may merge nets; steps 6–8 never do.

### 6.7 Tri-state (EL-29)

D3: a `z` drive becomes a `$tribuf` (word, tristate flag, `WIDTH`, ports `A`, `EN`, `Y`), and the
cell then goes one of two ways at the end of the module's emission (step 7b of EL-28, after every net is
merged, so the order of the assignments does not matter). Peter's ruling of 2026-10-13: a tri-state
driver on an internal net is mux logic, as Yosys's `tribuf -logic` makes it; FPGAs have no internal
tri-states.

- `assign y = en ? d : 'bz;` (either branch all z) → `$tribuf(A = d, EN = reduce_bool(en), Y = y)`
  (the branches' order sets an inverted enable through `$not`);
- `bufif1 (y, d, en)`, `bufif0`, `notif1`, `notif0` (Q3) → `$tribuf` with `EN = en` or `~en`
  and `A = d` or `~d`;
- `assign y = 'bz;` alone drives `$_CONSTZ_`.

**Top-level ports keep the `$tribuf`.** A cell with a net on a `$port_out` or `$port_inout` of the top
module is left as it is: several such drivers are a bus (IR check rule 4 allows tristate drivers to
share a net), and any other kind of driver there is A4's error. Yosys's `tribuf -logic` also leaves a
`$tribuf` when any bit of the net is an output port, so a bus that is assigned straight to an output
(`assign y = bus;`) is a top-level tri-state, whichever statement comes first.

**Every other tri-state driver becomes logic.** Which nets: the cells whose nets reach no port of the top
module (an internal wire; an output port of a module below the top is a located error, since the instance may drive a top-level port where the `$tribuf` must stay and a module is elaborated before its parents are known). Which drivers: all the drivers of the net
must be tri-state drivers (`cond ? val : 'z`, either branch all z, or a gate primitive above); a net with
a tri-state driver and another driver (an `assign`, a cell, a process, an instance output) is A4's error
"'t' has two drivers (also driven at f.v:L:C)", and so is a net driven by outputs of two child instances
(nets are never merged across modules). Why a mixed net stays an error: an ordinary driver always drives a
value, so while a tri-state enable is on the net has two drivers, and while all are off a value that is
not the don't care of the ruling; Yosys 0.55 shorts them, and there is no choice that is sound for every
input. How the drivers combine: the cells whose `Y` is the same list of nets are one group, in source
order. A group becomes one `$pmux` (`WIDTH` the nets, `S_WIDTH` the number of drivers):

| port | value |
|---|---|
| `A` | x, for every bit: no driver is enabled (a don't care) |
| `B` | the drivers' `A` (data), `WIDTH` bits each, **the last driver in the source first** |
| `S` | the drivers' `EN`, in the order of `B` |
| `Y` | the nets |

and its `$tribuf` cells are deleted. The order is Yosys's: `tribuf` walks the module's cell table, which
iterates newest first, and `opt` resolves the x of `A` against that order, so a different order would make
the netlist differ from the oracle's in the don't care case (found by `elab-equiv`: the first version put the
first driver first and `D3_bus_one_hot` was not equivalent when no driver was enabled). **Several enabled
at once:** the value is undefined, as for any `$pmux` (Yosys's formal model gives x; its gate lowering
and the simulator give the OR of the selected data, and the netlist 2D writes does the same, being the same
cell). Real bus contention has no meaning on an FPGA, so 2D does not invent a priority; the
warning says so. **One driver:** Yosys connects `Y = A` and drops the enable; 2D keeps a one-select
`$pmux` over x (the enable is visible), which `opt` reduces to the same netlist (verified equivalent,
`D3_single_driver`).

Errors, each located at the (first) driver and leaving the `$tribuf` in place: the drivers of one net
cover **overlapping but different** bit lists (`assign b[3:0] = ...; assign b[5:2] = ...;`: Yosys does not
merge those either, and a `$pmux` per list would be several drivers); one driver covers **both
top-level port bits and internal nets** (`assign {y, t} = ...`); a tri-state driver on an **`inout` or `output` port of
a module below the top**; an **`inout` pin of a child instance** on the net. Drivers of disjoint bit
ranges are separate groups.

The warning (§9.2), once per group: "tri-state drivers on internal net 'data' converted to logic (no driver
enabled: x; several enabled: undefined)", located at the first driver, naming the wire (`bus`, `bus[3]`,
`bus[7:4]`).

**`--vtr-compat`: Parmys's resolution (PHASE2 #27; Peter 2026-10-14).** Where Odin III and Parmys
disagree, the VTR flow's mode follows Parmys and says so. Parmys never builds the bus above:
Yosys 0.55's `opt_expr -mux_undef` turns `en ? x : 'bz` into `x` (the enable is dropped), and
`opt_clean` puts the net and all the `x` into one class of signals and keeps the one
representative `compare_signals` ranks first, shorting the others to it. Under the option, a group
of two or more drivers on an internal net whose data bits are all bits of **input ports** of the
module is resolved so: no `$pmux`, no enable; per bit the winner is the input bit another cell
reads, else the one on the wire with the lowest name (`strcmp`), else (one wire) the highest bit;
the net is merged into the winners and the other inputs are left unused. Micro
`syntax/multi_assignment`: `data` is `bus0`, `busout = enable ? bus0 : 1`, `s` unused, which is the
Parmys golden. The order of the assigns does not matter. The warning (D3, same place, instead of
the one above) names the net, the number of drivers, the data kept, the wires dropped and the IEEE
reading: "net 'data' has 4 drivers; --vtr-compat keeps only the data of 'bus0' as Parmys does (the
input Yosys keeps as the net's representative: one other logic reads, else the lowest name),
ignores every enable and drops 'bus1', 'bus2', 'bus3' (IEEE resolution would be a bus selected by
the enables: the data of the driver that is enabled, undefined for several)". Everything else keeps
the conversion above, with its own warning, under the option too: one driver (the same netlist
either way), a group with a driver whose data is a cell output, a constant or a child's output
(Parmys leaves several drivers on one net there, which the IR cannot hold), and every net on a
top-level port. Without the option nothing changes. Code: `elab2_trivtr.c`; unit test
`test_elab_vtr_multidriver.c`.

An `inout` port read inside the module reads its net.

**`inout` ports of instantiated modules** (the review's F22). An instance's `inout` pin is a
driver of the connected net (IR-3, driver partition) and may share it with other `inout` pins and
`$tribuf` outputs (a bus, rule 4). 2D accepts such a connection when the net has no other kind of
driver — the net is the child's to drive, and when it reaches a top-level `inout` the bus is D3's
legal case. A net driven both by an `inout` pin and by ordinary logic (an `assign`, a cell, a
process, an instance output) is an internal tri-state bus: "net 'x' is driven by inout port 'p'
of instance 'u' and by logic at f.v:L:C; internal tri-state nets are not supported (D3)". The same
net with a tri-state driver that would become logic is the same error with "an inout pin cannot share a net
with converted tri-state drivers" in place of the last clause. (The
three corpus micros with `inout` are Parmys-rejected, PHASE2 #6 D3.)

## 7. Expressions to word cells

### 7.1 The mapping (EL-30)

Stage 2 generates an expression in three steps with explicit stacks: type it (EL-13), then emit
it post-order, each node yielding an `odin3_sig` (a vector of `odin3_sigbit`: a net or a
constant bit 0/1/x/z, LSB first, at the node's emitted width, plus its sign); a node whose
operands are all constant is folded by the evaluator (EL-14) instead of emitting a cell. When a
constant must become a cell input it is wired to the shared constant nets (IR-4). Extension and
truncation are wiring (repeat the top net, or the constant 0 net), never a cell: Yosys's `$pos`
cells (`widthExtend`) have no IR counterpart because IR-1 makes extension a choice of nets.

Cell parameters follow genrtlil exactly: `A_WIDTH`/`B_WIDTH` = the widths of the operand signals
as generated (an identifier at its own width, not the context width), `Y_WIDTH` = the node's
final width, `A_SIGNED`/`B_SIGNED` = the operand's final signedness in context (an operand is
signed only if it is signed and its context is signed), so the cell extends its operands itself.

| Source | Cells (registry names; † = added by EL-31) |
|---|---|
| `a + b`, `a - b`, `a * b`, `a / b`, `a % b` | `$add $sub $mul $div $mod`; `Y_WIDTH` = final width |
| `a & b`, `a \| b`, `a ^ b`, `a ~^ b` | `$and $or $xor`, `$xnor`† |
| `~a`, `-a`, `+a` | operand extended to the final width by wiring first, then `$not`, `$neg`†; `+a` is the extended operand (no cell) |
| `&a ~&a \|a ~\|a ^a ~^a` | `$reduce_and`, `$reduce_and` (1 bit) + `$logic_not`†, `$reduce_or`, `$reduce_or` (1 bit) + `$logic_not`†, `$reduce_xor`, `$reduce_xnor`†; `Y_WIDTH` = max(final, 1), operand self-determined (`~&`/`~\|` as Yosys's parser builds them, `verilog_parser.y` `OP_NAND`/`OP_NOR`: the reduction's `Y_WIDTH` is 1, the `$logic_not`'s is max(final, 1); v4) |
| `!a`, `a && b`, `a \|\| b` | `$logic_not`†, `$logic_and`†, `$logic_or`†; operands self-determined; `Y_WIDTH` = max(final, 1) |
| `a == != < <= > >= b` | `$eq $ne $lt $le $gt $ge`; operands typed at their common width and sign (EL-13) and generated as any operand is (an identifier at its own width, an unsized apostrophe-form constant such as `'d1` or `'h3` at the common width, a plain decimal such as `1` at its own 32 bits, §17 #41), `Y_WIDTH` = max(final, 1) |
| `a === b`, `a !== b` (E9) | both sides constant: folded exactly (4-state `===`); one side a constant with x/z bits (after both are brought to their common width and sign, EL-13): a **wildcard compare**, as in Parmys — `$eq`/`$ne` over only the bit positions where the constant is 0 or 1 (the other operand's bits at those positions, selected by wiring), so an x/z bit matches any value; no 0/1 position left → constant 1 (`===`) or 0 (`!==`); warning "'===' compares against x/z bits; they match any value". Verified on 0.55 through the Parmys script: `a !== 4'b1x00` becomes `~a[3] \| a[1] \| a[0]`, the x bit dropped. Neither side has x/z: `$eq`/`$ne` with "'===' is treated as '==' (synthesis has no x or z)" |
| `a << b`, `a <<< b`, `a >> b`, `a >>> b` | `$shl` (both left shifts: `$sshl` ≡ `$shl` in simlib), `$shr`, `$sshr`; `A` at the context width and sign, `B` self-determined unsigned (`B_SIGNED` 0) |
| `a ** b` | `$shl(1, b)` when `a` is the constant 2 and `b` is unsigned (Yosys's rewrite), else `$pow`†; `B_SIGNED` = `b`'s own sign |
| `c ? a : b` | `c` constant and fully 0/1: the chosen branch only (Yosys), extended to the context; else `$mux(A = b, B = a, S = c')` with `WIDTH` = the wider branch as generated (the narrower is extended to it by wiring, by the ternary's sign), the result then extended to the context (v4.16; Yosys), where `c'` = `c` if 1 bit else `$reduce_bool`†(`c`). All three constant: folded (IEEE Table 5-21 for an x condition) |
| `{a, b}`, `{n{a}}` | wiring (no cell); result zero-extended to context |
| `$signed(a)`, `$unsigned(a)` | the operand's bits, with the new sign |
| identifiers, selects | EL-32 |
| function call | EL-38 (inlined into a process) |
| memory word `m[i]` | a `$mem` read port (EL-27) |

Attributes on an operator node (`(* … *)` on a `BINARY`) go to every cell that node creates (2A
§7). The emitted structure is the same as Yosys's `genRTLIL` cell for cell (minus `$pos`, plus
folding), so a 2D netlist and Yosys's differ only where §17 says.

### 7.2 Registry additions (EL-31) — IR change A (ruled: nine cells, §20 item 2)

The registry (`src/ir/cells/`, IR-8) has the Yosys word set of DESIGN §5.2. 2D emits only
registered ops, and Verilog-2005 needs **nine** more Yosys word cells, each one file entry with
Yosys's ports, parameters and simlib semantics:

| Cell | Shape | Semantics (simlib) | Need |
|---|---|---|---|
| `$neg` | unary | `Y = −A` | composable (`$sub(0, A)`) |
| `$xnor` | binary | `Y = ~(A ^ B)` | composable (`$not($xor)`) |
| `$reduce_xnor` | unary | `Y = ~^A`, 1 bit zero-extended | composable with care (a 1-bit `$not`, then zero-extension by wiring) |
| `$reduce_bool` | unary | `Y = \|A`, 1 bit zero-extended | identical to `$reduce_or` |
| `$logic_not` | unary | `Y = !A`, 1 bit zero-extended | composable (`$eq(A, 0)`) |
| `$logic_and`, `$logic_or` | binary | `Y = A && B`, `A \|\| B`, 1 bit zero-extended | composable (`$and` of two `$reduce_or`s) |
| `$shiftx` | binary | `Y = A >> B` (B signed: negative shifts left), filling with **x** | **required**: a non-constant select reads x out of range (E17); `$shr` would read 0, which is a different don't-care resolution from Parmys's (§14.6) |
| `$pow` | binary | `Y = A ** B` (`const_pow` above) | **required**: E7's variable exponent (micros `common/pow`, `twobits_arithmetic_power`) has no other registered form short of 2D lowering it to a multiplier chain |

The honest trade-off for the seven composable cells: they buy nothing in the exit test, which
compares after `opt` and `abc`; they keep EL-30 one cell per operator (simpler code and review),
keep `stats` comparable with Yosys's cell counts, and let 2F port `opt_expr`'s rules per cell
type. Appendix A's compositions cost about two cells per use. No `$shift` is added: nothing in 2D
emits it (EL-32 writes non-constant selects with the case method; Yosys emits `$shift` only for
its mask-and-shift lvalue method, §17 #4).

All nine reuse `odin3_cells_binary_*`/`odin3_cells_unary_*` (the `ODIN3_BINARY`/`ODIN3_UNARY`
macros); simulate hooks follow 1E's rules for the seven that are cheap (`$shiftx` and `$pow` get
`simulate = NULL` until a pass needs them). DESIGN §5.2's word list is amended. The alternative
that was considered — compositions from today's cells for the seven composable ones and 2D-side
lowering for `$pow` and `$shiftx` — stays in Appendix A for reference (§20 item 2).

### 7.3 Identifiers and selects (EL-32)

- **Identifiers**: a net or variable is its wire's nets (or, inside a process, their current
  substituted values, EL-35); a parameter is its constant; a genvar was a `NUMBER` already.
- **Constant selects** `x[i]`, `x[m:l]`, `x[b +: w]`, `x[b -: w]`: the bits at the declared
  indices (descending or ascending by the wire's range, EL-23). A part select written in the
  opposite direction of the declaration (`x[0:3]` of `[7:0]`) is "part-select [0:3] of 'x' is
  reversed relative to its declaration [7:0]". Bits outside the range read x with Yosys's warning
  ("range select [9:6] of 'x' is out of bounds; bits outside [7:0] are x"; one bit: "bit select [9] of 'x' is out of
  bounds; the bit is x"). A constant index with an x or z bit reads x, silently (§17 #40).
- **Non-constant reads** (E15, E16): `$shiftx` with `A` = the whole wire, `Y_WIDTH` = the select
  width, `B` = the index (for `-:`: base − width + 1, one `$sub(base, w − 1)`) offset to a bit position: minus the wire's
  low index when it is not 0 (a `$sub`, signed), and for an ascending range `(width − w) − pos`
  (a `$sub`); `B_SIGNED` is set when an offset was subtracted, when the index is 32 bits or
  wider or when the index is signed — genrtlil's offset arithmetic with a simpler base for `+:`/`-:` (§17 #42); a
  bit select's cells match Yosys's.
- **Non-constant writes** in a process (`y[s] = d`, `y[b +: 4] <= d`) are rewritten to a `case`
  over the position with one item per in-range position, each assigning that constant slice,
  no default (an out-of-range write changes nothing; a slice that straddles an end of the target
  writes the bits inside it): Yosys's case method (`nowrshmsk`), chosen
  over its default mask-and-shift method because it needs no `$shift` cell (none is registered, EL-31) and maps to
  the same mux structure after `proc`; equivalent in every case but two (§17 #4, #54). The position
  is a temporary assigned once, in the root case, before the switch (v4.21). The positions are the
  declared index values the index expression, at its own width and sign, can hold (a signed 2-bit
  index takes -2 to 1, as Yosys's items do) at which some bit of the slice lies in the target
  (v4.22); a blocking write has the case temporaries of EL-36 beside, so its switch also has the
  default case that holds them. Outside processes, a non-constant select target is EL-28's error;
  inside a concatenation it is `non-constant index into 'y' is not supported in a concatenation
  target` (§17 #55).
- **Memory words**: EL-27.

## 8. Processes

### 8.1 Processes in the IR: the `$proc` cell (EL-33) — IR change B (ruled, §20 item 1)

DESIGN §6 step 2 says "word-level IR with processes", but `docs/IR.md` has no process. `proc`
(2E) must see the decision structure of an `always` block — which branch assigns what, which
edge clocks it, `full_case`/`parallel_case` — to build mux trees, flip-flops and latches. A
process is therefore a **node** of a new built-in cell type `$proc` whose connectivity is
ordinary pins and whose decision structure (Yosys's `RTLIL::Process`) is an opaque parameter.
v1 proposed process objects that referred to nets by ID without pins; the review (F1) showed that
this breaks IR-3 (every driver is a pin) and every query built on it, so v2 adopts its
representation. Nothing in IR-3, IR-4, IR-6, IR-15 or check rule 4 changes.

**The cell type** (`src/ir/cells/proc.c`, IR-19):

| Item | Definition |
|---|---|
| name, granularity | `$proc`, `word` (the RTLIL view allows it; the netlist view, which needs `bit`, does not, so `lower` cannot meet one) |
| flags | `ODIN3_CT_PROCESS` (new type flag: the IR keeps a per-module count of live nodes whose type has it) |
| ports | `I` (input, width `I_WIDTH`): every net the process reads and does not drive — action right sides, switch signals, compare operands, sync signals, and the constants it uses (pins on the module's shared `$_CONST0_/1/X/Z_` nets, IR-4); `O` (output, width `O_WIDTH`): every net the process drives |
| parameters | `I_WIDTH` (INT ≥ 0), `O_WIDTH` (INT ≥ 1), `BODY` (a new value kind `ODIN3_VAL_PROC`, below) |
| hooks | `verify` validates `BODY` against the widths (below); `const_value`, `simulate` and the netlist writer hooks are NULL |

Each net appears on at most one `I` pin and at most one `O` pin of one `$proc`; a net never
appears on both (reading a net the process drives is written `OUT | k` in the body, not as a
self-loop pin). So `odin3_net_driver` of a process-driven net is its `O` pin, `odin3_net_sinks`
of a net the process reads include its `I` pin, fanout walks and `odin3_net_const_value` work
unchanged, and a second driver (another process, a cell) is rule 4's error as for any cell.

**The body** (`ODIN3_VAL_PROC`, stored in the module arena like `COVER`, IR-10; immutable once
the node exists):

```c
/* A position in a process body: an input pin, an output pin, or "anything" (compares only). */
typedef uint32_t odin3_pbit;
enum {
    ODIN3_PBIT_IN = 0U << 30,     /* | k: the net on I pin k */
    ODIN3_PBIT_OUT = 1U << 30,    /* | k: the net on O pin k (read: its current value) */
    ODIN3_PBIT_ANY = 2U << 30,    /* '-': matches any value; compare patterns only */
    ODIN3_PBIT_INDEX_MASK = (1U << 30) - 1
};
typedef struct odin3_pspan { uint32_t first, len; } odin3_pspan;        /* into pbits[] */

typedef enum odin3_sync_type {
    ODIN3_SYNC_POSEDGE = 1, ODIN3_SYNC_NEGEDGE,   /* edge of one net */
    ODIN3_SYNC_LEVEL0, ODIN3_SYNC_LEVEL1,         /* while the net is 0 / 1 (made by 2E; Yosys ST0/ST1) */
    ODIN3_SYNC_ALWAYS                              /* combinational (Yosys STa) */
} odin3_sync_type;

typedef struct odin3_proc_action { odin3_pspan lhs, rhs; odin3_prov_id prov; } odin3_proc_action;
typedef struct odin3_proc_case {
    uint32_t first_compare, n_compares;   /* pspans in compares[], each the switch's width */
    uint32_t first_action, n_actions;     /* in actions[] */
    uint32_t first_switch, n_switches;    /* in switches[] */
    odin3_prov_id prov;
} odin3_proc_case;
typedef struct odin3_proc_switch {
    odin3_pspan signal;
    uint32_t first_case, n_cases;         /* in cases[] */
    uint16_t flags;                       /* ODIN3_SWITCH_FULL_CASE | ODIN3_SWITCH_PARALLEL_CASE */
    odin3_prov_id prov;
} odin3_proc_switch;
typedef struct odin3_proc_sync {
    uint8_t type;                         /* odin3_sync_type */
    odin3_pbit signal;                    /* IN | k or OUT | k for edge and level types; 0 for ALWAYS */
    uint32_t first_action, n_actions;     /* in actions[] */
} odin3_proc_sync;
typedef struct odin3_proc_body {          /* the BODY parameter; cases[0] is the root */
    const odin3_pbit *pbits; uint32_t n_pbits;
    const odin3_pspan *compares; uint32_t n_compares;
    const odin3_proc_action *actions; uint32_t n_actions;
    const odin3_proc_case *cases; uint32_t n_cases;
    const odin3_proc_switch *switches; uint32_t n_switches;
    const odin3_proc_sync *syncs; uint32_t n_syncs;
} odin3_proc_body;
```

(Implementation note: `src/ir/proc.h` defines the four `ODIN3_PBIT_*` values as macros with
exactly these values, because a C17 enumeration constant must fit an `int` and `2U << 30` does
not.)

Body positions are pin positions, never net IDs, so merging, renaming and compacting nets never
touch a body. The only non-pin entry is `ANY`; every constant 0/1/x/z is an `I` pin on the
shared constant net (IR-4; the review's F8), so `odin3_net_const_value` stays the one constant
predicate and `proc_arst` finds reset values the same way it finds any constant.

**Reads of `OUT` positions** (the re-review's N6; 2E depends on this reading): in an action's
`rhs`, a switch signal or a compare, `OUT | k` is the **current value of the net on that pin**,
exactly as an RTLIL process reads a wire it assigns — for a sync output, the register's held
value; for a temporary, the value the case tree computed for it — never "the value being assigned
in this evaluation". So `q <= q + 1` reads the register, and the root hold action `t0(q) = q`
reads it too.

**Semantics** (RTLIL's): a case applies its actions in order, then its switches in order; a
switch takes the first of its cases whose compare list has an entry equal to the switch signal
(`ANY` bits match anything; a case with no compares is the default and always matches); a later
assignment to an `OUT` position overrides an earlier one. The root case runs whenever the process
is evaluated; on each sync event (an edge of its signal, a level, or `ALWAYS`: continuously) the
sync's actions copy `rhs` into `lhs` — they are the storage (or, for `ALWAYS`, the combinational
outputs). An `OUT` position assigned in the case tree but by no sync action is a combinational
value the process computes (2D's temporaries, EL-35); `proc` drives its net with the mux tree.

**`verify`** (run by check rule 5, IR §9, for every `$proc` node; FAST and FULL like rule 5): the
payload has kind `PROC`; every index is in range and every case, switch and action is used
exactly once (a tree); the root case has no compares; each compare and its switch signal have
the same width; an action's `lhs` and `rhs` have the same width, not 0; `lhs` holds only `OUT`
positions, none twice in one `lhs`; `ANY` appears only in compares; `IN | k` has `k < I_WIDTH`,
`OUT | k` has `k < O_WIDTH`; a sync's signal is one `IN` or `OUT` position for edge and level
types (an `OUT` position is a block clocked by a net it assigns, `always @(posedge a) a <= ~a;`,
which Yosys accepts as a flip-flop clocked by its own output; the re-review's N1) and 0 for
`ALWAYS`; every `OUT` position is the `lhs` of at least one action (a pin the body never
drives is rejected). No new check rule exists; rules 1–11 apply to `$proc` nodes as to any node.

**What the IR operations do with a `$proc` node** — all as for any cell, by construction:

- `odin3_node_create_connected` creates it (2D builds the body off-IR, then calls once);
  `odin3_node_delete` removes it (2E, after lowering); `odin3_node_replace` replaces it by one
  with the same port widths (2E's `proc_clean`/`proc_arst`-style rewrites; different widths:
  delete and create).
- `odin3_net_merge` moves pins, and a body refers to pins: nothing to rewrite.
  `odin3_net_delete` needs a pinless net, so it can never delete a net a process uses.
- `odin3_module_compact` renumbers the node and its pins like any node; the body needs no map. A
  dead `$proc` gets the usual tombstone (IR-6: kind node, type `$proc`, its parameters — the body
  included — and the nets its pins were on). IR-6's tombstones were sized for names and
  parameters; a body is larger, and the cost is accepted: about 0.05 GB on lenet.v after `proc`
  (16,314 bodies; §15), in exchange for history that still shows what each process was (the
  re-review's N7).
- Attributes use `ODIN3_OBJ_NODE` (the `always` block's `(* *)` attributes go there; a switch's
  are its two flags); provenance is the node's SOURCE record, with per-action, per-case and
  per-switch records inside the body (EL-45).
- `odin3_module_proc_count(module)` and `odin3_design_has_processes(design)` read the counter
  (O(1) and O(modules)).

**Passes and processes** (the review's F10; amends 1D). `odin3_pass_def` gains
`uint8_t processes`: `ODIN3_PASS_PROCESSES_REFUSE` (0, the default, so every existing and plugin
pass is safe) or `ODIN3_PASS_PROCESSES_OK`. Before running a `REFUSE` pass the pass manager calls
`odin3_design_has_processes` and, when it is true, fails the pass without running it:
`pass 'flatten': module 'm' has processes; run proc first` (`ODIN3_ERR_INVALID_ARG`, the script's
misuse). Built-ins marked `OK`: the readers (`read_blif`, `read_techlib`, `read_verilog`, which
create processes), `check`, `stats`, `compact`, `hierarchy`, `write_rtlil`, `write_verilog_ast`,
and `proc` (2E), and (v4.2) the eight opt passes of 2F (`opt`, `opt_expr`, `opt_merge`,
`opt_muxtree`, `opt_reduce`, `opt_share`, `opt_dff`, `opt_clean`), and 2B-3's project passes
(`read_project`, `dump_project`, `preprocess`, which never read the IR as logic; plan point (x)).
Marked `REFUSE`: `flatten`, `lower`, `abc`, `sim`, `write_blif`, `write_json`, `write_verilog`,
`write_dot`, and every plugin pass that does not declare `OK`. The refusal comes before the pass
run is opened, so a refused pass leaves no pass run (plan point (i)). The writers also refuse a
design with processes themselves (`module 'm' has processes; run proc first`, before the path is
opened) for callers that bypass the pass manager. (v4.2, mode P: v1–v4.1 also listed the opt
passes as `REFUSE`, to keep `opt` after `proc`; that order is no longer enforced.)

The opt passes treat a module with processes as Yosys 0.55's do (v4.2): `opt_expr`,
`opt_merge`, `opt_reduce`, `opt_share` and `opt_dff` treat every `$proc` node as opaque (never
evaluated, merged or deleted; its `O` pins drive unknown values, its `I` pins read), and
`opt_muxtree` and `opt_clean` skip a module that still has live `$proc` nodes, with an info line
(Yosys's `has_processes_warn`).

A pass marked `OK` that mutates the IR must treat a `$proc` node as opaque: no constant
propagation or CSE through it, its `O` pins are drivers like any cell's. ABI: `odin3_plugin_pass` has `void *reserved[4]`; the first slot
becomes `const uint8_t *processes` (NULL, as every existing plugin passes, means `REFUSE`), so no
layout changes. `hierarchy` in the `OK` list is the 1D built-in (`src/passes/builtin.c`,
`docs/PASSES.md`).

**ABI.** The `$proc` node is visible through the existing node accessors (type `$proc`, ports
`I`/`O`, `I_WIDTH`/`O_WIDTH`); `odin3_node_get_param_text` prints `BODY` as RTLIL `process` text
(EL-48), so the Python walk and plugins can see it; structured body accessors are deferred
(2E §24 item 8).

**Alternatives considered.** (a) v1's process objects over pinless nets (rejected by the review,
F1: a second connectivity mechanism, broken driver queries, O(process) merges, `opt` unsafe);
(b) keeping `always` bodies in the elaborated AST for `proc` to read (the IR would depend on a
store AST-16 frees, and `proc` would redo 2D's width and blocking analysis); (c) 2D lowering
processes to muxes itself (merges 2D and 2E and loses the structure `proc_arst`, `full_case` and
latch detection need).

### 8.2 Event controls and syncs (EL-34)

From the `ALWAYS`'s `EVENT_CONTROL`:

- `@(posedge a or negedge b …)` (T1): one sync per edge event, in order, `POSEDGE`/`NEGEDGE` on
  the event expression's net. The expression is generated as cells (it may be any 1-bit
  expression; gap 33's `wire pos_clk = CLK == CLK_POLARITY` is just a net). A wider expression is
  "posedge on 'x', which is 4 bits; a 1-bit signal is needed" (Yosys's error; IEEE would use the
  LSB).
- `@*`, `@(*)` (T2), and any list of level events (`@(a or b)`, `@(a, b)`, `@a`, T4): one
  `ALWAYS` sync. A level list that omits a signal the block reads (bits read before assignment,
  computed by the generator) is T3: "incomplete sensitivity list: 'x' is read but not listed; the
  block is built as @*" (once per block, naming the first such signal by name order). This
  analysis is 2D's own: Yosys prints a generic note for **every** level-event list ("Assuming
  pure combinatorial block … use of @* instead of @(...)") and never names a missing signal.
- Edge and level events in one list: "edge and level events in one event list are not supported"
  (Yosys: "Found non-synthesizable event list").
- `always` with no event control (T6, accepted: below), an event control inside the body (T12),
  `wait` (T13), `->` (T14), `fork` (T15), procedural `assign`/`deassign`/`force`/`release` (T16), `disable`
  (T19), `forever` (F4): stage 1's errors (§9.1). Delays (T11) were dropped with a warning.
- **T6 (accepted, Peter 2026-10-13, 2D-7b)**: an `always` (not `always_ff`/`always_comb`/
  `always_latch`) whose statement is not led by an event control, with no timing at all or with a
  delay (`always #5 clk = ~clk`, the delay itself T11's warning), is `always @*`: stage 1 writes
  `ALWAYS(TIMING_STMT(EVENT_CONTROL[star], body))`, the one form the elaborated store's check
  allows, and the generator builds the one `ALWAYS` sync of `@*` (EL-34, T2). Warning at the
  `ALWAYS`: "always without an event control treated as always @*" (§9.2, once per parsed
  node). This is Yosys 0.55's reading (a process with no sync rule is combinational). A delay
  in front of an event control (`always #5 @(a) …`) is not T6: the inner control is T12's error.

### 8.3 The process generator (EL-35)

`odin3_pg` builds one `$proc` node (EL-33) per `always` block (and per continuous-context
function call, EL-38, and per mem2reg read outside a process, EL-27), following Yosys's
`ProcessGenerator` step for step so that blocking and non-blocking semantics, and the structure
`proc` sees, are Yosys's (T7, T8; coverage §7 note 3). It works on an off-IR builder (cases,
switches, actions in vectors, whose signal bits are nets or the constants 0/1/x/z/`ANY`) and
creates the node at the end (step 7).

1. **Outputs.** Collect the block's lvalue bits (every bit any blocking or non-blocking
   assignment in it can write, selects resolved; a `case`-rewritten non-constant select counts
   all its positions), excluding process-local variables (below); sort by net ID. These are the
   block's **outputs**: the bits its sync actions drive (Yosys's `collect_lvalues(…, true,
   true)`, so a blocking-only temporary such as `t` in `t = a & b; r <= t;` is an output too,
   whose register is dead and removed by `opt`). For each output bit `b` create a fresh
   temporary net `t0(b)` (Yosys's `$0\x`). **Chunks** (v4.2): the sorted outputs split into
   maximal runs of consecutive bits of one variable in increasing bit order (nets of one wire are
   created consecutively, so a chunk is a run of the net-ID-sorted list broken at a change of
   wire or a gap in bit position; O(outputs)); `t0` of a chunk is that chunk's temporary group
   (Yosys's one `$0\x[h:l]` wire per chunk).
2. **Substitutions.** `lv`: bit → the net an assignment writes (initially `b → t0(b)`).
   `rv`: bit → its current value for reads in this block (initially empty: a read sees the net
   itself). Both are maps with an undo log, so a branch can save and restore them in O(changes).
3. **Root defaults.** For each output chunk `c`, one root action `t0(c) = c` (the value holds
   when no path assigns it — a latch for a combinational block, a flip-flop hold for a clocked
   one) — except when `--nolatches` is set and the block has no edge sync: then no root action,
   and `rv(b) = x` for each output bit (Yosys `-nolatches`: an unassigned path is x, never a
   latch).
4. **Syncs.** EL-34's syncs, each with one action `c ← t0(c)` per output chunk, in the outputs'
   order (v4.2; so `proc` builds one register per chunk, as Yosys does).
5. **Statements**, in order, with a frame stack (`odin3_pg_frame`: block, case, loop, call):
   - **Blocking `lhs = rhs`**: `rhs` generated at `max(L(lhs), L(rhs))` with reads through `rv`,
     truncated or extended to `L(lhs)`; `lhs` resolved to bits (a bit written twice in one lvalue
     keeps its first occurrence, Yosys); for each bit `rv(bit) = rhs bit`; the target bits
     `lv(lhs)` are removed from the current case and the switches nested in it (below), then the
     action `lv(lhs) = rhs` is appended to the current case, split into one action per chunk of
     `lv(lhs)`: a maximal run of its bits, in lvalue order, that are consecutive bits of one
     temporary group (v4.2, Yosys's `addChunkActions`; `{a, b} = x` gives two actions).
   - **Non-blocking `lhs <= rhs`**: the same without updating `rv` (a later read still sees the
     value at the block's start, T8).
   - **`begin … end`**: its statements in order; a named block's locals are module-level
     variables (EL-23) and ordinary outputs.
   - **`if`, `case`**: EL-36. **Loops**: EL-37. **Task calls** and calls in expressions:
     EL-38. **Memory reads and writes**: EL-39. **System tasks**: removed in stage 1 (S3).
6. **Removal** keeps, per target bit, the list of action slots that assign it, so "remove `x`
   from the current case and its nested switches" costs the number of earlier assignments to
   `x`, not the size of the tree (Yosys walks the tree; same result).
7. **Emission.** A block with no outputs and no temporaries (`always @* ;`, or a block whose
   only statements were system tasks S3 removed) creates **no node** and no warning beyond S3's
   (the re-review's N2: `O_WIDTH` must be at least 1). Otherwise every net some action assigns — the outputs (sync-action `lhs`) and every
   temporary (`t0`, case temporaries, call-frame and memory-write variables, select positions) —
   becomes an `O` pin, in net-ID order; every other net the body reads becomes an `I` pin, and
   each constant 0/1/x/z the body uses becomes one `I` pin on the module's shared constant net
   for that value (IR-4; at most four). The `I` pins, constant ones included, are in net-ID order
   (v4.3), and `ANY` stays in compares. The builder's bits
   are renumbered to `IN | k` / `OUT | k`, and one `odin3_node_create_connected` creates the
   `$proc` node with `I_WIDTH`, `O_WIDTH`, `BODY` and the two net vectors. Before that call the
   driver table (EL-28) is checked for every `O` net (T10/A4 are reported and the conflicting
   bit's actions dropped, so the node never makes a second driver).

**Process-local variables** never become outputs (they have no sync action): temporaries 2D
creates (case temporaries, EL-36; call frames, EL-38; select positions, EL-32; memory write
ports, EL-39) — whose nets are still `O` pins, because the process drives them and cells read
them — and **loop variables**, which the process does not drive at all (their values live only in
`rv`) — a variable whose every assignment in the block is a `for` initialisation or step
(EL-37) — unless something outside the block reads it, in which case it is an ordinary output.
Yosys makes loop counters outputs (a dead register `opt` removes) and so drives a shared
`integer i` from every block that loops on it; 2D's rule avoids that false T10 conflict and is
equivalent on every output (§17 #5).

Cost: O(statements × lvalue bits) plus the removals; the generator's memory is freed per block.

### 8.4 `if`, `case`, `casez`, `casex`; `full_case`, `parallel_case` (EL-36)

- **`if (c) s1 else s2`** is a switch on `$reduce_bool(c)` (or `c` when 1 bit) with a case
  `1'b1` → `s1` and a default case → `s2` (Yosys's parser builds `if` this way). A constant
  condition (0/1 only) keeps only the taken branch, no switch.
- **`case (e)`**: the switch signal is `e` and every compare is an item expression, all generated
  at the case width and sign (EL-13: the widest of `e` and all items, signed only if all are
  signed), reads through `rv`. Item expressions may be non-constant (C8: `case (1'b1)`).
  Patterns: in `casez` every `z`/`?` bit of a constant item becomes `ANY`; in `casex` every `x`
  and `z` bit; in `case` x and z bits stay as written (C9: Yosys's behaviour; `proc` decides what
  they match). Several expressions per item (C4) are several compares of one case. `default`
  (anywhere, C6) becomes the last case (Yosys). A switch whose selector and items are all constant
  by the AST (literals and parameters; v4.21) and known (no x or z) folds to the first item that
  matches, else the default item, else nothing runs; it makes no switch, and the dead items'
  assignments are not outputs (§17 #56).
- **Case temporaries** (Yosys's `this_case_eq_ltemp`): for a switch, the bits its branches can
  assign by *blocking* assignment get fresh temporaries `t1`; each case of the switch starts with
  `t1 = rv(bits)` and runs with `lv(bits) = t1`, saving and restoring `lv`/`rv` around each case;
  after the switch, `rv(bits) = t1` and the current case gets `lv(bits) = t1` (after removing
  earlier assignments to `lv(bits)`). Non-blocking assignments in a branch write `lv` directly
  (the root temporary), relying on "switch overrides action". **Chunks** (v4.2): `bits` are
  sorted (as the outputs, EL-35 step 1) and split into chunks, `t1` is one temporary group per
  chunk (Yosys's `new_temp_signal`: one `$1\x[h:l]` wire per chunk), and the entry `t1 = rv(bits)`,
  the exit `lv(bits) = t1` and the added default below are each one action per chunk
  (`addChunkActions`). An 8-bit `q` assigned in a `case` gives one 8-bit action per case, so
  `proc` builds one 8-bit `$pmux`, not eight 1-bit ones.
- **Default**: when the case has no `default` item and the branches assign something by `=`, a
  default case is added that assigns `t1 = rv(bits)` (unchanged); with `full_case` and no
  `default`, it assigns x instead (Yosys); one action per chunk either way (v4.2). With nothing to
  assign no default is added (§17 #57), except under `full_case`: the empty default case is still
  opened so the unlisted values hold (`<=` arms), as in Yosys (v4.24, review C1).
- **`full_case` and `parallel_case`** (C10, D1), from `(* *)` attributes or `// synopsys`
  metacomments on the `CASE` (2A §7, 2C §2.6): they set the switch flags `FULL_CASE` and
  `PARALLEL_CASE` and are honoured as Yosys honours them (the x default above; `proc` builds
  `$pmux` and prunes per the flags). 2D emits D1's per-use warning, located at the attribute, since
  it is the stage that reads the attribute: "'full_case' is honoured: case values no item lists assign x" and
  "'parallel_case' is honoured: case items are not prioritised" (amends 2A §7, which gave the
  warning to 2E).

### 8.5 Loops (EL-37)

- **`for (i = a; c; i = s) body`** (F1) is unrolled: `a` is evaluated (it may read constants and
  the values of variables the generator knows to be constant at this point: `rv` bits that are
  constants); `i` is bound to it in a loop frame (reads of `i` inside see the constant); `c` must
  evaluate to a constant ("for-loop condition is not constant (it reads 'x')"); while it is true,
  `body` is processed, then `s` is evaluated and bound. The loop variable may be `integer` or
  `reg` of any width (the binding is converted to its width and sign, so a `reg [1:0] i` loop
  `i < 4` never ends and hits the cap — as in simulation); counting down past 0 with a signed
  `integer` works (F1, gap 18). After the loop, `i` holds its final value: `rv(i)` is set and,
  when `i` is an output (EL-35), an action assigns it. A blocking assignment to `i` in the body is
  "loop variable 'i' is assigned in the loop body". `init`/`step` that do not assign the same
  variable: "for-loop initialisation and step must assign the same variable".
- **`repeat (n) body`** (F3): `n` is a `NUMBER` after stage 1; the body is processed `n` times.
- **`while`** outside a constant function: F2's error. **`forever`**: F4's error.
- Caps: `ODIN3_ELAB_MAX_LOOP_ITERATIONS` per loop and `ODIN3_ELAB_MAX_TOTAL_ITERATIONS` per run
  (EL-46), each a located error at the loop.

### 8.6 Functions and tasks (EL-38)

Non-constant calls are **inlined** (K1–K11), as Yosys's `simplify` does:

- **In a process**, before the statement that contains the call: a call frame maps every port,
  local and the result variable of the function (or task) to fresh process-local temporaries
  (unnamed nets of the declared widths and signs); the inputs are assigned from the arguments by
  blocking assignment (context = the formal's width); the body is processed in the frame (its
  statements run with the same `lv`/`rv` machinery, so `if`/`case`/loops inside work, K4, K5);
  then, for a task, each `output`/`inout` formal is copied to its actual by blocking assignment
  (K9; the actual must be a variable lvalue: "output argument 2 of task 't' must be a variable");
  for a function, the call's value in the expression is `rv(result)`. Calls inside the body
  (K6, K11) nest frames. Function-local variables never become outputs or flip-flops (Yosys's
  `nosync`).
- **In a continuous context** (an `assign` right side, a port connection, a net declaration
  assignment): a combinational process of its own (one `ALWAYS` sync) computes the call into a
  fresh temporary, and the expression reads the temporary (Yosys wraps the call in
  `always @*`).
- **Constant arguments**: the interpreter is tried first (EL-17); a constant result replaces the
  call. A function whose body is constant-only (`while`, recursion) and whose arguments are not
  constant is "function 'f' can be called only with constant arguments (it uses 'while')".
- **Recursion** without constant arguments cannot be inlined: "recursive call of 'f' cannot be
  inlined (its arguments are not constant)". Inlining depth is capped at
  `ODIN3_ELAB_MAX_INLINE_DEPTH` (1024; or `ODIN3_ELAB_MAX_CALL_DEPTH` if lower), a resource cap on
  heap records: a call is inlined from an explicit frame stack (2D-7; the walker is suspendable and
  the statement code pushes a frame for each expression and each call), so one call inside another
  costs no C stack. Only an index, a case selector or item, or a copy-out target that holds a call is
  still run by a nested frame loop (C recursion, capped at 128 levels; §17 #74).
- `automatic` (K8) changes nothing for inlining (every call gets fresh variables anyway).
- K12 (a timing control or a non-blocking assignment in a function) is stage 1's error. Argument
  count must match ("function 'f' has 2 inputs; 3 arguments given"); a task enable without
  parentheses (K10) has zero arguments.

### 8.7 Memories in processes (EL-39)

For an array kept as a memory (EL-27):

- **Read** `m[i]` (anywhere, rvalue): a `$mem` read port with `ADDR` = the raw index (through
  `rv`; EL-27: `$mem` subtracts `OFFSET` itself); the expression reads the port's `RD_DATA`
  nets.
- **Write** `m[i] <= d` / `m[i] = d` (only in a block with one edge sync; elsewhere EL-27 made it
  registers): up to three process-local variables per write statement — `addr` (the wider of
  the index expression and `ABITS`), `data` (the memory's width) and `en` (the memory's width) —
  with the statements `addr = x`, `data = x` and `en = 0` inserted at the block's start, in that
  order (v4: Yosys 0.55 inserts all three, `simplify.cc` 3167–3231, so every path assigns the
  temporaries and `proc` builds no feedback loop through them). They are blocking assignments
  processed like any other (EL-35/EL-36's removal rule), so under a conditional write the x/x/0
  values end in the switch's default case and the root assigns the case temporaries, and an
  unconditional write leaves the root with `en = all ones` alone; a constant index needs no `addr`
  variable and a constant full-word value no `data` variable (Yosys's `isConst` branches): the
  constant goes to the port directly. The write statement assigns `addr = i` (the raw index),
  `data = d`, `en = all ones`. The variables are process-local temporaries in EL-33's sense: `OUT` positions
  assigned in the case tree and by no sync action, so their nets are `O` pins of the `$proc`
  node that `proc` drives combinationally; the `$mem` write port's `WR_ADDR`, `WR_DATA`, `WR_EN`
  are those nets (their values at the end of the block), `WR_CLK` the sync's net,
  `WR_CLK_POLARITY` its edge. A partial write `m[i][3:0] <= d` sets `en` on those bits only
  (its other bits of `data` are don't-care x).

### 8.8 Initial blocks and initial values (EL-40)

D2: constant `initial` assignments, declaration initialisers and `$readmemh/b` set flip-flop and
memory initial values; other `initial` content is ignored with a warning. Stage 2 runs every
`INITIAL` (and every `VAR_DECL` initialiser, as `initial name = value;` at its position) through
the **interpreter** (EL-17), after the always-assigned bit set is known (EL-28 step 2), in source
order:

- Interpreter state: one 4-state value per variable bit and memory word written so far; reads of
  a bit not yet written by any `initial` see x; reads of anything that is not a constant or a
  value the `initial` blocks computed (an input, a net driven by logic) make the **statement**
  non-constant: "initial statement is not constant and is ignored" (once per statement, D2), its
  targets left as they were. Blocking and non-blocking assignments both just set the value.
  Loops, `if`, `case` and constant function calls work as in EL-17; memory words take constant
  indices (a non-constant index makes the statement non-constant).
- `$readmemh("file", m [, start [, finish]])` / `$readmemb` (S4): the file is resolved against the
  directory of the source file that contains the call (DESIGN §4.0: never the working directory;
  Yosys tries the working directory first — divergence §17 #6); its words are parsed as Yosys's
  `readmem` does (blank-separated tokens, `//` and `/* */` comments, `@hex` addresses, each word
  a sized literal of the memory's width in base 16 or 2), filling from `start` (default: the
  lowest index) toward `finish` (default: the highest). Errors: "cannot open 'f' for $readmemh",
  "bad address '@zz' in 'f' line N", "bad word 'g' in 'f' line N"; a file above 2^28 bytes is
  EL-46's cap. `$readmem` outside `initial` is "$readmemh is supported only in an initial
  block".
- **Results**, per bit or word with a known (0/1/z) value:
  - an output bit of some `always` block, or a memory word kept as a memory → its initial
    value: the `init` attribute (BITS, x where unknown) on the wire (Yosys's convention, which
    `proc` turns into flip-flop init values and the JSON writer already uses) or the `$mem`'s
    `INIT` words;
  - a variable that nothing else drives (no `always`, `assign`, instance, port input) → a
    constant driver (the bit is merged into the constant net; Yosys's `initial`-only process has
    the same effect);
  - a net driven by anything else (a continuous assignment, an instance, an input port) → the
    value is ignored with "initial value of 'x' is ignored: 'x' is driven by a continuous
    assignment" (or `an instance`, `an input port`).
- **A non-constant statement under `--vtr-compat`** (Peter 2026-10-13; §17 #85). Yosys 0.55 reads
  an `initial` block as a process without a sync rule, so `initial y = in;` leaves `connect \y
  \in` (verified on the 0.55 of VTR; Parmys writes `.names in y`), and a variable that an `always`
  block also assigns is its error `Failed to get a constant init value for \y: \in`. With the
  option, a statement that the interpreter finds not constant, that is a `=` or `<=` assignment
  (an optional delay is ignored, as Yosys ignores it) to a variable, or to a constant bit or part
  select of one, and whose target bits no `always` block assigns and nothing else drives, is the
  continuous assignment `assign x = e;` (made through EL-28's assignment, so its width rules and
  net merges are those), with the warning "initial value of 'x' is not constant: treated as a
  continuous assign (VTR/Parmys compatibility)" at the statement. Any bit of the target that an
  `always` block assigns, or that a continuous assignment, an instance, a gate or an input port
  drives, makes the statement keep the ignore, with "initial value of 'x' is not constant and is
  ignored: 'x' is also assigned in an always block (VTR/Parmys compatibility does not apply)" (or
  `driven by a continuous assignment`, `an instance`, `a gate`, `an input port`): Yosys stops
  there, Odin III warns and goes on. Other statements (an `if`, a loop, a concatenation or
  memory-word target, a variable select) and variable initialisers (`reg r = in;`) keep the
  ignore with D2's warning. The statements are taken in source order, so `y = a; y = 1;` (value
  first not constant, then constant) is the assignment, and the constant is then reported ignored
  ("driven by a continuous assignment"); Yosys keeps the last. Without the option, nothing changes.
- `$display`, `$finish` and friends in `initial` were removed with S3's warning.

### 8.9 Per-bit assignment rules (EL-41)

Checked by stage 2 per net bit after lvalue resolution, before the conflicting action or driver
is attached (EL-28), with both locations from the symbol table's assignment lists (2A §8):

- **T10**: a bit that is an output of two `always` blocks: "'r[2]' is assigned in two always
  blocks (also at f.v:14:9)". (Different bits of one variable in different blocks are legal.)
- **T9 (accepted, Peter 2026-10-13, 2D-7b)**: a bit assigned by both `=` and `<=` (in one block
  or several, source assignments only; inlined task outputs count as `=`) follows Yosys 0.55:
  every assignment is an action and the last one on a path wins (the `=` also updates the value
  later statements of the block read, EL-35). A warning names the variable, at the later
  assignment of the other kind, once per variable: "'r' is assigned with both '=' and '<='
  (also at f.v:20:5)". When the other kind's first assignment can run in the same evaluation of
  the same block (the root, one an arm and the other after its switch, arms of different
  switches; not different arms of one `if` or `case`), the warning is "'r' is assigned with both
  '=' and '<=' on one control path (also at f.v:20:5); simulation and synthesis may differ"
  (simulation applies a nonblocking update after the blocking ones, Yosys's last action wins:
  `q <= b; q = a;` is `q = b` in a simulator and `q = a` in the netlist). Both warnings may come
  once per variable. Each new assignment is compared with every recorded assignment of the
  other kind to the bit in the block (one record per distinct control path, so the cost is bounded
  by the block's structure), so a same-path pair after an exclusive first pair also gets the
  "simulation and synthesis may differ" warning. Two
  blocks assigning one bit remain T10's error.
- **A4**: EL-28. A bit driven by a process and by a continuous assignment, gate or instance is A4.
- Loop variables (EL-35) and function and task locals are exempt: they are not outputs.

## 9. Diagnostics

Every diagnostic goes through `odin3_diag(design, level, loc, fmt, …)` (2A §3.4): `file:line:col:
error|warning|info: message` at the file location of `loc`, then the macro and include chain.
Messages start in lower case, quote names in `'…'`, and end without a period (2B/2C style).
DESIGN §4.0 kinds are written `<kind>: <detail>` (2B §8), so the project-corpus harness reads the
kind; unlocated ones print `odin3: error: <kind>: <detail>`.

### 9.1 Errors (EL-42)

The REJECT rows, with the exact text the `tests/verilog/reject-2d/<row>.v` + `.err` handover
cases (2C §7.3) expect, at the node named:

| Row | Located at | Message |
|---|---|---|
| L9 | the `REAL` | `real number in an expression is not supported` |
| M6 | the `PORT_REF` | `port expression in a module port list is not supported` |
| M11 | the second declaration | `'x' is already declared (first at f.v:L:C)` (ports, nets, variables, parameters, functions, tasks, instances, named and generate blocks; duplicate modules are 2C's `duplicate_module`) |
| N17 | the `VAR_DECL` | `variable type 'real' is not supported` (`time`, `realtime`) |
| N18 | the `EVENT_DECL` | `named event 'e' is not supported` |
| E20 | the `CALL` | `system function '$random' is not supported` |
| E21 | the `HIER_NAME`/`CALL`/`TASK_CALL` | `hierarchical reference 'u.w' is not supported` |
| A4 | the second driver | `'y[3]' has two drivers (also driven at f.v:L:C)` (a whole-net conflict names the net: `'y' has two drivers …`) |
| T10 | the later block's assignment | `'r[2]' is assigned in two always blocks (also at f.v:L:C)` |
| T12 | the inner `TIMING_STMT` | `event control inside an always body is not supported` |
| T13 | the `WAIT` | `'wait' is not supported` |
| T14 | the `EVENT_TRIGGER` | `event trigger '->' is not supported` |
| T15 | the `PAR_BLOCK` | `'fork ... join' is not supported` |
| T16 | the `PROC_CONT_ASSIGN` | `procedural 'assign' is not supported` (`'deassign'`, `'force'`, `'release'`) |
| T19 | the `DISABLE` | `'disable' is not supported` |
| F4 | the `FOREVER` | `'forever' is not supported` |
| K12 | the statement | `timing control in function 'f' is not supported` / `nonblocking assignment in function 'f' is not supported` |
| I2 | the `INSTANCE` | `instance 'u' mixes ordered and named connections` |
| Q6 | the `GATE_DECL` | `switch-level primitive 'nmos' is not supported` (all MOS and pass kinds); `'pullup' is not supported` / `'pulldown' is not supported` |
| Q7 | the `UDP_DECL` | `user-defined primitive 'p' is not supported` (at the definition, whether or not it is instantiated) |
| S2 | the `CONFIG_DECL` | `configuration 'c' is not supported` |
| N15, P4 | the declaration | `net type 'tri0' is not supported` (`tri1`, `trireg`); `parameter type 'real' is not supported` (`realtime`, `time`) |
| F2 | the `WHILE` | `'while' is supported only in constant functions` |

(L14 is 2B's error and M11's duplicate module is 2C's `duplicate_module`; 2D raises every other
REJECT, including M11's other duplicates. The `reject-2d/` handover holds 23 cases: the 20 rows
other than L14, M11, T6 and T9, 2C plan point (i), and the REJECT parts of N15, P4 and F2
tabulated above. T6 and T9 are accepted since 2D-7b, with a warning each: `ignore-2d/T6` and
`ignore-2d/T9`.)

Other errors, by area (exact texts; `N`, `M`, names and locations filled in):

- **Hierarchy (EL-6–EL-11)**: `unresolved_module: module 'm' is not defined` (at the
  instantiation's module name); `unknown_top: 'm' is not a defined module`; `ambiguous_top: 'm'
  is defined in several libraries: 'a.m', 'b.m'`; `ambiguous_top: no top given and several
  modules are not instantiated: 'work.a', 'work.b'`; `no_top: no top given and every module is
  instantiated by another`; `no_top: no top given and no module is defined`; `module 'm' has no
  port 'p'`; `port 'p' of instance 'u' is connected twice`; `instance 'u' of 'm' has N connections
  but 'm' has M ports`; `module 'm' has no parameter 'P'`; `'P' is a localparam of module 'm' and
  cannot be overridden`; `too many parameter values for module 'm' (N given, M parameters)`;
  `parameter assignment of instance 'u' mixes ordered and named values`; `defparam target 'a.W'
  does not name a parameter of an instance below this module`; `param 'top.u.X' does not name a
  parameter` (at the project line); `recursive instantiation of 'm' (through 'a', 'b')`; `instance
  hierarchy deeper than 1024 at 'u'; recursive instantiation of 'm'?`; `cell type 't' has no
  parameter 'P'`; `implicit black box 'm' needs named connections`; `output port 'p' of instance
  'u' must be connected to a net`; `output port 'p' of instance 'u' is connected to a constant`
  (`inout port 'p' …` for an inout, v4.8); `connection of 'p' to instance array 'u' is W bits;
  expected P or N×P`; `connection K of instance array 'g' is W bits; expected 1 or N` (a gate
  terminal, v4.8); `cell type 't' has no port 'p'`; `too many parameter values for cell type 't'
  (N given, M parameters)`; `implicit black box 'm': port 'a' is P bits at its first instance;
  this connection is W bits`; `module 'adder' has the
  name of cell type 'adder' registered by an earlier read (hard cell); rename the module` (or
  `(black box)`), at the `MODULE`'s keyword (EL-6, F9); `duplicate_module: module 'm' is already
  defined by an earlier read (run N)` (EL-6, F27).
- **Constants (EL-12–EL-18)**: `<what> is not constant (it reads 'x')` where `<what>` is one of
  `range bound`, `replication count`, `parameter 'P'`, `generate condition`, `generate loop
  condition`, `for-loop condition`, `case item in a generate case`, `attribute value`, `$clog2
  argument`; `parameter 'P' depends on itself`; `function 'f' is not a constant function: <reason>`
  (reasons: `it reads module variable 'x'`, `it calls task 't'`, `it calls system task '$x'`, `it
  assigns 'x', which is not local`); `constant evaluation of 'f' exceeds 67108864 steps`;
  `constant function recursion deeper than 1024 in 'f'`; `constant evaluation of <what> nested
  deeper than 256` (the C re-entry cap, v4.6; 1024 when the call-depth cap is reached first);
  `replication count N is negative`;
  `replication count has x or z bits`; `range bound has x or z bits` (the binder, v4.9);
  `zero-width concatenation`; `width N exceeds the limit of 1048576 bits`. (An unsized constant in a concatenation is a warning, EL-13.)
- **Generate (EL-19–EL-22)**: `'i' is not a genvar`; `generate loop must assign genvar 'i' in its
  initialisation and step`; `genvar 'i' is already used by an enclosing loop`; `generate loop of
  'i' repeats the value N`; `port declarations are not allowed in a generate block`; `'g[5].t'
  does not exist (generate block 'g[5]' was not elaborated)`.
- **Declarations and ports (EL-23–EL-29)**: `'x' is not declared`; `'u' is not a net, variable
  or parameter` (the binder, v4.9); `'x' is not declared
  (`default_nettype is none)`; `port 'a' is not declared`; `'x' is declared as a port but is not
  in the port list`; `port 'q' is declared with ranges [3:0] and [7:0]`; `port 'q' has a value in both of its declarations`; `'w' is a wired net (wand)
  and cannot be assigned in an always block`; `'u' is a uwire and has a second driver (first at
  f.v:L:C)`; `memory 'm' of N bits exceeds the limit of 1073741824 bits`; `'m' is a memory; select
  a word`; `continuous assignment to 'y' with a non-constant index is not supported`; `non-constant index
  into 'y' is not supported in a concatenation target`; D3's four (EL-29, v4.28): `tri-state drivers on
  internal net 'x' overlap without driving the same bits (D3)`, `tri-state driver on 'x' mixes a top-level
  port bit and an internal net (D3)`, `tri-state driver on 'p' of an inout port of a module below the top
  is not supported (D3)`, `tri-state driver on 'y' of an output port of a module below the top is not supported: the instance may drive a top-level port, where z must stay (D3)`, `net 'x' is driven by inout port 'p' of instance 'u' and by logic at f.v:L:C; an
  inout pin cannot share a net with converted tri-state drivers (D3)`; `net 'x' is driven by
  inout port 'p' of instance 'u' and by logic at f.v:L:C; internal tri-state nets are not
  supported (D3)`; `array 'w' cannot be a memory (it is written in a combinational block) and
  --nomem2reg is set`.
- **Expressions (EL-30–EL-32)**: `part-select [0:3] of 'x' is reversed relative to its
  declaration [7:0]`; `'f' is not a function`; `'t' is not a task`; `function 'f' has N inputs; M
  arguments given`.
- **Processes (EL-34–EL-40)**: `posedge on 'x', which is N bits; a 1-bit signal is needed` (and
  `negedge`); `edge and level events in one event list are not supported`; `for-loop condition is
  not constant (it reads 'x')`; `for-loop initialisation and step must assign the same variable`;
  `loop variable 'i' is assigned in the loop body`; `loop exceeds 1048576 iterations`; `for-loop never ends: 'k' takes the value 7 again`; `loop creates more than 2097152 IR objects (is it endless?)`; `function call nesting deeper than 1024 at 'f'`;
  `unrolled loops exceed 16777216 iterations in this design`; `function 'f' can be called only
  with constant arguments (it uses 'while')`; `recursive call of 'f' cannot be inlined (its
  arguments are not constant)`; `output argument N of task 't' must be a variable`; `cannot open
  'f' for $readmemh`; `bad address '@zz' in 'f' line N`; `address out of range (beyond 2^40) '@...' in 'f' line N`; `an address argument of $readmemh is out of range (beyond 2^40)`; `bad word 'g' in 'f' line N`;
  `$readmemh is supported only in an initial block`.
- **Passes and budget**: `pass 'flatten': module 'm' has processes; run proc first` (the pass
  manager, EL-33; `ODIN3_ERR_INVALID_ARG`); `elaboration needs more than N bytes (the budget is
  60% of physical memory)` (EL-46).

### 9.2 Warnings and infos (EL-43)

The IGNORE rows, as the `tests/verilog/ignore-2d/<row>.warn` cases expect (one per occurrence,
at the construct; a construct elaborated several times — in a generate loop, in a module
specialized twice — warns once, keyed by its parsed node):

| Row | Located at | Warning |
|---|---|---|
| L18 | the `DIRECTIVE` | `` `timescale is ignored `` |
| L21 | the `DIRECTIVE` | `` `celldefine is ignored `` (likewise `` `endcelldefine ``, `` `unconnected_drive ``, `` `nounconnected_drive ``, `` `pragma ``) |
| P7 | the `PARAM_DECL` | `specparam 'p' is ignored` |
| N19 | the node | `drive strength is ignored`, `charge strength is ignored`, `net delay is ignored`, `'vectored' is ignored`, `'scalared' is ignored` |
| A2 | the `DELAY` | `delay on a continuous assignment is ignored` |
| A3 | the `STRENGTH` | `drive strength on a continuous assignment is ignored` |
| T11 | the `DELAY`/intra-assignment control | `delay is ignored` |
| Q4 | the `DELAY`/`STRENGTH` | `gate delay is ignored` / `gate drive strength is ignored` |
| S1 | the `SPECIFY_BLOCK` | `specify block is ignored` |
| S3 | the `TASK_CALL` | `system task '$display' is ignored` (any system task but `$readmemh`/`$readmemb`) |
| T6 | the `ALWAYS` | `always without an event control treated as always @*` (accepted, not ignored: §8.2) |
| D3 | the first tri-state driver of the net | `tri-state drivers on internal net 'data' converted to logic (no driver enabled: x; several enabled: undefined)` (accepted: §6.7; once per net); under `--vtr-compat` and with input-port data on every driver, instead `net 'data' has 4 drivers; --vtr-compat keeps only the data of 'bus0' as Parmys does (...), ignores every enable and drops 'bus1', 'bus2', 'bus3' (IEEE resolution would be a bus selected by the enables: ...)` (§6.7; PHASE2 #27) |
| T9 | the later assignment | `'r' is assigned with both '=' and '<=' (also at f.v:L:C)`, or `... on one control path (also at f.v:L:C); simulation and synthesis may differ` (accepted: §8.9) |

Other warnings: D1's two (EL-36), at the attribute; D2's two (EL-40) and, under `--vtr-compat`, its
two more (`initial value of 'x' is not constant: treated as a continuous assign (VTR/Parmys
compatibility)`; `initial value of 'x' is not constant and is ignored: 'x' is <also assigned in an
always block | driven by a continuous assignment | ...> (VTR/Parmys compatibility does not apply)`,
at the statement, once per statement; §8.8, §17 #85); D5/M12 (EL-24); M10 `'x' is implicitly
declared`; T3 (EL-34); E9's two (EL-30: `'===' is treated as '==' (synthesis has no x or z)`,
`'===' compares against x/z bits; they match any value`); E12's `unsized constant in a
concatenation is taken as N bits` (EL-13); `'a' and 'b' are connected only to each other and are
never driven` (EL-28); I4's resize warnings (EL-10); E17 and out-of-range
selects (EL-32); the mem2reg note (EL-27); `defparam of 'u1.W' is overridden by the named
parameter assignment of 'u1'`; `connection N of instance 'u' goes to an unnamed port position and
is ignored`; `'r' is a variable assigned by a continuous assignment; treated as a net`; `'w' is a
net assigned in an always block; treated as a variable`; `declaration in an unnamed block is a
SystemVerilog extension; the block is named '$unnamed_block$1'`; `` `default_nettype inside a
module takes effect at the next module ``; `assignment to bits outside 'y' [7:0] is ignored`;
`'w' is never driven`; 2A's literal truncation warning; the `$readmem` and initial-value warnings of Task 24 (EL-39, EL-40): `$readmemh of 'f' dropped N words outside the array (the first at address A)` (§17 #68), `$readmemh of 'f' truncated N words wider than the memory's W bits (the first, 'w', at line L)` (§17 #73), `task call in an initial block is not applied and is ignored` (§17 #70), `assignment to a word outside array 'm' is ignored`, `assignment to word N outside array 'm' is ignored`, and `initial value of 'x' is ignored: 'x' is driven by <driver>`. Infos: the top choice, the modules not
elaborated, a module found in several libraries, a black-box body ignored, an implicit black box,
the final summary line (EL-2).

Not a 2D warning (PHASE2 #25, 2F §17 #57): 2D unifies `A_SIGNED` and `B_SIGNED` for the 15 cell types of the
ruling (`$add $sub $mul $div $mod $and $or $xor $xnor $lt $le $gt $ge $eq $ne`), so none of those leaves it
mixed. The shifts, `$shiftx` and `$pow` can carry 1/0 flags: their B flag is the amount's (exponent's) own
(`s ** u` gives `$pow` 1/0, `s >>> u` gives `$sshr` 1/0), and they are not mixed-sign cells. One that arrives otherwise is signed only when both flags are
set, and `opt_expr` warns of it once (2F §16).

### 9.3 Error cap and what stops elaboration (EL-44)

- Errors are counted per `read_verilog` run; at `ODIN3_ELAB_MAX_ERRORS` (20, as 2C's cap) the
  stage stops with `too many errors; elaboration stopped after 20`.
- Stage 1 continues after an error at the next item (or the next module), so one run reports
  several; anything that depends on a failed item (an instance of a module whose parameters did
  not resolve, a block of a generate whose condition failed) is skipped silently, never reported
  twice. The exception, recorded deliberately (v4.15, §17 #36): the REJECT scan runs first, once per
  definition, and a module whose scan found an error skips passes E and W altogether. Its other
  errors of those passes (an undeclared name under `` `default_nettype none ``, a port range
  conflict, a bad parameter value) and its children's (their REJECT rows too: the children are never reached) are reported only
  after the scanned error is fixed; the info `not instantiated under the top` is not logged once an
  error was. Stage 2 does not start after any stage-1 error.
- Stage 2 skips the offending construct and continues (EL-2); a module whose ports could not be
  built stops stage 2 (its instances could not be created).
- Fatal at once: `ODIN3_ERR_NO_MEMORY`, every cap of §11 that guards memory (width, loop
  iterations, nodes, nets, memory bits, the hierarchy depth), and a `check` failure (a 2D bug).
- Exit statuses: `ODIN3_ERR_PARSE` for rejected input (always with at least one located
  diagnostic), `ODIN3_ERR_NO_MEMORY`, `ODIN3_ERR_CHECK` (a 2D bug), `ODIN3_ERR_INVALID_ARG`
  (misuse: a NULL store, a store of the wrong form; logged).

## 10. Provenance and attributes (EL-45)

- Every IR object 2D creates carries a SOURCE record (IR-12) from `odin3_prov_source` with: `run`
  = the `read_verilog` pass run (or the adapter's); `locs` = one `odin3_srcloc` per covered node
  from `odin3_srcman_srcloc` of the node's `{loc, end}` (raw `loc` kept, 2A §3.2); `ast` = the
  node's ID in the **elaborated** store (AST-15); `hier` = the module-relative scope path.
- **Which node**: a wire → its `DECLARATOR` (an implicit net → its first use); a port node → its
  `PORT_DECL`; an instance → its `INSTANCE`; a cell → the expression node that made it (an
  operator's `BINARY`, a select's `SELECT`, a ternary's `TERNARY`); a constant cell → none needed
  beyond the module's (shared, IR-4); a `$proc` node → its `ALWAYS` (or the `CALL` or memory read it was made for), and each action, case and switch
  → its statement (`BLOCKING_ASSIGN`, `IF`, `CASE`, `CASE_ITEM`); a temporary → the statement
  that needed it; a `$mem` → the array's `DECLARATOR`; the cells of an inlined function or task
  body → two locs, the body node first and the call (`CALL`/`TASK_CALL`) second, so two call
  sites of one function get distinct records; a wired-net gate (EL-26) → the net's declarator.
- **`hier`** is `module/scope/scope…`: the specialization's IR name, then each generate or named
  block scope with loop indices (`top/g_stage[2]`, `sub$W=8/genblk1`). IR modules are shared by
  every instance of a specialization, so the instance path is not a property of their objects;
  `flatten` adds it through its DERIVED records. (This reads IR-12's example `top/u1/gen[3]` as
  module-relative for hierarchical IR; noted for Peter, no field changes.)
- Records are hash-consed (IR-13), so one node in one scope is one record however many objects it
  makes, and each unrolled iteration of a procedural loop shares its body's records (the scope
  path does not change; generate iterations differ by `hier`).
- **Attributes** (2A §7, applied as written there): `MODULE` → the IR module; a declaration →
  each wire of its declarators (and port node); an instantiation → each instance node; a
  statement or expression → every cell its operation creates; `CASE` attributes → the switch
  (`full_case`/`parallel_case` as flags, others as attributes on the process); an `always` → the
  `$proc` node (`ODIN3_OBJ_NODE`). Values: none → INT 1, `NUMBER` → BITS, `STRING` → STRING. 2D adds
  `init` (BITS, EL-40) on wires and `parameter:<NAME>` on module instances (EL-11). Attributes on
  a dropped construct are dropped with it.

## 11. Caps and out-of-memory behaviour (EL-46)

Each cap is a named constant in `src/frontends/elab/elab.h`, checked **before** the allocation it
guards, reported as a located `ODIN3_ERR_PARSE` naming the cap, and lowered in tests through
`odin3_elab_test_set_limits` (hidden in `elab_test.h`, as 2A/2B/2C do).

| Cap | Value | Why |
|---|---|---|
| `ODIN3_ELAB_MAX_ERRORS` | 20 | as 2C; bounds diagnostic volume under fuzzing |
| `ODIN3_ELAB_MAX_WIDTH` | 2^20 bits | one declared object, expression, parameter or constant value; `= ODIN3_READER_MAX_WIDTH` (PHASE2 #5, 2A's literal cap), so no source can allocate gigabytes of pins through one object; keeps `*`, `/`, `**` on constants under ≈ 3 × 10^8 word operations |
| `ODIN3_ELAB_MAX_CELL_PINS` | 2^22 | pins of one cell 2D creates (`= ODIN3_READER_MAX_TOTAL_WIDTH`) |
| `ODIN3_ELAB_MAX_LOOP_ITERATIONS` | 2^20 | one generate, procedural, `repeat` or interpreted loop; the corpus's largest literal bound is 256 and a memory-clearing loop over a 64K-word RAM needs 2^16 |
| `ODIN3_ELAB_MAX_TOTAL_ITERATIONS` | 2^24 | all unrolled loop iterations of a run; nested loops multiply, and this stops a 2^20 × 2^20 bomb long before memory does |
| `ODIN3_ELAB_MAX_EVAL_STEPS` | 2^26 | statements plus expression nodes per top-level constant evaluation (a `while (1)` in a constant function) |
| `ODIN3_ELAB_MAX_CALL_DEPTH` | 1024 | constant-function recursion and inline nesting; real recursive constant functions are < 64 deep |
| `ODIN3_ELAB_MAX_INLINE_DEPTH` | 1024 | calls inlined inside one another (heap records of the call frames, not C stack, since 2D-7; v4.24 had C recursion and 256); the lower of this and `ODIN3_ELAB_MAX_CALL_DEPTH` applies; the located error is `function call nesting deeper than 1024 at 'f'` |
| `ODIN3_ELAB_MAX_LOOP_OBJECTS` | 2^21 | IR objects (nodes, pins, nets, wires) one unrolled procedural loop may create before it is called endless (v4.24); the cycle check of a loop whose test and step read only its variable stops the classic unsigned countdown first |
| `ODIN3_ELAB_MAX_REENTRY_DEPTH` | 256 | constant evaluations nested in C (typer callbacks, host re-entries; v4.6): each takes 4–6 KiB of C stack under ASan, where a function frame takes none |
| `ODIN3_ELAB_MAX_HIER_DEPTH` | 1024 | instance nesting below the top; real designs ≈ 10 (mcml: 7) |
| `ODIN3_ELAB_MAX_SPECIALIZATIONS` | 2^16 | modules made; lenet makes 138 |
| `ODIN3_ELAB_MAX_INSTANCES` | 2^24 | instance and gate nodes per run (instance-array bombs) |
| `ODIN3_ELAB_MAX_ELAB_NODES` | 2^25 | nodes of the elaborated store (≈ 1.4 GB at 2A's measured 41 B/node; lenet's parsed store is ≈ 3.4M nodes) |
| `ODIN3_ELAB_MAX_NET_BITS` | 2^26 | nets created per run (≈ 34 × lenet's 2.0M wire bits; ≈ 3 GB of IR at the cap) |
| `ODIN3_ELAB_MAX_MEMORY_BITS` | 2^30 | `SIZE × WIDTH` of one declared memory (not allocated unless initialised) |
| `ODIN3_ELAB_MAX_INIT_BITS` | 2^26 | initial-value bits materialised per run (one byte per bit in `odin3_value`, so ≤ 64 MB; lenet's memories total 12.3M bits) |
| `ODIN3_ELAB_MAX_READMEM_BYTES` | 2^28 | a `$readmem` file (= `ODIN3_SRC_MAX_FILE_BYTES`) |
| `ODIN3_ELAB_MAX_MODULE_NAME` | 128 bytes | longer specialization names are hashed (EL-9) |
| AST depth | `ODIN3_AST_MAX_DEPTH` (32768) | the builders enforce it on the elaborated store, as on the parsed one |
| `ODIN3_ELAB_MAX_BYTES` | 60% of physical memory (`sysconf(_SC_PHYS_PAGES) × _SC_PAGESIZE × 0.6`), lowered by the test hook | **one run-level byte budget** (the review's F24): the per-kind caps above are independent and their sum exceeds the machine (nodes + nets + pins + both stores), while a host OOM kills the WSL VM (memory rule, ≤ 60%). Checked at every module boundary of both stages and every 2^16 IR objects, against `odin3_ast_bytes_reserved` of the parsed and elaborated stores plus 2D's running estimate of the IR it built (48 B per node, 28 B per pin including the net's pin-array slot, 48 B per net, 64 B per wire, plus arena bytes for parameters and process bodies); over budget is "elaboration needs more than N bytes (the budget is 60% of physical memory)", fatal |

**Out of memory.** Every builder and IR call reserves before it mutates (1A, 2A, IR-15). On
`ODIN3_ERR_NO_MEMORY` 2D stops at once: stage 1 frees its environments, arenas and the partial
elaborated store; stage 2 frees its builders and leaves the IR it made valid (each object is
created whole or not at all); the pass returns `NO_MEMORY`. Unit tests inject a failure at every
allocation (`odin3_util_set_alloc_fail_after`) and assert the status, no leak and a passing
`check` (§14.1).

## 12. Files, data structures and ABI (EL-47)

| File | Contents |
|---|---|
| `src/frontends/verilog/elab.h` | `odin3_elab_run` (exists as 2C's stub; `elab_stub.c` is deleted) |
| `src/frontends/verilog/elab1.c`, `elab1_lookup.c`, `elab1_top.c`, `elab1_params.c`, `elab1_spec.c`, `elab1_expand.c`, `elab1_gen.c`, `elab1_decl.c`, `elab1_write.c`, `elab1_diag.c` | stage 1: run context, lookup, top, parameters, specialization queue, pass E, generate, declarations and ports, pass W, REJECT/IGNORE diagnostics |
| `src/frontends/elab/elab.h`, `elab_internal.h`, `elab_test.h` | the stage APIs, `odin3_elab_opts`, caps, test hooks |
| `src/frontends/elab/cval.c`, `cval_arith.c`, `ceval.c`, `ceval_interp.c`, `ctype.c` | values, operators, expression evaluation, the interpreter, typing (EL-12–EL-18) |
| `src/frontends/elab/bind.c` | the binder (EL-5) |
| `src/frontends/elab/elab2.c`, `elab2_wires.c`, `elab2_inst.c`, `elab2_assign.c`, `elab2_expr.c`, `elab2_select.c`, `elab2_mem.c`, `elab2_claim.c`, `elab2_wired.c`, `elab2_prescan.c`, `elab2_warn.c`, `elab2_tri.c` | stage 2 module emission (EL-23–EL-32) |
| `src/frontends/elab/pg.c`, `pg_case.c`, `pg_loop.c`, `pg_call.c`, `pg_init.c`, `pg_init_vtr.c` | the process generator and initial values (EL-35–EL-40) |
| `src/ir/cells/proc.c`, `src/ir/proc.[ch]` (body types, `verify`, body text), `src/ir/value.[ch]` (`ODIN3_VAL_PROC`), `src/ir/node.c` (the process counter), `src/passes/manager.[ch]` (the `processes` flag) | IR change B |
| `src/ir/cells/arith.c`, `logic.c`, `reduce.c`, `shift.c`, `builtin.c` | IR change A (nine cells) |
| `src/ir/module.[ch]` | IR change C |
| `src/backends/rtlil/writer.[ch]` | `write_rtlil` |
| `src/api/elab_abi.c`, `include/odin3/odin3.h` | ABI additions |

Stage 1's run context `odin3_elab1` holds: the design, the parsed and elaborated stores, the
options, the definition table (`odin3_elab_def`: kind, name, library, `MODULE` node, header and
body parameter lists, ports, the `blackbox` flag), the specialization table (`odin3_elab_spec`:
definition, key bytes and hash, name, depth, resolved parameter values, pending overrides, the
decision log, the elaborated `MODULE` ID; indexed by an `u64map` from key hash to a chain), the
FIFO queue, the run arena, the error counter. A specialization's pass-E environment
(`odin3_elab_env`) is a scope stack (`odin3_elab_scope`: kind, name, index, parent) and one
`u64map` `(scope << 32 | name) → odin3_elab_entry {kind, parsed decl, value (parameters,
genvars), width, sign}`. Stage 2's context `odin3_elab2` holds the IR module, the binding, the
symbol-to-wire map (the symbol table's `ir_wire`), the driver table (a `vec` indexed by net ID:
kind + AST node), the memory table (`odin3_elab_mem`: symbol, mem2reg flag, read and write port
records, init words), and the process generator `odin3_pg` (builder vectors, `lv`/`rv` maps with
undo logs, the frame stack). All containers are `src/util`'s (CLAUDE.md); every traversal uses an
explicit stack (lizard: no recursion).

**ABI** (v4.4: no bump; the ABI stays at the unreleased v4, ruling I1 and plan point (c), and
`ODIN3_ABI_VERSION` stays 4; the bump is decided once, at the first release or Phase-2 close;
v1–v4.3 said "one bump, after 2C's"): `odin3_ast_elaborate` (EL-2) with `odin3_elab_options`;
`odin3_ast_get_library_units_from` (2C §9 (b)); `odin3_module_get_proc_count`; the `$proc` node
through the existing node accessors, with `BODY` as text through `odin3_node_get_param_text`
(EL-33); `odin3_pass_def`'s `processes` flag (EL-33). IR change C has no ABI mirror (v4.4, plan
point (d): no ABI function creates modules or ports, and `odin3_plugin_port` stays frozen). No new
object kind (v1's `ODIN3_OBJ_PROC` is gone with F1). Each with the purpose/ownership/failure
paragraph (CLAUDE.md); cffi cdef updated.

## 13. `write_rtlil` (EL-48)

`write_rtlil <file>` writes the design as Yosys RTLIL text (the `write_rtlil` format Yosys 0.55
reads with `read_rtlil`): the bridge that lets the Yosys flow lower 2D's output, independent of
2E and 2F (§14.3), and later the way to compare 2E/2F with Yosys's `proc`/`opt`.

- One `module \name` per IR module in creation order (attributes first), `wire width W [offset
  O] [upto] [signed] [input|output|inout K] \name` per live wire (offset = the declared low
  index, `upto` for an ascending range; port wires numbered in port order), and one `wire $n<ID>`
  per net that is no wire's bit.
- A net is written as its primary `\w [k]` (`k` = the vector position, RTLIL's own 0-based bit
  index, which is IR's bit k); a net driven by `$_CONST0_/1/X/Z` as `1'0`, `1'1`, `1'x`, `1'z`
  (the constant cells are not written); every other `(wire, bit)` membership of a net (an alias,
  IR-2) as `connect \alias [j] \primary [k]`.
- Cells with `parameter \P <value>` and `connect \PORT { … }` (MSB first, RTLIL's order);
  instance nodes as cells of the module's type; `$mem` as written (Yosys's `$mem` v1); names of
  unnamed objects: `$c<ID>`, processes `$proc$<file>:<line>$<ID>`, from provenance.
- A `$proc` node (EL-33) as an RTLIL `process` block, not a cell: each body position is printed
  as the net on that pin (`IN | k` → the net on `I` pin k, `OUT | k` → the net on `O` pin k,
  constants as above because their pins sit on the constant nets, `ANY` as `-`); root actions as
  `assign`, switches as `switch` / `case <patterns>` / `end` with `attribute \full_case 1` and
  `\parallel_case 1` from the flags, syncs as `sync posedge \clk` / `negedge` / `low` / `high` /
  `always` with `update` lines. The node's attributes become the process's.
- Deterministic (IR-16 order), written atomically like the other writers. A Yosys round trip
  (`read_rtlil; write_rtlil`) is a test (§14.3).

**Implementation note (2D-1, v4.4; binding for 2E-1 and 2F-1).** The text 2D-1's `write_rtlil`
produces is specified exactly by `src/backends/rtlil/writer.h` (`odin3_rtlil_text`). Where the
bullets above differ from it, this note and `writer.h` win. The eight differences (review I1 of
`final-review-2D1`) are:

1. **Order and wire fields.** Per module, in creation order, the writer emits:
   - at no indent, the module's attributes;
   - `attribute \top 1` if the module is the design's top (`odin3_design_top`) and carries no
     `top` attribute of its own (Yosys's `write_rtlil` emits it too, and `read_rtlil` takes it as
     the top);
   - `module \name`;
   - at indent 2, the named wires, the net wires (point 8), the dangling wires (point 3), the
     cells, the processes and the alias connects (point 2), each group in ID order;
   - `end`.

   Every object's attributes come on the lines just before it. A named wire is written
   `wire [width W] [upto] [offset O] [input|output|inout K] [signed] \name`. This is the field
   order of Yosys 0.55's `rtlil_backend.cc`, not the `width offset upto signed dir` order above.
   - `width` appears only when W ≠ 1.
   - `upto` appears for an ascending range (msb < lsb).
   - `offset` appears only when O, the lower declared index, is not 0.
   - K is the 1-based port index.

   Unnamed wires are not written; their nets are spelled as nets (point 8).
2. **Alias connects per run, not per bit.** For each named wire in ID order, the writer finds
   each maximal run of bits that are not their net's own spelling: either the net's primary is
   another `(wire, bit)`, or the net is constant. Each run gives one line:
   - `connect \w <sig>` for the whole wire;
   - `connect \w [k] <sig>` for one bit of a wider wire;
   - `connect \w [hi:lo] <sig>` otherwise.

   Here `<sig>` is the run's nets as a sigspec, for example `connect \b \a` or
   `connect \y [1] 1'0` (2F M11). These lines replace the per-bit
   `connect \alias [j] \primary [k]` lines described above.
3. **`$p` dangling wires.** Each run of unconnected pins of a written cell (consecutive bits of
   one port) is declared as `wire [width N] $p<P>`, where P is the run's lowest pin ID. The
   cell's `connect` line spells the run as that wire. (A `$proc` with a position on an
   unconnected pin is refused; see point 6.)
4. **`$c<ID>` fallback.** A cell is written `\name`. It is written `$c<ID>` instead (ID = the
   node ID) when the node is unnamed, or when its name is also the name of a written wire or
   net wire (Yosys keeps cells and wires in one namespace), or when the name is empty or holds
   a blank, tab, CR, LF or NUL. A cell type is written as its name, with `\` prepended unless it
   starts with `$` or `\`. An instance's type (a module name) is always written `\name`, the same
   as that module's `module` line.
5. **`$mem` flags `1'0`.** On `$mem`, a 0-bit BITS parameter other than `INIT` is written `1'0`,
   because Yosys 0.55's cell check wants max(1, ports) bits. Those parameters are
   `RD_CLK_ENABLE`, `RD_CLK_POLARITY` and `RD_TRANSPARENT` with `RD_PORTS` 0, and
   `WR_CLK_ENABLE` and `WR_CLK_POLARITY` with `WR_PORTS` 0. `MEMID` is a quoted string, with `\`
   prepended unless it starts with `$` or `\`. Other parameter values are written as follows:
   - INT in decimal when it fits int32, otherwise as `64'<bits>` (`proc.h`);
   - BITS as `<n>'<bits>`, MSB first;
   - STRING quoted, with Yosys's escapes.
6. **Nine refusals.** In these cases `write_rtlil` fails with `ODIN3_ERR_INVALID_ARG` and the
   message `write_rtlil: module 'm': cell '<name>' of type '<t>' has no RTLIL form`. The text
   buffer stays unchanged, and an existing file keeps its contents. The nine cases are:
   - `$_DFF_P_`, `$_DFF_N_`, `$_DLATCH_P_`, `$_DLATCH_N_` and `$_FF_` (Odin III's carry an
     `INIT` parameter that Yosys's do not);
   - a non-constant `$sop` (`WIDTH`/`COVER` where Yosys has `DEPTH`/`TABLE`);
   - `$memrd` and `$memwr` (no `memory` record is written);
   - a `$mem` with an empty `MEMID`, which gets its own message ending `has an empty MEMID`.

   Three other inputs are also refused:
   - a module, wire, net, attribute or cell-type name that RTLIL cannot carry (empty, or holding
     a blank, tab, CR, LF or NUL);
   - a net written `\name` whose name a wire also has;
   - a `$proc` that `odin3_proc_print` refuses.

   A new cell type, such as 2E-1's flip-flops or 3D's `$lut`, must either match Yosys 0.55 or
   join this list.
7. **Black-box stubs** *(amended by 2E-1 Task 3, controller ruling)*. Black-box and hard cell
   types are not IR modules; after the modules `write_rtlil` writes one `module` stub per such
   type (declared models first, then the other instantiated types; `$` names excepted), as Yosys
   0.55's `read_verilog` makes a `(* blackbox *)` module: `attribute \blackbox 1`, `attribute
   \dynports 1` when a port's width depends on a parameter, the parameters with their defaults
   and the ports at the default widths (`src/backends/rtlil/writer.h`; a cell of the type has the
   widths of its own parameter set since v4.19, as Yosys's cells do, §17 #46). `hierarchy -check`
   accepts every parameterisation through one stub (with `dynports` it neither resizes nor
   derives the instances). `Cell::known()` is now true for these instances, so Yosys counts only
   their input connections as users, and `opt_clean` *removes* a black-box instance whose
   outputs are unused (without stubs it kept every instance); 2F's `opt_clean` mirrors this (2F
   §10). Yosys's `write_blif` writes only the low `min(connection, stub port)` bits of an instance
   whose type has a module in the design (`backends/blif/blif.cc:428`); the `elab-equiv` back end
   therefore runs `delete =A:blackbox` before `write_blif` (2D-7b: `single_port_ram` with
   `DATA_WIDTH 8` had come out 1 bit wide on side A only), so the instances are written at their
   own widths on both sides.
8. **Which nets get `wire $n`.** A net is spelled as a net of its own, and declared as a wire,
   exactly when it is live, not constant, and its primary is not a bit of a *named* wire
   (`odin3_proc_sig_is_net`). So the nets of unnamed wires, and nets with no wire at all, are
   included. The declaration is `wire \name` for a named net, else `wire $n<ID>`, in net ID order.
   The other nets are spelled as follows:
   - a net on a named wire's bit as `\w [k]`; runs of bits merge into `\w [hi:lo]`, and a whole
     wire into `\w`;
   - a constant net as `1'0`, `1'1`, `1'x` or `1'z`; runs merge, for example `2'x1`. The
     constant cells themselves are not written.

Processes are written after all cells and are named `$proc$<file>:<line>$<ID>`, from the
node's first SOURCE location. The name falls back to `$proc$<ID>`, which is also used when the
file name holds whitespace. Two CTest tests check the writer:
- `test_rtlil_writer` checks the text;
- `rtlil_yosys_roundtrip` runs when CMake finds Yosys 0.55. Its `every_cell` fixture holds one
  cell of every registered built-in `$` type except those refused in point 6, `$mem` and
  `$proc` (the latter two have fixtures of their own). A new cell definition that disagrees with
  Yosys 0.55's cell checker therefore fails this gate.

`docs/PASSES.md` gains the row; DESIGN §9's table gains "RTLIL (Yosys text) | — | yes |
word level with processes; the Yosys bridge for 2D–2F tests".

## 14. Testing (EL-49)

All tests run under ASan/UBSan in Debug; every store 2D builds passes `odin3_ast_check` FULL and
every IR it builds passes `odin3_check` FULL (the pass manager runs it after `read_verilog`).
Located-error tests assert the exact `file:line:col: error: …` line through a capturing log
sink. Every sweep runs smallest input first (memory rule). **Oracle binary** (the review's F11):
every Yosys run below is Parmys's Yosys 0.55 (`$VTR_ROOT/build/bin/yosys`) unless a line names
0.69; every golden and generated table records the Yosys version in its header.

### 14.1 Unit tests (Unity)

- `test_elab_cval.c`: every operator of EL-14 at widths 1, 7, 64, 65, 200 and with x/z operands,
  against a table generated once from Yosys 0.55 (`tools/elab-check/gen-const-table`: random
  operands, `localparam P = <expr>;` read back from Yosys's `write_rtlil`; committed with the
  version in its header), plus division by zero, `**` corner cases, unsized extension
  (`'hFFFF_FFFF` in 64 bits), strings, `""`.
- `test_elab_ctype.c`: one case per row of the EL-13 table, Chu p.45's `(a+b)>>1` versus
  `(0+a+b)>>1` (E22), mixed signedness (E23), comparison operand sizing (E8), shift amounts,
  ternary, concatenation (an unsized operand: the E12 warning and its stored width), function
  results; the unsized-parameter cases of EL-15/EL-16 (`parameter P = 'hFFFFFFFF` and
  `localparam [31:0] Q = 'hFFFFFFFF` into 64 bits: all ones; `localparam [35:0] R = 'hFFFFFFFF`:
  sized, zero-extended), also in the generated constant table.
- `test_elab_interp.c`: constant functions (loops, `while`, `case`, recursion to the depth cap,
  the step cap with `while (1)`), every "not a constant function" reason, `$clog2` 0/1/2/3/2^31.
- `test_elab_stage1_*.c` (on parsed stores built by 2C from small sources): lookup order across
  libraries and library units; a design module named like a registered cell type (EL-6's exact
  error) and a second `read_verilog` redefining a module (`duplicate_module`); top selection (one
  candidate, `no_top`, `ambiguous_top`, `unknown_top`, `--top` over the project top); parameter
  precedence (the five levels, the three Yosys-verified cases of EL-8, the defparam-loses
  warning); nested and generate-scoped `defparam` push-down, a `defparam` and a project `param`
  ending at a cell-type instance; project `param` paths (`param_instance`'s); specialization
  sharing and names (decimal, bit-string, string, hashed, `$2` collision, a collision with a
  registered cell type); recursion and the depth cap; the `label_genblks` naming of EL-21 — the
  review's probe as a case: `if (A) … else if (B) …` as the first construct with `A` = 0, `B` = 1
  gives `genblk1.genblk1.x`, a bare `if (B) assign …` consumes a counter value, a following `for`
  is `genblk3[0].w`, a user `genblk1` declared after the first `if` does not pad it and one
  declared before does; generate loops counting down, negative indices, a repeated value; instance
  and gate arrays with sliced connections; port merging (M1, M12); implicit nets under each
  `` `default_nettype ``; every §9.1 REJECT and §9.2 IGNORE through stage 1.
- `test_elab_bind.c`: scopes and symbols of hand-built elaborated stores (including an unsized
  `localparam` `NUMBER`), outward lookup, `g[0].t` and `blk.x` references, every binder error.
- `test_elab_stage2_*.c` (on **hand-built elaborated stores**, the slang seam, and on stage-1
  output): wire ranges and signedness (ports through IR change C), every row of the EL-30 table
  (cell type and every parameter asserted), `===`/`!==` in its three cases (exact fold; wildcard
  compare over the 0/1 positions, `a !== 4'b1x00` → a `$ne` over bits 3, 1, 0; plain `$eq`),
  selects (constant, out of range, `$shiftx` offsets for descending, ascending and negative
  ranges), continuous assignments by construction, by merge and the same-net no-op
  (`assign a = b; assign b = a;`), the driver table (A4, uwire, supply, wand/wor gates, a child's
  `inout` pin with and without logic on the net), tri-state (D3), port connection resizing (I4),
  memories — mem2reg rules 1–5 each, `--nomem2reg` with a net array, and **`$mem` addressing with
  `[1:4]`, `[4:1]` and `[-2:1]` ranges** (raw index on every port, `OFFSET` 1/1/−2; a write then a
  read of `m[i]` through `sim`-free evaluation of the bridge; the corpus has only `[0:N]`/`[N:0]`
  memories, so only these cases catch an offset bug) — and the process generator: the exact body
  of each `$proc` node (positions, cases, switches, actions, syncs, `I`/`O` pin nets, the
  constant pins on the shared constant nets) for T7 (`t = a & b; r <= t;`: **two** outputs, `t`
  and `r`, `t`'s register dead after `opt`, Yosys's `collect_lvalues`), T8, `if` chains,
  `case`/`casez`/`casex` patterns, `full_case`/`parallel_case`, both latch modes (default: root
  hold actions; `--nolatches`: x substitutions, no root actions), loops, task and function
  inlining (nested, in a loop, in a continuous context), memory writes (the three write-port
  variables as `O` pins with no sync action), `initial` (constants, loops, `$readmemh` with `@`
  addresses and comments, the non-constant statement warning, the three result kinds), T9/T10
  per bit.
- `test_ir_proc.c` (IR change B): the `$proc` type's ports and parameters; `verify` rejecting
  each malformed body (an index out of range, a constant or `ANY` in an `lhs`, an `OUT` position
  twice in one `lhs`, mismatched widths, an `OUT` pin never assigned, a sync signal that is an
  `OUT` position); rule 4 seeing an `O` pin as the driver and an `I` pin as a sink;
  `odin3_net_merge` moving a `$proc` pin with the body untouched; `odin3_module_compact`
  renumbering a module with a live and a dead `$proc` (tombstone with the body); the process
  counter across create/delete/replace; the pass manager refusing a `REFUSE` pass with the exact
  message and running an `OK` one; the writers' refusals; `odin3_node_get_param_text` of a body.
  `test_ir_cells.c` gains the nine new cells (ports, parameters, verify, the seven simulate hooks
  against simlib truth tables). `test_ir_module.c` gains IR change C (`has_range`, the derived
  width, the two `INVALID_ARG` cases, the signed port record).
- `test_rtlil_writer.c`: exact text for small modules (aliases, constants, ports with offsets and
  `upto`, a `$proc` node with every sync type and constant pins, `$mem`).
- Caps: each §11 cap hit through `odin3_elab_test_set_limits` with its exact message, the byte
  budget included. OOM: an injected failure at every allocation of both stages, asserting
  `NO_MEMORY`, no leak, a valid IR. Determinism: two runs give byte-identical `write_rtlil`
  output.

### 14.2 Coverage rows (`elab_coverage`)

`tests/elab/rows/<row>_<slug>.v` for every MUST and SHOULD row (125), each a small top module
exercising the row's variants (starting from 2C's coverage file of the row, minus the
parse-only variants that do not elaborate), with:

- `<row>.elab`: the stage-1 golden, `odin3_ast_dump` of the elaborated store. For the 54
  semantics-heavy rows: M1, M2, M12, P1–P6, P8, N2, N7–N13, E8, E9, E12, E15, E16, E22, E23,
  G1–G7, C4–C9, T7, T18, F1, K1–K11, I3, I6, the file starts `# hand-written` (the marker `--write`
  refuses to overwrite) but is **seeded from `elab-check --write`**, a regression lock, not an
  independent derivation (Task 25 review I2). Line 2 says so, and for the 19 rows with a
  `PARAM_DECL` or `GEN_BLOCK` (K3, K5, K7, K8, G1–G7, P1–P6, P8, T18) that the parameter values,
  generate block names and indices and surviving branches were checked by hand against the source
  and IEEE 1364-2005 (`task-25-golden-check.md`). All 54 rows are in the Yosys-equivalent set, which
  covers the netlist only. The rest are generated and reviewed once.
- `<row>.il`: the stage-2 golden, `write_rtlil` output **without** `--nolatches` (the default
  path: root hold actions), and `<row>.nolatches.il` for every row with a combinational `always`
  (the review's F25: both latch modes are 2D's to get right; 2E's latch tests cover what `proc`
  then does with the default path).
- an equivalence check against Yosys 0.55 through the bridge (§14.3), in the `--nolatches` mode on
  both sides (`equiv-check` cannot model latches, PHASE0 #7); rows with several clocks rely on
  the goldens.

Runner `tools/elab-check/elab-check [--write] <file>…` (C, links `odin3_core`): elaborate, check,
dump, compare goldens; exit 0/1/2.

### 14.3 The Yosys bridge (`tools/elab-check/elab-equiv`)

For a design `F` with top `T` (Python, stdlib), always with **Yosys 0.55** (`$VTR_ROOT/build/
bin/yosys`, Parmys's; 0.69 `read_rtlil` was also verified to accept the format):

- **A** (2D under test): `odin3 -p "read_verilog --nolatches F; write_rtlil a.il"`, then
  `yosys -p "read_rtlil a.il; hierarchy -top T; proc; flatten; opt; memory -nomap; opt;
  memory_map; opt -full; techmap; opt -fast; abc -g AND,NAND,OR,NOR,XOR,XNOR,MUX; opt_clean;
  delete =A:blackbox; write_blif -gates -undef + unconn a.blif"`;
- **B** (oracle): the same script with `read_verilog -sv -nolatches F` in place of
  `read_rtlil a.il`;
- `tools/equiv-check/equiv-check a.blif b.blif` (exit 0 equivalent).

Both sides go through one Yosys back end, so x is resolved by the same `opt` on both, and a
difference is 2D's (or a documented divergence). `--top` for a micro is the first `.model` of its
Parmys golden (EL-7). A second check, `read_rtlil a.il; write_rtlil b.il` in Yosys, asserts the
writer's text is valid RTLIL. The bridge is also 2E's and 2F's oracle later (their output through
`write_rtlil` against Yosys's `proc`/`opt`).

### 14.4 The 2C handover

`tests/verilog/reject-2d/` (23 cases: 20 rows plus the REJECT parts of N15, P4 and F2) and `tests/verilog/ignore-2d/` (12 rows, T6 and T9 added by 2D-7b), already registered
by 2C as `verilog_reject_2d`/`verilog_ignore_2d` and skipped while `odin3_elab_run` is a stub,
turn on with 2D; their `.err`/`.warn` texts are §9's tables. The project corpus's Verilog cases
(2B §10.2 `--with-frontend`) run: the positive phase-2 cases (`auto_top`, `param_instance`,
`param_string`, `top_param`, `single_file`, `multi_dir`, …) are equivalent to their `ref.blif`
through the bridge, the negative ones (`neg_unresolved_module`, `neg_unknown_top`,
`neg_ambiguous_top`, `neg_no_top`) give their kind, location and names. (The fixtures' own
`ref.cmd` oracles were recorded with Yosys 0.69; the bridge reruns them with 0.55 and a
disagreement between the two is reported as oracle drift, not as a 2D failure.)

### 14.5 Corpus sweeps and fuzzing

- `verilog_micro_elab`: every micro (539 files; the multi-file ones through their includer)
  under `odin3 --elab-only` (CLI sugar for `read_verilog <files>; stats`, like `--parse-only`),
  smallest first; failures allowed only when listed in `tests/elab/micro_expect_fail.txt` with a
  reason (Parmys-rejected micros, PHASE2 #4; the 2C list's entries). `verilog_vtr_elab`: the 31
  VTR and 33 Koios files, lenet.v last, when `VTR_ROOT` is set.
- `elab_equiv_micro` / `elab_equiv_vtr`: the bridge (Yosys 0.55) on the same sets (VTR ones
  nightly); a non-equivalent result fails unless listed in `tests/elab/equiv_expect_fail.txt`
  with a reason.
- **Crash fuzz** `tests/fuzz/fuzz_elab.c` (libFuzzer, `-DODIN3_FUZZ=ON`): bytes → preprocess →
  parse → elaborate with every cap lowered; invariants: status OK, PARSE (with a located
  diagnostic, at most 20, no `INVALID_ARG` logged) or injected NO_MEMORY; on OK `check` passes.
- **Differential generator**: `tools/verilog-gen --elab` (2C's generator, extended) emits random
  valid modules weighted toward 2D's traps — expression trees with random widths and signedness
  assigned to random-width targets (E22/E23), unsized parameters and literals, `===` against x
  patterns, parameterized instances with `#()`/`defparam`, generate loops and conditionals,
  `always` blocks mixing `=`/`<=`, `case`/`casez`, loops, functions and tasks, memories with
  non-zero low indices — and runs the bridge on each with **Yosys 0.55**: 200 seeds in CTest when
  it is found, 5,000 nightly; a non-equivalence is a failure with the seed and file kept. Nightly
  only, the same files also go through the bridge with **Yosys 0.69**; a file where 0.55 and 0.69
  disagree with each other is reported as **oracle drift** (a list for the next oracle update,
  ORACLES.md), never as a 2D failure.
- **Constant evaluator differential**: random constant expressions, 2D's value against Yosys
  0.55's (as in §14.1, generated per run), 10,000 per nightly run.
- **Cell consistency**: the evaluator against each word cell's simulate hook on random inputs
  (the two must agree on 0/1 inputs, since both are simlib).

### 14.6 Don't-cares and the comparison with Parmys goldens (the review's F12)

The exit test (PHASE2 #4) compares against Parmys BLIF goldens, not through the bridge, so x
matters there. Facts, checked on 2026-10-11:

- Parmys writes `write_blif -undef + unconn`: x bits that survive its `opt` are wired to the net
  `unconn`, declared as `.names unconn` with **no cover rows** — which is BLIF's constant 0.
  `tools/equiv-check` reads it as such (a zero-row `.names` drives its output; verified:
  `equiv-check` of `syntax/not_enough_wires.parmys.blif`, whose output is `unconn`, against itself
  is "equivalent (abc cec)", exit 0). All 934 Parmys goldens (467 per arch, k6_frac and EArch)
  declare `unconn`; 314 (157 per arch) use it in logic (recounted by the re-review). So `unconn` reaching an output does not stop the comparison; it fixes Parmys's
  x to 0 at the points where x survived.
- Where Parmys's `opt` resolved an x *before* the netlist (`opt_expr -mux_undef` picking the other
  mux input, `$eqx` lowering, `-undriven`), the golden contains a choice, and 2D's netlist is
  equivalent only if Odin III resolves the same x the same way.

The plan, before 2H relies on it:

1. **Keep x explicit until `opt`** (2D: `$shiftx`, `$_CONSTX_` pins, the `full_case` and
   `--nolatches` x values, the wildcard `===` of EL-30 matching Parmys's bitwise `$eqx`).
2. **2F mirrors Yosys's x rules** (`opt_expr`'s `-mux_undef`, `-undriven`, its `$eq`/`$mux`
   constant rules) cell type by cell type, so the common cases become identical, not merely
   equivalent; 2F's spec owns the list.
3. **Odin III's BLIF writer adopts the same convention**: an x constant net that reaches the
   netlist is written as `unconn` (`.names unconn` with no rows, i.e. 0), so the residual x is 0
   on both sides.
4. **`equiv-check --x-report`** (a new mode, owned by 2H): when two netlists are not equivalent,
   it recomputes the check with every primary output and latch input whose (sequential) fan-in
   cone in *either* netlist reaches `unconn` or an x constant removed from the comparison, and
   reports the removed outputs by name. The 2H table then has four outcomes per design:
   **identical** (`netlist-compare`), **equivalent** (`equiv-check`), **equivalent outside
   x-cones** (listed outputs, each reviewed by hand and recorded with a reason), and **differs**.
   Only the first two satisfy PHASE2 #4 as written; the third is reported to Peter as a set, with
   the decision whether it counts (§20 item 13).
5. **Follow-up in 2H's backlog: the defined-output miter** (the re-review's N5). `--x-report` is
   coarse: an output that depends on x for one input vector is dropped entirely. On Odin III's
   side x is known, so a precise, ABC-only check exists: write each x as a free input `u`,
   duplicate the netlist for `u = 0` and `u = 1` (`d0`, `d1`), and prove
   `∀in: d0(in) ≠ d1(in) ∨ d0(in) = g(in)` against the golden `g` — "Odin III's output agrees with
   Parmys wherever Odin III's output is defined".

## 15. Performance (EL-50)

Measured oracle (Yosys 0.55 as in Parmys, `read_verilog -sv -nolatches` + `hierarchy -check
-auto-top -purge_lib`, dev machine i7-13700 WSL2, 2026-10-11): **mcml.v** 0.48 s, 104 MB peak
RSS — 43 modules, 33,255 wires (970k wire bits), 6,237 cells, 554 processes, 16 memories;
**koios lenet.v** (11 MB) 14.3 s, 1.78 GB — 138 modules, 555k wires (2.0M bits), 381k cells,
16,314 processes, 735 memories (12.3M memory bits).

Targets for 2D, Release, dev machine, measured by `tests/bench/bench_elab.c` (`--elab` mode of
the 2C bench: per file, smallest first, the time of stage 1, binding, stage 2 separately, nodes
of both stores, IR objects, `odin3_ast_bytes_reserved`, peak RSS):

| Measure | Target |
|---|---|
| mcml.v, stage 1 + stage 2 (parse excluded) | ≤ 0.5 s |
| mcml.v, whole `read_verilog` peak RSS | ≤ 200 MB |
| lenet.v, stage 1 + stage 2 | ≤ 10 s |
| lenet.v, peak RSS | ≤ 2.0 GB |
| the 31 VTR files, total elaboration | ≤ 3 s |
| complexity | stage 1 O(parsed nodes × specializations + evaluation steps); stage 2 O(elaborated nodes + IR objects + Σ process statements × output bits) |

**lenet.v estimate** (the review's F28; the bench reports each term so a miss is explainable).
IR record sizes from `src/ir/ir_internal.h`: a pin record is 24 B plus 4 B in its net's pin array
(28 B); a net record ≈ 48 B; a node record ≈ 48 B plus its parameters in the arena; pin arrays
round up to their size class (×≈ 1.3 on average). Yosys's 381k cells at ≈ 60 pins each (a 16-bit
binary word cell has 48–49) are ≈ 23M pins → 23M × 28 B × 1.3 ≈ **0.84 GB**; nets: 2.0M wire bits
plus ≈ 6M cell-output and temporary nets (381k cells × ≈ 16 output bits) ≈ 8M × 48 B ≈ **0.38
GB**; the 16,314 `$proc` nodes add their `I`/`O` pins (≈ 2M pins, ≈ 0.07 GB) and bodies (≈ 0.05
GB); nodes 0.4M × 48 B ≈ 0.02 GB; wires 555k × 64 B + net vectors ≈ 0.04 GB; the two AST stores
coexist during stage 1 (the parsed store, ≈ 3.4M nodes × 41 B ≈ 0.14 GB, is freed before stage
2) and the elaborated store stays (≈ 0.14 GB); memories are not materialised (no `$readmem`
outside `ifdef QUARTUS`). After `proc` (2E, not 2D) the dead `$proc` tombstones keep their bodies
(≈ 0.05 GB, EL-33). Total ≈ **1.5–1.6 GB** at the stage-2 peak, inside the 2.0 GB target
with ≈ 25% headroom; the byte budget (EL-46) is 60% of the dev machine's 23 GB, so it never
fires on lenet.

The output goes verbatim into `docs/PHASE2.md` with 2D's entry; a missed target is investigated
and reported, never relaxed (2A's rule).

## 16. Review focus and concerns

1. **Don't-care resolution against Parmys** (§14.6). The bridge avoids it for 2D's own tests
   because both sides go through one Yosys; the exit test does not, and depends on 2F mirroring
   Yosys's x rules and on the `--x-report` outcome Peter rules on (§20 item 13).
2. **Width and sign typing.** EL-13 is the rule most likely to be subtly wrong (context through a
   ternary, a comparison inside an addition, a function result, an unsized parameter). The typing
   table tests, Chu's examples and the differential generator are the guard; reviewers should
   trace three expressions by hand through both sweeps.
3. **Blocking substitution.** The `lv`/`rv` maps with case temporaries reproduce Yosys's
   `ProcessGenerator`; a missing save/restore around a case branch gives a netlist that is wrong
   only on some paths. The exact-body tests of §14.1 pin it.
4. **Two passes per specialization.** Pass W must replay pass E's decisions exactly; any
   divergence makes `INSTANTIATION` names or generate blocks inconsistent. Pass W asserts the log
   is consumed in order and completely.
5. **The `$proc` cell.** It keeps every IR invariant by construction, but its `verify` hook is the
   only guard on the body; a body position off by one points at the wrong pin and still passes
   rule 5. The malformed-body tests of §14.1 and the round trip through `write_rtlil` and Yosys
   are the guards.

## 17. Divergences (from Yosys/Parmys, or from IEEE 1364-2005)

| # | 2D | Yosys/Parmys | IEEE | Why |
|---|---|---|---|---|
| 1 | names may be used before their declaration in the scope | same | declare before use | Parmys compatibility |
| 2 | modules not reachable from the top are not elaborated | elaborates every module at read time (errors in unused modules fail) | — | elaboration is per specialization; accepting more is harmless |
| 3 | `if (A) … else if (B) …` generate: the else branch is a scope, named like the then-branch; names nest as `genblk1.genblk1.x` for a first construct | same (verified on 0.55) | a directly nested conditional is not a separate scope (`genblk1.x`) | names match Yosys's |
| 4 | non-constant lvalue selects use the case method | mask-and-shift (`$shift`) unless `nowrshmsk` | — | no `$shift` cell needed; equivalent |
| 5 | loop variables are process-local unless read elsewhere | outputs (dead registers) | — | avoids a false T10 on a shared `integer i`; equivalent |
| 6 | `$readmem` files relative to the source file only | working directory first | tool-defined | DESIGN §4.0: never the working directory |
| 7 | project > named `#()` > first `defparam` > ordered `#()`; ordered `#()` with `defparam` accepted | same order; ordered + defparam is an error | `defparam` wins; last `defparam` wins | the exit bar is Parmys; the ordered case is undefined in Yosys |
| 8 | a specialization is keyed by resolved values; overrides equal to defaults share the base module; names `base$P=v` | `$paramod\base\P=…` whenever any override is given | — | fewer modules, IR-7 naming |
| 9 | automatic top = the single uninstantiated module, else an error | deepest hierarchy, ties to the last defined | — | DESIGN §4.0 and the project corpus; the micro harness passes `--top` |
| 10 | any undeclared identifier is an implicit net (warning) | same | only net lvalues and port connections | Parmys compatibility |
| 11 | an edge on a multi-bit signal is an error | same | uses the LSB | Yosys |
| 12 | T10, A4 are errors (T9 was one until 2D-7b, see #77) | accepted (A4/T10 later flagged by `check`) | T10 legal in simulation | the coverage audit's REJECT verdicts |
| 13 | `===`/`!==`: exact fold when both constant; a wildcard compare over the 0/1 positions when one side has x/z constant bits (warning); `$eq`/`$ne` otherwise | `$eqx`/`$nex`; `techmap` drops the x bits, giving the same wildcard compare (verified through the Parmys script) | x/z compared literally (always false against a signal in hardware) | matches Parmys's netlist |
| 14 | no `$pos` or `$sshl`; extension is wiring; constants folded at elaboration | `$pos`, `$sshl`; folding in `opt` | — | IR-1; same netlist after `opt` |
| 15 | memories are collected into one `$mem` at elaboration (raw-index ports, simlib applies `OFFSET`) | `$memrd`/`$memwr`/`$meminit` + `memory_collect` (raw-index ports) | — | the IR has no memory record |
| 16 | an unconnected input is driven by x | left undriven | floats (z) | an explicit don't-care, no undriven-net warning |
| 17 | a call's result width is its declared width everywhere | an unevaluated call's width is `\|l − r\|` (no +1) in `detectSignWidth` | declared width | a Yosys quirk, visible only for calls in untaken ternary branches |
| 18 | `$unnamed_block$<k>` numbered per module | global counter | — | determinism |
| 19 | `--nomem2reg` disables every mem2reg rule, rule 1 (net arrays) included, and errors on an array that then cannot be a memory | `-nomem2reg` skips the same rules; such designs fail later in `genrtlil` | — | follows Yosys; an earlier, located error |
| 20 | top candidates count instantiations in every generate branch, selected or not | counts the elaborated design | counts the elaborated design | the top is chosen before elaboration; only a module instantiated solely in a never-selected branch differs |
| 21 | an unsized constant in a concatenation is accepted at its stored width, with a warning | accepted silently (verified on 0.55) | illegal | the exit bar is Parmys |
| 22 | an unsized literal of exactly a ranged parameter's width stays unsized (extends by its top bit) | same (`simplify.cc` 2381–2390, verified) | sized to the range | matches Yosys's values |
| 23 | a wider output connection of a cell type or `(* blackbox *)` port is zero-extended | sign-extended when the black box's port is `signed` | sign per the port | IR-7b ports carry no signedness |
| 24 | a `$pow` read from BLIF (a `.subckt $pow`, as Yosys's `write_blif` writes it) is unsigned: `A_SIGNED` = `B_SIGNED` = 0, the widths implied by the formals | `write_blif` writes `.subckt $pow` with no parameters (unless `-param`, which the Parmys script does not use), so its own BLIF loses the operands' signedness too | — | BLIF carries no sign; no Parmys golden has a negative exponent (controller ruling on the Task 1 review) |
| 25 | a module's `(* top *)` attribute does not choose the top (EL-7's rule decides) | `hierarchy` takes a `(* top *)` module before `-auto-top` (`passes/hierarchy/hierarchy.cc` 1008–1013 in VTR's 0.55) | — | DESIGN §4.0's top rule; no VTR benchmark or micro uses the attribute (v4.5) |
| 26 | a project number keeps its literal's sign (`param top.u.W 3` is a signed 32-bit 3) | `chparam -set W 3` makes an unsigned `RTLIL::Const` | — | a project value means what the same text means in a `#()` (v4.7); only an untyped parameter in signed arithmetic sees the difference |
| 27 | gate arrays (`and g[1:0] (y, a, b)`) are expanded per EL-10 with P = 1 | fails: "Cell arrays of primitives are currently not supported", ending in an assertion at `simplify.cc` 2755 (verified on 0.55) | legal (Q5) | coverage row Q5; Parmys fails where 2D succeeds, so no golden can disagree (v4.8) |
| 28 | an output or inout connected to anything but a net lvalue (`a + b`, `w[i]`) is EL-10's error | elaborated, with "Drivers conflicting" warnings; only a constant output is an error | not a net lvalue: illegal | EL-10 I3; the constant cases match Yosys's own error (v4.8) |
| 29 | an instance array's lowest index gets the connection's low bits whatever the range's direction | same (verified on 0.55) | bits are distributed by range position (the left index gets the leftmost bits), which differs for ascending ranges | the exit bar is Parmys (v4.8) |
| 30 | a parameterized `(* blackbox *)` module keeps its name and one ports-only `MODULE` (the defaults' ports); each `INSTANTIATION` carries **every** parameter's final value | same name and module, the module's ports at the defaults' widths (verified on 0.55: PR 2D-3 final review probe 17; `pb #(.W(2))` beside a default `pb` in the fix round); the cell's `parameter`s are only the overrides given (none for a default instance) | — | EL-4: an `INSTANTIATION`'s params LIST holds the specialization's final values for every module instance; the values are the same, only the defaulted ones are spelled out (v4.10) |
| 31 | an empty non-ANSI port anywhere but last (`module n(a, , b)`, `(, a)`) is a syntax error; a trailing comma is accepted | same ("syntax error, unexpected ','"; verified on 0.55: probe 20, and `(, a)` and `(a, b, )` in the fix round) | legal (`port ::= [port_expression]`) | Yosys; IEEE's empty position has no use but a dropped connection (v4.10) |
| 32 | a user-named generate block and another declaration of its name in one scope (`wire g;` … `begin : g`) is M11's error (`'g' is already declared (first at …)`); an implicit `genblk<n>` beside a later user declaration of that name is accepted, as Yosys does (EL-21) | both accepted: the wire is `g`, the block's objects `g[0].t` (verified on 0.55, Task 14's batch, c9) | a generate block's name is declared in its scope: a clash is an error | IEEE: a name that is both a net and a block is ambiguous in a HIER_NAME (Task 14) |
| 33 | two generate constructs with one user block name in one scope (`if (1) begin : a … end` twice) are M11's error (`'a' is already declared (first at …)`) | both accepted and merged: `a.genblk1.q` and `a.z` (verified on 0.55, Task 14 review, M1) | the second declaration of a block name in a scope is an error | IEEE: a scope's names are unique; Yosys has no such check on generate blocks (Task 14 review) |
| 34 | a user block `genblk2` beside an implicit `genblk2` (two sibling `GEN_BLOCK`s of one name) is M11's error from the binder, after stage 1 has accepted the store; stage 1's naming does not pad past a user-named block | both accepted and merged: `genblk2.a` and `genblk2.b` (verified on 0.55, Task 14 review, M2) | the same name twice in a scope: an error | Yosys's seen set never holds a user-named block (`named_genblk`, EL-21), so only the binder sees the clash (Task 14 review) |
| 35 | stage 1 errors on a non-variable, non-parameter declaration in a block, function or task (`event`: "this declaration is not supported in a block or a subroutine") and on a repeated declaration of one name beyond the legal port pair (`wire x; reg x;`, `wire x; wire x;`: "'x' is already declared (first at …)", M11) | `event` in a block is a syntax error (as here); the repeated declarations are accepted and make one wire (probed on 0.55). A `localparam` in a function is accepted, as here since v4.13. Informational, Yosys accepts and we reject: a local param whose value reads a net (`localparam P = w;` in a function: "parameter 'P' is not constant (it reads 'w')") and a repeated local param name (`localparam A = 3; localparam A = 4;`, Yosys keeps the last; M11 here). Informational, we accept more than Yosys: `output q; reg [3:0] q;` (Yosys: "Incompatible re-declaration of wire"), `reg q; output q;` (reg before the direction; Yosys: "neither input nor output"), `` `default_nettype wand`` (Yosys errors "Unsupported default nettype" at the directive; we error only at the first implicit use of tri0). Informational, we reject more than Yosys: `{-1{a}}`, `{2'bx1{a}}` and `{0{a}}` alone, and `parameter P = q;` with q undeclared. Informational, Yosys rejects and we accept: a duplicate block name in two `always` blocks (stage 2 or the binder may catch it) | a name is declared once per scope: the repeated forms are errors | the plan's rulings (Task 15): a repeated name is an error, and the list records where Yosys and stage 1 differ so no later task reads one as a bug (v4.12, v4.13, v4.15) |
| 36 | stage 1's REJECT scan covers every parsed node of a reached module, including generate branches no condition selects; its statement-level rules (T12, K12, F2) read the enclosing construct | a construct is rejected only when the elaborator reaches it | — | one scan per definition gives deterministic, source-ordered errors and the same text for every specialization (Task 16) Because of it a module whose scan found an error skips passes E and W (§9.3, v4.15): its errors of those passes, and its children's REJECT rows, appear only after it is fixed |
| 37 | a port declaration and the net or variable declaration merged into it that both carry a value (`module m(input a, output wire w = a); wire w = ~a;`, `output reg q = 1'b0; reg q = 1'b1;`) are the error "port 'w' has a value in both of its declarations" at the second declaration | accepted: two drivers of `w` (`connect \w \a` and `connect \w $not…`), or two init processes for `q` (verified on 0.55, the PR 2D-4 final review's fixes) | a net is declared once with at most one declaration assignment; a variable has one initial value | the declaration assignment of a port is one value in the port's `DECLARATOR` (EL-4, v4.15); a second would have to be dropped or resolved, and both readings are silent wrong answers |
| 38 | a path into a named procedural block whose member does not exist (`blk.nope`) is the binder's error "'blk.nope' is not declared" | warning "Identifier `\blk.nope' is implicitly declared" and a new one-bit wire `blk.nope` (verified on 0.55, the PR 2D-4 final review's fixes) | an undeclared name is an error | Yosys treats the dotted path as one identifier and declares it implicitly (v4.15) |
| 39 | a generate block's or named block's name read as a value (`assign o = gb;`) is the binder-bound error `'gb' is not a net, variable or parameter` | an implicit one-bit net `gb`, with the warning "Identifier `\gb' is implicitly declared" (verified on 0.55: `connect \o \gb`) | an undeclared name is an error | the symbol table cannot hold a net beside the block of one name; the case is a typo in practice (Task 18, Task 13's carry m1) |
| 40 | a constant select index with an x or z bit (`w[1'bx]`) reads x bits; as an lhs it is the non-constant error (in a process the write is dropped with a warning, #58) | the index is read with x as 0 (verified on 0.55: `a[1'bx]` is `a[0]`, `y[1'bx] = c` drives `y[0]`) | an x or z index reads x (IEEE 1364-2005 4.2.1) | E17: the don't-care resolution of Parmys's `$shiftx` for variable indices; a constant x is the same case (Task 18) |
| 41 | a plain decimal (`1`, `5`) keeps its 32 bits in any context; the apostrophe forms (`'d1`, `'h3`, `33'd1`) are widened to it. The AST does not record the apostrophe, so a signed unsized decimal is taken as plain: `'sd5` is (wrongly) kept at 32 bits too | same, except `'sd5`, which Yosys widens (verified on 0.55: `y64 = a + 1` is `$add` with `B_WIDTH` 32, `a + 'd1` 64; `c ? a : 5` a 32-bit `$mux`) | an unsized literal is context-determined | matches Yosys's cells for every form but `'sd` (Task 18 fix round 1) |
| 42 | a non-constant `x[b -: w]` is one `$sub(b, w − 1)` on the index as generated, `x[b +: w]` has no cell for the base; `B_SIGNED` follows the index | `-:`: `$add(zext32(b), 1)`, `$sub(_, w)`, then a 32-bit signed `$shiftx`; `+:`: `$add(zext32(b), 0)` and the same `$shiftx` (verified on 0.55) | — | the plan mandates `b − (w − 1)`; the values are equal modulo 2^32 and the `$shiftx` reads the same bits (Task 18 fix round 1) |
| 43 | in a self-determined position (the operand of a reduction, `!`, `&&`, `||`, a ternary condition or a concatenation) an inner unary `~`, `-` or `+` over a signed name (`!(-s)`, `{~s, a}`) keeps `A_SIGNED` 1 on its cell | the inner cell has `A_SIGNED` 0: genrtlil passes no sign hint down a unary operator (verified on 0.55) | — | parameter only, the values are identical; deferred (Task 18 fix round 2) |
| 44 | one lvalue naming a net twice (`assign {n4[0], n4[0]} = c4[1:0] + 2'd1;`) is A4's error at the second part | accepted: one `$add` with two `Y` pins on `n4[0]`; the later `check` warns "multiple conflicting drivers" (verified on 0.55: `yosys -p "read_verilog t.v; proc; opt; check"`) | legal but multiply driven | an invalid IR otherwise (rule 4); the review carried it from Task 18 (Task 19) |
| 45 | a ternary with an all-`z` branch and `bufif`/`notif` become one `$tribuf` on a top-level output or inout port; on any other net they become logic (#79) | a `$mux` with a `z` input (and "Yosys has only limited support for tri-state logic"), `$tribuf` only after `tribuf` | tri-state nets | D3: the IR allows tristate drivers only on a bus that reaches a top port (Task 19); #79 supersedes the error for internal nets |
| 46 | the port widths of a source `(* blackbox *)` instance are exact for its parameter set (v4.19), as Yosys 0.55's connections are: a width expression over the cell's parameters, one table entry per set; two sets with equal IR integer values but literals of different sizes and a port width that depends on the size (`$bits`-like ranges, `~W`) are a located error; the cell's parameters are all of the set's (see #30) | the cell's connections have the widths of its own set (`hierarchy` derives the module for the check and drops it; verified on 0.55: `bb #(.W(16))` beside a default `bb` in one module, ranges `[W-1:0]`, `[W:0]`, `[2*W-1:0]`, a fixed `[7:0]` port, two parameters with one default, a string override; `module \bb` keeps the defaults' widths); an **instance array** is sliced by the *default* module's port width (`bb #(.W(16)) v[1:0] (.a(x2))` with a 16-bit `x2` is accepted, the elements extended to 16 bits; with a 32-bit one it is an error "Array cell … has invalid port vs. signal size") | — | 2D checks and slices an overridden array by its own set's widths (EL-10), so it accepts `x2` of 2 x 16 bits (Yosys refuses) and reads a 16-bit one as EL-10 does, not as Yosys's default-width slices; the `$bits`-like clash above has no Yosys counterpart (Yosys never names the set); `write_rtlil` stubs show `attribute \dynports 1` when a port width varies between sets, where Yosys sets it for any parameter-dependent range A string override of a black-box parameter whose default is numeric (`bb #(.MODE("fast")) u` against `parameter MODE = 1`) is the located error `value of parameter 'MODE' of 'bb' is not an integer`; Yosys 0.55 accepts it (verified: `hierarchy -top top; write_rtlil` keeps `parameter \MODE "fast"` on the cell and `parameter \MODE 1` on the module): fail-closed and rare, deferred. |
| 47 | in step (7) a wired bit with one driver is that driver's net merged into the wire's | no gate for one driver, the driver connected to the wire | — | the driver's net is only known after the assignments; the merge is pin-based (Task 19) |
| 48 | `(* nomem2reg *)` on an array exempts it from rules 2-6 only: a net array is still registers (rule 1), and a module's `(* nomem2reg *)` exempts every array of the module from rules 2-6 | `(* nomem2reg *)` skips the array, rule 1 included (a module's skips the whole mem2reg block): `(* nomem2reg *) wire [3:0] w [0:3]` stays a memory (verified on 0.55: `yosys -p "read_verilog t.v; write_rtlil"` shows `memory width 4 size 4 \w`) | — | the words of a net array are driven by `assign`, which no `$mem` write port can take; EL-27 already says rules 2-6 (Task 20) |
| 49 | the words of an array that became registers are wires named by the declared index (`w[1]` .. `w[4]` for `[1:4]`), a flattened array's by its row-major position | wires `w[0]` .. `w[size-1]`, and an implicit wire for any other declared index (verified on 0.55: `wire [3:0] w [1:4]` with `assign w[4] = a` gives wires `w[0]` .. `w[4]`) | — | one wire per word, no stray `w[0]` (Task 20) |
| 50 | a constant index past a register array reads x with `word select [7] of 'w' is out of bounds; the word is x`; as an assignment target it changes nothing, with `assignment to word [9] outside array 'w' is ignored` | x, silently (verified on 0.55: `assign y = w[7]` gives `connect \y 4'x`, no warning) | — | EL-43: every out-of-range select warns (Task 20) |
| 51 | an array with no read port, write port or initial value makes no `$mem` | a `memory` record stays until `opt_clean` (verified on 0.55: `memory width 8 size 4 \unused`) | — | the IR has no memory record (row 15), and a cell without ports is dead logic (Task 20) |
| 52 | the `replacing memory` warning is located at the array's declarator and names each place by the line of the node that makes it, a place inside a concatenation lvalue included | an unlocated warning; a place inside a concatenation lvalue has line 0 (verified on 0.55: `See c1.v:0` for `always @(*) {m[0], t} = {x, c};` with `reg [1:0] m [0:1]`; v4.18: a concatenation lvalue no longer forces registers by itself, so the line-0 place comes from the other rules) | — | diagnostics are located (§9); a line-0 place helps no reader (Task 20) |
| 53 | a constant-folded reduction or logical result (`&4'b1111`, `!1'b0`, `1'b1 && 1'b1`) keeps the context width of the expression it sits in (its value in the low bit, the rest zero) | Yosys keeps 1 bit there (verified on 0.55: `assign y = a + &4'b1111;` with an 8-bit `a` has `B_WIDTH 1` and `connect \B 1'1`, ours `B_WIDTH 8`, `8'00000001`; 6 of 450 random expressions of the review's differential differ, all this case) | — | the values are identical, only the width of an intermediate constant differs; the `$eq`/`$mux` cells around it see the same bits (PR 2D-5 review) |
| 54 | a non-constant `y[b -: w] = d` or `<= d` with an unsigned base `b` below `w - 1` whose slice straddles the bottom of `y` (`y[b -: 3]` of `[3:0]`, 3-bit `b` = 0 or 1) writes the bits of the slice inside `y` (v4.22) | writes nothing: the case method's offset `b - (w - 1)` is computed unsigned and wraps (verified on 0.55, `elab-equiv` reports `y[s -: 3]` with a 3-bit unsigned `s` as not equivalent: s = 0 and s = 1 leave `y` unchanged; the same for `[0:3]`, `[4:1]`, `[5:2]`; a signed base, `+:`, and the top end of `-:` are equivalent) | writes the in-range bits (IEEE 1364-2005 4.2.1) | Yosys drops data here; the straddling top end, the low end of `+:` and a signed base agree and are covered by `tests/verilog/case-bridge/select_straddle_*.v` (Task 22 fix round 1) |
| 55 | a non-constant select inside a concatenation target (`{y[s], z} <= d`) is the located error `non-constant index into 'y' is not supported in a concatenation target`; a word of a register array with a non-constant index stays pending until Task 23 | accepted, and silently wrong: the select is read as a `$shiftx` whose output wire is then assigned, so the write to `y` is lost (verified on 0.55: `{y[s], z} <= d2` leaves `y` undriven by the process and `$shiftx` output assigned) | an lvalue concatenation may hold a variable select | fail-closed: no silent wrong answer (Task 22) |
| 56 | an `if` or `case` whose condition or selector and items are literals and parameters folds to the branch it takes: no switch, and an output assigned only in a dead branch is no output of the block | `switch` on the constant with the matched item and an added default that holds the case temporaries (`switch 1'1 / case 1'1 / assign $1\y \a / case / assign $1\y \y`, verified on 0.55); the items after a match, and the items that cannot match, are removed before the outputs are collected (an all-dead `case` leaves an empty process), so a dead-only output is not an output there either | — | the same logic after `opt`; `elab-equiv` on `tests/verilog/case-bridge/const_fold.v` (Task 22) |
| 57 | a switch whose branches assign nothing by `=` and that has no `default` item has no default case (`if (e) q <= d;` is one case) | an empty default case (`case` with no action) on every `if` and `case` (verified on 0.55: `if (e) q <= d` ends in an empty `case `) | — | an empty case changes nothing **without `full_case`**; the IR body is smaller (Task 22). With `full_case` (attribute or metacomment) the empty default case is added as in Yosys, since a `proc_mux` reads an unlisted value of a switch with `FULL_CASE` and no default as x: `<=` arms hold (v4.24; `case-bridge/full_case_nonblocking_*.v`) |
| 58 | a constant index with an x or z bit as the target of a procedural select (`y[1'bx] = d;`, `y[1'bz] <= d;`) writes nothing, with the warning `assignment to bits outside 'y' [3:0] is ignored` | writes bit 0 (the index is read with x as 0; verified on 0.55: `y = 4'd0; y[1'bx] = d;` is `connect \y { 3'000 \d }`, `elab-equiv` reports NOT equivalent for x and for z) | an x or z index selects nothing (IEEE 1364-2005 4.2.1); the same rule as a read, #40 | silent in Yosys; the warning is EL-43's "every write outside the object warns" (Task 22 fix round 1) |
| 59 | a call frame's variables are plain process-local temporaries: `O` pins assigned in the case tree and by no sync action | Yosys makes `$0\f$func$…` temporaries for them and a sync action `update \f$func$…v 4'x` that drives the wire to x (verified on 0.55: `yosys -p "read_verilog f.v; write_rtlil"` on `q <= inc(d)`) | — | the sync action is a dead register after `opt`; EL-38's "no flip-flop for `v` or `inc`" |
| 60 | the actual of an input is read directly by the body (`$add` reads `d`, not the frame's `v`) | Yosys 0.55 does the same: `connect \A \d` (blocking assignment updates `rv`) | — | none: the brief's "reading the frame temporary of `v`" is the net the process also drives |
| 61 | a continuous-context call is a `$proc` whose output is the result variable's own wire; the expression reads that wire | Yosys wraps the call in `always @*` over `\f$func$…$result` and `connect`s the lhs to it | — | none |
| 62 | outputs of an `if`/`case` inside an unrolled loop are counted over every iteration's branches by the scan; the generator folds the taken branch per iteration | Yosys unrolls first, so a branch dead in every iteration writes nothing and its target is no output | scan-time unrolling (cost: the body scanned once per iteration) | none since v4.24 (review I2): emit drops every output no action assigns (its hold, sync update and driver claim), so the variable-indexed loop write `y[i] = a[i]` of a half of `y` holds and claims only that half, a branch dead in every iteration leaves its target unheld, and two blocks writing disjoint halves are no conflict; a real overlap is still `'y[3]' is assigned in two always blocks` (`call-bridge/loops_disjoint_halves.v`); a loop write under a runtime `if`/`case` (`if (a[i]) w[i] = b[i]; else w[i] = 0;`) claims only the bits some arm assigns (PR 2D-6 re-review N1, fixed in 2D-7b: a switch's exit action drives a target bit only where an arm assigned its case temporary), so two blocks writing disjoint halves that way are no conflict (`call-bridge/loops_if_else_both_halves.v` and the three siblings) while a real overlap is still rejected. |
| 63 | a register-array word with a variable index is supported for one-dimensional arrays only; a multi-dimensional one is the located error `non-constant index into the multi-dimensional register array 'm' is not supported` | Yosys's `mem2reg_as_needed_pass2` handles every dimension | the row-major address as a case selector | rare; the flattened address needs `$mul`/`$add` cells before the switch |
| 64 | the variables of a memory write join the open switches when the statement is built, and the root's start-of-block actions go in then | Yosys inserts `addr = x; data = x; en = 0` at the start of the top block when it simplifies the statement, before any process is generated | the scan listing them (it cannot count the writes of an unrolled loop or a call) | none in the body: the same cases, actions and temporaries a listed variable would give (bridge `mem-bridge`) |
| 65 | a word of a memory inside a nested concatenation target, and a non-constant bit or part select of a word, are located errors | Yosys splits the outer concatenation only (a nested one is not split) and shifts the data and the enable for a variable select | — | rare; both are refused loudly, never mis-written |
| 66 | two `always` blocks writing one word in one cycle: the later block's port wins (the priority is `$mem` v1's implicit later-port-wins; no `WR_PRIORITY_MASK` is emitted) | Yosys gives a port priority over the ports of its own process only (`current_memwr_visible`); across processes the result is undefined | IEEE 1364-2005 11: a race | `elab-equiv` reports NOT equivalent on a source that enables both (the bridge sources keep the enables exclusive). The micros `common/2r2w` (8-bit, two always blocks each writing `block_mem[addrN] <= wr_dataN` unconditionally) and `common/1r2w` (enables `en1`, `en2`) are this case, found by the `elab_equiv_micro` sweep of 2026-10-14 and listed in `tests/elab/micro_equiv_expect_fail.txt`. Counterexample: `addr1 == addr2`, `wr_data1 != wr_data2`, both enables on. Verified on the lowered netlists of each side (iverilog, `addr1 = addr2 = 3`, `wr_data1 = 8'hAA`, `wr_data2 = 8'h55`, read back the next cycle): Odin III 55 (the later block's port wins; Yosys's `memory` reads our `$mem` as `WR_PRIORITY_MASK 4'0100`), Yosys 0.55 `read_verilog` AA (`WR_PRIORITY_MASK 4'0000`, no order, `memory_map` lets the last port in its list win), iverilog on the source AA. The three tools disagree, so no value is IEEE-mandated. With different addresses (or `addr1 != addr2` whatever the data) both netlists and the source agree. Parmys golden (`golden/EArch/regression/verilog/common/{2r2w,1r2w}/*.parmys.blif`): one hard `dual_port_ram` per data bit with both write ports wired (`we1`/`we2`) and no priority encoded, so the same-address result is the primitive's, not the netlist's |
| 67 | `$readmemh/b` inside a loop of an `initial`, or an `if` whose condition is not constant, is a located error | Yosys reads it when the loop unrolls or the condition is constant | an `if` with a constant condition is the branch it takes (2D-7b: `if (INITIALIZE_FIFO == "yes") $readmemh(...)` in `dnnweaver.v`, else-if chains followed); a loop is not unrolled here | rare; the statement-level `$readmem` is the supported form |
| 68 | `$readmemh/b` loads the words that fit and warns once per call (`$readmemh of 'f' dropped N words outside the array (the first at address A)`) when the file has words past the last array word or after an `@addr` outside the array | Yosys drops those words silently | IEEE 1364-2005 17.2.8: an address outside the range is an error and the load ends | a write is never dropped silently; the in-range words are kept |
| 69 | a register array read with a variable index past its end (`m[ra]`, 3-bit `ra`, 4 words) reads x | Yosys's mem2reg truncates the address to `ceil_log2(size)` bits, so `ra` = 4..7 aliases words 0..3 | IEEE 1364-2005 7.4.1: an out-of-range read is x | `elab-equiv` reports NOT equivalent on such a source (the bridge sources keep the index in range); #50 covers constant indices |
| 70 | a task call as a statement of an `initial` block is not applied: `task call in an initial block is not applied and is ignored` | Yosys applies the call (verified on 0.55): when an `always` block also assigns the register, `initial tk(7);` gives it the `init` attribute 7; when none does, it drives the register constant (`connect \r 4'0111`) | — | rare; assign in the `initial` block itself |
| 71 | a signed `case` with a negative sized literal item beside a wider unsigned item (`case (sa) -4'sd1: ...; 8'd15: ...`, `sa` a `reg signed [3:0]`) takes the first item as the 8-bit `8'hFF` (the unary minus is context-determined to the case width, IEEE 1364-2005 5.5.1), so for `sa = -1` (zero-extended to `8'h0F` beside the unsigned `8'd15`) the second item matches | Yosys 0.55 gives the first item `8'h0F` (verified by `elab-equiv`: for `sa = -1` Yosys takes the first item, `y0 = 1`, Odin the second, `y0 = 3`; `sa == -4'sd1` agrees, only the case path differs) | IEEE 1364-2005 5.5.1 | Yosys evaluates the item without the case width. The bridge sources keep the items of one signedness |
| 72 | a function with a side effect on a variable `r`, called in an lvalue index while the right-hand side reads `r` (`y0[fi(a)] = r[0]`): the target is generated first, so the call's side effect is applied before the right-hand side reads `r` | Yosys 0.55 orders the call and the right-hand side's read of `r` otherwise (verified: `elab-equiv` reports NOT equivalent) | IEEE 1364-2005 leaves the order open (11.4) | a race by the standard; the bridge sources have no side effect in an index |
| 73 | `$readmemh/b` truncates a word with non-zero digits beyond the memory's width and warns once per call (`$readmemh of 'f' truncated N words wider than the memory's W bits (the first, 'w', at line L)`); leading zeros are no loss and no warning | Yosys 0.55 warns per word (`Literal has a width of 8 bit, but value requires 9 bit`) and truncates | IEEE 1364-2005 17.2.8: the file's word is sized to the memory | a write is never dropped silently; one warning per call as #68 (review I4) |
| 74 | a call is inlined from an explicit frame stack (2D-7, review I6): the expression walker is suspendable at a call, the statement code (an assignment's value, an if's condition, a call's actuals) pushes a generation frame for it and the frame loop pushes a call frame above, so one call inside another costs heap, never C stack; the nesting is capped at 1024 (`function call nesting deeper than 1024 at 'f'`, a resource cap). An index, a case selector or item, or a copy-out target that holds a call is still run by a nested frame loop (C recursion), capped at 128 levels (`function call nesting deeper than 128 at 'f'`); so is a call nested in a call's actuals in a continuous `assign` (`assign y = f(f(f(a)));`, PR 2D-7b review M9: the same cap and message; `tests/tools/test_inline_stack.py` runs 1000 deep under a 2 MiB stack); the cells, nets and names are those of the former recursion | Yosys inlines by AST rewriting to any depth | hoisting the calls out of the expression into temporaries first: it would run every callee body before the operators to its left, renumbering their nets, and read the variables a callee writes after it | the output is byte-identical to the recursive version (checked on the call, proc, case and mem bridges, the coverage rows and the micros); the rest of the C-recursive sites are exotic (a call inside an index or selector nested over 128 deep) |
| 75 | an unrolled `for` that never ends (`reg [3:0] k; for (k = 7; k >= 0; k = k - 1)`) is the located error `for-loop never ends: 'k' takes the value 7 again` when its test and step read only the variable (Brent's cycle check), else `loop creates more than 2097152 IR objects (is it endless?)` (`ODIN3_ELAB_MAX_LOOP_OBJECTS`) or the iteration cap `loop exceeds 1048576 iterations` | Yosys 0.55 has no limit: it unrolls until it runs out of memory | the caps of EL-37 | review M3: the cap used to fire only after 5.7 GB of cells |
| 76 | an `always` with no event control (`always x = a;`, `always #5 clk = ~clk;`) is `always @*`, with the warning "always without an event control treated as always @*" (T6; Peter 2026-10-13, PHASE2 #20) | Yosys 0.55 reads it the same way: the process has no sync rule, so it is combinational (verified: micros `task_endtask/{inside_port,outside_port,task_call_function,task_call_task}` are `elab-equiv`-equivalent) | an `always` with no timing runs forever in simulation (a zero-delay loop hangs) | accepted for Parmys compatibility; the warning names the risky construct |
| 77 | a bit assigned by both `=` and `<=` is accepted with a warning (T9; Peter 2026-10-13, PHASE2 #20); on one control path the warning says that simulation and synthesis may differ | Yosys 0.55 accepts it without a word: each assignment is an action and the last on a path wins (verified: `elab-equiv` equivalent for `q = a; q <= b;`, `q <= b; q = a;`, if/case arms, `full/matmul`; `syntax/multi_clock_reader_writer` equals Yosys's netlist under a merged-clock abstraction, `equiv-check` being single-clock) | a simulator applies the nonblocking update after the blocking ones, so `q <= b; q = a;` ends as `b` there and `a` in the netlist | Parmys compatibility; the warning marks the case where the netlist and a simulation disagree |
| 78 | `adder #(W) u (...)` (a positional or named width, bus connections) on the library's 1-bit hard `adder` with no `--lib` unit of that name is the located error "too many parameter values for cell type 'adder' (1 given, 0 parameters); the library cell has fixed port widths (an n-bit adder is a soft module Parmys supplies: read its vtr_primitives.v with read_verilog --lib, spec 3.1a)". With `read_verilog --lib vtr_primitives.v` the instance takes the library unit (the n-bit soft `adder #(WIDTH)`), with a warning (§3.1a precedence); micros `micro/adder_hard_block` and `micro/hard_adder_cin_propagation` elaborate and are `elab-equiv` equivalent to Yosys 0.55 with the same file read as a library (2D-7b) | Parmys reads `vtr_primitives.v`, whose `adder #(WIDTH=1)` is a soft module (`assign {cout, sumout} = a + b + cin`, buses a, b, sumout, 1-bit cin and cout) that its adder pass maps to a chain of 1-bit hard adders (the goldens' `adder_hard_block` netlist) | the 2D result is the soft module's `$add`, not the chain (see #80); a 1-bit `adder u(...)` keeps the hard cell even when the library is read | `lib/vtr.o3lib` declares `adder` as the 1-bit hard block the BLIF goldens instantiate (ports a, b, cin, cout, sumout; no parameter), and the writers, the BLIF reader's IR-7b check and the simulator rely on that; the n-bit instance needs the soft module, which the library read supplies. Bit-exact: the function is the same (`elab-equiv`), the structure is not |
| 79 | a tri-state driver on an internal net (a net with no bit on a top-level output or inout port once all merges are done) becomes mux logic, as `tribuf -logic`: the drivers of one net list are one `$pmux` (A = x, B = data with the last driver first, S = enables) and the warning "tri-state drivers on internal net 'data' converted to logic (no driver enabled: x; several enabled: undefined)" names it (D3; Peter 2026-10-13, PHASE2 #20). With several enables on at once the value is undefined (the `$pmux` rule; the netlist is the OR of the selected data); with none it is x; a net with a tri-state and an ordinary driver, overlapping but different bit lists, a driver over port and internal bits, a module-below-top `inout` or `output` port (review I1: the instance may drive a top-level port) and a child `inout` pin on the net stay located errors (§6.7) | Yosys 0.55 plain (`read_verilog; proc`) shorts the drivers: every driver's data reaches the net whatever the enables (micro `syntax/multi_assignment` becomes `busout = enable ? data : f` with `data` driven by the four data inputs at once, so `equiv-check` rejects its b.blif: "more than one driver"). With `tribuf -logic` after `proc; flatten` (`elab-equiv --yosys-tribuf`) the micro and `tests/elab/accept/D3_{single_driver,bus_one_hot,disjoint_halves}.v` are equivalent. `tribuf -logic` also keeps a `$tribuf` on any net with a bit on an output port, which 2D matches; it merges with the newest driver first and 2D follows it | a simulator gives z while no driver is on and x for contention; the netlist gives x (a don't care `opt` may resolve to any driver) and the OR of the data | Parmys reads the micro (it shorts the drivers, the same as plain Yosys); the general ruling follows the oracle's `tribuf -logic`, the one Yosys flow that treats the construct as a bus; the warning names the risky net. **Under `--vtr-compat` (Peter 2026-10-14, PHASE2 #27) Parmys's resolution wins:** a group of two or more drivers whose data is all input-port bits drops every enable and keeps, per bit, the input another cell reads, else the lowest-named wire, else the highest bit of one wire (Yosys's `opt_expr -mux_undef` then `opt_clean` representative), with the warning "net 'data' has 4 drivers; --vtr-compat keeps only the data of 'bus0' as Parmys does (...) ... (IEEE resolution would be a bus selected by the enables ...)"; micro `syntax/multi_assignment` becomes the Parmys golden `busout = enable ? bus0 : 1`. A group with a cell-output, constant or child-output driver, one driver, and top-level port nets keep the rules of this row under the option (Parmys leaves several drivers on one net there; the IR cannot hold them) | known divergence for 2H, closed by PHASE2 #27: without `--vtr-compat` the netlist is not the golden Parmys gives for `multi_assignment` (Parmys keeps only `bus0`; Yosys shorts the four drivers); with `--vtr-compat` (the VTR flow, the exit sweeps) Odin III follows Parmys (below). Constant enables (`1'b1 ? d : 'bz`) are not comparable with Yosys (it folds them into a plain connection, equiv-check then sees two drivers); 2D gives a `$pmux` over the constant selects (unit test `test_tribuf_constant_enables`). Without `--vtr-compat` the netlist is not the golden Parmys gives for `multi_assignment` (shorted drivers): it models a bus, which is what the source says. The don't care case is the part a user must not rely on Another known divergence for 2H (PR 2D-7b review M8): `y = bus | const` (a bus read through logic) is converted by 2D (`z | 0` is x by Verilog semantics), while Yosys `opt` folds `bus | 0` into the top-level output and keeps the `$_TBUF_`s, so `elab-equiv --yosys-tribuf` exits 2 (`only in B: $_TBUF_`); it is loud, never a false verdict. |
| 80 | a `read_verilog --lib` unit that shares a hard or black-box cell type's name (`vtr_primitives.v`'s `adder`, `multiply`, `single_port_ram` beside a tech library) is chosen per instance by the parameter rule of §3.1a, not always (Parmys: always the soft module, because it reads no cell of that name) | Parmys resolves every instance of `adder` to the soft module of `vtr_primitives.v` and maps it to hard adders later, in its own pass; the structural result for `adder #(W)` is a chain of 1-bit hard adders, ours a W-bit `$add` (2D-7b: `micro/adder_hard_block` and `micro/hard_adder_cin_propagation` are equivalent to Yosys but not netlist-identical to the Parmys goldens until 3E maps soft adders) | a 1-bit or parameterless `adder`, and `single_port_ram`/`dual_port_ram` with the cell's own parameters, stay the hard cells even when `vtr_primitives.v` is read; Parmys would build the soft RAM wrapper and map it later | an unconditional unit-over-cell rule would turn every 1-bit hard adder of a design into `$add` silently (review M5); a unit-never rule would make `adder #(W)` unreachable. The parameter rule keeps the golden-compatible meaning of every instance the cell can express and warns on each one it cannot | Bit-exact in function; the choice is logged (info or warning), never silent |
| 81 | an unused declared signal (a `reg`, an `integer` a `for` unrolls, a `wire` nothing reads) is a net with no pins; the IR check (rule 4) does not warn on a pinless net of a *named* wire. A callee's unused local gets an unnamed frame wire with no pin, which is deleted when the module is emitted (`rule 4: net has no pins` stays for a net of no wire or of an unnamed wire: a leak) | Yosys keeps a declared wire with no connections until `opt_clean`, and drops nothing at `read_verilog` | warning on every declared unused signal (286 on mkPktMerge: the warning hid real leaks); deleting declared signals (a design may name them in a later pass) | Peter's ruling of 2026-10-12 (PHASE2 #6): "net has no pins" is not a warning; IR.md rule 4 states the exemption; the only output change is the removal of one unreferenced `wire $nN` line per unused callee local |
| 82 | a variable-offset select write (`y[{1'b1,s} +: 2] = a;`, the case method of EL-32) followed in the same block by a `case`/`casez`/`casex` with two arms that write the same values (two empty `?` arms and a default; two arms with equal bodies) is emitted as written: the write's temporaries feed both arms' case temporaries, so one mux output feeds two slices of one `$pmux` | Yosys 0.55 `opt` (`opt_muxtree`) loses the write when `proc` is followed by `opt`: the first slice's select knowledge is written into the data pins of the mux the two slices share (the same bug as 2F's OP-13, §17 row 20 of the 2F spec: `n = $pmux(...)` feeding `$pmux(B = {n n}, S = {a1 !a1})` gives `y = 0` after `opt_muxtree` and `y = 2'b10` before; a hand-written `case (s)` source shows it on Yosys's own front end; Yosys 0.69 fixes it) | — | not a 2D miscompile (Task 28's seed 23 report said so; the root-cause check: `eval` of the RTLIL before opt, 2E `proc` + 2F `opt` and Yosys 0.69 agree, 0.55 does not). `elab-equiv` against 0.55 says NOT equivalent for the five sources of `tests/verilog/oracle-bug/`; `elab_oracle_muxtree_bug` pins both verdicts (0.69 equivalent, 0.55 not) and goes stale when 0.55 is fixed. 2D keeps Yosys's per-case temporaries (EL-36) rather than emit a different shape for one oracle's bug |
| 83 | `mem[i][p:q] <= d;` and `mem[i][b +: w] <= d;` (a bit or part select of a word of a register array whose index is a variable, in a process, including a `for` loop variable) is the located error `non-constant index into 'mem' is not supported here` | accepted (verified on 0.55: `mem[i0][2:0] <= a[2:0]` in a `for` loop) | — | the whole-word case of EL-27 writes every word's bits through one case over the words; a sub-word write needs a per-word bit range in the lvalue and in `pg_regs_apply` (reads and writes, blocking and non-blocking). Fail-closed and rare (Task 28 seed 134); the fuzz generator avoids it (`elab_profile.mem_index`). |
| 84 | a non-ANSI port merged with a `reg` or net that has unpacked dimensions (`output q; reg [3:0] q [0:1];`, either order) is the located error `'q' is a port and its declaration has unpacked dimensions; a port cannot be an array` at the declarator with the dimensions (PR 2D-7b review M10; it was dropped in silence) | Yosys 0.55 accepts it: the one-bit port `q` stays undriven and the array `q` is a separate memory (or registers `q[0]`, `q[1]`), so a design that writes `q[0]` does not drive the port | keeping the port and the array apart as Yosys does: nothing in a real design relies on it, and it needs a second symbol of one name in stage 2 | `test_port_with_unpacked_dimensions` (both orders; the legal merge stays clean); Yosys 0.55 probe |

| 85 | with `read_verilog --vtr-compat` (the VTR flow's setting), a non-constant `initial x = e;` (or `<=`, after an optional delay; a variable or a constant select of one) is `assign x = e;` when no `always` block assigns the bits and nothing else drives them, with the warning "initial value of 'x' is not constant: treated as a continuous assign (VTR/Parmys compatibility)"; otherwise, and without the option, the statement is ignored (x) with a warning (Peter 2026-10-13, PHASE2 #24) | Yosys 0.55 and Parmys read it as the combinational assignment (`connect \x \e`); a variable also assigned in an `always` block is Yosys's error `Failed to get a constant init value` | `initial` runs once at time 0: x holds `e` as it was then, and keeps it | the exit bar is Parmys, so the VTR flow follows it; the general case stays IEEE-sound (a value that changes with the input is not an initial value) |

## 18. Implementation split (EL-51)

Seven PRs; the plan (next) fixes tasks, files and interfaces. Model and effort per PHASE0 §9 and
PHASE2's 2D row (Opus `xhigh` for semantics, `high` once settled; the agent says when to switch).

| PR | Content | Depends on | Model/effort |
|---|---|---|---|
| 2D-1 IR | IR changes A (nine cells), B (the `$proc` cell: type, `ODIN3_VAL_PROC` body, `verify`, process counter, pass-manager `processes` flag and writer refusals), C (port range and sign); `write_rtlil`; ABI additions | the §20 rulings (made) | Opus `xhigh` (IR) |
| 2D-2 constants | `odin3_cval`, operators, typing (EL-13) with `UNSIZED` through parameters, the interpreter, `$clog2`; the Yosys 0.55 constant table | — (parallel with 2D-1) | Opus `xhigh` → `high` |
| 2D-3 stage 1 hierarchy | run context, lookup (and the taken-name checks), top, parameters and precedence, `defparam`/project push-down (cell-type targets included), specializations, pass E/W skeleton, connections, black boxes; the binder | 2D-2, 2C merged | Opus `xhigh` |
| 2D-4 stage 1 content | generate and naming, declarations and ports, implicit nets, `default_nettype`, IGNORE/REJECT diagnostics; `reject-2d`/`ignore-2d` on | 2D-3 | Opus `high` |
| 2D-5 stage 2 module | wires, ports, instances, gates, continuous assignments, expressions and selects, drivers, wired nets, tri-state and child `inout`s, memories, provenance, attributes, the byte budget; `--elab-only` | 2D-1, 2D-4 | Opus `xhigh` |
| 2D-6 stage 2 processes | the process generator and `$proc` emission, `case`, loops, inlining, memory writes, mem2reg, initial values, T9/T10 | 2D-5 | Opus `xhigh` |
| 2D-7 evidence | coverage rows and goldens (both latch modes), the bridge on Yosys 0.55, sweeps, fuzzers (0.55, nightly drift check against 0.69), `bench_elab`, `equiv-check --x-report` handed to 2H's backlog, the PHASE2 entry | grows from 2D-3; closes last | Opus `high`; Sonnet `high` for bulk goldens and bench runs |

Lanes: 2D-1 ‖ 2D-2, then 2D-3 → 2D-4 → 2D-5 → 2D-6 serially (they share the stage contexts),
2D-7 alongside. Each PR: tests first, `/lint`, a read-only critique before it opens (PHASE0 §A.1).

## 19. Amendments to other documents

Made when the PRs land (not by this spec):

- `docs/IR.md`: IR-19 (the `$proc` cell, EL-33: ports, parameters, the `ODIN3_VAL_PROC` value
  kind in IR-10, the `ODIN3_CT_PROCESS` type flag and the per-module counter); IR-9 (the RTLIL
  view allows `$proc`, the netlist view does not); IR-12's note that `hier` is module-relative;
  `odin3_port_spec`'s `has_range`/`msb`/`lsb`/`is_signed` (IR change C; IR-15's `module_add_port`
  summary names them). IR-3, IR-4 and IR-6 are unchanged; of the §9 check rules only rule 9 is
  tightened (IR change C): a port wire's sign must match its cell-type port's `is_signed`, and a
  scalar port is one bit wide.
- `docs/DESIGN.md`: §5.2 word list (+ the nine cells, + `$proc` as the process cell); §6 step 2
  ("processes" = `$proc` nodes) and the order the pass flag enforces (v4.2, mode P: `proc` before
  the `REFUSE` passes; the opt family may run before `proc`); §9 (the RTLIL row); §4.0
  (`--auto-blackbox`; the top rule unchanged).
- 1D spec and `docs/PASSES.md`: `odin3_pass_def.processes` and each built-in's value;
  `read_verilog` options; `write_rtlil`; `compact` (the symtab remap wired).
- 2A spec: §7's D1 warning is emitted by 2D, not 2E (EL-36); §4.6 points to EL-4 for the refined
  elaborated form, including the unsized `PARAM_DECL` value.
- `docs/specs/2026-10-09-2C-verilog-coverage.md`, row E12: 2D accepts an unsized constant in a
  concatenation at its stored width with a warning (Yosys and Parmys accept it; F6 ruling), instead
  of the located error the row describes.
- `tools/equiv-check`: `--x-report` (2H, §14.6).
- `docs/PHASE2.md`: 2D's entry (the controller writes the decision log).

## 20. Decisions for Peter

Ruled by the controller as agent defaults under Peter's overnight rule (2026-10-11), after the
Fable review; Peter may override any of them.

1. **IR change B — processes (EL-33).** *Ruled:* the review's representation: a `$proc` cell
   with real `I`/`O` ports and an opaque `BODY` payload (positions are pin indices; constants are
   pins on the shared constant nets), so IR-3's driver/sink queries and check rule 4 stay intact;
   `verify` validates the body; merge, delete and compact need nothing new; passes declare
   `processes = OK` or are refused. (v1's pinless process objects are withdrawn.)
2. **IR change A — registry cells (EL-31).** *Ruled:* add nine cells, `$neg $xnor $reduce_xnor
   $reduce_bool $logic_not $logic_and $logic_or $shiftx $pow`; `$shift` is dropped (nothing emits
   it). Appendix A keeps the compositions for reference.
3. **IR change C — port range and signedness (EL-24).** *Ruled as recommended,* with the
   review's `has_range` form.
4. **Latches.** *Ruled as recommended:* `--nolatches` off by default; the 2H harness and the Phase
   3 VTR scripts pass it, as Parmys does; both modes are tested (§14.2).
5. **Parameter precedence (EL-8).** *Ruled as recommended:* project > named `#()` > first
   `defparam` > ordered `#()` (Yosys, verified), with a warning when a `defparam` loses.
6. **Top selection (EL-7).** *Ruled as recommended:* DESIGN §4.0's rule; the 2H micro harness
   passes `--top` from the Parmys golden's first model.
7. **Undefined modules (EL-11).** *Ruled as recommended:* `unresolved_module`, with
   `--auto-blackbox` opt-in.
8. **Memories (EL-27).** *Ruled as recommended,* with the review's correction: Yosys's
   mem2reg-as-needed rules; one collected `$mem` per remaining array, asynchronous read ports,
   raw-index addresses (simlib applies `OFFSET`).
9. **`write_rtlil` in 2D (EL-48).** *Ruled as recommended,* on Yosys 0.55 for every comparison.
10. **Stage-2 failure (EL-2).** *Ruled as recommended:* a failed stage 2 leaves valid but
    incomplete modules (every port connected, undriven nets at worst); no module-delete primitive.
11. **Parmys leniency (EL-25, EL-23).** *Ruled as recommended:* implicit nets for any undeclared
    identifier and declarations in unnamed blocks, each with a warning; likewise (F6) an unsized
    constant in a concatenation is accepted with a warning.
12. **Unreachable modules (EL-7).** *Ruled as recommended:* not elaborated (an info line lists
    them).
13. **New (the review's F12): don't-cares in the exit comparison (§14.6).** *Agent default under
    the controller's F12 ruling:* keep x explicit through 2D, mirror Yosys's x rules in 2F, write
    surviving x as Parmys's `unconn`, and add `equiv-check --x-report` so 2H reports
    identical / equivalent / equivalent outside x-cones / differs. *For Peter:* whether
    "equivalent outside x-cones" (outputs listed and reviewed) may count toward PHASE2 #4's exit
    bar; recommended: no by default, yes per design when its listed outputs are reviewed and
    recorded in the exceptions list.

## Appendix A. Compositions considered for IR change A

Kept for reference (§20 item 2 adds the cells). Each uses only the pre-2D registry; `w` = the
node's final width, operands as in EL-30.

| Cell | Composition |
|---|---|
| `$neg(A)` | `$sub(A = const 0, B = A)`, both `SIGNED` flags = `A`'s, `Y_WIDTH` = w |
| `$xnor(A, B)` | `$not($xor(A, B))`, both at w |
| `$reduce_xnor(A)` | `$not` (`A_WIDTH` = `Y_WIDTH` = 1) of `$reduce_xor(A)`; bits 1.. of Y wired to constant 0 |
| `$reduce_bool(A)` | `$reduce_or(A)` |
| `$logic_not(A)` | `$eq(A, const 0)`, `Y_WIDTH` = w |
| `$logic_and(A, B)` | `$and($reduce_or(A), $reduce_or(B))` with `A_WIDTH` = `B_WIDTH` = 1, `Y_WIDTH` = w |
| `$logic_or(A, B)` | as `$logic_and` with `$or` |
| `$shiftx(A, B)` | `$mux(A = $shr(A, B'), B = all-x constant, S = out_of_range)` where `B'` is `B` made non-negative and `out_of_range` = `B < 0` (signed B) or `B > width(A) − w` (`$lt`/`$gt` cells); x stays explicit |
| `$pow(A, B)` | constant base 2: `$shl` (as now); otherwise square-and-multiply over B's bits: for bit k, `p_k = $mux(1, a_k, b[k])`, `a_{k+1} = $mul(a_k, a_k)`, product of the `p_k` with `$mul`, all at w; a negative signed exponent selects Yosys's special values with a final `$mux` |
