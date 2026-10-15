module m(input [2:0] a, input [2:0] b, input s, output [2:0] y);
  assign y = s ? b : a;
endmodule
