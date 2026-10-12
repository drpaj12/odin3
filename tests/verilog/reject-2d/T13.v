// T13: wait (REJECT).
module m (input clk, a, output reg x);
  always @(posedge clk) begin
    wait (a) x = 1'b1;
  end
endmodule
