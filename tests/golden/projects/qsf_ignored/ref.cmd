# Oracle for tests/golden/projects/qsf_ignored. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
"$YOSYS" -q -p "read_verilog src/blink.v; hierarchy -top blink; synth; dffunmap; write_blif out/ref.blif"
