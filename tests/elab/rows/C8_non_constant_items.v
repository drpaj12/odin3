// C8: non-constant case items — variants: case (1'b1) priority encoder with expression items, items that are signals
module c8_non_constant_items (input [3:0] r, input [1:0] s, a, b, output reg [1:0] y, output reg z);
  always @*
    case (1'b1)
      r[3]: y = 2'd3;
      r[2]: y = 2'd2;
      r[1]: y = 2'd1;
      default: y = 2'd0;
    endcase
  always @*
    case (s)
      a: z = 1'b1;
      b: z = 1'b0;
      default: z = 1'bx;
    endcase
endmodule
