---
name: new-pass
description: Use when the user asks to add, scaffold, or start a new Odin III pass (e.g. /new-pass opt, "add a proc pass"). Creates branch feat/<name>, the PASSES.md row, and (once src/ir/ exists) the pass source, Unity unit test, golden test directory and CMake registration.
---

# new-pass

Argument: `<name>` (lowercase snake_case, a pass from spec section 6 such as `opt`, `proc`, `lower`). Ask if missing or ambiguous.

## 1. Preflight
- Read `docs/DESIGN.md` sections 6 and 12, `docs/IR.md`, `docs/PASSES.md`.
- Refuse if `src/passes/<name>.c` or a PASSES.md row for `<name>` already exists.
- Working tree must be clean and not on `main` unless you are about to branch. Never commit to `main`.

## 2. Branch and PASSES.md row (always)
1. `git switch -c feat/<name>` from an up-to-date `main`.
2. Add a row to `docs/PASSES.md`, in pipeline order (spec section 6), replacing the "No passes yet" placeholder: `| <name> | <phase> | <input view -> output view> | src/passes/<name>.c | tests/golden/<name>/ | <notes> |`. Fill phase and views from spec section 6/12; write `TBD` rather than guess.

## 3. IR gate (hard stop)
Check that `src/ir/` contains sources, not just `README.md` (`ls src/ir`). The IR arrives in Phase 1.
If it does not exist: STOP after step 2. Do not create `.c`, tests, golden dirs or CMake entries, and do not invent IR, arena, vec or pass-manager APIs. Tell the human: "Branch feat/<name> and the PASSES.md row are ready. I cannot scaffold code yet: src/ir/ does not exist (Phase 1)." Commit the row (lint first) and push per decision #16.

## 4. Scaffold (only when src/ir/ exists)
Read the real IR headers and the pass-manager registration API first; copy an existing pass's shape.
- `src/passes/<name>.c`: one pass per file, registered with the pass manager. Add a header only if another module includes it (one header per module, spec 15.1); do not create `<name>.h` otherwise.
- `tests/unit/test_<name>.c`: Unity (`third_party/unity`), one failing test first. Tests may use magic numbers.
- `tests/golden/<name>/`: `input.*` and `expected.blif` (canonical), compared with `tools/netlist-compare`; add a short `README` stating what the case covers.
- CMake: add the source to the library list and an executable + `add_test(NAME test_<name> ...)` with `LABELS unit`, mirroring `test_api` in `CMakeLists.txt`; add a ctest entry for the golden compare.
- Run `cmake --preset debug && cmake --build --preset debug && ctest --preset debug`; the new test must fail for the right reason, nothing else may break.

## 5. Per-pass workflow (spec section 12)
`/superpowers:brainstorm` -> `write-plan` -> **human approves the plan** -> `execute-plan` -> `/run-micro` -> PR.

## Rules
- One pass per PR. Every pass has a golden test.
- Call IR `check` before and after the pass in debug builds.
- Functions return `odin3_status`; no `exit`/`abort`/`goto` (except `cleanup:`); no recursion in IR traversals; IR objects come from the Module arena.
- Run `/lint` before every commit (it is the pre-commit gate).
- Commit and push at every green checkpoint, before risky steps, and at session end (decision #16). `git push` needs human approval.
- Stop and ask on any IR-invariant doubt; IR/invariant changes and mapping algorithms need human review.
