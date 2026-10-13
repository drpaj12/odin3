// T20: null statements — variants: ; as the then-branch of an if, ; in a begin…end, ; as a case-item body, ; as the else-branch
module t20_null (input a, b, input [1:0] s, output reg y, z);
  always @* begin
    y = 1'b0;
    if (a)
      ;
    else
      y = b;
    ;
  end
  always @*
    case (s)
      2'd0: ;
      default: z = a;
    endcase
endmodule
