module m(input signed [2:0] a, input signed [4:0] b, output [4:0] y);
  assign y = a & b;
endmodule
