// unequal widths, signed and unsigned
module subprobe2 (input [7:0] ua, input [3:0] ub, input signed [7:0] sa, input signed [3:0] sb,
  output [8:0] uy, output signed [8:0] sy);
  wire [8:0] u = ua - ub;
  wire signed [8:0] s = sa - sb;
  assign uy = u;
  assign sy = s;
endmodule
