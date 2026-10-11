// T12: an event control inside an always body (REJECT).
module m (input clk, a, output reg x, z);
  always @(posedge clk) begin
    x = a;
    @(negedge clk) z = a;
  end
endmodule
