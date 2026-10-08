# D8': Core in structured C17 with a public C ABI

Status: Accepted (supersedes D8, the C++20 choice in spec v0.1)
Date: 2026-10-08

## Context
The v0.1 spec chose C++20 (D8). v0.2 changed this. The spec's rationale: simplest possible code
for agent authorship and human review; a stable plugin boundary; libabc is C; Odin II lineage;
complexity is enforceable by lint in C. Claude Code is the primary developer and the human is
architect/reviewer (spec §2).

## Decision
- The core is structured C17 with a public C ABI, `include/odin3/odin3.h`.
- Plugins are `.so` files or executables speaking that ABI; Python tools and plugins use it
  through `cffi`.
- C++ is confined to `adapters/` (the slang shim) and `third_party/`.
- Platform: WSL2 Ubuntu, CMake, GitHub Actions CI.
- Supersedes the C++20 choice in v0.1.

## Consequences
Positive:
- Code an agent can write and a human can review in one pass (spec §15 goal).
- A plugin boundary that is stable and language-neutral (spec §15.3).
Negative:
- No C++ containers: `util/` provides `arena`, `vec`, `hashmap`, `str`, `log`; nothing else
  defines a generic container (spec §15.1).
- Strict, tooling-heavy rules (spec §15.1-15.2): C17 with `-Werror`, no `goto` except
  `cleanup:`, no VLAs, no recursion in IR traversals, `odin3_status` returns, lint gate in
  pre-commit and required CI.
- The slang adapter is a separate C++ shared library exporting one C function,
  `odin3_read_slang(...)` (spec §4.2).
Forces on later phases:
- Phase 0: lint gate, CI, `tools/check_rules.py`, `tools/check-symbols.sh`.
- Phase 1: C ABI v0; "a Python plugin can walk the IR". PHASE0 §A.2 #10: host-side plugin ABI
  check (exported `odin3_plugin_abi_version`) lands in Phase 1.
- Phase 5: slang adapter in C++ under `adapters/`.
- IR is IDs, not pointers, so the ABI can hand out IDs safely (spec §5.1).
- PHASE0 §A.2 #6, #9, #11, #12 tune the lint rules (abort ban outside `check`, `ODIN3_WERROR`,
  one-letter names, magic numbers off under `tests/`).

## Related
- D8 (superseded), D2 (adapters), D3 (libabc), D7.
- Spec preamble, §2, §4.2, §5.1, §5.2, §15.
- Open questions touched: §13 #4 (memory layout).

Revisit if: lint-enforced simplicity stops paying for itself (for example the C container and
error-handling rules measurably slow agent or human work), or the C ABI cannot express a plugin
hook point the project needs.
