// Two specializations of one parameterized child (W=4 and W=8), one of them instantiated twice.
module leaf #(parameter W = 4) (input [W-1:0] a, input [W-1:0] b, output [W-1:0] y);
  assign y = (a & b) | (a ^ b);
endmodule
module top (input [7:0] x, input [7:0] z, output [3:0] o, output [3:0] p, output [7:0] q);
  leaf #(.W(4)) u4a (.a(x[3:0]), .b(z[3:0]), .y(o));
  leaf #(.W(4)) u4b (.a(z[3:0]), .b(x[3:0]), .y(p));
  leaf #(.W(8)) u8 (.a(x), .b(z), .y(q));
endmodule
