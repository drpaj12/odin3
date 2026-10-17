// 9-bit results from 8-bit operands: exercises the borrow / sign-extension top stage
module subwide (input [7:0] ua, input [7:0] ub, input signed [7:0] sa, input signed [7:0] sb,
  output [8:0] uy9, output signed [8:0] sy9);
  assign uy9 = {1'b0,ua} - {1'b0,ub};
  assign sy9 = sa - sb;
endmodule
