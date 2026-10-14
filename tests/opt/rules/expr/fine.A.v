// expr/fine.A: -fine: $logic_not of a constant -> one-bit constant
// options: -fine
module t(output y);
  wire [3:0] c = 4'b1111;
  assign y = !c;
endmodule
