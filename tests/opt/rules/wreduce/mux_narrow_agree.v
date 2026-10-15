// the top bit is the same net on both inputs: it bypasses the mux
module mux_narrow_agree(input s, input c, input [2:0] a, input [2:0] b, output [3:0] y);
  assign y = s ? {c, a} : {c, b};
endmodule
