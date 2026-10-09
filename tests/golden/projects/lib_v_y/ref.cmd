# Oracle for tests/golden/projects/lib_v_y. Run by `tools/project-fixtures/project-fixtures
# oracle` in a scratch copy of this directory, with $YOSYS and $GHDL set; writes out/ref.blif.
# versions: Yosys 0.69+270 (git sha1 c4a0a2c48, Release, GNU /usr/bin/c++ 13.3.0) | GHDL 4.1.0 (Ubuntu 4.1.0+dfsg-0ubuntu2.1) [Dunoon edition]
set -euo pipefail
mkdir -p out
# The -v file is read whole; Yosys's own -libdir search stands for -y +libext+.v.
"$YOSYS" -q -p "read_verilog src/top.v lib/prims.v; hierarchy -top top -libdir lib/cells; synth; dffunmap; write_blif out/ref.blif"
