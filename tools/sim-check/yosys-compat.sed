# yosys-compat.sed — makes a Yosys Verilog dump acceptable to Icarus, for older Yosys (e.g. the
# apt 0.33 in CI). Such a Yosys writes the empty TABLE of a $sop with DEPTH 0 (a .names with
# inputs and no rows) as `{0{1'b0}}`, a zero replication Icarus refuses in a parameter value.
# Newer Yosys writes `.TABLE()`. models/sop.v ignores TABLE when DEPTH is 0.
s/\.TABLE({0{1'b0}})/.TABLE(1'b0)/g
