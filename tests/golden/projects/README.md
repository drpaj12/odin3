# tests/golden/projects — project-input corpus

Test inputs for the project path (DESIGN §4.0 is the grammar; PHASE1 #18, #19): one small
design per case, written once as sources and expressed in every project format that can
express it, with the record each format must parse to, the design it resolves to, and an oracle
netlist (or, for a negative case, the located error). Odin III cannot read these yet; the
corpus is here so the readers have their tests waiting.

`tools/project-fixtures/project-fixtures check` keeps the corpus self-consistent and prints the
counts per phase (CTest `project_fixtures`, and `python_tools` through
`tests/tools/test_project_fixtures.py`, so CI runs it). `project-fixtures oracle` reruns every
`ref.cmd` in a scratch copy of its case, smallest case first, checks the tool versions the
`ref.cmd` records, and compares with the committed `ref.blif` (`--write` updates both); it needs
Yosys and GHDL, so it runs locally, not in CI.

## Phases

Every case parses to its expected record from `.o3proj`, `-f` and `.qsf` in **Phase 2**
(records do not need a front end). Elaboration, the cross-format netlist comparison and the
oracle are due in the case's `phase`: **2** Verilog/BLIF, **5** SystemVerilog, VHDL and mixed
(slang, GHDL), **6** VQM. The `odin2` format (`"format_phase": {"odin2": "odin2-reader"}`)
joins when the Odin II XML reader lands. Negative cases are phase 2.

## Case matrix

Formats: `o3proj` = `project.o3proj`, `-f` = `files.f`, `qsf` = the case's `.qpf` (or the
`.qsf` given in `entry`), `odin2` = `odin2.xml`. A format is absent when it cannot express the
case (`-f`: no parameters, libraries, arch or rules; `.qsf`: always a top; Odin II XML: only
Verilog/BLIF files, an arch file, an output and optimizations).

| Case | Phase | Features | Formats | Oracle / expected error |
|---|---|---|---|---|
| `auto_top` | 2 | verilog, auto top, odin2 ignored elements | o3proj -f odin2 | yosys |
| `blif_netlist` | 2 | blif, netlist input, odin2 inputs form | o3proj -f odin2 | yosys |
| `defines` | 2 | verilog, defines, +define+, VERILOG_MACRO, ifdef | o3proj -f qsf | yosys |
| `f_diamond` | 2 | verilog, nested -f, duplicate listing | o3proj -f | yosys |
| `f_options` | 5 | verilog, systemverilog, -top, $VAR, -sv, CRLF | o3proj -f | yosys |
| `f_vs_F` | 2 | verilog, -f, -F, nested lists | o3proj -f | yosys |
| `include_dirs` | 2 | verilog, incdir, include search order | o3proj -f qsf | yosys |
| `include_order` | 2 | verilog, incdir order, SEARCH_PATH | o3proj -f qsf | yosys |
| `lib_v_y` | 2 | verilog, -v, -y, +libext+, libfile/libdir | o3proj -f | yosys |
| `mixed_verilog_top` | 5 | mixed, verilog top, vhdl | o3proj -f qsf | ghdl+yosys |
| `mixed_vhdl_top` | 5 | mixed, vhdl top, verilog | o3proj -f qsf | ghdl+yosys |
| `multi_dir` | 2 | verilog, multi-file, directories, odin2 inputs form | o3proj -f qsf odin2 | yosys |
| `nested_f` | 2 | verilog, nested -f, -F, incdir | o3proj -f | yosys |
| `o3proj_arch_rules` | 2 | verilog, arch, available, hide, cell, map, thresholds, limit, patterns, flow | o3proj | yosys |
| `odin2_arch` | 2 | verilog, odin2 arch_file, odin2 optimizations, split | o3proj odin2 | yosys |
| `qpf_revisions` | 2 | verilog, qpf revisions, top params | o3proj qsf | yosys |
| `qsf_ignored` | 2 | verilog, qsf ignored, device, Tcl syntax | o3proj qsf | yosys |
| `qsf_mapping` | 2 | verilog, mapping rules, qsf translation | o3proj qsf | yosys |
| `qsf_qip` | 2 | verilog, QIP_FILE, qip_path idiom, nested qip | o3proj qsf | yosys |
| `qsf_user_libs` | 2 | verilog, USER_LIBRARIES, VERILOG_INCLUDE_FILE, -hdl_version | o3proj qsf | yosys |
| `qsf_vqm` | 6 | vqm, qsf entry, VQM_FILE | o3proj qsf | none |
| `relative_cwd` | 2 | verilog, relative paths | o3proj -f qsf odin2 | yosys |
| `single_file` | 2 | verilog, single file, auto top, odin2 legacy form | o3proj -f qsf odin2 | yosys |
| `sv_package` | 5 | systemverilog, package, compile order | o3proj -f qsf | yosys |
| `top_param` | 2 | verilog, top params, generate | o3proj qsf | yosys |
| `vhdl_dep_order` | 5 | vhdl, analysis order | o3proj -f qsf | ghdl+yosys |
| `vhdl_generic` | 5 | vhdl, generic override | o3proj qsf | ghdl+yosys |
| `vhdl_same_entity` | 5 | vhdl, libraries, same unit name | o3proj qsf | hand+yosys |
| `vhdl_same_filename` | 5 | vhdl, libraries, same file name | o3proj qsf | ghdl+yosys |
| `vhdl_two_libs` | 5 | vhdl, libraries, top in a library | o3proj qsf | ghdl+yosys |
| `vhdl_work` | 5 | vhdl, multi-file, work | o3proj -f qsf | ghdl+yosys |
| `neg_ambiguous_revision` | 2 | negative, ambiguous revision, import | o3proj qsf | `ambiguous_revision` |
| `neg_ambiguous_top` | 2 | negative, ambiguous top | o3proj -f odin2 | `ambiguous_top` |
| `neg_duplicate_module` | 2 | negative, duplicate module | o3proj -f qsf odin2 | `duplicate_module` |
| `neg_f_cycle` | 2 | negative, -f cycle | -f | `f_cycle` |
| `neg_include_not_found` | 2 | negative, include not found | o3proj -f qsf odin2 | `include_not_found` |
| `neg_missing_file` | 2 | negative, missing file | o3proj -f qsf odin2 | `missing_file` |
| `neg_no_top` | 2 | negative, no top | o3proj -f odin2 | `no_top` |
| `neg_syntax` | 2 | negative, syntax | o3proj -f qsf odin2 | `syntax` |
| `neg_undefined_variable` | 2 | negative, undefined variable | -f | `undefined_variable` |
| `neg_unknown_file_type` | 2 | negative, unknown file type | -f | `unknown_file_type` |
| `neg_unknown_key` | 2 | negative, unknown key | o3proj | `unknown_key` |
| `neg_unknown_option` | 2 | negative, unknown option | -f qsf | `unknown_option` |
| `neg_unknown_top` | 2 | negative, unknown top | o3proj -f qsf | `unknown_top` |
| `neg_unresolved_module` | 2 | negative, unresolved module | o3proj -f qsf odin2 | `unresolved_module` |

Oracles: `yosys`; `ghdl+yosys` (GHDL `--synth --out=verilog`, then Yosys); `hand+yosys`
(`vhdl_same_entity`: GHDL writes both `core` entities as one module name, so `ref/equiv.v` is a
hand-written Verilog equivalent; it is equivalent to `vhdl_same_filename`'s GHDL reference);
none (`qsf_vqm`: Yosys has no Cyclone V primitive library; Phase 6).

## A case directory

- Sources (`src/`, `lib/`, `net/`, `arch/`, `flow/`, `ip/`, …): tiny files over several
  directories. Fixtures use synchronous resets and initial values only, because
  `tools/equiv-check` rejects asynchronous latches.
- Project files for the formats in `formats` only (`files.f` and nested lists, `.qpf` +
  `.qsf`, `.qip`, `project.o3proj`, `odin2.xml`). Every file in the case (sources and project
  files) is used by some format, or listed in `unused` and then must not be read (`lib_v_y`
  keeps an `-y` cell nobody instantiates, `include_order` a shadowed header, `qpf_revisions` the
  revision not chosen).
- `expected.json`:
  - `description`, `features`, `phase` (2, 5 or 6), `formats`, `format_phase` (only
    `{"odin2": "odin2-reader"}`, exactly when `odin2` is a format);
  - optional `entry` (format → project file, e.g. `relative_cwd` keeps them in `proj/`,
    `qsf_vqm` gives a `.qsf` directly), `env` (the environment a `-f` list's `$VAR` sees; the
    harness passes nothing else), `options` (`{"qsf": {"revision": …}}`, the `--revision`
    option), `unused`;
  - positive cases: `project`, the normalized record (below); `format_overrides`, the fields a
    format states differently while elaborating to the same design — only `top`, `ignored` and
    `libdirs` (`SEARCH_PATH` is also a library directory) for `qsf`, and `top`, `output`,
    `ignored` for `odin2`; `resolved`: `top` (`library.name`), `units` (the design units the
    elaboration reaches from the top, `library.name`, sorted), `read` (every source file read:
    listed files, `` `include``d headers and library files loaded — a superset of the files
    holding `units`, e.g. a `-v` file whose other modules are unused; sorted), `order` (design
    files in analysis order); `oracle` and, for `null` or `hand+yosys`, `oracle_reason`;
  - negative cases: `error`: `kind`, `at` (`file:line` relative to the case; one string, one
    per format, or `null` when the error has no single location) and `names` (the path, key,
    option, header, module, candidates, revisions, variable, or `-f` chain).
- `ref.cmd` + `ref.blif` (positive cases with an oracle): a bash script run in a scratch copy
  of the case with `$YOSYS` and `$GHDL` set, writing `out/ref.blif`. Line 3 records the tool
  versions (`# versions: Yosys … | GHDL …`). Verilog/SV: `read_verilog [-sv] [-I…] [-D…]`,
  `hierarchy -top … [-libdir …]`, `synth`, `dffunmap` (flip-flops as BLIF `.latch`es, which
  `tools/equiv-check` reads), `write_blif`; top parameters via `chparam` + `rename -top`; VHDL:
  `ghdl -a --work=<lib>` for other libraries, `ghdl --synth --std=08 [-g…] --out=verilog` per
  top, then Yosys. Mapping rules, arch and flow are stored, not applied, in Phase 2, so the
  reference is plain synthesis.

### The normalized record

Every key is always present, in this order. Paths are relative to the case directory, `/`
separated, normalized: a reader records each path as given and as resolved (absolute), and the
test harness makes the absolute path relative to the case directory before comparing.

| Key | Value |
|---|---|
| `files` | `[{path, language, library}]` in listing order, each (path, library) once; language `verilog`, `systemverilog`, `vhdl`, `blif`, `vqm`, `edif`; library default `work` |
| `libfiles` | `-v` / `libfile`: `[{path, language}]` |
| `libdirs`, `libext` | `-y` / `libdir` / `SEARCH_PATH` / `USER_LIBRARIES` directories; `+libext+` / `libext` (none given: `.v`) |
| `incdirs` | include path, in order |
| `defines` | `[{name, value}]`, `value` null for a name alone; a string macro's value keeps its quotes |
| `top` | `null` (select automatically) or `module` / `library.module` |
| `params` | `[{scope, name, value, type}]`: scope the top or an instance path under it; type `number` or `string` |
| `arch` | `{kind: o3lib\|vpr_xml, path}` or `{kind: device, family, device}`, in load order |
| `rules` | `map` (`scope`, `subject`, `action` `to`/`soft`/`keep`, `cells`, optional `min_width`/`max_width`/`min_depth`), `split` (`subject`, `width`, `depth`), `limit` (`cell`, `count`), `patterns` (`path`) |
| `inventory` | `available`: `[{cell, count}]` |
| `overrides` | `hide` (`cell`), `cell` (`name`, `from`, `params`) |
| `flow` | flow script path or null (stored, not executed) |
| `output` | Odin II `<output>`: `{path, format}` or null |
| `ignored` | sorted, unique names of ignored `.qsf`/`.qip` assignments and commands (`LOCATION` for pins, `-hdl_version`) or Odin II elements/attributes; one info line `info: <file>: ignored: A, B, …` |
| `duplicates` | sorted paths listed more than once (read once); one info line |

The error kinds are listed at the end of DESIGN §4.0.

## Adding a case

1. Make `tests/golden/projects/<case>/` with the sources and one project file per format that
   can express the case (tens of lines per file).
2. `tools/project-fixtures/project-fixtures parse tests/golden/projects/<case> <format>` prints
   what each format parses to; write `expected.json` from it, and check that every difference
   between formats is intended (a permitted `format_overrides` entry, else fix the fixture).
3. Positive case: write `ref.cmd` (header as in the others), then
   `project-fixtures oracle --case <case> --write`.
4. `project-fixtures check`, `tools/lint.sh`, and add the case to the matrix above.

## Contract for the readers

For every case and every format in its `formats`, from any working directory, in the phases
above:

1. `odin3 read_project <entry>` (with the case's `env` and `options`) produces the expected
   record (`project` with that format's `format_overrides`), including the `ignored` and
   `duplicates` info lines.
2. Every format of a case elaborates to the same IR — identical under `tools/netlist-compare` —
   whose top, units and analysis order match `resolved`.
3. That netlist is equivalent to `ref.blif` under `tools/equiv-check` (cases with an oracle).
4. A negative case fails with the expected `kind` at the expected `file:line`, naming `names`,
   in every format.
