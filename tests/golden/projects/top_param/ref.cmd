# Oracle for tests/golden/projects/top_param. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
"$YOSYS" -q -p "read_verilog src/stage_reg.v src/top.v; chparam -set WIDTH 16 -set DEPTH 3 top; hierarchy -top top; synth; rename -top top; dffunmap; write_blif out/ref.blif"
