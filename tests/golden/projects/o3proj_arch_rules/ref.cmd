# Oracle for tests/golden/projects/o3proj_arch_rules. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
# Arch, rules and flow are stored, not applied, in Phase 2: plain synthesis is the reference.
"$YOSYS" -q -p "read_verilog src/top.v; hierarchy -top top; synth; dffunmap; write_blif out/ref.blif"
