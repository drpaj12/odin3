# Oracle for tests/golden/projects/multi_dir. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
"$YOSYS" -q -p "read_verilog src/arith/adder.v src/ctrl/pick.v src/top.v; hierarchy -top top; synth; dffunmap; write_blif out/ref.blif"
