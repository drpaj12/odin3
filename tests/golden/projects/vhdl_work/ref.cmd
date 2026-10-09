# Oracle for tests/golden/projects/vhdl_work. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
"$GHDL" --synth --std=08 --workdir=out --out=verilog src/sub/add4.vhd src/sub/reg4.vhd src/top.vhd -e top > out/top.v
"$YOSYS" -q -p "read_verilog out/top.v; hierarchy -top top; synth; dffunmap; write_blif out/ref.blif"
