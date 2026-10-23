# adapters/slang — the SystemVerilog adapter (v1.1.0)

C++20 shim linking the vendored slang (`third_party/slang`, `VENDORING.md`/`BUILDING.md`). It
walks slang's **elaborated** AST (post-parameter, post-generate, packages and imports resolved)
and writes the Odin III **elaborated** AST (2D spec EL-4) through the public `odin3_ast_*`
builders, then runs stage 2 (`odin3_ast_elaborate`). It exports exactly one C function,
`odin3_read_slang` (`odin3_slang.h`), and is built as a separate shared library
(`libodin3_slang.so`) so the core stays C17 (DESIGN §4.2, §12.1; ADR D8′).

Invocation: the CLI registers `odin3_read_slang` with `odin3_sv_reader_register` (ABI v6) at
start-up. `read_verilog` hands every design file whose project language is SYSTEMVERILOG, or
whose path ends `.sv`/`.svh`, to it; a read mixing Verilog and SystemVerilog files, `--lib` and
`--parse-only` are refused for SystemVerilog for now.

## What it maps

| SystemVerilog | Odin III elaborated AST |
|---|---|
| module ports (`input/output/inout logic`, `wire`, `reg`, `int`) | `PORT_DECL` (`IN_HEADER`, `NET` = wire or `DT` = reg/integer), one `DECLARATOR` |
| `logic`, `wire`, `reg`, `bit`, `integer`, packed vectors, packed/unpacked arrays | `NET_DECL`/`VAR_DECL`, packed range in the declaration, unpacked `RANGE`s in the declarator |
| module `parameter`/`localparam` | `PARAM_DECL[localparam]` with the final `NUMBER`/`STRING` value; instances carry one named `CONNECTION` per parameter |
| packages, wildcard/qualified imports, `enum` values, `$bits`, constant expressions | folded to `NUMBER`/`STRING` leaves (no package or enum names reach stage 2) |
| packed **and** unpacked structs (+ unions) | one packed vector `[W-1:0]`; `s.f` is a `SELECT` of `f`'s bit range (first field MSB) |
| `always_ff` / `always_comb` / `always_latch` | `ALWAYS` with that sub; `always @(...)` is `ALWAYS[always]` over `TIMING_STMT[EVENT_CONTROL, S]`, `@*`/`@(*)` the `STAR` event control |
| `assign`, `initial`, blocking/nonblocking/compound assignments, `if`, `case/casez/casex`, `for`, `repeat`, `while`, blocks, local declarations | the matching Odin nodes; a `StatementList` is an unnamed `SEQ_BLOCK` |
| module instances (ordered/named, arrays expanded by slang) | `INSTANTIATION` (the child specialization's name) → `INSTANCE` → one named `CONNECTION` per port in port order |
| interface instances | flattened into the module: each signal is a net named `<instance>.<signal>`; an input port of the interface is tied with a `CONT_ASSIGN` |
| interface ports (with a modport) | flattened into one module port per modport signal, named `<port>.<signal>`, in modport order; the parent's connection is `<iface instance>.<signal>` |
| generate | already expanded by slang; scope names are flattened into the net names (`genblk1.x`), so no `GEN_BLOCK` is emitted |
| block-scoped variables and `for` loop variables | hoisted: named `blk$<k>$<name>` (block locals stay in the `SEQ_BLOCK` locals list; loop variables are declared at module level) |

Source locations: each slang source buffer becomes a design `FILE` buffer with its line table;
`loc`/`end` are copied verbatim (a macro-expanded location maps to its fully expanded file
location), and the IR's `SOURCE` records point back at the `.sv` text.

## Agent defaults and limits (v1.1.0)

Defaults taken while implementing, all visible here and in the lane's `## Result`:

- **Specialization names**: the top keeps its definition's name; every other distinct instance
  body takes the definition's name, with `$2`, `$3`, … when the name is taken (EL-9's
  `base$P=v` value-keyed names are stage 1's; the adapter only needs unique, deterministic
  names). Bodies of the same definition with the same types share one `MODULE`
  (`InstanceBodySymbol::hasSameType`, instance caching disabled so interfaces flatten per
  instance).
- **Parameter value widths**: a parameter's `NUMBER` carries the final value's width (not EL-4's
  0 for a still-unsized value; slang's `SVInt` does not keep `is_unsized`). This changes only how
  a value extends in a wider context, and the value itself is exact.
- **Unpacked structs/unions** are flattened to a packed vector too; all member accesses go
  through the adapter's offsets, so the model is self-consistent.
- **Unsupported, a located `ODIN3_ERR_PARSE`, never a silent drop**: functions and tasks (and their
  calls), `foreach`/`do while`/`forever`, `break`/`continue`/`return`, `case inside`,
  `unique/priority`, intra-assignment timing, delays/strength, real/realtime/string variables,
  assertions, processes/final, `ref` ports, interface-port pass-through (a parent port connected
  to a child interface port), and `--auto-blackbox`.
- `--lib`, `--parse-only` and mixed Verilog/SystemVerilog reads are refused for SystemVerilog;
  `read_verilog -icells`/`-netlist` are not applied on the SV path.
- A library gate instance read back from structural Verilog binds to a registered
  bit-granularity cell type (`src/frontends/verilog/elab1_lookup.c`, `resolve_outside`): this is
  what makes `write_verilog`'s gate-cell netlists (BUF/INV/NAND2/NOR2/DFF) readable by Odin III
  again.

## Tests

- `tests/unit/test_read_slang.c` (CTest `test_read_slang`): `odin3_read_slang` for the golden
  `sv_package` project, `tests/unit/data/slang/sv_types.sv` (package, enum, struct, memory,
  always_comb/ff/latch) and `sv_iface.sv` (interface instance and port): module/port counts, the
  ALWAYS subs, flattened names `s.data`/`bus.valid`, and the AST-2 range and provenance
  invariants on every node.
- `tests/cli/slang_flow.sh` (CTest `cli_slang_flow`): the five verilog-eval references copied to
  `tests/unit/data/slang/verilog-eval` plus the golden project and the two fixtures go through
  `read_verilog; proc; opt; lower; abc --gates cmos6; dfflibmap cmos6; write_verilog` and
  `write_blif --gates-as-names`; the structural Verilog is read back with the owned front end and
  `tools/equiv-check` proves the two BLIFs equivalent, with Yosys 0.55 as the oracle when it is
  on PATH. `tests/golden/slang/alwaysblock2.blif` is the gate golden.
