module m(input en, input [7:0] d, output reg [7:0] q);
  always @* if (en) q = d;
endmodule
