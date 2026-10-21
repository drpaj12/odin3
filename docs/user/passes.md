# Command and pass reference

This page is generated when the documentation is built. The command-line options come from
`odin3 --help`; each pass entry joins its row of `docs/PASSES.md` (signature, phase, views,
source files, tests, notes) with the usage text the registered pass prints. Nothing here is
copied by hand: a pass added to `docs/PASSES.md` appears on the next build, and the build fails
if no `odin3` binary can be found to read `--help` from.

## Command line

```{include} _generated/cli_options.md
```

A script is a list of commands separated by `;` or newlines, so these are the same run:

```bash
odin3 -p "read_verilog a.v; proc; opt; write_blif out.blif"
odin3 script.o3
```

## Passes

Entries are in pipeline order, as in `docs/PASSES.md`.

```{include} _generated/passes_body.md
```
