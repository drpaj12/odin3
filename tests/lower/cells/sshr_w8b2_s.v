module m(input signed [7:0] a, input [1:0] b, output [7:0] y);
  assign y = a >>> b;
endmodule
