// T13: wait (REJECT) — variants: wait (a) with a statement, wait (b); with no statement
module t13_wait (input clk, a, b, output reg r);
  always @(posedge clk) begin
    wait (a) r = b;
    wait (b);
  end
endmodule
