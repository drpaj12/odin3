// T8 (2D spec 8.3): a non-blocking assignment reads the value at the block's start.
module t (input clk, a, output reg r1, r2);
  always @(posedge clk) begin
    r1 <= a;
    r2 <= r1;
  end
endmodule
