# Oracle for tests/golden/projects/blif_netlist. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
"$YOSYS" -q -p "read_blif net/adder.blif; hierarchy -top fa_reg; synth; dffunmap; write_blif out/ref.blif"
