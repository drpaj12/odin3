// C6: default not last and an empty item statement — variants: default between items, an item whose statement is ; (2'd1: ;)
module c6_default_position (input [1:0] s, input [3:0] a, b, output reg [3:0] y);
  always @*
    case (s)
      2'd0: y = a;
      default: y = b;
      2'd1: ;
      2'd2: y = a ^ b;
    endcase
endmodule
