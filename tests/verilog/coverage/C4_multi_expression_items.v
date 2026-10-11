// C4: several expressions per case item — variants: three expressions 3'd0, 3'd1, 3'd2:, two expressions, a single expression, default
module c4_multi_item (input [2:0] s, input [3:0] a, b, output reg [3:0] y);
  always @*
    case (s)
      3'd0, 3'd1, 3'd2: y = a;
      3'd3, 3'd4: y = b;
      3'd5: y = a & b;
      default: y = 4'd0;
    endcase
endmodule
