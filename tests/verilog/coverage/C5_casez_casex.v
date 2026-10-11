// C5: casez and casex — variants: casez with ? patterns and a z pattern, casex with x patterns and a ? pattern, default in each
module c5_casez_casex (input [3:0] r, output reg [1:0] y1, y2);
  always @*
    casez (r)
      4'b1???: y1 = 2'd3;
      4'b01??: y1 = 2'd2;
      4'b001z: y1 = 2'd1;
      default: y1 = 2'd0;
    endcase
  always @*
    casex (r)
      4'b1xxx: y2 = 2'd3;
      4'b01xx: y2 = 2'd2;
      4'b001?: y2 = 2'd1;
      default: y2 = 2'd0;
    endcase
endmodule
