# Oracle for tests/golden/projects/qsf_mapping. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
# Mapping rules are stored, not applied, in Phase 2: the plain synthesis is the reference.
"$YOSYS" -q -p "read_verilog src/mul.v src/top.v; hierarchy -top top; synth; dffunmap; write_blif out/ref.blif"
