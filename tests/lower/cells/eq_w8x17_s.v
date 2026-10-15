module m(input signed [7:0] a, input signed [16:0] b, output y);
  assign y = a == b;
endmodule
