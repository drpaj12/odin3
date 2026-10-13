// Task 22: casex treats x and z in an item as wildcards.
module t (input [3:0] r, output reg [2:0] g);
  always @*
    casex (r)
      4'b1xxx: g = 3'd4;
      4'bx1xx: g = 3'd3;
      4'bxx1x: g = 3'd2;
      4'bxxx1: g = 3'd1;
      default: g = 3'd0;
    endcase
endmodule
