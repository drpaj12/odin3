// expr/identwrt.b: $add with a zero A operand -> B
module t(input [3:0] a, output [3:0] y);
  wire [3:0] z = 4'b0000;
  assign y = z + a;
endmodule
