module m(input en, input [3:0] d, output reg [3:0] q);
  always @* if (!en) q = d;
endmodule
