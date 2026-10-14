// merge/pmux_pairs: $pmux with the same (select, data) pairs in another order -> one
module t(input s0, input s1, input [1:0] a, input [1:0] b, input [1:0] c, output reg [1:0] y, output reg [1:0] z);
  always @* begin
    (* parallel_case *) case (1'b1)
      s0: y = a;
      s1: y = b;
      default: y = c;
    endcase
  end
  always @* begin
    (* parallel_case *) case (1'b1)
      s1: z = b;
      s0: z = a;
      default: z = c;
    endcase
  end
endmodule
