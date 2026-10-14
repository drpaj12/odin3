// expr/identwrt.a: $add with a zero B operand -> A
module t(input [3:0] a, output [3:0] y);
  wire [3:0] z = 4'b0000;
  assign y = a + z;
endmodule
