# src/ast — Phase 2
Common AST and scoped symbol table shared by all front ends (spec §4.5; design: docs/specs/2026-10-09-2A-ast-design.md). One store per read run, held by the design and freed at the end of its read pass unless kept (AST-15/16); provenance keeps raw source-manager locations either way.
