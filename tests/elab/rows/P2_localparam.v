// P2: localparam — variants: one localparam, a list, a ranged localparam, a localparam used in an assign
module p2_localparam (output [31:0] y, output [3:0] z);
  localparam L = 4;
  localparam M = 5, N = 6;
  localparam [3:0] R = 4'd9;
  assign y = M;
  assign z = R;
endmodule
