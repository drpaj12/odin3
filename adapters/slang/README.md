# adapters/slang — Phase 5
C++ shim linking slang; walks slang's elaborated AST and emits the Odin III AST through the C ABI. Exports exactly one C function, `odin3_read_slang(...)`, built as a separate shared library. The only C++ allowed outside third_party/ (spec §4.2, D8′).
