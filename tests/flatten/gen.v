// A generate loop of instances and a conditional generate, nested one level.
module leaf (input a, input b, output y);
  assign y = a ^ b;
endmodule
module mid (input [2:0] a, input [2:0] b, output [2:0] y);
  genvar i;
  generate
    for (i = 0; i < 3; i = i + 1) begin : g
      leaf u (.a(a[i]), .b(b[i]), .y(y[i]));
    end
  endgenerate
endmodule
module top (input [2:0] x, input [2:0] z, output [2:0] o, output c);
  mid m (.a(x), .b(z), .y(o));
  generate
    if (1) begin : sel
      leaf v (.a(x[0]), .b(z[2]), .y(c));
    end
  endgenerate
endmodule
