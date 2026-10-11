// T12: event controls inside a body (REJECT) — variants: @(posedge clk) x = a; @(negedge clk) y = b; in one always, an event control with no statement
module t12_body_events (input clk, a, b, output reg x, y);
  always begin
    @(posedge clk) x = a;
    @(negedge clk) y = b;
    @(posedge clk);
  end
endmodule
