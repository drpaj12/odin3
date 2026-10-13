// T7: blocking assignments — variants: combinational always, a temporary written then read in a clocked always (t = a & b; r <= t;)
module t7_blocking (input clk, a, b, output reg y, r);
  reg t;
  always @*
    y = a ^ b;
  always @(posedge clk) begin
    t = a & b;
    r <= t;
  end
endmodule
