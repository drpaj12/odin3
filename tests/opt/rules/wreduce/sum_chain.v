// Yosys's help example: 32-bit adders become the widths the operands need
module sum_chain(input [3:0] a, input [3:0] b, input [3:0] c, output [7:0] y);
  assign y = a + b + c + 1;
endmodule
