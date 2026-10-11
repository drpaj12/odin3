// T4: @ident without parentheses — variants: @clk on an always, @en inside a statement
module t4_at_ident (input clk, en, d, output reg q, r);
  always @clk
    q <= d;
  always @(posedge clk) begin
    @en r <= d;
  end
endmodule
