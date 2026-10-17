// control: result width == operand width, so the top stage is only the MSB sum bit
module subeq (input [7:0] ua, input [7:0] ub, input signed [7:0] sa, input signed [7:0] sb,
  output [7:0] uy, output signed [7:0] sy);
  assign uy = ua - ub;
  assign sy = sa - sb;
endmodule
