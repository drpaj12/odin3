module m(input [7:0] a, input [7:0] b, input s, output [7:0] y);
  assign y = s ? b : a;
endmodule
