module m(input [1:0] a, input [1:0] b, input s, output [1:0] y);
  assign y = s ? b : a;
endmodule
