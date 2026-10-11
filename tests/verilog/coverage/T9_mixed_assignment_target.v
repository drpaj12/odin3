// T9: one target assigned with both = and <= (REJECT) — variants: = then <= in one clocked always
module t9_mixed_target (input clk, a, b, output reg q);
  always @(posedge clk) begin
    q = a;
    q <= b;
  end
endmodule
