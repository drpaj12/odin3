// 4-state fixture: how an unknown selector is taken by if, case and ?: (iverilog: else branch,
// default branch, bitwise merge).
module ifx(input c, input [1:0] s, input a, input b, output reg y, output reg z, output t);
  always @* begin
    if (c) y = a; else y = b;
  end
  always @* begin
    case (s)
      2'd0: z = a;
      2'd1: z = b;
      default: z = 1'b0;
    endcase
  end
  assign t = c ? b : a;
endmodule
