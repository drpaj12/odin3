// reduce/pmux_equal_slices: $pmux slices equal to each other -> their selects ORed
module t(input s0, input s1, input s2, input [1:0] a, input [1:0] b, input [1:0] c, output reg [1:0] y);
  always @* begin
    (* parallel_case *) case (1'b1)
      s0: y = a;
      s1: y = b;
      s2: y = b;
      default: y = c;
    endcase
  end
endmodule
