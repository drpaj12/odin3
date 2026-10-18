module m(input [7:0] a, input [16:0] b, output [16:0] y);
  assign y = a ~^ b;
endmodule
