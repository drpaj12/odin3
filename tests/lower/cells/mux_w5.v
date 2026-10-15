module m(input [4:0] a, input [4:0] b, input s, output [4:0] y);
  assign y = s ? b : a;
endmodule
