# Oracle for tests/golden/projects/vhdl_same_entity. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
# versions: Yosys 0.69+270 (git sha1 c4a0a2c48, Release, GNU /usr/bin/c++ 13.3.0) | GHDL 4.1.0 (Ubuntu 4.1.0+dfsg-0ubuntu2.1) [Dunoon edition]
set -euo pipefail
mkdir -p out
# GHDL 4.1 writes alpha.core and beta.core both as `module core`, so the reference is
# ref/equiv.v, a hand-written Verilog equivalent (alpha_core, beta_core, top).
"$YOSYS" -q -p "read_verilog ref/equiv.v; hierarchy -top top; synth; dffunmap; write_blif out/ref.blif"
