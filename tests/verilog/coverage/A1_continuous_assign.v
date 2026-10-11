// A1: continuous assignment — variants: to a net, to a bit, to a part, to a concatenation, a list assign a = …, b = …;
module a1_assign (input [3:0] a, b, output [3:0] y, output [7:0] w, output c, output [3:0] s, output p, q);
  assign y = a;
  assign w[0] = a[0];
  assign w[7:1] = {b, a[3:1]};
  assign {c, s} = a + b;
  assign p = a[1], q = b[1];
endmodule
