# Oracle for tests/golden/projects/lib_v_y. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
# The files the -v/-y search loads (lib/cells/or2.v is not needed).
"$YOSYS" -q -p "read_verilog src/top.v lib/prims.v lib/cells/and2.v lib/cells/mux2.v; hierarchy -top top; synth; dffunmap; write_blif out/ref.blif"
