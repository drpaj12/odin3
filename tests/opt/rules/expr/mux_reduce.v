// expr/mux_reduce: $pmux with an undefined select bit -> narrower
// options: -mux_undef
module t(input s, input t, input [1:0] a, input [1:0] c, input [1:0] d, output reg [1:0] y);
  always @* begin
    (* parallel_case *) case (1'b1)
      s: y = a;
      1'bx: y = 2'd2;
      t: y = d;
      default: y = c;
    endcase
  end
endmodule
