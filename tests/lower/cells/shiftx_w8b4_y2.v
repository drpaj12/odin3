module m(input [7:0] a, input [3:0] b, output [1:0] y);
  assign y = a[b +: 2];
endmodule
