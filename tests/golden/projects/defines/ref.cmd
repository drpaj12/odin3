# Oracle for tests/golden/projects/defines. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
"$YOSYS" -q -p "read_verilog -DUSE_SUB -DWIDTH=12 src/top.v; hierarchy -top top; synth; dffunmap; write_blif out/ref.blif"
