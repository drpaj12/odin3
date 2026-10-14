module mux_narrow_unused(input s, input [3:0] a, input [3:0] b, output [1:0] y);
  wire [3:0] t = s ? a : b;
  assign y = t[1:0];
endmodule
