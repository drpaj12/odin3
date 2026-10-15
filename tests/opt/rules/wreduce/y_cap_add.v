// the sum of two 8-bit operands needs 9 bits; the rest of the 32-bit Y is 0
module y_cap_add(input [7:0] a, input [7:0] b, output [31:0] y);
  assign y = a + b;
endmodule
