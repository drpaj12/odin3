module m(input signed [7:0] a, input [2:0] b, output [7:0] y);
  assign y = a >>> b;
endmodule
