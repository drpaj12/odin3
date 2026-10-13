// N7: net declaration assignment — variants: wire w = e with a range, a scalar wire with a literal, two declarators with initialisers in one declaration
module n7_net_init (input [3:0] a, b, output [3:0] y);
  wire [3:0] w = a;
  wire t = 1'b1, u = 1'b0;
  assign y = w;
endmodule
