module m(input [16:0] a, input [16:0] b, input s, output [16:0] y);
  assign y = s ? b : a;
endmodule
