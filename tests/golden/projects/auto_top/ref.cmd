# Oracle for tests/golden/projects/auto_top. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
"$YOSYS" -q -p "read_verilog src/leaf.v src/root.v src/mid.v; hierarchy -top root; synth; dffunmap; write_blif out/ref.blif"
