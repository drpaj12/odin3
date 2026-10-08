# Odin III — working agreement for Claude Code

Odin III is an MIT-licensed HDL elaboration and front-end synthesis framework for FPGA CAD
research, the successor to Odin and Odin II. It reads Verilog/SystemVerilog/VHDL and structural
netlists into one hierarchical, provenance-tracked IR, does architecture-driven partial mapping,
links ABC for soft logic, and writes netlists for VTR, Intel/Altera and visualisation.
Peter is the architect; you are the primary developer. You proceed on your own recommendations
and ask him only when he is needed (rule 11).

**Start sessions in this directory** (`cd ~/odin3-ws/odin3 && claude`): `.claude/settings.json`
and the project skills load only from the session's starting directory. `~/odin3-ws/golden`,
`work` and `external` are reachable as additional directories.

## Read first

- `docs/DESIGN.md` — the spec. Read the relevant sections before any task.
- `docs/DESIGN.md` §5 **and** `docs/IR.md` before touching anything in `src/ir/`.
- The op-registry file (once it exists, Phase 1) for any task.
- `docs/PHASE0.md` §A.1 (how work is split across agents) and §A.2 (decisions log).
- `docs/ADR/` — the decisions behind the design. Do not contradict an ADR; propose a change.

**When unsure about an IR invariant, stop and ask. Do not guess.**

## Build and test

```bash
cmake --preset debug && cmake --build --preset debug    # C17, ASan + UBSan
ctest --preset debug                                     # unit + tools tests
cmake --preset release && cmake --build --preset release
tools/lint.sh                                            # the gate; /lint wraps it
python3 -m venv .venv && .venv/bin/pip install -r requirements-dev.txt   # once
```

Oracles and comparison (see `docs/ORACLES.md`):

```bash
export VTR_ROOT=~/odin3-ws/external/vtr-verilog-to-routing
tools/run-oracle.sh [--name group/leaf] design.v arch.xml   # goldens -> ~/odin3-ws/golden
tools/netlist-compare/netlist-compare a.blif b.blif          # 0 identical, 1 differ, 2 error
tools/equiv-check/equiv-check a.blif b.blif                  # 0 equivalent, 1 not, 2 error
tools/golden-sample/golden-sample [--write]                  # pick the committed BLIF sample
```

## Working rules

1. **One pass per PR.** Every pass has a unit test, a golden test, and a `docs/PASSES.md` row
   (`/new-pass` scaffolds them).
2. **Per-pass workflow:** `superpowers:brainstorming` → `superpowers:writing-plans` → Peter
   approves → `superpowers:executing-plans` → `/run-micro` → PR. No code before an approved plan.
3. **`check` runs before and after every pass in Debug.** Never disable it to get a pass through.
4. **Never write to `~/odin3-ws/external/`** (upstream VTR, Yosys, …): read only. Rebuilding an
   upstream oracle (docs/ORACLES.md) changes every golden, so it is Peter's call and runs outside
   the sandbox.
5. **Never commit to `main`.** It is protected on GitHub (PR + green `ci`, squash merge only) and
   locally (pre-commit `no-commit-to-branch`). Work on `feat/<name>` branches; merge your own PR
   when `ci` is green (decision #17).
6. **Run `/lint` before every commit.** The pre-commit hook runs the gate without clang-tidy; CI
   runs all of it. Never weaken `.clang-tidy`, add `NOLINT`, or relax a gate without Peter's
   approval.
7. **Run `/run-micro` before claiming a micro passes.** Evidence before assertions: quote the
   command and its output.
8. **Push early** (decision #16): commit and push the branch at every green checkpoint, before
   any risky step (large refactor, dispatching agents, regenerating goldens), and at session end.
9. **The repo is the source of truth** (decision #15). Docs live only in `docs/`; never create
   copies elsewhere. Goldens live in `~/odin3-ws/golden`: the full set on disk; its repo
   (`odin3-golden`) commits every `.prov`/`.log` but only the BLIF sample `tools/golden-sample`
   picks (decision #30). The full BLIF set is an archive hosted off GitHub.
10. **Peter reviews** every IR/invariant change and every mapping algorithm (spec §12). No PR
    merges red.
11. **Autonomy by default** (decision #17): act on your recommendation and log it in the current
    phase's decisions log as an agent default he may override. Stop for Peter only when a step
    needs his hands, is irreversible or outward-facing, changes a design rule (spec, ADRs, IR
    invariants), or you have no clear recommendation — then ask in one numbered list.
12. **Multi-agent work** follows `docs/PHASE0.md` §A.1: you orchestrate and own git; implementer
    agents get disjoint files and a written interface; a read-only critique agent reviews every
    PR before it opens; you triage every finding, list rejected ones in the PR description, and
    get one re-check on each high finding after fixing it.

## Code standard (spec §15; the gate checks most of it, the rest is on you)

- **C17 only** in the core. Flags: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wstrict-prototypes -Wmissing-prototypes -Wvla -Werror`. C++ only in `adapters/` and
  `third_party/`.
- **Small functions:** cyclomatic complexity ≤ 15, ≤ 60 lines, ≤ 5 parameters (lizard,
  clang-tidy).
- **No recursion in IR traversals** — use an explicit worklist (the IR can be 2M nodes deep).
- **No VLAs. No `goto`** except the single `cleanup:` label for error paths.
- **Errors:** return `odin3_status`. `exit()` only in `src/cli/`. `abort()`, `raise(SIGABRT)`,
  `__builtin_trap()` only in `check` (debug). `assert()` is allowed.
- **Memory:** IR objects live in their Module's arena; passes never `malloc` IR objects.
  Cross-references are `uint32_t` IDs, never pointers. Only `src/util/` defines containers
  (`arena`, `vec`, `hashmap`, `str`, `log`).
- **Headers:** one per module; include what you use. Everything public goes through
  `include/odin3/odin3.h`, and every function there gets a paragraph: purpose, ownership of
  returned memory, failure modes.
- **Names:** `odin3_` prefix for the public ABI; snake_case; one-letter names only `i`/`j`/`k`.
  Name your constants (`readability-magic-numbers` is on outside `tests/`).
- **Format:** clang-format (LLVM base, 100 columns, 4 spaces) is applied automatically after
  every edit of a `.c`/`.h` file.
- **Python** (`tools/`, `plugins/`): stdlib-only tools, `ruff check` and `mypy --strict` clean,
  tests with `unittest` under `tests/tools/`.

## Layout

`include/odin3/` public ABI · `src/{util,ir,ast,frontends,passes,backends,sim,cli,api}` core ·
`adapters/slang` SystemVerilog shim · `plugins/` C and Python examples · `tools/` oracle and lint
tools · `tests/{unit,golden,micro,tools}` · `third_party/{abc,unity}` submodules · `docs/` spec,
ADRs, IR, passes, oracles, phase checklists.

## Where things are going

Phase 0 (now): environment, oracles, goldens, gate. Phase 1: `util/`, core IR, op registry, pass
manager, `check`, provenance, C ABI v0, BLIF read/write, writers, simulator. See spec §12 for
Phases 2–7 and their exit tests.
