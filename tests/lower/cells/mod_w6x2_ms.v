module m(input signed [5:0] a, input [1:0] b, output [5:0] y);
  assign y = a % b;
endmodule
