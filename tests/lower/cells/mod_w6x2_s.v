module m(input signed [5:0] a, input signed [1:0] b, output [5:0] y);
  assign y = a % b;
endmodule
