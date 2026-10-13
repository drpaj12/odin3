// expr/fine.B: -fine: $logic_and with a constant B -> reduced constant
// options: -fine
module t(input a, output y);
  wire [3:0] c = 4'b1111;
  assign y = a && c;
endmodule
