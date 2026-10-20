// 5.2: a power ($pow instance; Yosys has no SAT model of it, so the proof is unproven).
module cell_pow(input [3:0] a, input [2:0] e, output [7:0] y);
  assign y = a ** e;
endmodule
