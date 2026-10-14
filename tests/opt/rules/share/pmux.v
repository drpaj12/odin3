// share/pmux: $add cells sharing an operand under a $pmux
module t(input s0, input s1, input [3:0] a, input [3:0] b, input [3:0] c, input [3:0] d, output reg [3:0] y);
  always @* begin
    (* parallel_case *) case (1'b1)
      s0: y = a + b;
      s1: y = a + c;
      default: y = d;
    endcase
  end
endmodule
