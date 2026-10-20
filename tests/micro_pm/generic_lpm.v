// 4.2 B-6 (review H2): lpm_compare is a black box of lib/lpm.o3lib, not a model of the arch; it
// has a library record, so it is not an unknown block.
module generic_lpm (
    input clk,
    input [3:0] a,
    input [3:0] b,
    output y
);
  lpm_compare #(.lpm_width(4)) u_cmp (.dataa(a), .datab(b), .aeb(y), .clock(clk));
endmodule
