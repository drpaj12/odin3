---
name: lint
description: Use before every commit, when the user says /lint or asks to run the lint gate, and whenever CI reports a lint failure. Runs tools/lint.sh in full (clang-format, rules, clang-tidy, cppcheck, lizard, ruff, mypy) and fixes violations at the source.
---

# lint

## Run
Run `tools/lint.sh` from the repo root (full gate, includes clang-tidy; add `--no-tidy` only if the user asks for a quick pass). Output of passing tools is hidden; failing tools print their diagnostics, then a summary table. Missing tools count as FAIL (`pip install -r requirements-dev.txt`, or use `.venv`).

## Summarize
Group the diagnostics **by file, then by tool** (table or nested list): `file -> tool -> line: message`. Lead with the summary table PASS/FAIL per tool. If everything passes, say so in one line.

## Fix
- **clang-format**: run `clang-format -i <files>` on the offending files (never under `third_party/`), then re-run the gate. Formatting is applied automatically anyway; no need to ask.
- **rules** (`tools/check_rules.py`): `exit`/`_exit`/`_Exit`/`quick_exit` only in `src/cli/`; no `abort()`, `raise(SIGABRT)`, `__builtin_trap()` outside `check` in debug; `goto` only for the single `cleanup:` label. Return `odin3_status` instead.
- **clang-tidy**: `bugprone`, `cert`, `misc`, `performance`, `readability` (function size, cognitive complexity); one-letter names are only `i`/`j`/`k`; magic numbers are allowed under `tests/` only. Fix the code (name the constant, split the function).
- **cppcheck / lizard**: lizard limits are cyclomatic complexity 15, 60 lines, 5 parameters per function in `src/`. Split functions or pass a struct. No recursion in IR traversals, no VLAs.
- **ruff / mypy --strict**: fix the Python in `tools/`, `plugins/`, `tests/tools/`; add real type annotations.
Quote the rule from spec section 15 (`docs/DESIGN.md`) when explaining a violation.

## Never
- Never weaken `.clang-tidy`, `.clang-format`, `pyproject.toml` lint config, `tools/lint.sh`, CI or the pre-commit hook to get a pass.
- Never add `NOLINT`, `// cppcheck-suppress`, `# noqa` or `# type: ignore` without the human's explicit approval of that specific suppression.
- Never edit `third_party/` or `external/` to silence anything (they are excluded or read-only).
If a rule looks wrong for the case, stop and ask the human.

Re-run `tools/lint.sh` until `lint: PASS` before claiming clean.
