module leaf #(parameter W=4) (input [W-1:0] a, input [W-1:0] b, output [W-1:0] y);
  reg [W-1:0] t;
  always @* t = a & b;
  assign y = t ^ a;
endmodule
module mid (input [3:0] a, input [3:0] b, output [3:0] y);
  leaf #(.W(4)) u (.a(a), .b(b), .y(y));
endmodule
module top (input [3:0] x, input [3:0] z, output [3:0] o, output [3:0] p);
  mid m1 (.a(x), .b(z), .y(o));
  mid m2 (.a(z), .b(x), .y(p));
endmodule
