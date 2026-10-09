# Oracle for tests/golden/projects/mixed_vhdl_top. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
# vadd is an unbound component to GHDL: it emits an instance of module vadd.
"$GHDL" --synth --std=08 --workdir=out --out=verilog src/top.vhd -e top > out/top.v
"$YOSYS" -q -p "read_verilog src/verilog/vadd.v out/top.v; hierarchy -top top; synth; dffunmap; write_blif out/ref.blif"
