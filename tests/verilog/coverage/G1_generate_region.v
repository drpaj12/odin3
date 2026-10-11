// G1: a generate region with several items — variants: genvar declared inside the region, a net declaration, an assign, a loop generate and an always in one region, an empty region
module g1_region (input clk, input [3:0] a, output [3:0] y, output reg q);
  generate
    genvar i;
    wire [3:0] t;
    assign t = ~a;
    for (i = 0; i < 4; i = i + 1) begin : g
      assign y[i] = t[i];
    end
    always @(posedge clk)
      q <= a[0];
  endgenerate
  generate
  endgenerate
endmodule
