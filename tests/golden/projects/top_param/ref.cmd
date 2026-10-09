# Oracle for tests/golden/projects/top_param. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
# versions: Yosys 0.69+270 (git sha1 c4a0a2c48, Release, GNU /usr/bin/c++ 13.3.0) | GHDL 4.1.0 (Ubuntu 4.1.0+dfsg-0ubuntu2.1) [Dunoon edition]
set -euo pipefail
mkdir -p out
"$YOSYS" -q -p "read_verilog src/stage_reg.v src/top.v; chparam -set WIDTH 16 -set DEPTH 3 top; hierarchy -top top; synth; rename -top top; dffunmap; write_blif out/ref.blif"
