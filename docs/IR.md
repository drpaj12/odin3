# Odin III IR

Stub — written in Phase 1 before any code in `src/ir/`. It will specify, with the human's sign-off:

- Object model and ownership (Design, Module, Node, Pin, Net; arenas; ID types).
- Invariants enforced by `check` (driver counts, dangling pins, widths, granularity views).
- Op registry entry format and the granularity tags.
- Provenance fields and naming (`hier/path/cellname@file:line`).
- What a pass may and may not mutate.

Until then the source of truth is `docs/DESIGN.md` §5. When unsure about an invariant: stop and ask.
