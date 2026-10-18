module m(input signed [7:0] a, input signed [16:0] b, output [16:0] y);
  assign y = a ~^ b;
endmodule
