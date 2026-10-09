# Oracle for tests/golden/projects/sv_package. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
"$YOSYS" -q -p "read_verilog -sv src/pkg/alu_pkg.sv src/alu.sv src/top.sv; hierarchy -top top; synth; dffunmap; write_blif out/ref.blif"
