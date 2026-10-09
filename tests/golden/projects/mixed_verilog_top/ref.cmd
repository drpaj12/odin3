# Oracle for tests/golden/projects/mixed_verilog_top. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
"$GHDL" --synth --std=08 --workdir=out --out=verilog src/vhdl/scale.vhd -e scale > out/scale.v
"$YOSYS" -q -p "read_verilog src/top.v out/scale.v; hierarchy -top top; synth; dffunmap; write_blif out/ref.blif"
