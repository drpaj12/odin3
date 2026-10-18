module m(input [3:0] a, input [2:0] b, output [1:0] y);
  assign y = a[b +: 2];
endmodule
