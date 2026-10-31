module comb(input [3:0] a, input [3:0] b, input sel, output [4:0] sum, output [3:0] mx, output eq);
  assign sum = a + b;
  assign mx = sel ? a : b;
  assign eq = (a == b);
endmodule
