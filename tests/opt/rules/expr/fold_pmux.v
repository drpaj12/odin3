// expr/fold_pmux: all-constant $pmux -> its 4-state fold
module t(input a, output reg [3:0] y);
  wire s0 = a & 1'b0;
  wire s1 = a | 1'b1;
  always @* begin
    (* parallel_case *) case (1'b1)
      s0: y = 4'd1;
      s1: y = 4'd2;
      default: y = 4'd3;
    endcase
  end
endmodule
