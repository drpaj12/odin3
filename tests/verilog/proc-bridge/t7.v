// T7 (2D spec 8.3): a blocking temporary read by the next, non-blocking, statement.
module t (input clk, a, b, output reg r);
  reg t;
  always @(posedge clk) begin
    t = a & b;
    r <= t;
  end
endmodule
