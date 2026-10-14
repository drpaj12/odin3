// expr/bitwise_logic_one_const: 4-bit AND with one constant side -> per-bit wiring
module t(input [3:0] a, output [3:0] y);
  assign y = a & 4'b1010;
endmodule
