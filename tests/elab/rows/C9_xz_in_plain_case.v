// C9: x and z bits in the items of a plain case — variants: an item with x bits, an item with a z bit
module c9_xz_items (input [1:0] s, output reg [1:0] y);
  always @*
    case (s)
      2'b0x: y = 2'd1;
      2'b1z: y = 2'd2;
      default: y = 2'd0;
    endcase
endmodule
