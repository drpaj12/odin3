// Task 22: casez wildcards (ANY) with overlapping items: the first matching item wins.
module t (input [3:0] r, output reg [1:0] g);
  always @*
    casez (r)
      4'b1???: g = 2'd3;
      4'b01??: g = 2'd2;
      4'b001?: g = 2'd1;
      default: g = 2'd0;
    endcase
endmodule
