# 1D — Pass manager, C ABI v0 for the IR, Python plugin walk

Status: agent-approved under PHASE1 #11 (Peter delegated the ABI like the IR; he reviews
afterwards). Phase 1, sub-project 1D. Spec §3, §15.3; ADR D8′.

## Goal

1. A **pass manager**: every reader, writer and transformation is a registered pass; running a
   pass opens a provenance pass run (IR-13), runs `check` before and after in Debug builds
   (spec §3, CLAUDE.md rule 3), times it and logs it.
2. A **public C ABI** in `include/odin3/odin3.h` exposing the IR to plugins and the CLI: design
   handles, pass registration and invocation, ID-based iteration and accessors, attributes,
   cell-type registration, provenance queries, logging.
3. The Phase 1 exit test (spec §12): **a Python plugin can walk the IR** through `cffi`.

## Pass manager

`odin3_pass_def {name, help, run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args)}`;
a process-global registry (built-ins + plugins, like cell types); `odin3_pass_run(design, name,
args)` = begin a pass run named `name` → `check_design` FULL (Debug, or `--check` in Release) →
run → `check_design` FULL → log `pass <name>: <ms> ms`; a failing post-check fails the pass.
Built-in passes in Phase 1: `read_blif <path>`, `write_blif <path>`, `read_techlib <path>`,
`check [--fast]`, `compact`, `write_json/verilog/dot <path> [opts]`, `stats`, `sim <cycles>
<seed>` (when 1E lands). The CLI runs a script: `odin3 [--plugin x.so]… -p "read_blif a.blif;
check; write_blif b.blif"` or `odin3 script.o3`.

## C ABI v0

Rules: only `odin3_` names; opaque `odin3_design`; IR IDs are plain `uint32_t` in the ABI with
the module passed alongside (IR-5); strings are `const char *` valid until the design changes
(documented per function); every function has the purpose/ownership/failure paragraph
(CLAUDE.md); NULL handles are `ODIN3_ERR_INVALID_ARG` at the ABI boundary (the internal "never
NULL" rule is enforced here, not trusted); the cffi cdef block stays attribute- and
preprocessor-free. Groups:

- design: create, destroy, run pass, top module (get/set; PHASE1 #18), module count/at, find module
  by name;
- module: name, node/net/wire/port counts and ID ends (iteration in ID order with liveness),
  find node/net/wire by name;
- node: type name, granularity, name, parameter count / name / value (int, bits as a string of
  `0 1 x z`, string, cover as text), pin span per port, port count/name/direction/width;
- pin: node, port, bit, net; net: name, driver pin, pin count/at (drivers first), alias names;
- attributes: get/set string attributes on node/net/wire/module;
- cell types: register a type from a plain-data definition struct with **reserved hook slots**
  (`void *reserved[4]`, per the 1B final review) so later hooks do not break the ABI;
- passes: register a pass (plugin `odin3_plugin_init` calls it);
- provenance: for an object, visit its source locations (`file`, `line`, `col`) via callback;
  for a `file:line`, visit the objects (module, kind, ID, live) via callback;
- logging: set level, set sink (forwarded from `util/log`).

ABI version: bumped once for this sub-project (from the value 1C leaves, 2 → 3).

## Python

`plugins/python/odin3.py` (cffi ABI mode against the header's cdef block) gains a thin Pythonic
layer: `Design.read_blif(path)`, iteration helpers (`design.modules()`, `module.nodes()`,
`node.pins()`, `net.driver()`), and provenance (`node.sources()`). `plugins/python/walk.py`
prints per-module counts and a cell-type histogram. Exit test: CTest `python_walk` reads the
committed BLIF fixtures through Python, and the counts/histogram equal the C `stats` pass output
for the same file (skipped, not failed, where cffi is absent — as today).

## Out of scope (Phase 1)

Executable hooks with JSON over stdin/stdout (needs a JSON reader, Phase 7); matcher-pattern
registration (Phase 4); reader/writer registration beyond passes; thread safety.
