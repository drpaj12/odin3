// C7: default without a colon — variants: default y = b; (a statement), default ; (a null statement)
module c7_default_no_colon (input [1:0] s, input a, b, output reg y, z);
  always @*
    case (s)
      2'd0: y = a;
      default y = b;
    endcase
  always @* begin
    z = 1'b0;
    case (s)
      2'd1: z = a;
      default ;
    endcase
  end
endmodule
