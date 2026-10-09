// multiply.v — behavioural model of the VTR `multiply` hard block for the sim-check reference.
// lib/vtr.o3lib: `out = a * b`, unsigned, A_WIDTH + B_WIDTH result bits; every golden declares
// 36 x 36 -> 72 (tests/golden/techlib), the defaults here.
module multiply(a, b, out);
  parameter A_WIDTH = 36;
  parameter B_WIDTH = 36;
  input [A_WIDTH-1:0] a;
  input [B_WIDTH-1:0] b;
  output [A_WIDTH+B_WIDTH-1:0] out;
  assign out = a * b;
endmodule
