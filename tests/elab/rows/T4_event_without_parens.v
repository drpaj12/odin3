// T4: @ident without parentheses — variants: @clk on an always (dropped: @en inside a statement, stage 1 rejects it)
module t4_at_ident (input clk, en, d, output reg q, r);
  always @clk
    q <= d;
  always @(posedge clk)
    r <= d & en;
endmodule
