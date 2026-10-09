# Oracle for tests/golden/projects/relative_cwd. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
"$YOSYS" -q -p "read_verilog src/sub/parity.v src/top.v; hierarchy -top top; synth; dffunmap; write_blif out/ref.blif"
