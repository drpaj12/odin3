# Hierarchical references into another module (2026-11-02)

Purpose: tinyriscv (Seeker-Proc, Leman Russ III) reads `u_tinyriscv.u_regs.regs[26]` in an always block of
`tinyriscv_soc_top.v`, which `read_verilog` rejected as E21. Measure: the design elaborates, as under Yosys.

## What Yosys does (0.69, the binary in external/yosys/build, read in `frontends/ast/simplify.cc`)

Yosys has no hierarchy in a name. A dotted identifier is looked up in the module's scope as one string;
when nothing answers and `default_nettype` is not none, it becomes an auto-wire of that name (warning
"Identifier `\u.w' is implicitly declared"). Checked cases:

| source | Yosys | Odin III |
| --- | --- | --- |
| `assign z = u.w;` (u an instance) | auto-wire `\u.w`, undriven | same, WARNING |
| `leaf v(.a(u.w), ...)` (port connection) | auto-wire | same, WARNING (not an error) |
| `f.x`, f a function | auto-wire `\f.x` | same, WARNING |
| `top.a`, `top` the module, `a` declared | `a` (prefix popped, `try_pop_module_prefix`) | `a`, no warning |
| `top.nope` | auto-wire `\top.nope` | same, WARNING |
| `wire \u.w ;` declared, `u.w` used | that wire, no auto-wire | that wire, no warning |
| `g.s` (generate block g with s) | the flattened `\g.s` | the same (resolved path) |
| `g.sub.x` (g lacks sub) | auto-wire `\g.sub.x` | ERROR (stricter) |
| `u[0].w` | auto-wire `\u[0].w` | ERROR (stricter) |
| `u.f(1)` | ERROR can't resolve function | ERROR |
| `#(.P(u.w))` | ERROR non-constant value | ERROR |
| `localparam L = u.w;` | silent while unused | ERROR (stricter) |
| `[u.w:0]` range, `generate if (u.w)` | ERROR non-constant | ERROR |
| `default_nettype none` + `u.w` | ERROR implicitly declared | ERROR |

Errors that survive in Odin III: an index on a path that names no block, a call, a parameter override, a
constant expression (localparam, range bound, generate condition), a generate-block prefix whose member is
missing, and any reference under `default_nettype none` (also tri0, tri1, trireg). Cases: `tests/verilog/reject-2d/E21_*`,
accepted ones `tests/verilog/ignore-2d/E21_*`, unit tests in `tests/unit/test_elab_stage1_gen.c`.

Flatten: after `flatten` the foreign net keeps its name `u.w` and the real child wire `w` of instance `u` becomes
`u.w_1` (the unique-name rule). The foreign net is undriven and not connected to the child, as in Yosys. Pinned by
`tests/flatten/hierref.v` with `tests/flatten/expected/hierref.*` and `tests/tools/test_hier_ref.py`.

## tinyriscv report

- Elaboration: `tinyriscv_soc_top.v` elaborates with 2 warnings (the foreign reference nets).
- `lower` failed: no bit-level `$tribuf` from the gpio inout (fixed in `fix/tribuf-simtb`: the enable is dropped, WARNING).
- A patched copy (the inout replaced) lowers and simulates.
- `sim_tb` limited an instance to 16 port connections and had no `%t` format; both fixed in `fix/tribuf-simtb`
  (4096 connections; `%t` in the timescale precision).
