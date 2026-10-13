// merge/nomux: -nomux: identical $add cells merge, identical $mux cells stay
// options: -nomux
module t(input s, input [3:0] a, input [3:0] b, output [3:0] y, output [3:0] z, output [3:0] u, output [3:0] v);
  assign y = a + b;
  assign z = a + b;
  assign u = s ? a : b;
  assign v = s ? a : b;
endmodule
