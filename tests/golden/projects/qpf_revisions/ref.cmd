# Oracle for tests/golden/projects/qpf_revisions. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
# versions: Yosys 0.69+270 (git sha1 c4a0a2c48, Release, GNU /usr/bin/c++ 13.3.0) | GHDL 4.1.0 (Ubuntu 4.1.0+dfsg-0ubuntu2.1) [Dunoon edition]
set -euo pipefail
mkdir -p out
# Revision fast: STAGES 2.
"$YOSYS" -q -p "read_verilog src/mac.v src/top.v; chparam -set STAGES 2 top; hierarchy -top top; synth; rename -top top; dffunmap; write_blif out/ref.blif"
