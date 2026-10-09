# Oracle for tests/golden/projects/qpf_revisions. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
"$YOSYS" -q -p "read_verilog src/mac.v src/top.v; chparam -set STAGES 1 top; hierarchy -top top; synth; rename -top top; dffunmap; write_blif out/ref.blif"
