# tests/golden/projects — project-input corpus

Test inputs for the Phase 2 project path (DESIGN §4.0; PHASE1 #18, #19): one small design per
case, written once as sources and expressed in every project format that can express it, with
the record each format must parse to and an oracle netlist of the elaborated design. Odin III
cannot read these yet; the corpus is here so Phase 2 has its tests waiting.

`tools/project-fixtures/project-fixtures check` keeps the corpus self-consistent (CTest
`project_fixtures`, and `python_tools` through `tests/tools/test_project_fixtures.py`, so CI
runs it). `project-fixtures oracle` reruns every `ref.cmd`, smallest case first, and compares
with the committed `ref.blif` (`--write` updates it); it needs Yosys and GHDL, so it runs
locally, not in CI.

## Case matrix

Formats: `o3proj` = `project.o3proj`, `-f` = `files.f`, `qsf` = the case's one `.qpf` (and the
`.qsf` of its revision), `odin2` = `odin2.xml`. A format is absent when it cannot express the
case (`-f` has no top, parameters or libraries; `.qsf` always has a top; Odin II XML has only
Verilog/BLIF files, an arch file and an output).

| Case | Features | Formats | Oracle / expected error |
|---|---|---|---|
| `single_file` | verilog, single file, auto top, odin2 legacy form | o3proj -f qsf odin2 | yosys |
| `multi_dir` | verilog, multi-file, directories, odin2 inputs form | o3proj -f qsf odin2 | yosys |
| `include_dirs` | verilog, incdir, include search order | o3proj -f qsf | yosys |
| `defines` | verilog, defines, +define+, VERILOG_MACRO, ifdef | o3proj -f qsf | yosys |
| `top_param` | verilog, top params, generate | o3proj qsf | yosys |
| `auto_top` | verilog, auto top, odin2 ignored elements | o3proj -f odin2 | yosys |
| `sv_package` | systemverilog, package, compile order | o3proj -f qsf | yosys |
| `vhdl_work` | vhdl, multi-file, work | o3proj -f qsf | ghdl+yosys |
| `vhdl_two_libs` | vhdl, libraries | o3proj qsf | ghdl+yosys |
| `vhdl_same_filename` | vhdl, libraries, same file name | o3proj qsf | ghdl+yosys |
| `vhdl_same_entity` | vhdl, libraries, same unit name | o3proj qsf | none: GHDL writes both entities as `module core` |
| `mixed_verilog_top` | mixed, verilog top, vhdl | o3proj -f qsf | ghdl+yosys |
| `mixed_vhdl_top` | mixed, vhdl top, verilog | o3proj -f qsf | ghdl+yosys |
| `lib_v_y` | verilog, -v, -y, +libext+, libfile/libdir | o3proj -f | yosys |
| `nested_f` | verilog, nested -f, -F, incdir | o3proj -f | yosys |
| `qpf_revisions` | verilog, qpf revisions, top params | o3proj qsf | yosys |
| `qsf_ignored` | verilog, qsf ignored, device, Tcl syntax | o3proj qsf | yosys |
| `qsf_mapping` | verilog, mapping rules, qsf translation | o3proj qsf | yosys |
| `blif_netlist` | blif, netlist input, odin2 inputs form | o3proj -f odin2 | yosys |
| `o3proj_arch_rules` | verilog, arch, available, hide, cell, map, thresholds, limit, patterns, flow | o3proj | yosys |
| `relative_cwd` | verilog, relative paths (project files in `proj/`) | o3proj -f qsf odin2 | yosys |
| `neg_missing_file` | negative, missing file | o3proj -f qsf odin2 | `missing_file` |
| `neg_unknown_key` | negative, unknown key | o3proj | `unknown_key` |
| `neg_ambiguous_top` | negative, ambiguous top | o3proj -f odin2 | `ambiguous_top` |
| `neg_no_top` | negative, no top (mutual instantiation) | o3proj -f odin2 | `no_top` |
| `neg_f_cycle` | negative, -f cycle | -f | `f_cycle` |
| `neg_include_not_found` | negative, include not found | o3proj -f qsf odin2 | `include_not_found` |
| `neg_duplicate_module` | negative, duplicate module | o3proj -f qsf odin2 | `duplicate_module` |

## A case directory

- `src/…` (also `lib/`, `net/`, `arch/`, `flow/`): the design, tiny files over several
  directories. Every file is used by some format, or listed in `"unused"` (and then must not be
  read: `lib_v_y` keeps an `-y` cell nobody instantiates).
- Project files: `project.o3proj`, `files.f` (plus nested lists), `<proj>.qpf` +
  `<revision>.qsf`, `odin2.xml`; only for the formats listed in `expected.json`
  (`"entry"` overrides the location, e.g. `relative_cwd` keeps them in `proj/`).
- `expected.json`:
  - `description`, `features`, `formats` (subset of `o3proj`, `f`, `qsf`, `odin2`);
  - positive cases: `project`, the normalized record (below); `format_overrides`, the fields a
    format states differently while elaborating to the same design (`-f`/`odin2` have no top
    and so auto-select it; a `.qsf` always has one, the revision name by default; `odin2`
    records its `<output>` and ignored elements; a `.qsf` lists its ignored assignments);
    `resolved`: `top`, `units` reached from it (`library.name`, sorted), `read` (every source
    file read: project files, `` `include``d files, library files loaded, sorted); `oracle`:
    `"yosys"`, `"ghdl+yosys"` or `null` with `oracle_reason`;
  - negative cases: `error`: `kind`, `at` (`file:line` relative to the case, one string for
    all formats or one per format, or `null` when the error has no single location) and
    `names` (the offending path, key, macro file, module, the candidates, or the `-f` chain).
- `ref.cmd` + `ref.blif` (positive cases with an oracle): the exact commands, run with the
  case directory as the working directory (a scratch copy) and `$YOSYS`, `$GHDL` set, writing
  `out/ref.blif`. Verilog/SV: `read_verilog [-sv] [-I…] [-D…]`, `hierarchy -top`, `synth`,
  `dffunmap` (so flip-flops are BLIF `.latch`es that `tools/equiv-check` reads), `write_blif`;
  top parameters via `chparam` + `rename -top`; VHDL: `ghdl --synth --std=08 --out=verilog`
  per top (after `ghdl -a --work=<lib>` for other libraries), then Yosys over everything.
  Mapping rules are stored, not applied, in Phase 2, so the reference is plain synthesis.

### The normalized record

Every key is always present, in this order (paths relative to the case directory, `/`
separated, normalized):

| Key | Value |
|---|---|
| `files` | `[{path, language, library}]` in order; language `verilog`, `systemverilog`, `vhdl`, `blif`, `vqm`, `edif`; library default `work` |
| `libfiles` | `-v` / `libfile`: `[{path, language}]`, modules used only when instantiated |
| `libdirs`, `libext` | `-y` / `libdir` directories and `+libext+` / `libext` extensions (none: the bare module name) |
| `incdirs` | include path, in order |
| `defines` | `[{name, value}]`, `value` null for a name alone |
| `top` | declared top(s); `[]` = select automatically |
| `params` | `[{module, name, value}]` top parameter overrides, values as text |
| `arch` | `[{kind: o3lib\|vpr_xml, path}]` or `{kind: device, family, device}`, in load order |
| `rules` | `map` (`scope`, `subject`, `action` `to`/`soft`/`keep`, `cells`, optional `min_width`/`max_width`/`min_depth`), `limit` (`cell`, `count`), `patterns` (`path`) |
| `inventory` | `available`: `[{cell, count}]` |
| `overrides` | `hide` (`cell`), `cell` (`name`, `from`, `params`) |
| `flow` | flow script path or null (stored, not executed) |
| `output` | Odin II `<output>`: `{path, format}` or null |
| `ignored` | sorted names of ignored `.qsf` assignments / commands or Odin II elements; a reader prints them in one info line: `info: <file>: ignored: A, B, C` |

### Error kinds

`missing_file` (a named file or directory does not exist), `unknown_key` (`.o3proj`),
`unknown_option` (`-f`), `unknown_file_type` (no language for an extension), `syntax`,
`f_cycle`, `include_not_found` (located at the `` `include``), `duplicate_module` (located at
the second definition), `ambiguous_top` (names the candidates), `no_top`, `unknown_top`,
`unresolved_module`. Errors in a project file are located at its line; source errors at the
source line.

## Adding a case

1. Make `tests/golden/projects/<case>/` with the sources and one project file per format that
   can express the case (keep it small: tens of lines per file).
2. `tools/project-fixtures/project-fixtures parse tests/golden/projects/<case> <format>` prints
   what each format parses to; write `expected.json` from it, and check every difference
   between formats is intended (it becomes a `format_overrides` entry or a fixture fix).
3. Positive case: write `ref.cmd`, then `project-fixtures oracle --case <case> --write`.
4. `project-fixtures check`, `tools/lint.sh`, and add the case to the matrix above.

## Phase 2 contract

For every case and every format in its `formats`:

1. `odin3 read_project <entry>` (any working directory) produces the expected record
   (`project` with that format's `format_overrides`), including the `ignored` info line.
2. All formats of a case produce the same IR: their netlists are identical under
   `tools/netlist-compare`, and the elaborated top and units match `resolved`.
3. The netlist is equivalent to `ref.blif` under `tools/equiv-check` (cases with an oracle).
4. A negative case fails with the expected `kind` at the expected `file:line`, naming
   `names`, in every format.
