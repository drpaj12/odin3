# Oracle for tests/golden/projects/vhdl_same_filename. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
set -euo pipefail
mkdir -p out
"$GHDL" -a --std=08 --workdir=out --work=alpha src/alpha/core.vhd
"$GHDL" -a --std=08 --workdir=out --work=beta src/beta/core.vhd
"$GHDL" --synth --std=08 --workdir=out -Pout --out=verilog src/top.vhd -e top > out/top.v
"$YOSYS" -q -p "read_verilog out/top.v; hierarchy -top top; synth; dffunmap; write_blif out/ref.blif"
